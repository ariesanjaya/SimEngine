// Backend OpenEXR di atas tinyexr.
//
// **Satu header, diambil lewat FetchContent seperti stb dan cgltf.** tinyexr
// menangani ZIP, PIZ, dan RLE — yang benar-benar dipakai berkas EXR di jalur
// kerja ini. Pustaka gambar yang lebih besar sempat ditimbang dan ditolak;
// ukurannya beserta apa yang hilang karenanya ada di docs/DEPENDENCIES.md.
//
// Berkasnya tetap ikut dibangun tanpa tinyexr, mengikuti pola `UsdImport.cpp`:
// yang hilang ketika pustakanya tidak ada adalah kemampuannya, bukan berkasnya.

#include "Backend.h"

#if SIM_WITH_TINYEXR

#include "PixelOps.h"

#include <tinyexr.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace sim::imageio {
namespace {

const std::vector<std::string>& ExrExtensions() {
    static const std::vector<std::string> kExtensions{".exr"};
    return kExtensions;
}

/// Pembebas otomatis; jalur galat di bawah ada belasan dan tidak satu pun boleh
/// meninggalkan alokasi tinyexr.
struct ExrScope {
    EXRHeader header{};
    EXRImage image{};
    bool imageLoaded = false;

    ExrScope() {
        InitEXRHeader(&header);
        InitEXRImage(&image);
    }
    ~ExrScope() {
        if (imageLoaded) {
            FreeEXRImage(&image);
        }
        FreeEXRHeader(&header);
    }
    ExrScope(const ExrScope&) = delete;
    ExrScope& operator=(const ExrScope&) = delete;
};

std::string TakeError(const char* message) {
    if (message == nullptr) {
        return "unknown EXR error";
    }
    std::string text = message;
    FreeEXRErrorMessage(message);
    return text;
}

/// Urutan kanal seperti tersimpan di berkas, dipetakan ke urutan RGBA.
///
/// **EXR menyimpan kanalnya terurut abjad**, jadi peta RGB sungguhan datang
/// sebagai B, G, R — dan membacanya apa adanya menukar merah dengan biru pada
/// setiap berkas EXR yang pernah dimuat. tinyexr menyerahkannya apa adanya dan
/// tidak menyusun ulang sendiri; sebagian pustaka EXR lain melakukannya, jadi
/// jangan mengandalkan ingatan dari pustaka sebelah.
///
/// Berkas yang bukan RGB dibiarkan dalam urutan berkasnya. Ia bukan gambar
/// warna, jadi tidak ada urutan "benar" untuk dipaksakan — dan namanya ikut
/// terbawa di `channelNames` supaya pemanggil bisa menolaknya.
std::vector<int> ChannelOrder(const EXRHeader& header) {
    std::vector<int> order(static_cast<std::size_t>(header.num_channels));
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = static_cast<int>(i);
    }

    const auto find = [&header](const char* name) {
        for (int i = 0; i < header.num_channels; ++i) {
            if (std::strcmp(header.channels[i].name, name) == 0) {
                return i;
            }
        }
        return -1;
    };

    const int r = find("R");
    const int g = find("G");
    const int b = find("B");
    if (r < 0 || g < 0 || b < 0) {
        return order;
    }

    std::vector<int> rgba{r, g, b};
    if (const int a = find("A"); a >= 0) {
        rgba.push_back(a);
    }
    // Kanal lain — depth, mask, cryptomatte — ikut di belakang, dalam urutan
    // berkasnya. Tidak dibuang: pemanggil yang meminta seluruh kanal berhak
    // mendapatkannya.
    for (int i = 0; i < header.num_channels; ++i) {
        if (std::find(rgba.begin(), rgba.end(), i) == rgba.end()) {
            rgba.push_back(i);
        }
    }
    return rgba;
}

/// Menyalin data planar tinyexr menjadi piksel berselang-seling.
///
/// tinyexr menyerahkan `images[kanal][piksel]`; seluruh mesin ini bekerja
/// dengan piksel yang kanalnya berdampingan.
void InterleaveScanlines(const EXRImage& image, const std::vector<int>& order, float* out) {
    const auto width = static_cast<std::size_t>(image.width);
    const auto height = static_cast<std::size_t>(image.height);
    const std::size_t channels = order.size();
    for (std::size_t c = 0; c < channels; ++c) {
        const auto* source = reinterpret_cast<const float*>(image.images[order[c]]);
        for (std::size_t i = 0; i < width * height; ++i) {
            out[i * channels + c] = source[i];
        }
    }
}

/// EXR berubin disusun ulang menjadi satu gambar utuh.
///
/// tinyexr menyerahkan ubinnya mentah — dokumentasinya menyebut penyusunannya
/// urusan aplikasi. Berkas berubin bukan kasus langka: itu bentuk yang keluar
/// dari Nuke dan Houdini, dan mengabaikannya berarti separuh EXR produksi
/// dimuat sebagai gambar kosong.
void InterleaveTiles(const EXRHeader& header, const EXRImage& image,
                     const std::vector<int>& order, uint32_t width, uint32_t height, float* out) {
    const std::size_t channels = order.size();
    const auto tileWidth = static_cast<std::size_t>(header.tile_size_x);
    for (int t = 0; t < image.num_tiles; ++t) {
        const EXRTile& tile = image.tiles[t];
        for (std::size_t c = 0; c < channels; ++c) {
            const auto* source = reinterpret_cast<const float*>(tile.images[order[c]]);
            for (int y = 0; y < tile.height; ++y) {
                const auto targetY = static_cast<std::size_t>(tile.offset_y * header.tile_size_y + y);
                if (targetY >= height) {
                    continue;
                }
                for (int x = 0; x < tile.width; ++x) {
                    const auto targetX =
                        static_cast<std::size_t>(tile.offset_x * header.tile_size_x + x);
                    if (targetX >= width) {
                        continue;
                    }
                    // Ubin menyimpan barisnya selebar ubin penuh, bukan selebar
                    // bagian yang terpakai — ubin di tepi kanan gambar punya
                    // `width` lebih kecil dari `tile_size_x`, dan memakai yang
                    // salah menggeser seluruh sisa gambarnya.
                    out[(targetY * width + targetX) * channels + c] =
                        source[static_cast<std::size_t>(y) * tileWidth + static_cast<std::size_t>(x)];
                }
            }
        }
    }
}

class ExrBackendImpl final : public IBackend {
public:
    const char* Name() const override { return "tinyexr"; }

    const char* Version() const override { return SIM_TINYEXR_VERSION_STRING; }

    const std::vector<std::string>& ReadableExtensions() const override { return ExrExtensions(); }

    ImageIoResult Read(const std::filesystem::path& path, const ReadOptions& options,
                       Image& out) const override {
        ImageIoResult result;
        const std::string name = path.string();

        EXRVersion version{};
        if (ParseEXRVersionFromFile(&version, name.c_str()) != TINYEXR_SUCCESS) {
            result.error = "cannot read " + name + ": not an EXR file";
            return result;
        }
        if (version.multipart != 0) {
            // Multipart adalah beberapa gambar dalam satu berkas, dan tidak ada
            // jalur di mesin ini yang bisa menyatakan bagian mana yang
            // dimaksud. Ditolak dengan menyebut sebabnya, bukan dimuat bagian
            // pertamanya diam-diam.
            result.error = "cannot read " + name + ": multipart EXR is not supported";
            return result;
        }

        ExrScope scope;
        const char* message = nullptr;
        if (ParseEXRHeaderFromFile(&scope.header, &version, name.c_str(), &message) !=
            TINYEXR_SUCCESS) {
            result.error = "cannot read " + name + ": " + TakeError(message);
            return result;
        }
        if (scope.header.num_channels <= 0) {
            result.error = "image is empty: " + name;
            return result;
        }

        // **half diminta sebagai float sejak dekode.** Tipe keempat berarti
        // setiap pemanggil harus menanganinya, dan tidak satu pun jalur di
        // mesin ini bisa memakai half secara langsung.
        for (int i = 0; i < scope.header.num_channels; ++i) {
            if (scope.header.pixel_types[i] == TINYEXR_PIXELTYPE_HALF) {
                scope.header.requested_pixel_types[i] = TINYEXR_PIXELTYPE_FLOAT;
            }
        }

        if (LoadEXRImageFromFile(&scope.image, &scope.header, name.c_str(), &message) !=
            TINYEXR_SUCCESS) {
            result.error = "cannot read " + name + ": " + TakeError(message);
            return result;
        }
        scope.imageLoaded = true;

        if (scope.image.width <= 0 || scope.image.height <= 0) {
            result.error = "image is empty: " + name;
            return result;
        }
        for (int i = 0; i < scope.header.num_channels; ++i) {
            // UINT adalah kanal id — objectId, cryptomatte. Menafsirkannya
            // sebagai float menghasilkan angka yang tidak berarti apa-apa.
            if (scope.header.pixel_types[i] == TINYEXR_PIXELTYPE_UINT) {
                result.error = "cannot read " + name + ": channel '" +
                               scope.header.channels[i].name + "' is an integer id channel";
                return result;
            }
        }

        const std::vector<int> order = ChannelOrder(scope.header);

        ImageDesc desc;
        desc.width = static_cast<uint32_t>(scope.image.width);
        desc.height = static_cast<uint32_t>(scope.image.height);
        desc.channels = static_cast<uint32_t>(order.size());
        desc.type = PixelType::Float32;
        // EXR selalu scene-linear menurut spesifikasinya. Ini bukan pembacaan
        // metadata melainkan sifat formatnya, dan karena itu bisa dinyatakan
        // tanpa menebak.
        desc.colorSpace = ColorSpace::Linear;
        desc.channelNames.reserve(order.size());
        for (const int channel : order) {
            desc.channelNames.emplace_back(scope.header.channels[channel].name);
        }
        // **Alfa EXR premultiplied menurut spesifikasinya**, kecuali berkasnya
        // menyatakan sebaliknya — dan tinyexr tidak menyampaikan pernyataan itu.
        // Menganggapnya straight membuat setiap tepi tekstur beralfa salah
        // terang, dan membuat backend ini tidak sepakat dengan OIIO pada berkas
        // yang sama.
        desc.premultipliedAlpha =
            std::find(desc.channelNames.begin(), desc.channelNames.end(), "A") !=
            desc.channelNames.end();
        out.Allocate(desc);

        if (scope.image.images != nullptr) {
            InterleaveScanlines(scope.image, order, out.AsF32());
        } else if (scope.image.tiles != nullptr) {
            InterleaveTiles(scope.header, scope.image, order, desc.width, desc.height,
                            out.AsF32());
        } else {
            result.error = "cannot read " + name + ": no scanline or tile data";
            return result;
        }

        ApplyReadOptions(options, name, out);
        result.ok = true;
        return result;
    }

    ImageIoResult Read(std::span<const uint8_t> bytes, std::string_view /*extensionHint*/,
                       const ReadOptions& /*options*/, Image& /*out*/) const override {
        ImageIoResult result;
        // Sengaja tidak didukung, bukan terlewat. Jalur baca-dari-memori ada
        // untuk thumbnail, dan thumbnail EXR belum punya pemakai — sementara
        // menuliskannya berarti satu jalur lagi yang harus diuji dan tidak ada
        // yang menjalankannya. Ditolak dengan menyebut alasannya.
        (void)bytes;
        result.error = "EXR can only be read from a file, not from memory";
        return result;
    }

    /// **Hanya `.exr`, dan hanya float.** Satu-satunya pemakainya keluaran
    /// renderer: gambar acuan path tracer dan readback HDR. Keduanya radiansi
    /// linier, dan justru itu alasannya harus EXR — PNG menjepitnya di 1,0 dan
    /// memetakan nadanya, sehingga sebuah uji tungku yang jawabannya "tepat
    /// 1,0" tidak bisa dibaca kembali dari sana sama sekali.
    const std::vector<std::string>& WritableExtensions() const override {
        return ExrExtensions();
    }

    /// Menulis EXR scanline, half, kompresi ZIP.
    ///
    /// **Half, bukan float.** Radiansi keluaran renderer punya rentang dinamis
    /// besar tetapi presisi yang tidak menuntut 24 bit mantissa; half memotong
    /// berkasnya separuh dan itu pilihan yang sama dengan yang diambil hampir
    /// seluruh pipeline produksi. Yang membacanya kembali mendapat float —
    /// `Read` di atas sudah mengonversinya.
    ///
    /// **tinyexr, bukan OpenEXR.** Yang ditulis di sini scanline RGB/RGBA satu
    /// bagian, tanpa tile, tanpa deep, tanpa multipart — bagian EXR yang paling
    /// sederhana, dan tinyexr sudah ada di build ini. Untuk berkas produksi
    /// yang menuntut seluruh spesifikasinya, backend OpenImageIO sudah
    /// didahulukan bila terpasang, dan ia memakai OpenEXR sungguhan.
    ImageIoResult Write(const std::filesystem::path& path, const Image& image) const override {
        ImageIoResult result;
        if (image.desc.type != PixelType::Float32) {
            result.error = "EXR is written from float pixels only, got " +
                           std::string(ToString(image.desc.type));
            return result;
        }
        if (image.desc.channels != 3 && image.desc.channels != 4) {
            result.error = "EXR is written with 3 or 4 channels, got " +
                           std::to_string(image.desc.channels);
            return result;
        }
        if (image.desc.width == 0 || image.desc.height == 0) {
            result.error = "cannot write an empty image to " + path.string();
            return result;
        }
        if (image.bytes.size() < image.desc.ByteCount()) {
            result.error = "pixel buffer is shorter than its description claims";
            return result;
        }

        // **tinyexr menuntut kanal terpisah, bukan interleaved**, dan urutannya
        // terbalik: A, B, G, R. Urutan itu bukan selera — pembaca EXR menyusun
        // kanal menurut abjad, dan yang menulisnya dengan urutan lain
        // menghasilkan berkas yang terbuka dengan warna tertukar di alat lain.
        const std::size_t count =
            static_cast<std::size_t>(image.desc.width) * image.desc.height;
        const uint32_t channels = image.desc.channels;
        const float* source = reinterpret_cast<const float*>(image.bytes.data());

        std::vector<std::vector<float>> planes(channels, std::vector<float>(count));
        for (std::size_t i = 0; i < count; ++i) {
            for (uint32_t c = 0; c < channels; ++c) {
                planes[c][i] = source[i * channels + c];
            }
        }

        std::vector<float*> pointers(channels);
        std::vector<EXRChannelInfo> infos(channels);
        std::vector<int> pixelTypes(channels, TINYEXR_PIXELTYPE_FLOAT);
        std::vector<int> requestedTypes(channels, TINYEXR_PIXELTYPE_HALF);

        const char* const names[4] = {"R", "G", "B", "A"};
        for (uint32_t c = 0; c < channels; ++c) {
            const uint32_t reversed = channels - 1 - c;
            pointers[c] = planes[reversed].data();
            const char* name = names[reversed];
            std::snprintf(infos[c].name, sizeof(infos[c].name), "%s", name);
        }

        EXRHeader header;
        InitEXRHeader(&header);
        header.num_channels = static_cast<int>(channels);
        header.channels = infos.data();
        header.pixel_types = pixelTypes.data();
        header.requested_pixel_types = requestedTypes.data();
        header.compression_type = TINYEXR_COMPRESSIONTYPE_ZIP;

        EXRImage exr;
        InitEXRImage(&exr);
        exr.num_channels = static_cast<int>(channels);
        exr.images = reinterpret_cast<unsigned char**>(pointers.data());
        exr.width = static_cast<int>(image.desc.width);
        exr.height = static_cast<int>(image.desc.height);

        std::error_code code;
        std::filesystem::create_directories(path.parent_path(), code);

        const std::string name = path.string();
        const char* message = nullptr;
        if (SaveEXRImageToFile(&exr, &header, name.c_str(), &message) != TINYEXR_SUCCESS) {
            result.error = "cannot write " + name + ": " + TakeError(message);
            return result;
        }
        result.ok = true;
        return result;
    }

    ImageIoResult Probe(const std::filesystem::path& path, ImageDesc& out) const override {
        ImageIoResult result;
        const std::string name = path.string();

        EXRVersion version{};
        if (ParseEXRVersionFromFile(&version, name.c_str()) != TINYEXR_SUCCESS) {
            result.error = "cannot read " + name + ": not an EXR file";
            return result;
        }

        ExrScope scope;
        const char* message = nullptr;
        if (ParseEXRHeaderFromFile(&scope.header, &version, name.c_str(), &message) !=
            TINYEXR_SUCCESS) {
            result.error = "cannot read " + name + ": " + TakeError(message);
            return result;
        }

        // Kepala berkasnya saja: `data_window` sudah menyatakan ukurannya, dan
        // tidak ada satu piksel pun yang didekode di sini.
        const std::vector<int> order = ChannelOrder(scope.header);
        out = ImageDesc{};
        out.width = static_cast<uint32_t>(scope.header.data_window.max_x -
                                          scope.header.data_window.min_x + 1);
        out.height = static_cast<uint32_t>(scope.header.data_window.max_y -
                                           scope.header.data_window.min_y + 1);
        out.channels = static_cast<uint32_t>(order.size());
        out.type = PixelType::Float32;
        out.colorSpace = ColorSpace::Linear;
        for (const int channel : order) {
            out.channelNames.emplace_back(scope.header.channels[channel].name);
        }
        out.premultipliedAlpha =
            std::find(out.channelNames.begin(), out.channelNames.end(), "A") !=
            out.channelNames.end();
        result.ok = true;
        return result;
    }
};

}  // namespace

const IBackend* ExrBackend() {
    static const ExrBackendImpl backend;
    return &backend;
}

}  // namespace sim::imageio

#else  // SIM_WITH_TINYEXR

namespace sim::imageio {

const IBackend* ExrBackend() { return nullptr; }

}  // namespace sim::imageio

#endif  // SIM_WITH_TINYEXR
