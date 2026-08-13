#pragma once

#include "Sim/Whitebox/WhiteboxMesh.h"

#include <filesystem>
#include <string>

/// Jalan keluar dari format ini.
///
/// **Blockout adalah titik awal, bukan penjara.** Whitebox berguna justru selama
/// bentuknya masih berubah tiap hari; begitu ruangannya selesai, yang dikerjakan
/// berikutnya adalah menghias, dan itu pekerjaan DCC. Format yang tidak bisa
/// ditinggalkan memaksa orang memilih antara alat yang tepat dan pekerjaan yang
/// sudah ada — dan hampir selalu yang dikorbankan adalah alatnya.
///
/// OBJ dipilih karena tiga alasan sekaligus: mesin ini sudah bisa membacanya
/// kembali, setiap DCC bisa membukanya, dan ia teks — yang berarti hasilnya bisa
/// diperiksa mata dan diuji tanpa memuat pustaka apa pun.
namespace sim::whitebox {

struct ExportResult {
    bool ok = false;
    std::string error;

    explicit operator bool() const { return ok; }
};

/// Menulis `.obj` beserta `.mtl` di sebelahnya.
///
/// `.mtl`-nya bukan hiasan: pembaca OBJ mengelompokkan segitiga menurut material
/// yang **terdaftar**, jadi berkas tanpanya kembali sebagai satu ruas tunggal —
/// dan enam sisi yang tadinya bermaterial berbeda menjadi satu.
///
/// Namanya diturunkan dari `path`: "Ruang.obj" bersanding dengan "Ruang.mtl".
ExportResult ExportObj(const WhiteboxMesh& box, const std::filesystem::path& path);

/// Isi `.obj`-nya sebagai teks. `materialLibrary` menjadi baris `mtllib`;
/// kosong berarti barisnya tidak ditulis.
std::string BuildObj(const WhiteboxMesh& box, const std::string& materialLibrary);

/// Isi `.mtl` yang bersanding dengannya: satu material per slot yang dipakai.
std::string BuildMtl(const WhiteboxMesh& box);

/// Nama material untuk sebuah slot, dipakai `.obj` dan `.mtl` supaya keduanya
/// tidak mungkin menyebut nama yang berbeda.
std::string MaterialName(int slot);

}  // namespace sim::whitebox
