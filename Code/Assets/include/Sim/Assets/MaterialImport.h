#pragma once

#include "Sim/Assets/MeshData.h"
#include "Sim/Material/MaterialInstance.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

/// Menerjemahkan material hasil impor mesh menjadi aset material mesin ini.
///
/// **Satu induk, banyak instance.** Material yang datang dari berkas mesh —
/// glTF, FBX, maupun USD — seluruhnya sudah dinormalkan importirnya menjadi
/// `MeshMaterial`, dan seluruhnya memetakan ke lima parameter yang sama. Jadi
/// yang dibangkitkan bukan satu graph per material melainkan satu `.simmatinst`
/// per material di atas satu induk bersama, `Resources/Materials/Material
/// Impor.simmat`. Memperbaiki induknya memperbaiki seluruh material impor
/// sekaligus; membangkitkan graph per material berarti perbaikan yang sama harus
/// diulang di setiap berkas yang pernah diimpor siapa pun.
///
/// **Kenapa pemetaannya bisa langsung.** Konvensi OpenPBR mesin ini kebetulan —
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

/// GUID induk `Material Impor.simmat`, sebagaimana tercatat di `.meta`-nya.
///
/// **Tetap, bukan dicari saat berjalan.** Instance menyimpan GUID induknya
/// bukan jalurnya, jadi induknya boleh dipindah maupun diganti nama tanpa
/// memutus satu pun instance — tapi itu hanya berlaku selama GUID-nya sendiri
/// tidak berubah. Menuliskannya di sini berarti berkas `.meta` di sebelah aset
/// dan tetapan ini adalah satu keputusan di dua tempat; keduanya harus sama.
inline constexpr std::string_view kImportedMaterialGuid = "5f3c9a21-7d4e-4b16-9c08-2ab6e15d7f40";

/// Mengubah satu material hasil impor menjadi instance dari material induk.
material::MaterialInstance MaterialInstanceFromMesh(const MeshMaterial& source,
                                                    const Uuid& parent);

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
                            const Uuid& parent, std::vector<std::string>& outWritten,
                            std::string& error);

}  // namespace sim::assets
