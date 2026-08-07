#pragma once

#include "Sim/Material/ShaderCache.h"

#include <filesystem>
#include <string>

namespace sim::material {

/// Bahan untuk merakit modul Slang yang utuh dari keluaran `CompileMaterial`.
struct MaterialModuleOptions {
    /// Isi `openpbr.slang`, **ditanam apa adanya** ke dalam modul.
    std::string prelude;
    std::string vertexEntry = "vertexMain";
    std::string fragmentEntry = "fragmentMain";
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
/// **Konstanta spesialisasi ketiganya terpakai**, dan masing-masing hanya muncul
/// di tahap yang membutuhkannya: `kSkinned` dan `kInstanced` di SPIR-V vertex,
/// `kAlphaTest` di fragment. Yang tidak dibaca sebuah tahap dibuang slangc dari
/// modul tahap itu — dan itu tidak merusak apa pun, karena Vulkan mengabaikan
/// entri `VkSpecializationInfo` yang `constantID`-nya tidak ada di modul.
///
/// **Atribut vertex selalu tujuh, apa pun nilai konstantanya.** Spesialisasi
/// terjadi saat pipeline dibuat, sedangkan daftar antarmuka `OpEntryPoint` sudah
/// terkunci di modul: `boneIndices` dan `boneWeights` tetap dideklarasikan
/// meskipun `kSkinned` mati. Mesh tanpa data skin karena itu tetap harus
/// menyediakan kedua atribut — cara yang biasa adalah binding ber-stride nol
/// yang menunjuk buffer nol. Yang tidak boleh adalah membiarkannya tidak
/// terpasang: nilainya jadi tidak terdefinisi, dan itu bukan sesuatu yang muncul
/// sebagai pesan.
std::string AssembleMaterialModule(const std::string& generatedSlang,
                                   const MaterialModuleOptions& options = {});

/// Membaca `openpbr.slang` dari direktori shader.
///
/// Mengembalikan string kosong bila tidak terbaca — dan modul yang dirakit
/// tanpa prelude tidak akan dikompilasi, yang memang seharusnya: material tanpa
/// model shading bukan material yang bisa jalan sebagian.
std::string LoadOpenPbrPrelude(const std::filesystem::path& shaderDirectory);

/// Permintaan kompilasi untuk sebuah material, siap diberikan ke `ShaderCache`.
///
/// **Satu modul, dua entry point, dua entri cache.** Sumbernya sama persis untuk
/// kedua tahap; yang membedakan kuncinya adalah tahap dan nama entry point-nya,
/// dan SPIR-V yang dihasilkan memang berbeda. Merakit dua modul terpisah akan
/// membuat struct varying tertulis dua kali — dan struct varying yang berbeda
/// antara tahap vertex dan fragment adalah kegagalan link pipeline, bukan
/// sesuatu yang bisa ditemukan dengan membaca salah satunya.
CompileRequest MakeMaterialRequest(const std::string& generatedSlang, ShaderStage stage,
                                   const MaterialModuleOptions& options = {});

/// Tata letak descriptor yang ditulis modul, supaya sisi C++ tidak menebaknya.
///
/// Pembagiannya mengikuti seberapa sering isinya berubah: per-frame, per-objek,
/// per-material. Satu set yang mencampur ketiganya memaksa mengikat ulang
/// seluruhnya setiap kali salah satunya berganti.
struct MaterialBindings {
    static constexpr uint32_t kFrameSet = 0;
    static constexpr uint32_t kFrameParams = 0;
    static constexpr uint32_t kBoneMatrices = 1;
    static constexpr uint32_t kInstanceTransforms = 2;

    static constexpr uint32_t kObjectSet = 1;
    static constexpr uint32_t kObjectParams = 0;

    static constexpr uint32_t kMaterialSet = 2;
    static constexpr uint32_t kMaterialParams = 0;

    /// Tekstur ke-`index` dan sampler-nya, berselang-seling mulai binding 1 —
    /// jadi nomornya tidak bergantung pada berapa banyak tekstur seluruhnya.
    static constexpr uint32_t TextureBinding(uint32_t index) { return 1 + index * 2; }
    static constexpr uint32_t SamplerBinding(uint32_t index) { return 2 + index * 2; }
};

/// Lokasi atribut vertex yang diharapkan modul. Sisi C++ menyusun
/// `VkVertexInputAttributeDescription` dari sini, bukan dari hitungan sendiri.
struct MaterialVertexLocation {
    static constexpr uint32_t kPosition = 0;
    static constexpr uint32_t kNormal = 1;
    static constexpr uint32_t kTangent = 2;
    static constexpr uint32_t kUv0 = 3;
    static constexpr uint32_t kColor = 4;
    static constexpr uint32_t kBoneIndices = 5;
    static constexpr uint32_t kBoneWeights = 6;
};

}  // namespace sim::material
