#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace sim::assets {

/// Gambar kecil siap unggah, selalu RGBA8.
struct ThumbnailImage {
    std::vector<uint8_t> rgba;
    uint32_t width = 0;
    uint32_t height = 0;

    bool IsValid() const { return width > 0 && height > 0 && !rgba.empty(); }
};

/// Membuat thumbnail dari sebuah berkas gambar.
///
/// Berjalan di thread latar: mendekode, mengecilkan ke dalam kotak `maxSize`
/// dengan proporsi terjaga, lalu menyimpannya ke cache disk supaya sesi
/// berikutnya tidak perlu mendekode ulang.
///
/// Kunci cache adalah hash isi berkas, bukan waktu ubahnya. Waktu ubah bisa
/// sama walau isinya berganti — misalnya berkas yang dipulihkan dari arsip —
/// dan thumbnail basi yang tidak pernah menyegarkan diri lebih menyesatkan
/// daripada tidak ada thumbnail sama sekali. Isinya toh sudah dibaca untuk
/// didekode, jadi hash-nya nyaris gratis.
///
/// Mengembalikan gambar tidak valid bila berkasnya bukan gambar yang dikenali.
ThumbnailImage MakeThumbnail(const std::filesystem::path& path,
                             const std::filesystem::path& cacheDir, uint32_t maxSize);

}  // namespace sim::assets
