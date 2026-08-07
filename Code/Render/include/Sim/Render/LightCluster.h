#pragma once

#include "Sim/Core/Math.h"
#include "Sim/Render/Frustum.h"

#include <cstdint>
#include <span>
#include <vector>

namespace sim::render {

enum class ClusterLightType : uint8_t {
    Point,
    Spot,
};

/// Lampu yang ikut penyaringan cluster, sudah dalam ruang dunia.
///
/// Directional tidak ada di sini: ia mengenai setiap cluster, jadi memasukkannya
/// hanya menambah satu entri ke setiap daftar. Ia ditangani terpisah bersama
/// cascade-nya.
struct ClusterLight {
    ClusterLightType type = ClusterLightType::Point;
    Vec3 position{0.0f};
    /// Arah pancar spot, dari lampu ke luar. Tidak berarti untuk point.
    Vec3 direction{0.0f, -1.0f, 0.0f};
    float range = 10.0f;
    /// Kosinus setengah sudut kerucut luar. 1 berarti kerucut tak berlebar.
    float cosOuterAngle = 0.7f;
};

struct ClusterGridSettings {
    uint32_t tilesX = 16;
    uint32_t tilesY = 9;
    uint32_t slices = 24;
    /// Banyaknya lampu paling banyak yang disimpan satu cluster.
    ///
    /// Dipotong, bukan dibiarkan tumbuh. Daftar yang panjangnya tak terbatas
    /// membuat satu cluster buruk — sudut ruangan dengan dua puluh lampu —
    /// menentukan biaya seluruh frame, dan yang terlihat adalah frame rate yang
    /// jatuh di satu tempat tanpa sebab yang jelas. Yang terpotong dilaporkan
    /// lewat `ClusterAssignment::overflowed`.
    uint32_t maxLightsPerCluster = 32;
};

/// Kisi cluster di ruang pandang.
///
/// **Eksponensial di kedalaman, seragam di layar.** Irisan seragam sepanjang
/// kedalaman memberi beberapa meter pertama satu irisan dan ratusan meter
/// terakhir sisanya, padahal lampu berkerumun justru di dekat kamera.
/// Pembagian eksponensial membuat tiap irisan mewakili rasio kedalaman yang
/// sama, sehingga cluster punya bentuk yang kira-kira sebangun di mana pun.
class ClusterGrid {
public:
    void Build(const ClusterGridSettings& settings, float fovYRadians, float aspect, float nearZ,
               float farZ);

    uint32_t TilesX() const { return settings_.tilesX; }
    uint32_t TilesY() const { return settings_.tilesY; }
    uint32_t Slices() const { return settings_.slices; }
    uint32_t ClusterCount() const { return settings_.tilesX * settings_.tilesY * settings_.slices; }

    /// **Indeks ubin adalah indeks ubin LAYAR: baris 0 di atas.** Sama dengan
    /// yang dihitung shader dari `gl_FragCoord`, yang sumbu Y-nya menunjuk ke
    /// bawah. Pembalikan terhadap +Y ruang pandang terjadi sekali, di
    /// `ClusterBounds` — di tempat yang memang tahu soal konvensi proyeksi.
    uint32_t IndexOf(uint32_t x, uint32_t y, uint32_t slice) const {
        return (slice * settings_.tilesY + y) * settings_.tilesX + x;
    }

    /// Irisan kedalaman untuk sebuah jarak pandang. Dijepit ke rentang yang sah.
    uint32_t SliceOf(float viewDepth) const;
    /// Batas dekat dan jauh sebuah irisan, meter.
    Vec2 SliceBounds(uint32_t slice) const;

    /// Kotak sebuah cluster **di ruang pandang**, dengan +Z menjauhi kamera.
    ///
    /// Kotak, bukan enam bidang frustum. Uji bola terhadap bidang menganggap
    /// bola berada di dalam selama ia di sisi dalam setiap bidang — dan itu
    /// benar untuk volume cembung tak terbatas, bukan untuk cluster yang
    /// terbatas: bola yang lewat di dekat sudut memenuhi keenam bidang tanpa
    /// pernah menyentuh cluster-nya. Uji bola-kotak lebih ketat dan lebih murah.
    Aabb ClusterBounds(uint32_t x, uint32_t y, uint32_t slice) const;

private:
    ClusterGridSettings settings_;
    float nearZ_ = 0.1f;
    float farZ_ = 100.0f;
    float tanHalfY_ = 1.0f;
    float tanHalfX_ = 1.0f;
    float sliceScale_ = 1.0f;
    float sliceBias_ = 0.0f;
};

/// Hasil penetapan lampu ke cluster.
///
/// Disimpan sebagai satu larik indeks ditambah rentang per cluster — bentuk yang
/// sama dengan yang nanti diunggah ke GPU. Vektor per cluster akan berarti
/// ribuan alokasi kecil setiap frame.
struct ClusterAssignment {
    struct Range {
        uint32_t offset = 0;
        uint32_t count = 0;
    };
    std::vector<Range> ranges;
    std::vector<uint32_t> indices;
    /// Banyaknya cluster yang daftarnya terpotong `maxLightsPerCluster`.
    uint32_t overflowed = 0;

    std::span<const uint32_t> LightsOf(uint32_t cluster) const {
        const Range& range = ranges[cluster];
        return {indices.data() + range.offset, range.count};
    }
};

/// Menetapkan lampu ke cluster. `view` mengubah ruang dunia ke ruang pandang.
ClusterAssignment AssignLights(const ClusterGrid& grid, const Mat4& view,
                               std::span<const ClusterLight> lights,
                               const ClusterGridSettings& settings);

/// Apakah sebuah bola memotong kotak. Dipisah supaya bisa diuji sendiri.
bool SphereIntersectsAabb(const Vec3& centre, float radius, const Aabb& box);

/// Apakah kerucut spot memotong sebuah bola.
///
/// Uji yang tepat, bukan bola pembatas kerucut: spot sempit yang panjang punya
/// bola pembatas yang jauh lebih besar daripada kerucutnya, dan memakainya akan
/// menyalakan lampu itu di hampir seluruh cluster di sekitarnya.
bool ConeIntersectsSphere(const Vec3& apex, const Vec3& direction, float range,
                          float cosOuterAngle, const Vec3& centre, float radius);

}  // namespace sim::render
