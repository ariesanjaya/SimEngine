#pragma once

#include "Sim/Material/MaterialCompiler.h"
#include "Sim/Material/ShaderCache.h"

#include <filesystem>
#include <string>
#include <vector>

namespace sim::material {

/// Bahan untuk merakit modul Slang yang utuh dari keluaran `CompileMaterial`.
struct MaterialModuleOptions {
    /// Isi `openpbr.slang`, **ditanam apa adanya** ke dalam modul.
    std::string prelude;
    /// Lapisan yang mungkin dipakai, dari `MaterialCompileResult::lobes`.
    /// Bawaannya menyalakan semuanya — yang tidak menyebutnya mendapat perilaku
    /// sebelum penyaringan ini ada.
    SurfaceLobes lobes;
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

/// Membaca sebuah atau beberapa berkas shader beserta seluruh `#include`-nya,
/// ditanam menjadi satu teks.
///
/// **Ditanam, bukan diserahkan ke `-I`-nya slangc** — alasan yang sama persis
/// dengan prelude OpenPBR: kunci cache adalah hash teks sumber, jadi sumber yang
/// cuma menulis `#include "cluster_common.slang"` menghasilkan kunci yang tidak
/// berubah ketika berkas itu berubah, dan cache akan dengan patuh menyerahkan
/// SPIR-V yang dibangun terhadap deklarasi yang sudah tidak ada lagi.
///
/// Berkas yang sudah pernah ditanam tidak ditanam lagi — `#ifndef` guard di
/// dalamnya tetap benar, tetapi menanam berkas yang sama dua kali membuat
/// modulnya berlipat tanpa guna.
std::string InlineShaderIncludes(const std::filesystem::path& shaderDirectory,
                                 const std::vector<std::string>& roots);

/// Bahan untuk merakit shader fragmen material **untuk pass forward renderer**.
struct ForwardMaterialOptions {
    /// Isi `openpbr.slang`, ditanam apa adanya.
    std::string prelude;
    /// Deklarasi milik renderer: varying kotak dan seluruh set 0-nya. Diisi
    /// dengan `InlineShaderIncludes(dir, {"box_varyings.slang",
    /// "cluster_common.slang", "gi_resolve.slang"})` — yaitu **berkas yang sama
    /// persis** yang di-`#include` `box.frag`.
    std::string frameDeclarations;
    SurfaceLobes lobes;
    std::string fragmentEntry = "main";
};

/// Merakit shader fragmen material yang menggantikan `box.frag` di pass forward.
///
/// **Set 0-nya set 0 renderer, dan bukan salinannya.** Deklarasinya datang dari
/// berkas shader renderer sendiri, ditanam apa adanya — jadi tidak ada daftar
/// kedua berisi dua puluh satu binding yang harus dijaga sepakat dengan yang
/// pertama. Itulah yang membuat keputusan "perluas set 0 renderer" di
/// docs/ANALISA-TEKSTUR-PERMUKAAN.md tidak berbiaya seperti yang diperkirakan di
/// sana.
///
/// **Tahap vertexnya tetap `box.vert`.** Yang berganti hanya shader fragmen, dan
/// itu yang membuat ketidaksepakatan atribut vertex antara modul material dan
/// pipeline kotak — yang tercatat sebagai rintangan di § E8.4 — tidak perlu
/// diselesaikan sama sekali: tidak ada dua tata letak, hanya satu.
///
/// **Yang belum ada di sini, dan sengaja disebut:** spekular tak-langsung. Set 0
/// renderer tidak memuat peta lingkungan terprafilter maupun LUT DFG — keduanya
/// hanya ada di pratinjau material — jadi `evaluateOpenPBR_IBL` dipanggil dengan
/// keduanya nol, yang menyisakan suku difusnya saja. Akibatnya logam tampak
/// gelap di luar sorotan langsungnya sampai renderer ikut memanggang IBL-nya.
std::string AssembleForwardMaterialModule(const std::string& generatedSlang,
                                          const ForwardMaterialOptions& options);

/// Permintaan kompilasi untuk shader fragmen pass forward.
CompileRequest MakeForwardMaterialRequest(const std::string& generatedSlang,
                                          const ForwardMaterialOptions& options);

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
