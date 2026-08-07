#pragma once

#include "Sim/Core/Math.h"
#include "Sim/Render/Frustum.h"

#include <array>
#include <cstdint>
#include <vector>

namespace sim::render {

inline constexpr uint32_t kMaxSdfCascades = 4;

struct SdfClipmapSettings {
    /// Sisi satu kaskade, voxel. Kubik.
    uint32_t resolution = 128;
    uint32_t cascadeCount = 3;
    /// Ukuran voxel kaskade terhalus, meter.
    float finestVoxelSize = 0.1f;
    /// Pengali ukuran voxel antar-kaskade.
    ///
    /// **Harus pangkat dua, dan itu bukan kerapian melainkan syarat.** Kaskade
    /// yang lebih kasar harus punya kisi yang selaras dengan yang lebih halus,
    /// supaya sebuah titik yang berpindah kaskade jatuh di tempat yang sama.
    /// Pengali 3 membuat kedua kisinya bergeser satu sama lain, dan yang
    /// terlihat adalah jahitan yang bergerak saat kamera maju.
    uint32_t voxelScale = 4;

    /// Lebar pita jarak yang disimpan, dalam satuan voxel.
    ///
    /// **Yang disimpan hanya pita tipis di sekitar permukaan.** Delapan bit yang
    /// dipaksa mewakili seluruh jangkauan kaskade menghasilkan langkah kuantisasi
    /// hampir satu meter di kaskade terkasar — sphere tracing dengan langkah
    /// sebesar itu menembus dinding. Di luar pita, nilainya jenuh; sphere
    /// tracing di sana melangkah sepanjang pita dan mengulang, yang justru
    /// perilaku yang benar.
    float bandVoxels = 4.0f;
};

/// Wilayah voxel yang harus ditulis ulang.
struct SdfScrollRegion {
    uint32_t cascade = 0;
    /// Kotak dalam koordinat voxel **dunia** (bukan texel), setengah terbuka.
    glm::ivec3 min{0};
    glm::ivec3 max{0};

    int64_t VoxelCount() const {
        const glm::ivec3 size = glm::max(max - min, glm::ivec3(0));
        return static_cast<int64_t>(size.x) * size.y * size.z;
    }
};

struct SdfScrollResult {
    std::vector<SdfScrollRegion> regions;
    /// True bila ada kaskade yang bergeser lebih jauh daripada lebarnya sendiri
    /// dan harus ditulis ulang seluruhnya.
    bool fullRewrite = false;
};

/// Clipmap SDF global: beberapa kaskade kubik yang mengikuti kamera.
///
/// **Teksturnya tidak bergerak; titik asalnyalah yang bergerak.** Kamera yang
/// maju satu voxel hanya menuntut satu lempeng tepi ditulis ulang, bukan seluruh
/// volume. Tanpa pengalamatan toroidal, sebuah kaskade 128³ berarti 2 juta voxel
/// ditulis ulang setiap frame kamera bergerak — dikali tiga kaskade — dan
/// anggaran 0,4 ms untuk pembaruan clipmap habis sebelum satu ray pun
/// ditembakkan.
///
/// Titik asalnya dikancing ke kelipatan ukuran voxel. Alasannya sama persis
/// dengan pengancingan texel pada cascade bayangan: titik asal yang bergerak
/// pecahan voxel membuat seluruh isi kisi bergeser setiap frame, jadi tidak ada
/// satu pun voxel yang bisa dipakai ulang.
class SdfClipmap {
public:
    void Configure(const SdfClipmapSettings& settings);

    const SdfClipmapSettings& Settings() const { return settings_; }
    uint32_t CascadeCount() const { return settings_.cascadeCount; }

    /// Ukuran voxel sebuah kaskade, meter.
    float VoxelSize(uint32_t cascade) const;
    /// Setengah lebar kaskade di dunia, meter.
    float HalfExtent(uint32_t cascade) const;
    /// Jarak terjauh yang tercakup kaskade terkasar.
    float MaxRange() const;

    /// Koordinat voxel pojok kaskade — sudah dikancing.
    glm::ivec3 VoxelOrigin(uint32_t cascade) const { return origins_[cascade]; }
    /// Titik asal kaskade di dunia, meter.
    Vec3 WorldOrigin(uint32_t cascade) const;

    /// Kaskade terhalus yang masih memuat sebuah titik, atau -1 kalau di luar
    /// semuanya.
    int CascadeFor(const Vec3& worldPosition) const;

    /// Memindahkan seluruh kaskade mengikuti kamera, dan melaporkan wilayah
    /// yang isinya menjadi basi.
    SdfScrollResult Scroll(const Vec3& cameraPosition);

    /// Koordinat voxel dunia → indeks texel di dalam tekstur, toroidal.
    ///
    /// Sisa pembagian yang benar untuk bilangan negatif. `%` C++ memberi sisa
    /// bertanda — −1 % 128 adalah −1, bukan 127 — dan indeks negatif membaca
    /// di luar tekstur. Itu jenis kesalahan yang tidak muncul sampai kamera
    /// melewati titik nol dunia.
    uint32_t WrapAxis(int32_t coordinate) const;
    glm::uvec3 TexelOf(uint32_t cascade, const glm::ivec3& worldVoxel) const;

    /// Koordinat voxel dunia sebuah titik, pada kaskade tertentu.
    glm::ivec3 VoxelOf(uint32_t cascade, const Vec3& worldPosition) const;

    /// Sebuah wilayah voxel dunia, sudah dipetakan ke kotak texel yang tidak
    /// membungkus.
    struct TexelBox {
        glm::uvec3 min{0};
        glm::uvec3 max{0};
        /// Voxel dunia yang bersesuaian dengan `min`. Dibutuhkan pengisi untuk
        /// tahu titik dunia mana yang sedang ditulisnya.
        glm::ivec3 worldMin{0};

        uint32_t VoxelCount() const {
            const glm::uvec3 size = max - min;
            return size.x * size.y * size.z;
        }
    };

    /// Memecah sebuah wilayah menjadi kotak-kotak texel yang tidak membungkus.
    ///
    /// **Sebuah lempeng di dunia bisa terbelah di dalam tekstur.** Pengalamatan
    /// toroidal berarti wilayah yang bersambung di dunia melompati tepi tekstur
    /// dan muncul kembali di sisi seberangnya — sampai delapan potongan bila
    /// ketiga sumbunya membelah. Menyalinnya sebagai satu kotak akan menimpa
    /// texel milik bagian dunia yang sama sekali lain, dan yang terlihat adalah
    /// permukaan yang muncul di tempat yang salah setelah kamera berjalan cukup
    /// jauh.
    void SplitWrapped(const SdfScrollRegion& region, std::vector<TexelBox>& out) const;

    /// Rentang jarak yang bisa diwakili satu texel 8-bit pada kaskade ini.
    float BandRadius(uint32_t cascade) const;
    /// Jarak → nilai 0..1 yang disimpan, dan sebaliknya.
    float EncodeDistance(uint32_t cascade, float distance) const;
    float DecodeDistance(uint32_t cascade, float encoded) const;

private:
    SdfClipmapSettings settings_;
    std::array<glm::ivec3, kMaxSdfCascades> origins_{};
    bool placed_ = false;
};

}  // namespace sim::render
