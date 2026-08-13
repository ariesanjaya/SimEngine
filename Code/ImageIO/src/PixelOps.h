#pragma once

#include "Sim/ImageIO/ImageIO.h"

#include <string>

namespace sim::imageio {

/// Konversi piksel yang dipakai bersama semua backend.
///
/// Ada di sini, bukan diulang di masing-masing, karena aturannya harus sama
/// persis di ketiganya. Dua backend yang membulatkan 16-bit ke 8-bit dengan cara
/// yang sedikit berbeda menghasilkan berkas yang terbaca berbeda menurut
/// backend mana yang kebetulan memegang formatnya — dan bedanya terlalu kecil
/// untuk terlihat, tapi cukup untuk membuat uji round-trip gagal sesekali.

/// Mengubah tipe piksel.
///
/// **Tanpa gamma, dan itu disengaja.** Konversinya murni skala: 255 menjadi
/// 1.0, tidak lebih. Yang memutuskan apakah angka itu sRGB atau linear adalah
/// slot material yang memakainya, bukan pembacanya. (Jalur konversi stb sendiri
/// menerapkan pow(x, 2.2) di sini — sebuah keputusan colorspace yang
/// disembunyikan di dalam pemanggilan dekode.)
void ConvertType(const Image& source, PixelType target, Image& out);

/// Mengubah jumlah kanal ke 1, 3, atau 4.
///
/// Mengikuti konvensi stb supaya berkas yang sama menghasilkan piksel yang sama
/// lewat backend mana pun: kanal yang ditambahkan bernilai opak untuk alfa dan
/// replikasi untuk warna, dan penurunan ke satu kanal memakai luminansi
/// berbobot, bukan rata-rata.
void ConvertChannels(const Image& source, uint32_t target, Image& out);

/// Menerapkan `ReadOptions` pada gambar yang sudah didekode.
///
/// Backend yang bisa memaksa bentuknya saat dekode (stb) memanggil ini juga —
/// panggilannya menjadi no-op, dan sebagai gantinya tidak ada backend yang perlu
/// mengingat mana bagian ReadOptions yang sudah ditangani dan mana yang belum.
///
/// `what` muncul di peringatan; ia jalur berkas atau "<memory>".
void ApplyReadOptions(const ReadOptions& options, const std::string& what, Image& image);

}  // namespace sim::imageio
