#include "Sim/Assets/TextureBake.h"

#include "Sim/Assets/BlockCompress.h"
#include "Sim/Assets/Ktx2Write.h"
#include "Sim/Core/Log.h"
#include "Sim/ImageIO/ImageIO.h"
#include "Sim/ImageIO/MipChain.h"
#include "Sim/ImageIO/TextureColor.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>

namespace sim::assets {
namespace {

/// Dinaikkan setiap kali hasil bake bisa berubah untuk sumber yang sama —
/// filter mip diganti, encoder diganti, tabel format diubah. Kunci cache ikut
/// berubah karenanya, jadi aset yang sudah pernah di-bake ikut dikerjakan ulang.
constexpr uint64_t kBakerVersion = 2;

/// Nomor `VkFormat` yang dipakai baker ini.
///
/// **Angka, bukan enum Vulkan.** `Sim::Assets` tidak menyertakan Vulkan dan
/// tidak seharusnya: baker berjalan tanpa device, tanpa surface, dan pada mesin
/// build yang bisa saja tidak punya GPU sama sekali. Nomornya sendiri ditetapkan
/// spesifikasi KTX2 sebagai nomor `VkFormat`, jadi ia memang bagian dari format
/// berkasnya, bukan detail API.
namespace format {
constexpr uint32_t kR8Unorm = 9;
constexpr uint32_t kR8G8B8A8Unorm = 37;
constexpr uint32_t kR8G8B8A8Srgb = 43;
constexpr uint32_t kR16Unorm = 70;
constexpr uint32_t kR32G32B32A32Sfloat = 109;
constexpr uint32_t kBc1RgbUnorm = 131;
constexpr uint32_t kBc1RgbSrgb = 132;
constexpr uint32_t kBc4Unorm = 139;
constexpr uint32_t kBc5Unorm = 141;
constexpr uint32_t kBc7Unorm = 145;
constexpr uint32_t kBc7Srgb = 146;
}  // namespace format

std::atomic<uint64_t> g_bakeCount{0};

uint64_t HashInto(uint64_t hash, const void* data, std::size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (std::size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

constexpr uint64_t kFnvOffset = 1469598103934665603ull;

/// Bentuk piksel yang diminta dari dekoder untuk sebuah pemakaian.
///
/// **Diminta saat dekode, bukan dikonversi sesudahnya.** `ReadOptions` sudah
/// menyediakannya, dan menaikkan gambar delapan bit menjadi float setelah
/// dekode berarti dua buffer ratusan megabyte hidup bersamaan.
imageio::ReadOptions ReadOptionsFor(TextureUsage usage) {
    imageio::ReadOptions options;
    switch (usage) {
        case TextureUsage::Color:
            // Empat kanal, meski alfanya tidak dipakai: `VK_FORMAT_R8G8B8_*`
            // sah menurut Vulkan tetapi hampir tidak pernah didukung perangkat
            // keras, dan tekstur yang formatnya ditolak saat unggah adalah
            // kegagalan yang muncul jauh dari sini.
            options.channels = 4;
            break;
        case TextureUsage::NormalMap:
            options.channels = 4;
            options.type = imageio::PixelType::UInt8;
            break;
        case TextureUsage::Mask:
            options.channels = 1;
            options.type = imageio::PixelType::UInt8;
            break;
        case TextureUsage::Height:
            // Tipenya dibiarkan seperti di berkas. Heightmap enam belas bit yang
            // diturunkan ke delapan bit menghasilkan terasering yang terlihat
            // sebagai kesalahan terrain, bukan sebagai kesalahan impor.
            options.channels = 1;
            break;
        case TextureUsage::Hdr:
            options.channels = 4;
            options.type = imageio::PixelType::Float32;
            break;
    }
    return options;
}

/// Kegunaan menurut `Sim::ImageIO` — yaitu, apakah angkanya warna atau bukan.
imageio::TextureUsage ColorUsageFor(TextureUsage usage) {
    return usage == TextureUsage::Color ? imageio::TextureUsage::Color
                                        : imageio::TextureUsage::Data;
}

/// Format yang dipilih untuk sebuah gambar yang sudah didekode.
struct ChosenFormat {
    /// Nol berarti tidak ada format yang cocok.
    uint32_t vkFormat = 0;
    bool compressed = false;
    BlockFormat block = BlockFormat::Bc7;
    bool perceptual = true;
};

/// Format tanpa kompresi. Dipakai `compress = false`, dan dipakai jalur yang
/// memang belum punya encoder bloknya.
uint32_t PlainFormat(TextureUsage usage, const imageio::ImageDesc& desc) {
    switch (desc.type) {
        case imageio::PixelType::Float32:
            return desc.channels == 4 ? format::kR32G32B32A32Sfloat : 0;
        case imageio::PixelType::UInt16:
            return desc.channels == 1 ? format::kR16Unorm : 0;
        case imageio::PixelType::UInt8:
            break;
    }
    if (desc.channels == 1) {
        return format::kR8Unorm;
    }
    if (desc.channels != 4) {
        return 0;
    }
    // **sRGB di sini adalah tafsir, bukan konversi.** `_SRGB` dan `_UNORM`
    // berisi byte yang identik; yang berbeda hanya apa yang dilakukan sampler
    // saat membacanya. Keputusan itu diambil sekali, di `NeedsSrgbGpuFormat`,
    // dan yang mengambilnya kedua kali dengan aturan sendiri adalah cara tekstur
    // berakhir didekode dua kali atau tidak sama sekali.
    return imageio::NeedsSrgbGpuFormat(ColorUsageFor(usage), desc) ? format::kR8G8B8A8Srgb
                                                                   : format::kR8G8B8A8Unorm;
}

/// Pemetaan `usage` ke format — **tabel, bukan tebakan**.
///
/// Yang tidak ada di tabel jatuh ke format tanpa kompresi, bukan ke format blok
/// yang kira-kira cocok: tekstur yang lebih besar dari seharusnya adalah masalah
/// anggaran, sedangkan tekstur yang bloknya salah adalah gambar yang salah.
ChosenFormat ChooseFormat(const TextureSettings& settings, const imageio::ImageDesc& desc) {
    ChosenFormat chosen;
    chosen.vkFormat = PlainFormat(settings.usage, desc);
    if (!settings.compress || desc.type != imageio::PixelType::UInt8) {
        // Float dan 16-bit tidak punya jalur blok di sini. HDR menunggu BC6H di
        // T4, dan heightmap enam belas bit memang **sengaja** tidak dikompresi:
        // BC4 menyimpan endpoint delapan bit, dan terasering yang dihasilkannya
        // pada terrain terbaca sebagai kesalahan terrain — dicari berhari-hari
        // di tempat yang salah.
        return chosen;
    }

    switch (settings.usage) {
        case TextureUsage::Color: {
            if (desc.channels != 4) {
                return chosen;
            }
            const bool srgb = imageio::NeedsSrgbGpuFormat(imageio::TextureUsage::Color, desc);
            // BC1 hanya kalau keduanya diminta: tanpa alfa, dan kualitas
            // `cepat`. Itu satu-satunya cara pengguna meminta "mode hemat" yang
            // disebut rencananya — setengah ukuran BC7, dan gradiennya
            // berpita. Yang tidak memintanya mendapat BC7.
            const bool thrifty =
                settings.alpha == TextureAlpha::None && settings.quality == TextureQuality::Fast;
            chosen.compressed = true;
            chosen.perceptual = true;
            chosen.block = thrifty ? BlockFormat::Bc1 : BlockFormat::Bc7;
            if (thrifty) {
                chosen.vkFormat = srgb ? format::kBc1RgbSrgb : format::kBc1RgbUnorm;
            } else {
                chosen.vkFormat = srgb ? format::kBc7Srgb : format::kBc7Unorm;
            }
            break;
        }
        case TextureUsage::NormalMap:
            if (desc.channels != 4) {
                return chosen;
            }
            // BC5, dan **tanpa metrik perseptual**. Metrik itu membelanjakan bit
            // pada kanal yang paling dilihat mata; ketiga kanal sebuah normal
            // sama-sama berarti arah, dan yang dihasilkannya adalah pencahayaan
            // yang bergeser sedikit di seluruh permukaan.
            chosen.compressed = true;
            chosen.perceptual = false;
            chosen.block = BlockFormat::Bc5;
            chosen.vkFormat = format::kBc5Unorm;
            break;
        case TextureUsage::Mask:
            if (desc.channels != 1) {
                return chosen;
            }
            chosen.compressed = true;
            chosen.perceptual = false;
            chosen.block = BlockFormat::Bc4;
            chosen.vkFormat = format::kBc4Unorm;
            break;
        case TextureUsage::Height:
        case TextureUsage::Hdr:
            break;
    }
    return chosen;
}

std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path) {
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

BakeResult Fail(std::string message) {
    BakeResult result;
    result.error = std::move(message);
    return result;
}

}  // namespace

uint64_t TextureCacheKey(const std::filesystem::path& source, const TextureSettings& settings) {
    const std::vector<uint8_t> bytes = ReadFileBytes(source);
    if (bytes.empty()) {
        return 0;
    }
    uint64_t hash = HashInto(kFnvOffset, bytes.data(), bytes.size());
    // Medannya dimasukkan satu per satu sebagai byte, bukan struct-nya secara
    // utuh: `TextureSettings` punya padding di antara medan, dan padding tidak
    // diinisialisasi — hash atasnya berbeda antar pemanggilan untuk pengaturan
    // yang sama persis, dan cache-nya tidak akan pernah kena satu kali pun.
    const uint8_t fields[] = {
        static_cast<uint8_t>(settings.usage),   static_cast<uint8_t>(settings.quality),
        static_cast<uint8_t>(settings.alpha),   static_cast<uint8_t>(settings.compress),
        static_cast<uint8_t>(settings.generateMips),
    };
    hash = HashInto(hash, fields, sizeof(fields));
    hash = HashInto(hash, &kBakerVersion, sizeof(kBakerVersion));
    return hash;
}

std::filesystem::path TextureCachePath(const std::filesystem::path& cacheDir, uint64_t key) {
    char name[24];
    std::snprintf(name, sizeof(name), "%016llx.ktx2", static_cast<unsigned long long>(key));
    return cacheDir / name;
}

uint64_t TextureBakeCount() { return g_bakeCount.load(std::memory_order_relaxed); }

BakeResult BakeTexture(const std::filesystem::path& source, const TextureSettings& settings,
                       const std::filesystem::path& cacheDir) {
    const uint64_t key = TextureCacheKey(source, settings);
    if (key == 0) {
        return Fail("cannot read " + source.string());
    }
    const std::filesystem::path cached = TextureCachePath(cacheDir, key);

    std::error_code error;
    if (std::filesystem::exists(cached, error)) {
        BakeResult result;
        result.ok = true;
        result.path = cached;
        result.fromCache = true;
        // Dimensi dan formatnya tidak dibaca ulang dari berkasnya di sini.
        // Yang memuatnya adalah renderer, lewat pembaca KTX2 di `Sim::RHI`, dan
        // membacanya dua kali hanya untuk mengisi tiga angka yang tidak dipakai
        // siapa pun adalah pekerjaan yang persis dihindari cache ini.
        return result;
    }

    const auto started = std::chrono::steady_clock::now();

    imageio::Image image;
    const imageio::ImageIoResult read =
        imageio::Read(source, ReadOptionsFor(settings.usage), image);
    if (!read) {
        return Fail(read.error);
    }
    if (image.desc.width == 0 || image.desc.height == 0) {
        return Fail(source.filename().string() + " decoded to an empty image");
    }

    // Alfa diluruskan **sebelum** apa pun yang lain, dan tidak pernah didekode
    // sRGB di CPU. Yang mendekode adalah sampler lewat format `_SRGB`; melakukan
    // keduanya menghasilkan tekstur yang tergelapkan dua kali.
    if (image.desc.premultipliedAlpha) {
        imageio::ToStraightAlpha(image);
    }

    const ChosenFormat chosen = ChooseFormat(settings, image.desc);
    if (chosen.vkFormat == 0) {
        return Fail(source.filename().string() + ": no Vulkan format for " +
                    std::to_string(image.desc.channels) + " channels of " +
                    imageio::ToString(image.desc.type));
    }

    imageio::MipOptions mipOptions;
    mipOptions.usage = ColorUsageFor(settings.usage);
    mipOptions.renormalize = settings.usage == TextureUsage::NormalMap;

    std::vector<imageio::Image> chain;
    if (settings.generateMips) {
        chain = imageio::BuildMipChain(image, mipOptions);
    } else {
        chain.push_back(std::move(image));
    }
    if (chain.empty()) {
        return Fail(source.filename().string() + ": mip chain is empty");
    }

    Ktx2WriteDesc desc;
    desc.vkFormat = chosen.vkFormat;
    desc.width = chain.front().desc.width;
    desc.height = chain.front().desc.height;
    desc.levels.reserve(chain.size());

    // Hasil kompresi tiap level dipegang di sini sampai berkasnya selesai
    // ditulis: `Ktx2WriteLevel::bytes` meminjam, tidak memiliki.
    std::vector<std::vector<uint8_t>> compressed;
    compressed.reserve(chain.size());

    CompressOptions compressOptions;
    compressOptions.format = chosen.block;
    compressOptions.quality = settings.quality;
    compressOptions.perceptual = chosen.perceptual;

    for (const imageio::Image& level : chain) {
        Ktx2WriteLevel entry;
        entry.width = level.desc.width;
        entry.height = level.desc.height;
        if (chosen.compressed) {
            compressed.emplace_back();
            if (!CompressBlocks(compressOptions, level.bytes, level.desc.width, level.desc.height,
                                level.desc.channels, compressed.back())) {
                return Fail(source.filename().string() + ": cannot compress level " +
                            std::to_string(desc.levels.size()));
            }
            entry.bytes = compressed.back();
        } else {
            entry.bytes = level.bytes;
        }
        desc.levels.push_back(entry);
    }

    // Ditulis ke berkas sementara lalu dipindahkan. Berkas cache yang tertulis
    // separuh — karena disk penuh, karena proses dimatikan — akan terlihat
    // sebagai cache hit pada pemanggilan berikutnya dan dimuat sebagai tekstur
    // yang rusak. Rename di sistem berkas yang sama bersifat atomik, jadi yang
    // pernah terlihat di jalur akhirnya hanya berkas yang utuh.
    const std::filesystem::path temporary = cached.string() + ".part";
    const Ktx2WriteResult written = WriteKtx2(desc, temporary);
    if (!written) {
        std::filesystem::remove(temporary, error);
        return Fail(written.error);
    }
    std::filesystem::rename(temporary, cached, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return Fail("cannot move " + temporary.string() + " into place: " + error.message());
    }

    g_bakeCount.fetch_add(1, std::memory_order_relaxed);

    BakeResult result;
    result.ok = true;
    result.path = cached;
    result.vkFormat = chosen.vkFormat;
    result.width = desc.width;
    result.height = desc.height;
    result.levelCount = static_cast<uint32_t>(desc.levels.size());
    result.milliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    SIM_INFO("Assets", "Baked {} ({}x{}, {} levels, format {}) in {:.1f} ms",
             source.filename().string(), result.width, result.height, result.levelCount,
             chosen.vkFormat, result.milliseconds);
    return result;
}

}  // namespace sim::assets
