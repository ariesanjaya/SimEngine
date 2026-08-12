#pragma once

#include "Sim/Core/Math.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace sim::assets {

/// Satu vertex mesh yang sudah siap diunggah.
///
/// **Tata letaknya sengaja sama dengan `BoxVertex` di renderer.** Selama
/// keduanya cocok, mesh yang diimpor dan kubus bawaan bisa memakai pipeline
/// yang sama persis — dan pipeline kedua yang hanya berbeda pada offset atribut
/// adalah pipeline yang suatu saat berbeda pada hal lain tanpa ada yang
/// menyadarinya.
struct MeshVertex {
    Vec3 position{0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    Vec2 uv{0.0f};
};

/// Paling banyak empat bone memengaruhi satu vertex.
///
/// **Empat, dan itu batas yang dipilih bukan diwarisi.** Pengaruh kelima dan
/// seterusnya hampir selalu berbobot di bawah satu persen — yang tidak bisa
/// dibedakan mata — sementara masing-masing menambah satu indeks, satu bobot,
/// dan satu perkalian matriks per vertex per frame. Yang dibuang dinormalkan
/// kembali ke empat yang tersisa, bukan dibiarkan hilang: bobot yang tidak
/// berjumlah satu menyusutkan vertexnya ke arah titik asal.
inline constexpr int kMaxInfluences = 4;

/// Pengaruh skinning satu vertex.
///
/// **Tata letaknya ABI dengan atribut skin di `Shaders/instance_common.slang`.**
/// Struct ini diunggah apa adanya sebagai vertex buffer kedua — `bones` menjadi
/// `R16G16B16A16_UINT` pada lokasi 8, `weights` menjadi `R32G32B32A32_SFLOAT`
/// pada lokasi 9. Menyisipkan medan di antaranya menggeser bobot menjadi indeks
/// dan sebaliknya, dan yang terlihat bukan galat melainkan kulit yang mengikuti
/// bone yang salah.
struct SkinInfluence {
    std::array<uint16_t, kMaxInfluences> bones{};
    std::array<float, kMaxInfluences> weights{};

    float WeightSum() const {
        float sum = 0.0f;
        for (const float weight : weights) {
            sum += weight;
        }
        return sum;
    }
    /// Membuang yang terlemah bila lebih dari empat, lalu menormalkan sisanya.
    void Normalize();
};

/// Jumlah berbobot keempat matriks kulit sebuah vertex.
///
/// **Acuan CPU untuk yang dijalankan vertex shader.** `skinMatrix` di
/// `Shaders/skin_common.slang` menghitung hal yang sama persis, dan keduanya
/// diuji terhadap pose yang sama — rumus yang berselisih di antara keduanya
/// muncul sebagai kulit yang benar di satu tempat dan salah di tempat lain,
/// tanpa satu pun galat.
///
/// Indeks bone di luar `palette` diperlakukan sebagai matriks satuan. Shader
/// tidak memeriksanya — memeriksa batas di lingkaran terdalam adalah ongkos yang
/// dibayar setiap vertex setiap frame untuk keadaan yang dijaga importir — jadi
/// yang di sini adalah jaring pengaman untuk ujinya, bukan perilaku yang ditiru.
Mat4 SkinMatrix(const SkinInfluence& influence, std::span<const Mat4> palette);

/// Titik ruang bind yang sudah diulit. Sama dengan `skinPosition` di shader.
Vec3 SkinPoint(const SkinInfluence& influence, std::span<const Mat4> palette, const Vec3& point);

/// Arah (normal, tangent) yang sudah diulit: bagian 3x3 matriks kulit saja.
///
/// Benar untuk bone rotasi + translasi + skala seragam; skala tak seragam
/// menuntut invers transpose. Hasilnya **tidak** dinormalkan — shader
/// menormalkannya sesudah transform instance, dan menormalkan dua kali bukan
/// hal yang sama dengan menormalkan sekali di ujung.
Vec3 SkinDirection(const SkinInfluence& influence, std::span<const Mat4> palette,
                   const Vec3& direction);

/// Satu bone hasil impor: nama, induk, dan transform bind terhadap induknya.
struct SkeletonBone {
    std::string name;
    /// Indeks induk, atau -1. **Selalu lebih kecil daripada indeks bone ini
    /// sendiri** — urutan topologis yang dituntut `animation::Skeleton`, dan
    /// yang membuat transform global bisa dihitung satu lintasan maju.
    int parent = -1;
    Vec3 translation{0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f};
};

struct SkeletonData {
    std::vector<SkeletonBone> bones;

    bool IsValid() const { return !bones.empty(); }
    /// Indeks bone bernama itu, atau -1.
    int Find(std::string_view name) const;
    /// Benar bila setiap induk mendahului anaknya.
    bool IsTopological() const;
};

/// Material seperti yang tertulis di berkas mesh-nya.
///
/// **Bentuk yang sengaja dangkal, bukan `MaterialGraph`.** Yang ada di FBX
/// adalah beberapa angka dan beberapa jalur tekstur; menerjemahkannya langsung
/// menjadi graph di sini berarti modul Assets harus mengenal katalog node
/// material, dan itu modul yang lain. Yang menerjemahkannya adalah yang punya
/// keduanya — sama seperti rangka, yang di sini datar dan menjadi
/// `animation::Skeleton` di tempat lain.
struct MeshMaterial {
    std::string name;
    Vec3 baseColor{0.8f, 0.8f, 0.8f};
    float roughness = 0.5f;
    float metalness = 0.0f;
    Vec3 emissive{0.0f, 0.0f, 0.0f};
    float opacity = 1.0f;

    /// Jalur tekstur **relatif terhadap berkas mesh-nya**, pemisahnya sudah
    /// dinormalkan ke `/`. Kosong berarti tidak ada.
    ///
    /// Relatif, bukan absolut: jalur absolut di dalam berkas FBX menunjuk mesin
    /// tempat ia diekspor — `C:\Users\...` — dan hampir tidak pernah ada di
    /// mesin yang membukanya.
    std::string baseColorTexture;
    std::string normalTexture;
    std::string roughnessTexture;
    std::string metalnessTexture;
    std::string emissiveTexture;

    bool HasTexture() const {
        return !baseColorTexture.empty() || !normalTexture.empty() ||
               !roughnessTexture.empty() || !metalnessTexture.empty() ||
               !emissiveTexture.empty();
    }
};

/// Satu ruas indeks yang digambar dengan satu material.
///
/// **Ada karena seluruh node digabung menjadi satu mesh.** Penggabungan itu
/// pilihan lama — satu buffer per aset alih-alih satu per node — dan harganya
/// dibayar di sini: sebuah rig Mixamo punya dua node bermaterial berbeda, jadi
/// mesh gabungannya tidak bisa lagi digambar dengan satu panggilan.
struct SubMesh {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    /// Indeks ke `MeshData::materials`, atau -1 bila berkasnya tidak menyebut
    /// material untuk ruas ini.
    int material = -1;
};

/// Mesh dalam bentuk yang bisa langsung diunggah: segitiga, indeks 32-bit.
struct MeshData {
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    /// Sejajar dengan `vertices`, atau kosong bila mesh ini tidak ber-skin.
    std::vector<SkinInfluence> influences;
    /// Rangka yang mengulit mesh ini. Kosong bila tidak ada.
    SkeletonData skeleton;
    /// Material yang dirujuk berkasnya, sesuai urutan di dalamnya.
    std::vector<MeshMaterial> materials;
    /// Ruas indeks per material, berurutan dan tanpa celah. **Selalu berisi
    /// sedikitnya satu ruas** untuk mesh yang sah — mesh tanpa material sama
    /// sekali tetap punya satu ruas ber-`material` -1, sehingga jalur gambarnya
    /// tidak perlu membedakan "punya ruas" dan "tidak".
    std::vector<SubMesh> parts;
    /// AABB dalam ruang lokal mesh.
    Vec3 boundsMin{0.0f};
    Vec3 boundsMax{0.0f};

    bool IsValid() const {
        return !vertices.empty() && !indices.empty() && indices.size() % 3 == 0;
    }
    /// Ber-skin bila setiap vertex punya pengaruhnya dan rangkanya ada.
    bool IsSkinned() const {
        return skeleton.IsValid() && influences.size() == vertices.size();
    }
    std::size_t TriangleCount() const { return indices.size() / 3; }

    /// Menghitung ulang AABB dari vertex yang ada.
    ///
    /// **Batasnya dihitung, bukan diterima dari berkasnya.** FBX menyimpan batas
    /// yang dihitung DCC-nya sendiri, sebelum transform node diterapkan dan
    /// sebelum satuan dikonversi — memakainya berarti frustum culling membuang
    /// mesh yang sesungguhnya terlihat, dan yang tampak adalah objek yang lenyap
    /// saat kamera diputar sedikit.
    void ComputeBounds();
};

/// Menyatukan vertex kembar dan membangun indeksnya.
///
/// Masukannya daftar segitiga tanpa indeks — tiga vertex berurutan per segitiga.
///
/// **Perbandingannya bit-per-bit, bukan dengan toleransi.** Toleransi menyatukan
/// vertex di dua sisi tepi tajam yang normalnya memang berbeda, dan yang
/// terlihat adalah tepi kotak yang tiba-tiba membulat. Yang dicari di sini
/// hanyalah vertex yang benar-benar sama, yaitu yang muncul karena satu titik
/// dipakai beberapa segitiga.
/// `influenceSoup` boleh kosong; bila diisi ia harus sepanjang `triangleSoup`,
/// dan pengaruhnya ikut menentukan kembar-tidaknya sebuah vertex — dua titik
/// yang sama tapi berbeda bobot skin adalah dua vertex yang berbeda.
MeshData BuildIndexedMesh(const std::vector<MeshVertex>& triangleSoup,
                          const std::vector<SkinInfluence>& influenceSoup = {});

/// Memuat **hanya rangka** dari berkas FBX atau OBJ.
///
/// Urutan bone-nya sama persis dengan yang dihasilkan `LoadMesh` atas berkas
/// yang sama — dan itu bukan kebetulan melainkan syarat: indeks bone di dalam
/// `SkinInfluence` menunjuk ke urutan itu, jadi rangka yang urutannya berbeda
/// menghasilkan kulit yang mengikuti tulang yang salah tanpa satu pun galat.
///
/// **Ada terpisah karena pemakainya tidak butuh geometrinya.** Yang memutar klip
/// pada sebuah mesh ber-rig butuh nama dan hierarki bone untuk memasang track,
/// bukan vertexnya — dan vertexnya sudah ada di GPU, diurai perender. Mengurai
/// ulang rig sebelas megabyte hanya untuk membaca 65 nama adalah ongkos yang
/// tidak perlu ada.
///
/// Mengembalikan rangka kosong pada kegagalan, dengan sebabnya di `error`.
/// Menyusun ulang indeks supaya segitiga bermaterial sama bersebelahan, lalu
/// mencatat ruasnya di `mesh.parts`.
///
/// `triangleMaterial` sepanjang jumlah segitiga `mesh`. **Dipakai bersama kedua
/// importir**: FBX dan glTF sama-sama menghasilkan segitiga bermaterial campur,
/// dan pembagian yang ditulis dua kali adalah dua pembagian yang suatu saat
/// tidak sepakat.
void GroupByMaterial(MeshData& mesh, const std::vector<int>& triangleMaterial);

/// Memuat mesh dari berkas glTF atau GLB.
///
/// **Jauh lebih sedikit jebakan daripada FBX**, dan itu sifat formatnya: glTF
/// menetapkan tangan-kanan, Y ke atas, dan satuan meter di dalam
/// spesifikasinya, jadi tidak ada konvensi yang harus ditebak. Materialnya pun
/// per primitive, bukan per muka, sehingga memetakan satu-satu ke `SubMesh`.
///
/// **Tidak menulis apa pun ke disk.** Tekstur GLB tertanam di dalam berkasnya;
/// yang tercatat di `MeshMaterial` hanyalah nama logisnya, dan yang
/// mengeluarkannya menjadi berkas adalah langkah tersendiri yang dipanggil
/// dengan sengaja.
MeshData LoadGltfMesh(const std::filesystem::path& path, std::string& error);

/// Mengeluarkan tekstur sebuah berkas glTF/GLB menjadi berkas di `folder`.
///
/// **Langkah tersendiri, bukan bagian `LoadMesh`.** `LoadMesh` dipanggil
/// perender di jalur daftar gambar; fungsi yang menulis berkas sebagai efek
/// samping menggambar adalah fungsi yang menulis berkas pada waktu yang tidak
/// bisa ditebak siapa pun — dan ke folder mana pun yang kebetulan sedang aktif.
/// Yang di sini dipanggil dengan sengaja, oleh editor.
///
/// Nama berkasnya sama persis dengan yang tercatat di `MeshMaterial`, karena
/// keduanya berasal dari satu fungsi penamaan. Gambar yang tertanam ditulis apa
/// adanya — isinya sudah PNG atau JPEG, jadi tidak ada yang perlu dikodekan
/// ulang. Berkas yang sudah ada **tidak** ditimpa: tekstur yang sudah disunting
/// orang tidak boleh hilang karena mesh-nya diimpor ulang.
///
/// `outWritten` diisi nama berkas yang benar-benar ditulis. Mengembalikan false
/// hanya bila berkasnya sendiri tidak terbaca — nol gambar bukan kegagalan.
bool ExtractGltfTextures(const std::filesystem::path& path, const std::filesystem::path& folder,
                         std::vector<std::string>& outWritten, std::string& error);

SkeletonData LoadSkeleton(const std::filesystem::path& path, std::string& error);

/// Memuat mesh dari berkas FBX atau OBJ.
///
/// **Menggantikan importir pass-through E5 untuk `AssetType::Mesh`.** Sampai
/// sekarang `.fbx` hanya diperiksa keberadaannya, dan yang digambar viewport
/// adalah kubus satuan untuk setiap mesh renderer — tidak peduli aset mana yang
/// ditetapkan.
///
/// Seluruh node digabung menjadi satu mesh, masing-masing sudah dikalikan
/// transform dunianya. Yang hilang karena itu adalah struktur hierarkinya; yang
/// didapat adalah satu buffer per aset alih-alih satu per node.
///
/// Mengembalikan mesh kosong pada kegagalan, dengan sebabnya di `error`.
MeshData LoadMesh(const std::filesystem::path& path, std::string& error);

}  // namespace sim::assets
