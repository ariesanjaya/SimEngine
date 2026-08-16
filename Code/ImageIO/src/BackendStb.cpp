// Backend stb — satu-satunya tempat di seluruh `Code/` yang boleh memanggil
// `stbi_`.
//
// Batas itu ditegakkan oleh uji, bukan oleh disiplin: `SimImageIOTests`
// menyisir `Code/` dan gagal kalau menemukan `stbi_` di berkas lain. Alasannya
// bukan kerapian melainkan bahwa setiap titik dekode langsung adalah satu
// tempat lagi yang harus disentuh ketika backend kedua masuk.
//
// **Keadaan global stb tidak pernah disentuh.** `stbi_ldr_to_hdr_gamma`,
// `stbi_hdr_to_ldr_gamma`, dan `stbi_set_flip_vertically_on_load` semuanya
// mengubah variabel proses; thumbnail didekode dari TaskPool, jadi menyetelnya
// per panggilan adalah balapan data yang muncul sebagai gambar salah gamma
// sesekali. Konsekuensinya: konversi antar-tipe piksel dikerjakan sendiri di
// bawah, bukan diserahkan ke jalur konversi stb.

#include "Backend.h"

#include "PixelOps.h"
#include "PngWrite.h"

#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>

namespace sim::imageio {
namespace {

/// Kenapa tidak seluruh yang bisa dibaca stb.
///
/// stb juga membaca GIF, PIC, dan PNM. Ketiganya sengaja tidak ada di sini:
/// daftar ini adalah janji dukungan, dan GIF animasi yang diimpor lalu menjadi
/// frame pertamanya tanpa peringatan adalah persis bentuk janji yang tidak
/// ditepati. Alasan lengkap per format ada di docs/PLAN-IMAGEIO.md.
///
/// `.psd` tetap ada meski dukungan stb atasnya terbatas pada composited view
/// 8/16-bit. Yang membuatnya bisa dipertanggungjawabkan adalah bahwa berkas di
/// luar batas itu **gagal dengan pesan** — bukan terbaca separuh. Batas itu
/// disebut apa adanya di docs/DEPENDENCIES.md, bersama harga yang harus dibayar
/// untuk menghilangkannya.
const std::vector<std::string>& StbExtensions() {
    static const std::vector<std::string> kExtensions{
        ".bmp", ".hdr", ".jpeg", ".jpg", ".png", ".psd", ".tga",
    };
    return kExtensions;
}

std::string FailureReason() {
    const char* reason = stbi_failure_reason();
    return reason != nullptr ? reason : "unrecognised image";
}

/// Tipe piksel yang paling mendekati apa yang ada di berkas.
///
/// Diperiksa dalam urutan ini karena `stbi_is_hdr` hanya membaca beberapa byte
/// pertama (tanda tangan Radiance), sementara `stbi_is_16_bit` harus mengurai
/// kepala PNG. Berkas yang bukan HDR membayar pemeriksaan kedua; yang HDR tidak.
PixelType NativeTypeOfFile(const char* path) {
    if (stbi_is_hdr(path) != 0) {
        return PixelType::Float32;
    }
    return stbi_is_16_bit(path) != 0 ? PixelType::UInt16 : PixelType::UInt8;
}

PixelType NativeTypeOfMemory(const stbi_uc* bytes, int length) {
    if (stbi_is_hdr_from_memory(bytes, length) != 0) {
        return PixelType::Float32;
    }
    return stbi_is_16_bit_from_memory(bytes, length) != 0 ? PixelType::UInt16 : PixelType::UInt8;
}

/// Radiance RGBE menyimpan radiance linear; sisanya tidak menyatakan apa-apa
/// yang bisa dibaca stb, dan menebaknya di sini adalah tebakan yang tidak bisa
/// dikoreksi lagi di atasnya.
ColorSpace ColorSpaceOf(PixelType native) {
    return native == PixelType::Float32 ? ColorSpace::Linear : ColorSpace::Unknown;
}

/// Menyusun hasil dari pointer yang dikembalikan stb, lalu membebaskannya.
///
/// `channels` adalah jumlah kanal **di buffer**, yang sama dengan yang diminta
/// bila pemanggil memaksanya, dan sama dengan yang ada di berkas bila tidak.
/// Keduanya sering berbeda, dan memakai yang salah menghasilkan pembacaan
/// melewati ujung buffer — bukan gambar yang salah, melainkan crash.
void Adopt(void* pixels, int width, int height, uint32_t channels, PixelType type,
           ColorSpace space, Image& out) {
    ImageDesc desc;
    desc.width = static_cast<uint32_t>(width);
    desc.height = static_cast<uint32_t>(height);
    desc.channels = channels;
    desc.type = type;
    desc.colorSpace = space;
    out.desc = desc;
    out.bytes.resize(desc.ByteCount());
    std::memcpy(out.bytes.data(), pixels, out.bytes.size());
    stbi_image_free(pixels);
}

class StbBackendImpl final : public IBackend {
public:
    const char* Name() const override { return "stb"; }

    const std::vector<std::string>& ReadableExtensions() const override { return StbExtensions(); }

    /// **Hanya PNG.** stb bisa menulis TGA, BMP, JPEG, dan HDR juga, dan tidak
    /// satu pun dari keempatnya ditulis oleh jalur kerja di sini. Menyalakannya
    /// berarti menjanjikan berkas yang bisa dibuka alat lain tanpa ada satu pun
    /// uji yang pernah membukanya.
    const std::vector<std::string>& WritableExtensions() const override {
        static const std::vector<std::string> kExtensions{".png"};
        return kExtensions;
    }

    ImageIoResult Write(const std::filesystem::path& path, const Image& image) const override {
        ImageIoResult result;
        std::vector<uint8_t> bytes;
        if (const ImageIoResult encoded = Encode(image, bytes); !encoded) {
            result.error = "cannot write " + path.string() + ": " + encoded.error;
            return result;
        }

        std::error_code code;
        std::filesystem::create_directories(path.parent_path(), code);
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream) {
            result.error = "cannot open " + path.string();
            return result;
        }
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        if (!stream) {
            result.error = "write failed: " + path.string();
            return result;
        }
        result.ok = true;
        return result;
    }

    /// Greyscale, RGB, dan RGBA — 8 atau 16 bit per sampel, bukan float.
    ///
    /// Satu kanal untuk heightmap, weightmap, peta hole, dan peta kepadatan;
    /// berwarna untuk `editor.screenshot`, yang mengirim jendela editor ke agen
    /// MCP. Float tetap ditolak: PNG tidak punya tempat untuknya, dan yang
    /// membutuhkannya sudah punya EXR.
    ImageIoResult Encode(const Image& image, std::vector<uint8_t>& out) const override {
        ImageIoResult result;
        const uint32_t channels = image.desc.channels;
        const bool channelsOk = channels == 1 || channels == 3 || channels == 4;
        if (!channelsOk || image.desc.type == PixelType::Float32) {
            result.error =
                "PNG is written as 8-bit or 16-bit grey/RGB/RGBA; this image is " +
                std::to_string(channels) + "-channel " + ToString(image.desc.type);
            return result;
        }
        if (!EncodePng(image, out)) {
            result.error = "PNG encode failed";
            return result;
        }
        result.ok = true;
        return result;
    }

    ImageIoResult Read(const std::filesystem::path& path, const ReadOptions& options,
                       Image& out) const override {
        ImageIoResult result;
        const std::string name = path.string();
        const PixelType native = NativeTypeOfFile(name.c_str());
        const int desired = static_cast<int>(options.channels);

        int width = 0;
        int height = 0;
        int inFile = 0;
        void* pixels = nullptr;
        switch (native) {
            case PixelType::UInt8:
                pixels = stbi_load(name.c_str(), &width, &height, &inFile, desired);
                break;
            case PixelType::UInt16:
                pixels = stbi_load_16(name.c_str(), &width, &height, &inFile, desired);
                break;
            case PixelType::Float32:
                pixels = stbi_loadf(name.c_str(), &width, &height, &inFile, desired);
                break;
        }
        if (pixels == nullptr) {
            result.error = "cannot read " + name + ": " + FailureReason();
            return result;
        }
        return Finish(pixels, width, height, inFile, native, options, name, out);
    }

    ImageIoResult Read(std::span<const uint8_t> bytes, std::string_view /*extensionHint*/,
                       const ReadOptions& options, Image& out) const override {
        ImageIoResult result;
        const auto length = static_cast<int>(bytes.size());
        const PixelType native = NativeTypeOfMemory(bytes.data(), length);
        const int desired = static_cast<int>(options.channels);

        int width = 0;
        int height = 0;
        int inFile = 0;
        void* pixels = nullptr;
        switch (native) {
            case PixelType::UInt8:
                pixels = stbi_load_from_memory(bytes.data(), length, &width, &height, &inFile,
                                               desired);
                break;
            case PixelType::UInt16:
                pixels = stbi_load_16_from_memory(bytes.data(), length, &width, &height, &inFile,
                                                  desired);
                break;
            case PixelType::Float32:
                pixels = stbi_loadf_from_memory(bytes.data(), length, &width, &height, &inFile,
                                                desired);
                break;
        }
        if (pixels == nullptr) {
            result.error = std::string("cannot decode image from memory: ") + FailureReason();
            return result;
        }
        return Finish(pixels, width, height, inFile, native, options, "<memory>", out);
    }

    ImageIoResult Probe(const std::filesystem::path& path, ImageDesc& out) const override {
        ImageIoResult result;
        const std::string name = path.string();
        int width = 0;
        int height = 0;
        int channels = 0;
        if (stbi_info(name.c_str(), &width, &height, &channels) == 0) {
            result.error = "cannot read " + name + ": " + FailureReason();
            return result;
        }
        const PixelType native = NativeTypeOfFile(name.c_str());
        out = ImageDesc{};
        out.width = static_cast<uint32_t>(width);
        out.height = static_cast<uint32_t>(height);
        out.channels = static_cast<uint32_t>(channels);
        out.type = native;
        out.colorSpace = ColorSpaceOf(native);
        result.ok = true;
        return result;
    }

private:
    /// Bagian yang sama untuk kedua jalur baca: memeriksa dimensi, mengambil
    /// alih buffer stb, lalu mengubah tipenya bila yang diminta bukan yang ada
    /// di berkas.
    static ImageIoResult Finish(void* pixels, int width, int height, int channelsInFile,
                                PixelType native, const ReadOptions& options,
                                const std::string& what, Image& out) {
        ImageIoResult result;
        if (width <= 0 || height <= 0) {
            stbi_image_free(pixels);
            result.error = "image is empty: " + what;
            return result;
        }
        const uint32_t channels = options.channels != 0 ? options.channels
                                                        : static_cast<uint32_t>(channelsInFile);
        Adopt(pixels, width, height, channels, native, ColorSpaceOf(native), out);

        // Jumlah kanalnya sudah dipaksa stb saat dekode, jadi bagian itu menjadi
        // no-op; yang tersisa dikerjakan konversi bersama, supaya aturannya
        // sama persis dengan yang dipakai backend EXR dan TIFF.
        ApplyReadOptions(options, what, out);
        result.ok = true;
        return result;
    }
};

}  // namespace

const IBackend& StbBackend() {
    static const StbBackendImpl backend;
    return backend;
}

}  // namespace sim::imageio
