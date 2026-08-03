#pragma once

#include <cstddef>
#include <vector>

/// Kemampuan stb yang hanya terlihat di dalam TU implementasinya.
namespace stb_impl {

/// Memampatkan dengan deflate (aliran zlib) memakai kompresor yang sudah ada di
/// `stb_image_write`.
///
/// Dibungkus, bukan dipanggil langsung, karena dua alasan yang sama-sama praktis:
/// stb tidak menaruh deklarasi `stbi_zlib_compress` di bagian header berkasnya —
/// hanya definisinya di bagian implementasi — dan buffer yang dikembalikannya
/// harus dibebaskan dengan alokator yang sedang dipakai stb. Membungkusnya
/// membuat kepemilikan buffer itu tidak pernah keluar dari TU yang tahu keduanya.
std::vector<unsigned char> Deflate(const unsigned char* data, std::size_t length, int quality);

}  // namespace stb_impl
