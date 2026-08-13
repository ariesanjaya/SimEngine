// Backend OpenImageIO — opsional, dan **didahulukan bila ada**.
//
// **Kenapa didahulukan padahal backend lain sudah menangani formatnya.** Bukan
// karena lebih cepat — untuk PNG 8-bit semuanya setara — melainkan karena ia
// membawa yang lain tidak punya: metadata colorspace untuk PNG dan JPEG, PSD
// yang utuh alih-alih composited view saja, dan DDS untuk aset lama. Metadata
// yang dibuang di lapisan I/O tidak bisa ditemukan kembali di lapisan material.
//
// **Tidak wajib, dan tidak boleh menjadi wajib.** stb, tinyexr, dan libtiff
// menangani seluruh format yang dipakai jalur kerja di sini, jadi build bersih
// tidak membutuhkan berkas ini sama sekali. Yang hilang tanpanya cuma DDS dan
// ketelitian di atas — bukan kemampuan memuat aset. Alasan panjangnya, termasuk
// biaya yang membuatnya tidak dijadikan syarat, ada di docs/DEPENDENCIES.md.
//
// Berkasnya tetap ikut dibangun tanpa OIIO, mengikuti pola `UsdImport.cpp`.
//
// **TU ini tidak boleh melihat spdlog.** Header OIIO menarik fmt 10; spdlog
// membawa fmt 12 di dalam dirinya, dan keduanya mendefinisikan makro yang sama
// (`FMT_VERSION`, `FMT_BEGIN_NAMESPACE`, dan seterusnya). Satu TU yang melihat
// keduanya gagal dikompilasi jauh di dalam salah satu header fmt, di baris yang
// tidak menyebut sebabnya. Karena itu peringatan dicatat lewat `LogWarning` di
// `Backend.h`, yang implementasinya tinggal di TU lain.
//
// Dua TU yang masing-masing melihat satu versi sama sekali tidak bermasalah:
// namespace inline fmt menyandang versinya, jadi simbol keduanya memang berbeda.

#include "Backend.h"

#if SIM_WITH_OIIO

#include "PixelOps.h"

#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/imageio.h>

#include <algorithm>
#include <memory>

namespace sim::imageio {
namespace {

/// Sembilan format, bukan tiga puluh dua.
///
/// Build ini sendiri membawa lebih banyak — cineon, dpx, fits, ico, iff, pnm,
/// rla, sgi, softimage pic, zfile — dan tidak satu pun diekspos. Daftarnya
/// dikurasi, bukan diwarisi: setiap ekstensi di sini adalah janji dukungan, dan
/// bahaya konkretnya bukan teori. Asset Browser menawarkan impor, artis
/// mengimpor berkas yang formatnya "didukung", dan yang didapat adalah sesuatu
/// yang tidak dimaksudkannya.
///
/// `.dds` **hanya ada di daftar ini**, dan hanya sebagai berkas sumber: OIIO
/// mendekompres bloknya menjadi piksel mentah, jadi ia berperan sebagai aset
/// lama yang diimpor, bukan sebagai bentuk runtime. Bentuk runtime tetap KTX2.
const std::vector<std::string>& OiioExtensions() {
    static const std::vector<std::string> kExtensions{
        ".bmp", ".dds", ".exr", ".hdr", ".jpeg", ".jpg", ".png", ".psd", ".tga", ".tif", ".tiff",
    };
    return kExtensions;
}

/// Tipe piksel SimEngine yang paling mendekati apa yang ada di berkas.
///
/// EXR half **dikonversi ke float saat baca**, tidak dibawa apa adanya. Tipe
/// keempat berarti setiap pemanggil harus menanganinya, dan tidak satu pun
/// jalur di mesin ini yang bisa memakai half secara langsung.
PixelType NativeTypeOf(const OIIO::ImageSpec& spec) {
    switch (spec.format.basetype) {
        case OIIO::TypeDesc::UINT8:
        case OIIO::TypeDesc::INT8:
            return PixelType::UInt8;
        case OIIO::TypeDesc::UINT16:
        case OIIO::TypeDesc::INT16:
            return PixelType::UInt16;
        default:
            // half, float, double, dan bilangan bulat 32-bit semuanya ke float.
            return PixelType::Float32;
    }
}

OIIO::TypeDesc ToOiioType(PixelType type) {
    switch (type) {
        case PixelType::UInt8: return OIIO::TypeDesc::UINT8;
        case PixelType::UInt16: return OIIO::TypeDesc::UINT16;
        case PixelType::Float32: return OIIO::TypeDesc::FLOAT;
    }
    return OIIO::TypeDesc::UINT8;
}

/// Ruang warna dari metadata berkas, bukan dari tebakan atas isinya.
///
/// **Inilah yang benar-benar bertambah dibanding backend lain.** stb tidak
/// membaca chunk `sRGB` maupun `gAMA` sama sekali, jadi setiap PNG keluar
/// sebagai `Unknown`; OIIO menyatakannya.
ColorSpace ColorSpaceOf(const OIIO::ImageSpec& spec) {
    const OIIO::string_view name = spec.get_string_attribute("oiio:ColorSpace");
    if (name == "sRGB") {
        return ColorSpace::Srgb;
    }
    if (name == "linear" || name == "Linear" || name == "lin_srgb" ||
        name == "scene_linear" || name == "lin_rec709") {
        return ColorSpace::Linear;
    }
    return ColorSpace::Unknown;
}

/// Alfa premultiplied tercatat sebagai `oiio:UnassociatedAlpha`.
///
/// Konvensinya terbalik dari namanya: OIIO membaca alfa sebagai *associated*
/// (premultiplied) kecuali berkasnya menyatakan sebaliknya.
bool PremultipliedAlphaOf(const OIIO::ImageSpec& spec) {
    return spec.get_int_attribute("oiio:UnassociatedAlpha", 0) == 0;
}

/// Nama kanal apa adanya, sebanyak kanal yang benar-benar dibawa pulang.
std::vector<std::string> ChannelNamesOf(const OIIO::ImageSpec& spec, uint32_t channels) {
    std::vector<std::string> names;
    const auto count = std::min<std::size_t>(channels, spec.channelnames.size());
    names.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        names.emplace_back(spec.channelnames[i]);
    }
    return names;
}

/// Membaca dari `ImageInput` yang sudah terbuka.
///
/// **Satu `ImageInput` per panggilan, tidak pernah dipakai bersama.** Impor
/// massal berjalan di TaskPool, dan `ImageCache` memang aman untuk dipakai
/// beberapa thread — tapi `ImageInput` tidak, dan mengasumsikannya aman
/// menghasilkan crash acak yang hanya muncul saat folder besar diimpor.
ImageIoResult ReadOpened(std::unique_ptr<OIIO::ImageInput> input, const ReadOptions& options,
                         const std::string& what, Image& out) {
    ImageIoResult result;
    const OIIO::ImageSpec& spec = input->spec();
    if (spec.width <= 0 || spec.height <= 0 || spec.nchannels <= 0) {
        result.error = "image is empty: " + what;
        return result;
    }

    const PixelType native = NativeTypeOf(spec);
    const auto channelsInFile = static_cast<uint32_t>(spec.nchannels);

    // Dibaca apa adanya dulu, lalu dibentuk oleh konversi bersama. Menyerahkan
    // pemaksaan kanal ke `read_image` akan membuat aturannya berbeda dari
    // backend lain — dan aturan yang berbeda menghasilkan piksel yang berbeda
    // untuk berkas yang sama, menurut backend mana yang kebetulan memegangnya.
    ImageDesc desc;
    desc.width = static_cast<uint32_t>(spec.width);
    desc.height = static_cast<uint32_t>(spec.height);
    desc.channels = channelsInFile;
    desc.type = native;
    desc.colorSpace = ColorSpaceOf(spec);
    desc.premultipliedAlpha = PremultipliedAlphaOf(spec);
    desc.channelNames = ChannelNamesOf(spec, channelsInFile);
    out.Allocate(desc);

    if (!input->read_image(0, 0, 0, static_cast<int>(channelsInFile), ToOiioType(native),
                           out.bytes.data())) {
        result.error = "cannot read " + what + ": " + input->geterror();
        return result;
    }
    input->close();

    ApplyReadOptions(options, what, out);
    result.ok = true;
    return result;
}

class OiioBackendImpl final : public IBackend {
public:
    const char* Name() const override { return "oiio"; }

    const char* Version() const override { return SIM_OIIO_VERSION_STRING; }

    const std::vector<std::string>& ReadableExtensions() const override {
        return OiioExtensions();
    }

    ImageIoResult Read(const std::filesystem::path& path, const ReadOptions& options,
                       Image& out) const override {
        ImageIoResult result;
        const std::string name = path.string();
        std::unique_ptr<OIIO::ImageInput> input = OIIO::ImageInput::open(name);
        if (input == nullptr) {
            result.error = "cannot read " + name + ": " + OIIO::geterror();
            return result;
        }
        return ReadOpened(std::move(input), options, name, out);
    }

    ImageIoResult Read(std::span<const uint8_t> bytes, std::string_view extensionHint,
                       const ReadOptions& options, Image& out) const override {
        ImageIoResult result;
        // Berkas di memori dibaca lewat IOProxy, bukan lewat berkas sementara di
        // disk. Jalur thumbnail memanggil ini untuk setiap berkas di folder;
        // menulis salinan ke disk untuk masing-masing akan mengubah pemindaian
        // folder menjadi pekerjaan tulis-baca sebanyak isi foldernya.
        //
        // Ini juga jalur yang dibutuhkan tekstur di dalam paket USDZ, yang
        // sampai sebagai byte dan bukan sebagai berkas.
        OIIO::Filesystem::IOMemReader reader(bytes.data(), bytes.size());
        // Namanya cuma petunjuk pemilih pembaca; isinya tetap dari proxy-nya.
        const std::string name = std::string("memory") + std::string(extensionHint);
        std::unique_ptr<OIIO::ImageInput> input = OIIO::ImageInput::open(name, nullptr, &reader);
        if (input == nullptr) {
            result.error = std::string("cannot decode image from memory: ") + OIIO::geterror();
            return result;
        }
        return ReadOpened(std::move(input), options, "<memory>", out);
    }

    ImageIoResult Probe(const std::filesystem::path& path, ImageDesc& out) const override {
        ImageIoResult result;
        const std::string name = path.string();
        std::unique_ptr<OIIO::ImageInput> input = OIIO::ImageInput::open(name);
        if (input == nullptr) {
            result.error = "cannot read " + name + ": " + OIIO::geterror();
            return result;
        }
        // Hanya kepalanya: `open` sudah mengisi spec, dan tidak ada satu piksel
        // pun yang didekode di sini.
        const OIIO::ImageSpec& spec = input->spec();
        out = ImageDesc{};
        out.width = static_cast<uint32_t>(spec.width);
        out.height = static_cast<uint32_t>(spec.height);
        out.channels = static_cast<uint32_t>(spec.nchannels);
        out.type = NativeTypeOf(spec);
        out.colorSpace = ColorSpaceOf(spec);
        out.premultipliedAlpha = PremultipliedAlphaOf(spec);
        out.channelNames = ChannelNamesOf(spec, out.channels);
        input->close();
        result.ok = true;
        return result;
    }
};

}  // namespace

const IBackend* OiioBackend() {
    static const OiioBackendImpl backend;
    return &backend;
}

}  // namespace sim::imageio

#else  // SIM_WITH_OIIO

namespace sim::imageio {

const IBackend* OiioBackend() { return nullptr; }

}  // namespace sim::imageio

#endif  // SIM_WITH_OIIO
