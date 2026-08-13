#pragma once

#include "Sim/Core/Math.h"

#include <cstdint>
#include <vector>

namespace sim {

/// Grid jarak bertanda yang sudah dibake, di ruang lokal sebuah mesh.
///
/// **Tipe biasa, tanpa satu pun tipe OpenVDB.** Itulah seluruh alasan ia ada:
/// yang membakenya adalah `Sim::Volume` di atas OpenVDB, yang memakainya adalah
/// `Sim::Render` saat mengisi clipmap — dan keduanya tidak boleh saling
/// mengenal. OpenVDB adalah pengondisi aset; ia tidak pernah ikut ke jalur yang
/// dikirim ke pemain.
///
/// Ia tinggal di `Sim::Core` karena itulah satu-satunya modul yang sudah ada di
/// bawah keduanya, dan isinya memang wadah data murni — sekelas `Curve` di
/// sebelahnya, bukan sebuah sistem.
///
/// **Nilainya jenuh di luar pita.** Level set hanya menyimpan jarak yang tepat
/// di dekat permukaan; di luar itu nilainya `+band` (luar) atau `-band` (dalam).
/// Itu bukan kekurangan melainkan yang memang dibutuhkan: penyandian clipmap
/// pun jenuh di jarak yang sama, jadi menyimpan lebih jauh hanya membuang
/// memori pada angka yang akan dijepit juga.
struct SdfGrid {
    /// Pusat voxel (0,0,0) di ruang lokal mesh.
    Vec3 origin{0.0f};
    /// Sisi satu voxel, meter (ruang lokal).
    float voxelSize = 0.0f;
    /// Jarak di mana nilainya jenuh — sama dengan `bandVoxels * voxelSize`.
    float band = 0.0f;
    uint32_t sizeX = 0;
    uint32_t sizeY = 0;
    uint32_t sizeZ = 0;
    /// X tercepat, lalu Y, lalu Z.
    std::vector<float> distances;

    bool Empty() const { return distances.empty(); }
    std::size_t VoxelCount() const {
        return static_cast<std::size_t>(sizeX) * sizeY * sizeZ;
    }

    /// Nilai satu voxel. Indeks di luar grid mengembalikan `+band`: di luar
    /// kotak yang dibake, yang bisa dikatakan hanyalah "setidaknya sejauh pita".
    float At(int32_t x, int32_t y, int32_t z) const;

    /// Jarak terinterpolasi trilinear pada sebuah titik di **ruang lokal mesh**.
    ///
    /// Titik di luar grid mengembalikan `+band`, dengan alasan yang sama — dan
    /// itu arah yang aman untuk sphere tracing: melangkah terlalu pendek hanya
    /// membuang langkah, sedangkan melangkah terlalu jauh menembus dinding.
    float SampleLocal(const Vec3& local) const;
};

}  // namespace sim
