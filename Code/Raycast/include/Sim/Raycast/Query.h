#pragma once

#include "Sim/Raycast/RayScene.h"

#include <cstdint>

/// Tiga pertanyaan yang boleh diajukan kepada sebuah `RayScene`.
///
/// **Tiga, dan bukan satu yang serbaguna.** `Occluded` berhenti pada perpotongan
/// pertama mana pun; `Raycast` harus mencari yang terdekat. Menyatukannya
/// menjadi satu fungsi berbendera berarti setiap uji bayangan membayar
/// penelusuran lengkap untuk jawaban ya/tidak — dan uji bayangan adalah query
/// yang jumlahnya paling banyak.
namespace sim::raycast {

/// Jarak yang berarti "tidak ada batas".
inline constexpr float kUnbounded = 1e30f;

/// Satu perpotongan sinar.
struct RayHit {
    bool hit = false;
    InstanceId instance = InstanceId::Invalid;
    /// Segitiga ke berapa **di dalam geometrinya**, bukan di dalam scene. Ia
    /// yang membuat pemanggil bisa menemukan atribut simpulnya kembali.
    uint32_t primitive = 0;
    /// 64 bit yang diserahkan pemanggil saat `AddInstance`, dikembalikan apa
    /// adanya. Di sinilah nomor entity tinggal.
    uint64_t userData = 0;

    /// Jarak sepanjang arah yang **sudah dinormalkan** oleh query.
    float distance = 0.0f;
    Vec3 position{0.0f};
    /// Normal **geometri** ruang dunia, dari perkalian silang rusuk segitiga.
    ///
    /// Bukan normal simpul yang diinterpolasi: modul ini hanya membaca posisi,
    /// dan menormalkan sesuatu yang tidak dibacanya berarti berbohong. Yang
    /// membutuhkan normal halus menginterpolasinya sendiri dari `barycentric`.
    Vec3 normal{0.0f};
    /// `(u, v)` di dalam segitiganya. Lihat `TriangleHit` di `Sim/Core/Intersect.h`.
    Vec2 barycentric{0.0f};

    explicit operator bool() const { return hit; }
};

/// Perpotongan terdekat sepanjang sinar.
///
/// `direction` tidak perlu bernorma satu — ia dinormalkan di sini, karena arah
/// yang panjangnya bukan satu membuat `distance` berskala lain tanpa ada yang
/// menyebutkannya. Aturan yang sama dengan `whitebox::PickPolygon`.
RayHit Raycast(const RayScene& scene, const Vec3& origin, const Vec3& direction,
               float maxDistance = kUnbounded);

/// Apakah ada **sesuatu** di antara dua titik sepanjang sinar.
///
/// Berhenti pada perpotongan pertama yang ditemukan, bukan yang terdekat.
bool Occluded(const RayScene& scene, const Vec3& origin, const Vec3& direction,
              float maxDistance);

/// Titik terdekat pada permukaan mana pun terhadap sebuah titik.
struct ClosestPoint {
    bool found = false;
    InstanceId instance = InstanceId::Invalid;
    uint32_t primitive = 0;
    uint64_t userData = 0;
    Vec3 position{0.0f};
    float distance = 0.0f;

    explicit operator bool() const { return found; }
};

/// Mencari titik permukaan terdekat di dalam `maxDistance`.
///
/// **Batasnya wajib, dan bukan kenyamanan.** Pencarian titik terdekat tanpa
/// batas awal harus menelusuri setiap instance sebelum bisa memangkas apa pun;
/// sebuah radius membuat tingkat atas membuang hampir semuanya di simpul
/// pertama.
ClosestPoint FindClosestPoint(const RayScene& scene, const Vec3& point, float maxDistance);

}  // namespace sim::raycast
