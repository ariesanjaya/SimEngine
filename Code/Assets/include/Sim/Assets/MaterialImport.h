#pragma once

#include "Sim/Assets/MeshData.h"
#include "Sim/Assets/TextureSettings.h"
#include "Sim/Material/MaterialInstance.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

/// Menerjemahkan material hasil impor mesh menjadi aset material mesin ini.
///
/// **Satu induk per bentuk material, banyak instance.** Material yang datang
/// dari berkas mesh sudah dinormalkan importirnya, jadi yang dibangkitkan bukan
/// satu graph per material melainkan satu `.simmatinst` per material di atas
/// induk bersama. Memperbaiki induknya memperbaiki seluruh material impor
/// sekaligus; membangkitkan graph per material berarti perbaikan yang sama harus
/// diulang di setiap berkas yang pernah diimpor siapa pun.
///
/// Induknya **dua**, dan itu bukan pengingkaran atas kalimat di atas melainkan
/// akibat langsung dari cara lapisan OpenPBR dimatikan. `DetectLobes` membaca
/// **graph induk**, bukan nilai instance: sebuah pin `coatWeight` yang
/// dikemudikan parameter selalu terbaca "mungkin bukan nol", jadi setiap
/// material yang memakai induk ber-coat ikut membawa kode coat di dalam
/// SPIR-V-nya. Satu induk lengkap untuk semuanya karena itu berarti setiap
/// material glTF di dunia — yang tidak punya coat maupun fuzz sama sekali —
/// membayar kedua lapisan itu.
///
///  - `Material Impor.simmat` — lima angka dan satu tekstur, untuk sumber yang
///    memang hanya punya itu: glTF, USD, dan FBX Lambert/Phong. Tidak berubah
///    sedikit pun oleh adanya yang kedua.
///  - `Material Impor OpenPBR.simmat` — seluruh pin OpenPBR beserta enam slot
///    tekstur, untuk sumber yang **menyatakan dirinya** OpenPBR: dokumen
///    `.mtlx` di sebelah berkas mesh, atau blok parameter 3ds Max di dalam
///    FBX-nya. Yang memakainya membayar coat, fuzz, anisotropi, dan Oren–Nayar
///    — dan itu memang lapisan yang materialnya sendiri sebutkan.
///
/// Yang memilih di antara keduanya adalah ada-tidaknya `MeshMaterial::openPbr`,
/// bukan tebakan atas isi materialnya.
///
/// **Kenapa pemetaan yang datar pun bisa langsung.** Konvensi OpenPBR mesin ini kebetulan —
/// dan bukan kebetulan, keduanya menurunkan dari model yang sama — sudah sejalan
/// dengan glTF di tiga hal yang biasanya jadi sumber kesalahan:
///
/// - `specularRoughness` perseptual, dikuadratkan menjadi alpha di dalam
///   `openpbr.slang`. Sama persis dengan `roughnessFactor` glTF, jadi nilainya
///   disalin apa adanya. Mengkuadratkannya di sini membuat setiap permukaan
///   terlalu mengkilap.
/// - `specularIor` bawaan 1,5 menghasilkan F0 = 0,04, yaitu nilai dielektrik
///   tetap yang dipakai glTF.
/// - `baseDiffuseRoughness` bawaan 0 berarti Lambert, dan itu memang model difus
///   glTF.
///
/// Ketiganya karena itu tidak ikut ditimpa — nilai bawaan induknya sudah benar.
namespace sim::assets {

/// Jalur induknya relatif terhadap `Resources/`.
///
/// **Di `Sistem/`, bukan di sebelah material bawaan lainnya.** Yang ada di
/// `Materials/` adalah titik awal — material sederhana yang memang dimaksudkan
/// untuk disalin orang. Yang ini bukan: ia induk bersama, dan menyalinnya justru
/// membuang satu-satunya gunanya, yaitu satu tempat untuk memperbaiki seluruh
/// material impor sekaligus.
inline constexpr std::string_view kImportedMaterialAsset =
    "Materials/Sistem/Material Impor.simmat";

/// Jalur induk OpenPBR, relatif terhadap `Resources/`.
inline constexpr std::string_view kImportedOpenPbrMaterialAsset =
    "Materials/Sistem/Material Impor OpenPBR.simmat";

/// GUID induk `Material Impor.simmat`, sebagaimana tercatat di `.meta`-nya.
///
/// **Tetap, bukan dicari saat berjalan.** Instance menyimpan GUID induknya
/// bukan jalurnya, jadi induknya boleh dipindah maupun diganti nama tanpa
/// memutus satu pun instance — tapi itu hanya berlaku selama GUID-nya sendiri
/// tidak berubah. Menuliskannya di sini berarti berkas `.meta` di sebelah aset
/// dan tetapan ini adalah satu keputusan di dua tempat; keduanya harus sama.
inline constexpr std::string_view kImportedMaterialGuid = "5f3c9a21-7d4e-4b16-9c08-2ab6e15d7f40";

/// GUID induk `Material Impor OpenPBR.simmat`. Aturan yang sama: berkas `.meta`
/// di sebelah asetnya dan tetapan ini adalah satu keputusan di dua tempat.
inline constexpr std::string_view kImportedOpenPbrMaterialGuid =
    "7c1b4e88-3a52-4d09-b6f1-9e0d5a2c8410";

/// Kedua induk, sudah terurai menjadi GUID.
///
/// **Dipegang satu struct, bukan dua argumen.** Penulis instance harus memilih
/// per material — satu berkas FBX boleh memuat material OpenPBR dan material
/// Phong sekaligus — jadi keduanya harus sampai ke sana bersamaan. Dua parameter
/// terpisah yang harus selalu diisi bersama adalah dua parameter yang suatu saat
/// tertukar.
struct ImportedMaterialParents {
    Uuid flat;
    Uuid openPbr;

    bool IsValid() const { return flat.IsValid() && openPbr.IsValid(); }
};

/// Kedua induk bawaan, dari tetapan GUID di atas.
ImportedMaterialParents DefaultImportedMaterialParents();

/// Nama parameter tekstur di induk. Disebut di satu tempat, dipakai importir
/// maupun yang menggambar — dua ejaan berarti tekstur yang tersimpan tetapi
/// tidak pernah terpasang.
inline constexpr std::string_view kBaseColorTextureParameter = "baseColorTexture";

/// Slot tekstur induk OpenPBR, beserta untuk apa masing-masing dipakai.
///
/// **`usage` ikut karena importir adalah satu-satunya yang tahu.**
/// `GuessUsageFromName` menebak dari nama berkas, dan tebakan itu benar untuk
/// `batu_n.png` lalu meleset untuk `T_Wall_02.png` — sedangkan yang menulis
/// material tahu persis bahwa berkas itu dipasang di slot normal. Peta normal
/// yang salah didekode sebagai sRGB tidak memunculkan galat; ia memunculkan
/// permukaan yang cekung di tempat yang seharusnya cembung.
inline constexpr std::string_view kOpenPbrBaseColorTexture = "baseColorTexture";
inline constexpr std::string_view kOpenPbrMetalnessTexture = "baseMetalnessTexture";
inline constexpr std::string_view kOpenPbrRoughnessTexture = "specularRoughnessTexture";
inline constexpr std::string_view kOpenPbrEmissiveTexture = "emissiveTexture";
inline constexpr std::string_view kOpenPbrOpacityTexture = "opacityTexture";
inline constexpr std::string_view kOpenPbrNormalTexture = "normalTexture";

/// Sakelar peta normal di induk OpenPBR.
///
/// **Ada karena putih bukan identitas untuk peta normal.** Saluran lain
/// mengalikan teksturnya, dan tekstur yang tidak diisi terbaca putih — yaitu
/// identitas perkalian. Peta normal tidak: `2 × putih − 1` adalah (1,1,1),
/// sebuah normal miring 45° di setiap piksel. Jadi induknya menyusunnya sebagai
/// `lerp(datar, terdekode, amount)`, dan yang menyalakannya adalah importir,
/// tepat ketika ia benar-benar memasang sebuah peta.
inline constexpr std::string_view kNormalTextureAmountParameter = "normalTextureAmount";

/// Mengubah jalur tekstur relatif milik `MeshMaterial` menjadi GUID aset.
///
/// **Diserahkan pemanggil, bukan dikerjakan di sini.** Jalurnya relatif terhadap
/// berkas mesh dan kerap naik satu tingkat (`..\checkerA.tga`), jadi
/// menyelesaikannya menuntut tahu di mana berkas mesh itu berada — dan
/// menyalinnya ke dalam project menuntut tahu di mana project itu berada.
/// Keduanya pengetahuan editor, bukan pengetahuan modul aset.
///
/// Mengembalikan GUID tak sah bila teksturnya tidak ada atau tidak bisa
/// disalin; materialnya lalu tetap ditulis, hanya tanpa tekstur.
///
/// `usage` menyatakan untuk apa tekstur itu dipasang, supaya yang menyalinnya
/// bisa menuliskan `TextureSettings` yang benar alih-alih menyerahkannya pada
/// tebakan nama berkas.
using TextureResolver = std::function<Uuid(std::string_view relativePath, TextureUsage usage)>;

/// Mengubah satu material hasil impor menjadi instance dari material induk.
///
/// `resolveTexture` boleh kosong — materialnya lalu hanya membawa kelima
/// parameter skalarnya.
material::MaterialInstance MaterialInstanceFromMesh(const MeshMaterial& source,
                                                    const ImportedMaterialParents& parents,
                                                    const TextureResolver& resolveTexture = {});

/// Menulis satu `.simmatinst` untuk tiap material di `mesh`, ke dalam `folder`.
///
/// Nama berkasnya diturunkan dari nama material di berkas sumbernya lewat
/// `SafeAssetFileName` — nama material datang dari berkas yang dibuat orang
/// lain, dan `../` di dalamnya tidak boleh menjadi jalur. Nama yang bentrok
/// diberi akhiran nomor; nama yang kosong seluruhnya memakai nomor materialnya.
///
/// `outWritten` diisi nama berkas yang benar-benar ditulis, sejajar urutannya
/// dengan `mesh.materials`, supaya pemanggilnya bisa memasangkan ruas mesh ke
/// aset materialnya.
bool WriteMaterialInstances(const MeshData& mesh, const std::filesystem::path& folder,
                            const ImportedMaterialParents& parents,
                            std::vector<std::string>& outWritten, std::string& error,
                            const TextureResolver& resolveTexture = {});

}  // namespace sim::assets
