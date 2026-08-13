// Backend TIFF di atas libtiff.
//
// **Kenapa libtiff dan bukan pembaca TIFF mini.** TIFF bukan satu format
// melainkan sebuah wadah: LZW, deflate, dan PackBits; jalur strip dan jalur
// tile; predictor mendatar; 8, 16, dan 32 bit; bilangan bulat dan IEEE float.
// Heightmap yang benar-benar datang dari World Machine, Gaea, dan sumber GIS
// memakai kombinasi-kombinasi itu, bukan TIFF tanpa kompresi. Pembaca yang
// menangani sebagiannya akan menolak berkas yang justru paling sering dipakai —
// dan itu persis kegagalan yang membuat orang berhenti memakai jalurnya.
//
// libtiff sudah ada di setiap distro, berukuran ratusan kilobyte, dan
// mendekode semuanya secara transparan.

#include "Backend.h"

#if SIM_WITH_LIBTIFF

#include "PixelOps.h"

#include <tiffio.h>

#include <cstring>
#include <vector>

namespace sim::imageio {
namespace {

const std::vector<std::string>& TiffExtensions() {
    static const std::vector<std::string> kExtensions{".tif", ".tiff"};
    return kExtensions;
}

/// Menutup `TIFF*` di setiap jalur keluar; jalur galat di bawah ada belasan.
struct TiffScope {
    TIFF* handle = nullptr;
    ~TiffScope() {
        if (handle != nullptr) {
            TIFFClose(handle);
        }
    }
    TiffScope() = default;
    TiffScope(const TiffScope&) = delete;
    TiffScope& operator=(const TiffScope&) = delete;
};

/// Peringatan libtiff dibuang, galatnya tidak.
///
/// TIFF di alam liar penuh tag tidak dikenal dan pelanggaran kecil spesifikasi
/// yang tidak menghalangi pembacaan sama sekali; bawaan libtiff mencetaknya ke
/// stderr. Keluaran itu muncul di tengah log editor sebagai kalimat yang
/// terlihat seperti kesalahan padahal berkasnya dimuat dengan baik.
void SilenceWarnings() {
    static const bool once = [] {
        TIFFSetWarningHandler(nullptr);
        return true;
    }();
    (void)once;
}

struct TiffFormat {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t channels = 0;
    PixelType type = PixelType::UInt8;
};

/// Menerjemahkan tag TIFF menjadi bentuk yang dikenal mesin ini.
///
/// Yang tidak bisa diterjemahkan **ditolak dengan menyebut tagnya**, bukan
/// ditebak. TIFF 1-bit, 64-bit double, dan kanal berukuran campuran semuanya
/// sah menurut spesifikasinya dan tidak satu pun punya arti di jalur terrain —
/// memaksanya masuk menghasilkan tinggi yang salah tanpa satu pun galat.
bool ReadFormat(TIFF* tiff, TiffFormat& format, std::string& error) {
    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t samples = 1;
    uint16_t bits = 8;
    uint16_t sampleFormat = SAMPLEFORMAT_UINT;
    uint16_t planar = PLANARCONFIG_CONTIG;

    TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height);
    TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLESPERPIXEL, &samples);
    TIFFGetFieldDefaulted(tiff, TIFFTAG_BITSPERSAMPLE, &bits);
    TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLEFORMAT, &sampleFormat);
    TIFFGetFieldDefaulted(tiff, TIFFTAG_PLANARCONFIG, &planar);

    if (width == 0 || height == 0 || samples == 0) {
        error = "image is empty";
        return false;
    }
    if (planar != PLANARCONFIG_CONTIG) {
        error = "planar (separated) TIFF is not supported; save it with interleaved samples";
        return false;
    }
    if (samples > 4) {
        error = "TIFF has " + std::to_string(samples) + " samples per pixel; at most 4 are used";
        return false;
    }

    if (bits == 8 && sampleFormat != SAMPLEFORMAT_IEEEFP) {
        format.type = PixelType::UInt8;
    } else if (bits == 16 && sampleFormat != SAMPLEFORMAT_IEEEFP) {
        format.type = PixelType::UInt16;
    } else if (bits == 32 && sampleFormat == SAMPLEFORMAT_IEEEFP) {
        format.type = PixelType::Float32;
    } else {
        error = "unsupported TIFF sample layout: " + std::to_string(bits) + " bits, " +
                (sampleFormat == SAMPLEFORMAT_IEEEFP    ? "float"
                 : sampleFormat == SAMPLEFORMAT_INT     ? "signed integer"
                                                        : "unsigned integer");
        return false;
    }

    format.width = width;
    format.height = height;
    format.channels = samples;
    return true;
}

/// TIFF berjalur strip: baris demi baris.
///
/// libtiff mendekode kompresinya sendiri di balik `TIFFReadScanline`, jadi LZW,
/// deflate, dan PackBits tidak perlu diurus di sini sama sekali.
bool ReadStrips(TIFF* tiff, const TiffFormat& format, uint8_t* out, std::string& error) {
    const std::size_t stride = static_cast<std::size_t>(format.width) * format.channels *
                               BytesPerSample(format.type);
    for (uint32_t y = 0; y < format.height; ++y) {
        if (TIFFReadScanline(tiff, out + static_cast<std::size_t>(y) * stride,
                             static_cast<uint32_t>(y)) < 0) {
            error = "failed to read scanline " + std::to_string(y);
            return false;
        }
    }
    return true;
}

/// TIFF berubin: ubin demi ubin, lalu disusun.
///
/// Heightmap besar hampir selalu berubin — ia bentuk yang membuat pembacaan
/// sebagian mungkin, dan itulah alasan alat terrain memakainya.
bool ReadTiles(TIFF* tiff, const TiffFormat& format, uint8_t* out, std::string& error) {
    uint32_t tileWidth = 0;
    uint32_t tileHeight = 0;
    TIFFGetField(tiff, TIFFTAG_TILEWIDTH, &tileWidth);
    TIFFGetField(tiff, TIFFTAG_TILELENGTH, &tileHeight);
    if (tileWidth == 0 || tileHeight == 0) {
        error = "tiled TIFF without tile dimensions";
        return false;
    }

    const std::size_t sampleBytes = BytesPerSample(format.type);
    const std::size_t pixelBytes = sampleBytes * format.channels;
    const std::size_t imageStride = static_cast<std::size_t>(format.width) * pixelBytes;
    const std::size_t tileStride = static_cast<std::size_t>(tileWidth) * pixelBytes;

    std::vector<uint8_t> tile(static_cast<std::size_t>(TIFFTileSize(tiff)));
    for (uint32_t y = 0; y < format.height; y += tileHeight) {
        for (uint32_t x = 0; x < format.width; x += tileWidth) {
            if (TIFFReadTile(tiff, tile.data(), x, y, 0, 0) < 0) {
                error = "failed to read tile at " + std::to_string(x) + "," + std::to_string(y);
                return false;
            }
            // Ubin di tepi kanan dan bawah menyimpan baris selebar ubin penuh,
            // bukan selebar bagian yang terpakai. Menyalin seluruh ubin akan
            // menulis melewati ujung baris gambarnya.
            const uint32_t copyWidth = std::min(tileWidth, format.width - x);
            const uint32_t copyHeight = std::min(tileHeight, format.height - y);
            for (uint32_t row = 0; row < copyHeight; ++row) {
                std::memcpy(out + (static_cast<std::size_t>(y + row) * imageStride) +
                                static_cast<std::size_t>(x) * pixelBytes,
                            tile.data() + static_cast<std::size_t>(row) * tileStride,
                            static_cast<std::size_t>(copyWidth) * pixelBytes);
            }
        }
    }
    return true;
}

class TiffBackendImpl final : public IBackend {
public:
    const char* Name() const override { return "libtiff"; }

    const char* Version() const override { return SIM_LIBTIFF_VERSION_STRING; }

    const std::vector<std::string>& ReadableExtensions() const override {
        return TiffExtensions();
    }

    const std::vector<std::string>& WritableExtensions() const override {
        return TiffExtensions();
    }

    /// Menulis TIFF tanpa kehilangan satu bit pun.
    ///
    /// **Deflate, bukan tanpa kompresi.** Heightmap 4096² 16-bit berukuran 32 MB
    /// mentah dan sekitar sepertiganya setelah dimampatkan, dan deflate adalah
    /// kompresi tanpa kehilangan — round-trip tetap identik bit per bit, yang
    /// diperiksa uji I3.
    ImageIoResult Write(const std::filesystem::path& path, const Image& image) const override {
        ImageIoResult result;
        SilenceWarnings();

        std::error_code code;
        std::filesystem::create_directories(path.parent_path(), code);

        TiffScope scope;
        scope.handle = TIFFOpen(path.string().c_str(), "w");
        if (scope.handle == nullptr) {
            result.error = "cannot open " + path.string();
            return result;
        }

        const ImageDesc& desc = image.desc;
        const uint16_t bits = static_cast<uint16_t>(BytesPerSample(desc.type) * 8);
        TIFFSetField(scope.handle, TIFFTAG_IMAGEWIDTH, desc.width);
        TIFFSetField(scope.handle, TIFFTAG_IMAGELENGTH, desc.height);
        TIFFSetField(scope.handle, TIFFTAG_SAMPLESPERPIXEL, static_cast<uint16_t>(desc.channels));
        TIFFSetField(scope.handle, TIFFTAG_BITSPERSAMPLE, bits);
        TIFFSetField(scope.handle, TIFFTAG_SAMPLEFORMAT,
                     desc.type == PixelType::Float32 ? SAMPLEFORMAT_IEEEFP : SAMPLEFORMAT_UINT);
        TIFFSetField(scope.handle, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
        TIFFSetField(scope.handle, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(scope.handle, TIFFTAG_PHOTOMETRIC,
                     desc.channels == 1 ? PHOTOMETRIC_MINISBLACK : PHOTOMETRIC_RGB);
        TIFFSetField(scope.handle, TIFFTAG_COMPRESSION, COMPRESSION_DEFLATE);
        TIFFSetField(scope.handle, TIFFTAG_ROWSPERSTRIP,
                     TIFFDefaultStripSize(scope.handle, 0));

        const std::size_t stride =
            static_cast<std::size_t>(desc.width) * desc.channels * BytesPerSample(desc.type);
        for (uint32_t y = 0; y < desc.height; ++y) {
            // Const dibuang karena tanda tangan libtiff meminta pointer non-const
            // walau jalur tulisnya tidak mengubah buffernya.
            auto* row = const_cast<uint8_t*>(image.bytes.data() + static_cast<std::size_t>(y) * stride);
            if (TIFFWriteScanline(scope.handle, row, y, 0) < 0) {
                result.error = "cannot write " + path.string() + ": failed at row " +
                               std::to_string(y);
                return result;
            }
        }
        result.ok = true;
        return result;
    }

    ImageIoResult Read(const std::filesystem::path& path, const ReadOptions& options,
                       Image& out) const override {
        ImageIoResult result;
        SilenceWarnings();
        const std::string name = path.string();

        TiffScope scope;
        scope.handle = TIFFOpen(name.c_str(), "r");
        if (scope.handle == nullptr) {
            result.error = "cannot read " + name + ": not a TIFF file";
            return result;
        }

        TiffFormat format;
        if (std::string error; !ReadFormat(scope.handle, format, error)) {
            result.error = "cannot read " + name + ": " + error;
            return result;
        }

        ImageDesc desc;
        desc.width = format.width;
        desc.height = format.height;
        desc.channels = format.channels;
        desc.type = format.type;
        // TIFF menyimpan photometric, bukan ruang warna. Heightmap dan tekstur
        // datang lewat tag yang sama, jadi menebaknya di sini adalah tebakan
        // yang tidak bisa dikoreksi lagi di atasnya — yang memutuskan adalah
        // slot material yang memakainya.
        desc.colorSpace = ColorSpace::Unknown;
        out.Allocate(desc);

        const bool ok = TIFFIsTiled(scope.handle) != 0
                            ? ReadTiles(scope.handle, format, out.bytes.data(), result.error)
                            : ReadStrips(scope.handle, format, out.bytes.data(), result.error);
        if (!ok) {
            result.error = "cannot read " + name + ": " + result.error;
            return result;
        }

        ApplyReadOptions(options, name, out);
        result.ok = true;
        return result;
    }

    ImageIoResult Read(std::span<const uint8_t> bytes, std::string_view /*extensionHint*/,
                       const ReadOptions& /*options*/, Image& /*out*/) const override {
        ImageIoResult result;
        // Sengaja tidak didukung, dengan alasan yang sama seperti EXR: jalur
        // baca-dari-memori ada untuk thumbnail, dan thumbnail TIFF belum punya
        // pemakai.
        (void)bytes;
        result.error = "TIFF can only be read from a file, not from memory";
        return result;
    }

    ImageIoResult Probe(const std::filesystem::path& path, ImageDesc& out) const override {
        ImageIoResult result;
        SilenceWarnings();
        const std::string name = path.string();

        TiffScope scope;
        scope.handle = TIFFOpen(name.c_str(), "r");
        if (scope.handle == nullptr) {
            result.error = "cannot read " + name + ": not a TIFF file";
            return result;
        }

        TiffFormat format;
        if (std::string error; !ReadFormat(scope.handle, format, error)) {
            result.error = "cannot read " + name + ": " + error;
            return result;
        }

        // Tagnya saja; tidak ada satu strip pun yang didekode.
        out = ImageDesc{};
        out.width = format.width;
        out.height = format.height;
        out.channels = format.channels;
        out.type = format.type;
        result.ok = true;
        return result;
    }
};

}  // namespace

const IBackend* TiffBackend() {
    static const TiffBackendImpl backend;
    return &backend;
}

}  // namespace sim::imageio

#else  // SIM_WITH_LIBTIFF

namespace sim::imageio {

const IBackend* TiffBackend() { return nullptr; }

}  // namespace sim::imageio

#endif  // SIM_WITH_LIBTIFF
