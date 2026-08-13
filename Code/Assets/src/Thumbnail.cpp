#include "Sim/Assets/Thumbnail.h"

#include "Sim/Core/Log.h"
#include "Sim/ImageIO/ImageIO.h"

#include <stb_image_resize2.h>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace sim::assets {
namespace {

/// FNV-1a 64-bit. Bukan hash kriptografis, dan memang tidak perlu: yang
/// dibutuhkan hanya "apakah isinya berubah", bukan ketahanan terhadap tabrakan
/// yang dibuat sengaja.
uint64_t HashBytes(const std::vector<uint8_t>& bytes) {
    uint64_t hash = 1469598103934665603ull;
    for (const uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return {};
    }
    const std::streamsize size = stream.tellg();
    if (size <= 0) {
        return {};
    }
    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

/// Berkas cache: header kecil berisi dimensi, lalu piksel RGBA mentah.
///
/// Sengaja tidak dikompresi jadi PNG. Isinya kecil (paling besar 160x160 = 100
/// KB) dan yang ingin dihindari justru biaya dekode — menyimpannya terkompresi
/// berarti membayar dekode lagi setiap kali, yang persis pekerjaan yang
/// dihindari cache ini.
constexpr uint32_t kCacheMagic = 0x314D4854;  // "THM1"

bool ReadCache(const std::filesystem::path& path, ThumbnailImage& out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    uint32_t magic = 0;
    stream.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    stream.read(reinterpret_cast<char*>(&out.width), sizeof(out.width));
    stream.read(reinterpret_cast<char*>(&out.height), sizeof(out.height));
    if (!stream || magic != kCacheMagic || out.width == 0 || out.height == 0 ||
        out.width > 4096 || out.height > 4096) {
        return false;
    }

    const std::size_t bytes = static_cast<std::size_t>(out.width) * out.height * 4;
    out.rgba.resize(bytes);
    stream.read(reinterpret_cast<char*>(out.rgba.data()), static_cast<std::streamsize>(bytes));
    // Berkas cache yang terpotong — misalnya karena editor mati saat menulis —
    // diperlakukan seperti tidak ada, bukan dipakai separuh.
    return static_cast<std::size_t>(stream.gcount()) == bytes;
}

void WriteCache(const std::filesystem::path& path, const ThumbnailImage& image) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        return;
    }
    stream.write(reinterpret_cast<const char*>(&kCacheMagic), sizeof(kCacheMagic));
    stream.write(reinterpret_cast<const char*>(&image.width), sizeof(image.width));
    stream.write(reinterpret_cast<const char*>(&image.height), sizeof(image.height));
    stream.write(reinterpret_cast<const char*>(image.rgba.data()),
                 static_cast<std::streamsize>(image.rgba.size()));
}

}  // namespace

ThumbnailImage MakeThumbnail(const std::filesystem::path& path,
                             const std::filesystem::path& cacheDir, uint32_t maxSize) {
    ThumbnailImage result;
    const std::vector<uint8_t> bytes = ReadFile(path);
    if (bytes.empty()) {
        return result;
    }

    char name[32];
    std::snprintf(name, sizeof(name), "%016llx_%u.thumb",
                  static_cast<unsigned long long>(HashBytes(bytes)), maxSize);
    const std::filesystem::path cachePath = cacheDir / name;

    if (ReadCache(cachePath, result)) {
        return result;
    }

    // Dipaksa 4 kanal 8-bit: gambar 1 dan 3 kanal ikut dinaikkan jadi RGBA, dan
    // yang 16-bit maupun float diturunkan — sehingga jalur unggah GPU hanya
    // perlu mengenal satu format.
    imageio::ReadOptions options;
    options.channels = 4;
    options.type = imageio::PixelType::UInt8;

    imageio::Image image;
    // Ekstensinya ikut dikirim sebagai petunjuk: isinya sudah di memori, tapi
    // backend yang memilih pembaca berdasarkan nama tetap membutuhkannya.
    const imageio::ImageIoResult decoded =
        imageio::Read(bytes, path.extension().string(), options, image);
    if (!decoded) {
        SIM_WARN("Assets", "thumbnail: {}", decoded.error);
        return result;
    }

    const auto width = static_cast<int>(image.desc.width);
    const auto height = static_cast<int>(image.desc.height);
    const uint8_t* pixels = image.AsU8();
    if (pixels == nullptr || width <= 0 || height <= 0) {
        return result;
    }

    // Proporsi dijaga: thumbnail yang dipaksa persegi membuat tekstur panjang
    // terlihat gepeng dan sulit dikenali, padahal justru itu gunanya.
    const float scale = static_cast<float>(maxSize) /
                        static_cast<float>(std::max(width, height));
    const auto targetWidth = static_cast<uint32_t>(
        std::max(1.0f, std::round(static_cast<float>(width) * std::min(scale, 1.0f))));
    const auto targetHeight = static_cast<uint32_t>(
        std::max(1.0f, std::round(static_cast<float>(height) * std::min(scale, 1.0f))));

    result.width = targetWidth;
    result.height = targetHeight;
    result.rgba.resize(static_cast<std::size_t>(targetWidth) * targetHeight * 4);

    if (targetWidth == static_cast<uint32_t>(width) &&
        targetHeight == static_cast<uint32_t>(height)) {
        std::memcpy(result.rgba.data(), pixels, result.rgba.size());
    } else {
        stbir_resize_uint8_linear(pixels, width, height, 0, result.rgba.data(),
                                  static_cast<int>(targetWidth),
                                  static_cast<int>(targetHeight), 0, STBIR_RGBA);
    }

    WriteCache(cachePath, result);
    return result;
}

}  // namespace sim::assets
