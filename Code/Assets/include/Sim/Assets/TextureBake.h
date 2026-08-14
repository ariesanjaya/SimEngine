#pragma once

#include "Sim/Assets/TextureSettings.h"

#include <cstdint>
#include <filesystem>
#include <string>

/// Baker tekstur: dari berkas sumber menjadi `.ktx2` di cache.
///
/// Membaca sumbernya lewat `Sim::ImageIO`, membangkitkan mip **di ruang
/// linear**, memilih `VkFormat` sesuai pemakaiannya, dan menulis kontainernya
/// lewat libktx.
namespace sim::assets {

struct BakeResult {
    bool ok = false;
    std::string error;

    /// Berkas `.ktx2` di dalam cache. Terisi hanya bila `ok`.
    std::filesystem::path path;

    /// True bila berkasnya sudah ada dan tidak ada yang dikerjakan ulang.
    bool fromCache = false;

    /// Nomor `VkFormat`. Ada di sini supaya uji bisa mengunci aturan yang
    /// salahnya tidak terlihat: normal map tidak pernah ber-format `_SRGB`, dan
    /// base color tidak pernah linear.
    uint32_t vkFormat = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t levelCount = 0;

    /// Nol pada `fromCache`. Dicatat karena rencananya menuntut angka, bukan
    /// kesan: bake 4K yang memakan beberapa detik menuntut menurunkan kualitas
    /// bawaannya.
    double milliseconds = 0.0;

    explicit operator bool() const { return ok; }
};

/// Kunci cache: hash isi berkas sumber, hash pengaturannya, dan versi baker.
///
/// **Isi, bukan jalur.** Cache berkunci jalur akan salah setiap kali berkas
/// berpindah atau diganti isinya tanpa berganti nama, dan salahnya berupa
/// tekstur basi yang terlihat benar — tidak ada satu pun tanda bahwa yang
/// tergambar bukan berkas yang ada di disk.
///
/// Versi baker ikut karena encoder yang berubah menghasilkan blok yang berbeda
/// dari sumber yang sama persis. Tanpanya, memperbaiki bug di encoder tidak akan
/// pernah terlihat pada satu pun aset yang sudah pernah di-bake.
///
/// Nol bila sumbernya tidak bisa dibaca.
uint64_t TextureCacheKey(const std::filesystem::path& source, const TextureSettings& settings);

/// Jalur berkas cache untuk sebuah kunci.
std::filesystem::path TextureCachePath(const std::filesystem::path& cacheDir, uint64_t key);

/// Berapa kali baker benar-benar mengerjakan sebuah tekstur sejak proses
/// dimulai — bukan berapa kali ia dipanggil.
///
/// **Ada supaya cache dibuktikan dengan hitungan, bukan dengan waktu.**
/// Pemanggilan kedua yang lebih cepat juga dihasilkan cache halaman sistem
/// berkas, dan itu membuktikan hal yang lain.
uint64_t TextureBakeCount();

/// Membangun `.ktx2` untuk sebuah tekstur, atau memakai yang sudah ada di cache.
BakeResult BakeTexture(const std::filesystem::path& source, const TextureSettings& settings,
                       const std::filesystem::path& cacheDir);

}  // namespace sim::assets
