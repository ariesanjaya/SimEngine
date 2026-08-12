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

/// Mesh dalam bentuk yang bisa langsung diunggah: segitiga, indeks 32-bit.
struct MeshData {
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    /// Sejajar dengan `vertices`, atau kosong bila mesh ini tidak ber-skin.
    std::vector<SkinInfluence> influences;
    /// Rangka yang mengulit mesh ini. Kosong bila tidak ada.
    SkeletonData skeleton;
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
