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

    /// Indeks material per voxel, sejajar `distances`. Kosong berarti grid ini
    /// tidak membawa warna sama sekali.
    ///
    /// **Indeks dan palet, bukan RGB per voxel.** Untuk Sponza itu 12,7 MB
    /// terhadap 51 MB, dan warnanya tetap persis — sebuah mesh punya puluhan
    /// material, bukan jutaan warna. Nol berarti "tidak diketahui": voxel di
    /// luar pita, tempat tidak ada permukaan yang bisa ditanyai warnanya.
    std::vector<uint8_t> materials;
    /// Albedo linear per indeks material. `palette[0]` tidak pernah dipakai —
    /// ia tempat nol yang berarti "tidak diketahui".
    std::vector<Vec3> palette;

    bool Empty() const { return distances.empty(); }
    bool HasAlbedo() const { return materials.size() == distances.size() && palette.size() > 1; }

    /// Albedo pada sebuah voxel, atau `fallback` bila voxel itu tidak punya
    /// material — di luar pita, atau grid tanpa warna sama sekali.
    ///
    /// **Tetangga terdekat, bukan trilinear.** Yang disimpan indeks, dan
    /// interpolasi antar indeks menghasilkan indeks yang menunjuk material yang
    /// bukan salah satu dari keduanya. Warna permukaan pun memang tidak
    /// berpadu: batu yang bertemu kain bertemu di sebuah tepi, bukan di sebuah
    /// gradien.
    Vec3 AlbedoAt(int32_t x, int32_t y, int32_t z, const Vec3& fallback) const;
    /// Albedo pada sebuah titik di ruang lokal mesh.
    Vec3 AlbedoLocal(const Vec3& local, const Vec3& fallback) const;
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
