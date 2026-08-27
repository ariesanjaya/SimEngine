#pragma once

#include <filesystem>

namespace sim {

/// Folder milik pengguna, dijawab satu kali untuk seluruh engine.
///
/// **Ada di Core, bukan di Platform, justru karena SimHeadless tidak menautkan
/// SDL.** Editor dan mode headless menulis ke folder konfigurasi yang sama —
/// log, layout, preferensi — jadi keduanya wajib menjawab pertanyaan "di mana"
/// dengan jawaban yang sama persis. Sebelum ini masing-masing menghitungnya
/// sendiri di `main.cpp`-nya, dan keduanya sudah terlanjur berbeda: yang satu
/// jatuh ke direktori kerja, yang lain ke `.`, dan yang satu memakai
/// `SDL_getenv` sementara yang lain `std::getenv`. Perbedaan seperti itu tidak
/// pernah terlihat sampai seseorang mencari log yang ternyata ditulis di tempat
/// lain.
///
/// Di Windows `HOME` biasanya tidak ada sama sekali — ia konvensi POSIX, bukan
/// konvensi Windows. Yang membaca `HOME` di sana selalu jatuh ke cadangannya,
/// sehingga editor menulis `.simengine` ke direktori kerja yang kebetulan
/// sedang aktif: berbeda tiap kali dijalankan dari tempat berbeda.

/// Folder rumah pengguna.
///
/// `USERPROFILE` di Windows, `HOME` di tempat lain. Bila keduanya tidak ada —
/// yang praktis hanya terjadi di lingkungan layanan tanpa profil — hasilnya
/// direktori kerja saat ini, supaya program tetap berjalan alih-alih mati
/// karena tidak tahu ke mana harus menulis.
std::filesystem::path HomeDirectory();

/// Folder konfigurasi per-pengguna: `<home>/.simengine`.
///
/// Berisi layout dock, panel yang terbuka, pintasan, log, dan cache thumbnail.
std::filesystem::path ConfigDirectory();

/// Folder dokumen pengguna, tempat project dibuat secara bawaan.
///
/// Di Windows ini **tidak** dihitung sebagai `<home>/Documents`. Folder Documents
/// di sana boleh dipindahkan pengguna, dan OneDrive memang memindahkannya —
/// menebaknya dari nama berarti project ditulis ke folder yang tidak ikut
/// tersinkron dan tidak muncul di tempat pengguna terbiasa mencarinya. Yang
/// ditanya karena itu adalah Windows sendiri.
std::filesystem::path DocumentsDirectory();

}  // namespace sim
