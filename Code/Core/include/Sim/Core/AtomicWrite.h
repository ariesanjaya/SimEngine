#pragma once

#include <filesystem>

namespace sim {

/// Nama berkas sementara untuk penulisan atomik ke `target`.
///
/// **Unik per penulis, dan itu yang membuat rename-nya benar-benar atomik.**
/// Pola tulis-lalu-pindahkan dipakai setiap artefak masak di mesin ini, dan
/// alasannya selalu sama: sebuah proses yang mati di tengah tulis meninggalkan
/// berkas terpotong yang jalan berikutnya temukan sebagai cache yang sah.
///
/// Alasan kedua yang lebih halus: **dua penulis bisa memanggang artefak yang
/// sama pada saat yang sama.** Kuncinya adalah isi berkas sumbernya, bukan siapa
/// yang memanggangnya — jadi jalur geometri CPU dan jalur unggah GPU bisa
/// menuliskan berkas yang sama persis, bersamaan. Rename memang atomik, tetapi
/// nama sementara yang **dibagi** membatalkan gunanya: keduanya membuka aliran
/// ke satu berkas, isinya menjadi campuran, dan rename yang atomik justru yang
/// membuat campuran itu terlihat sah.
///
/// Pid dan penghitung, bukan alamat atau angka acak: keduanya terbatas dan bisa
/// dibaca, jadi sisa berkas dari proses yang mati bisa dikenali pemiliknya
/// alih-alih menjadi nama yang tidak berarti bagi siapa pun.
std::filesystem::path UniqueTemporaryPath(const std::filesystem::path& target);

}  // namespace sim
