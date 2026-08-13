#pragma once

#include "Sim/Core/Math.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sim {

/// Grid skalar padat — kerapatan asap, suhu api, atau apa pun yang dibawa
/// sebuah berkas volume.
///
/// **Tipe biasa, tanpa satu pun tipe OpenVDB**, dengan alasan yang sama seperti
/// `SdfGrid` di sebelahnya: yang membacanya adalah `Sim::Volume` di atas
/// OpenVDB, yang mengunggahnya ke GPU adalah `Sim::Render`, dan keduanya tidak
/// boleh saling mengenal.
///
/// **Padat, bukan jarang** — dan itu keputusan yang layak disebut. Berkas VDB
/// jarang justru karena kebanyakan isinya kosong, jadi memadatkannya membuang
/// keunggulan formatnya. Yang membeli kembali biayanya adalah tujuannya:
/// tekstur 3D di GPU pun padat, jadi pemadatan harus terjadi di suatu tempat —
/// dan melakukannya sekali saat impor lebih murah daripada di setiap frame.
/// Volume yang terlalu besar untuk dipadatkan ditolak dengan pesan, bukan
/// dipadatkan sampai kehabisan memori.
struct VolumeGrid {
    /// Nama grid di dalam berkasnya — "density", "temperature", "flames".
    /// Satu berkas VDB bisa membawa beberapa, dan yang dipakai harus disebut.
    std::string name;

    /// Pusat voxel (0,0,0) di ruang lokal berkasnya.
    Vec3 origin{0.0f};
    /// Sisi satu voxel di ruang lokal, meter.
    float voxelSize = 0.0f;

    uint32_t sizeX = 0;
    uint32_t sizeY = 0;
    uint32_t sizeZ = 0;

    /// Nilai di luar grid. Nol untuk kerapatan — di luar asapnya memang tidak
    /// ada asap — dan itulah yang membuat raymarch bisa berhenti lebih awal.
    float background = 0.0f;

    /// Rentang nilai yang benar-benar ada di dalamnya.
    ///
    /// Dibawa karena unggahan GPU membutuhkannya: format 8 dan 16 bit menuntut
    /// nilainya dinormalkan, dan menormalkan dengan rentang yang ditebak
    /// membuat asap tipis menghilang atau api menjadi putih rata.
    float minValue = 0.0f;
    float maxValue = 0.0f;

    /// X tercepat, lalu Y, lalu Z — tata letak yang sama dengan unggahan
    /// tekstur 3D, jadi tidak ada penyusunan ulang saat unggah.
    std::vector<float> values;

    bool Empty() const { return values.empty(); }
    std::size_t VoxelCount() const {
        return static_cast<std::size_t>(sizeX) * sizeY * sizeZ;
    }

    /// Nilai satu voxel; di luar grid mengembalikan `background`.
    float At(int32_t x, int32_t y, int32_t z) const;

    /// Nilai terinterpolasi trilinear pada titik di **ruang lokal berkasnya**.
    float SampleLocal(const Vec3& local) const;

    /// Kotak batas grid di ruang lokal, dari pusat voxel pertama ke pusat voxel
    /// terakhir.
    void LocalBounds(Vec3& outMin, Vec3& outMax) const;

    /// Kotak yang benar-benar ditempati volumenya: `LocalBounds` ditambah
    /// setengah voxel di setiap sisi.
    ///
    /// **Inilah kotak yang dipakai raymarch**, dan karena itu juga kotak yang
    /// digambar sebagai wireframe di editor. Nilainya disimpan di pusat voxel,
    /// jadi kotak pusat-ke-pusat memotong separuh voxel terluar — dan wireframe
    /// yang memakai kotak yang berbeda dari yang dijejaki adalah alat bantu yang
    /// berbohong tepat ketika ia paling dibutuhkan.
    void PaddedLocalBounds(Vec3& outMin, Vec3& outMax) const;

    /// Kotak yang ditempati volumenya di ruang dunia, untuk penempatan tanpa
    /// rotasi. Satu-satunya definisi yang dipakai bersama raymarch dan wireframe.
    void WorldBounds(const Vec3& position, float scale, Vec3& outMin, Vec3& outMax) const;
};

}  // namespace sim
