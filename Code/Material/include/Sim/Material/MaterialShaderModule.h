#pragma once

#include "Sim/Material/ShaderCache.h"

#include <filesystem>
#include <string>

namespace sim::material {

/// Bahan untuk merakit modul Slang yang utuh dari keluaran `CompileMaterial`.
struct MaterialModuleOptions {
    /// Isi `openpbr.slang`, **ditanam apa adanya** ke dalam modul.
    std::string prelude;
    /// Nama entry point fragment.
    std::string entryPoint = "fragmentMain";
};

/// Merakit modul Slang yang bisa dikompilasi dari keluaran `CompileMaterial`.
///
/// Keluaran kompiler graph sendiri belum bisa diberikan ke `slangc`: ia berisi
/// `evalMaterial()` dan tidak punya entry point, tidak punya varying, dan tidak
/// punya yang memanggil model shading-nya. Fungsi inilah yang melengkapinya.
///
/// **Prelude ditanam, bukan di-`import`.** Itu keputusan yang dibuat demi
/// cache-nya: kunci cache adalah hash dari teks sumber yang diberikan ke
/// kompilator, jadi sumber yang cuma menulis `import openpbr;` menghasilkan
/// kunci yang **tidak berubah ketika `openpbr.slang` berubah** — dan cache akan
/// dengan patuh menyerahkan SPIR-V yang dibangun terhadap model shading yang
/// sudah tidak ada lagi. Menanamnya membuat setiap perubahan pada model shading
/// otomatis membatalkan seluruh material, tanpa satu pun daftar dependensi yang
/// harus dipelihara tangan.
///
/// Harganya: modul yang dikompilasi jadi panjang, dan setiap material membayar
/// parsing prelude-nya sendiri. Itu ongkos sekali per material per perubahan,
/// dibayar oleh cache yang justru jadi benar — pertukaran yang jelas arahnya.
///
/// **Konstanta spesialisasi dideklarasikan di sini, ketiganya.** Baru
/// `kAlphaTest` yang benar-benar dipakai; `kSkinned` dan `kInstanced` menunggu
/// tahap vertex yang datang bersama pipeline mesh. Ketiganya tetap
/// dideklarasikan sekarang supaya nomor `constant_id`-nya terkunci sejak awal —
/// nomor yang bergeser belakangan berarti sakelar yang tertukar tanpa satu pun
/// pesan galat.
///
/// Akibat yang akan membingungkan orang yang membuka `spirv-dis`: hanya
/// `SpecId 2` yang muncul di modul. Konstanta yang tidak dibaca siapa pun
/// dibuang slangc, jadi kedua yang lain belum ada di SPIR-V sampai tahap vertex
/// memakainya. Itu tidak merusak apa pun — Vulkan mengabaikan entri
/// `VkSpecializationInfo` yang `constantID`-nya tidak ada di modul.
std::string AssembleMaterialModule(const std::string& generatedSlang,
                                   const MaterialModuleOptions& options = {});

/// Membaca `openpbr.slang` dari direktori shader.
///
/// Mengembalikan string kosong bila tidak terbaca — dan modul yang dirakit
/// tanpa prelude tidak akan dikompilasi, yang memang seharusnya: material tanpa
/// model shading bukan material yang bisa jalan sebagian.
std::string LoadOpenPbrPrelude(const std::filesystem::path& shaderDirectory);

/// Permintaan kompilasi untuk sebuah material, siap diberikan ke `ShaderCache`.
CompileRequest MakeMaterialRequest(const std::string& generatedSlang,
                                   const MaterialModuleOptions& options = {});

}  // namespace sim::material
