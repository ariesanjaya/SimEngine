#pragma once

#include "Sim/Core/Math.h"

#include <cstdint>
#include <filesystem>
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

/// Mesh dalam bentuk yang bisa langsung diunggah: segitiga, indeks 32-bit.
struct MeshData {
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    /// AABB dalam ruang lokal mesh.
    Vec3 boundsMin{0.0f};
    Vec3 boundsMax{0.0f};

    bool IsValid() const {
        return !vertices.empty() && !indices.empty() && indices.size() % 3 == 0;
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
MeshData BuildIndexedMesh(const std::vector<MeshVertex>& triangleSoup);

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
