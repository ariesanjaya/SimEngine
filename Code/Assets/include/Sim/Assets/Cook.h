#pragma once

#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Core/Uuid.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sim::assets {

/// Apa yang ikut dikirim dan apa yang tidak.
///
/// **Dipisah dari yang menyalinnya.** Rencananya bisa dibaca, dibandingkan, dan
/// diuji tanpa satu berkas pun ditulis — dan `--dry-run` karena itu bukan jalur
/// kedua yang bisa menyimpang dari jalur sungguhannya, melainkan jalur yang sama
/// yang berhenti sebelum langkah terakhir.
struct CookPlan {
    /// Level yang jadi titik awal penelusuran, terurut.
    std::vector<std::filesystem::path> levels;

    /// Aset yang terjangkau dari level mana pun, terurut menurut jalurnya.
    std::vector<Uuid> reachable;
    /// Aset yang ada di project tapi tidak terjangkau siapa pun.
    std::vector<Uuid> unreachable;

    std::uintmax_t reachableBytes = 0;
    std::uintmax_t unreachableBytes = 0;

    /// **Tidak ada laporan referensi menggantung, dan itu keputusan.**
    ///
    /// Dua percobaan, keduanya gagal karena sebab yang sama. Yang pertama
    /// melaporkan setiap GUID di berkas level yang tidak ada di indeks — tapi
    /// setiap entity punya GUID sendiri, bentuknya sama persis, jadi sebelas
    /// dari sebelas laporan salah. Yang kedua membatasinya pada
    /// `AssetRecord::dependencies` dengan alasan daftar itu diurai importir per
    /// format; ternyata tidak — `CollectGuids` menyapu seluruh JSON, termasuk
    /// GUID node di dalam sebuah material.
    ///
    /// Tidak ada satu pun tempat di engine ini yang tahu *field mana* pada
    /// sebuah aset berarti "referensi ke aset lain". Sampai ada, laporan
    /// menggantung hanya bisa ditebak — dan sebuah pemangkas aset yang menebak
    /// akan menghentikan CI atas project yang sehat. Ini menunggu skema
    /// referensi per tipe aset, bukan menunggu kode yang lebih pintar.
};

/// Seluruh GUID yang muncul di sebuah teks.
///
/// **Dicari sebagai teks, bukan lewat pengurai per format.** Setiap aset di
/// engine ini berformat JSON dan menyebut aset lain lewat GUID; sebuah pengurai
/// per format berarti satu tempat baru yang harus diperbarui setiap kali ada
/// tipe aset baru — dan yang lupa diperbarui menghasilkan aset yang terpangkas
/// padahal dipakai. Harganya: sebuah GUID yang kebetulan muncul di dalam string
/// bebas ikut terbaca. Salah ke arah menyertakan terlalu banyak, dan itu arah
/// yang benar untuk sebuah pemangkas.
std::vector<Uuid> GuidsIn(std::string_view text);

/// Menelusuri dari level ke seluruh aset yang bisa dicapainya.
///
/// Penelusurannya transitif lewat `AssetRecord::dependencies` — indeks yang
/// sama yang dipakai `UsersOf`, hanya dibaca ke arah sebaliknya.
CookPlan PlanCook(const AssetDatabase& database,
                  const std::vector<std::filesystem::path>& levels);

}  // namespace sim::assets
