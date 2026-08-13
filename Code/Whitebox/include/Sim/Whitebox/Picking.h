#pragma once

#include "Sim/Whitebox/WhiteboxMesh.h"

/// Memilih sisi dengan sinar.
///
/// **Yang dikembalikan poligon, bukan face.** Pengguna menunjuk sebuah sisi;
/// bahwa sisi itu kebetulan tersusun dari dua segitiga adalah urusan mesin, dan
/// mengembalikan segitiga berarti mendorong "sisi" hanya menggerakkan
/// separuhnya.
namespace sim::whitebox {

struct PolygonHit {
    bool hit = false;
    PolygonHandle polygon = PolygonHandle::Invalid;
    /// Face yang benar-benar tertembus. Dipakai penyunting untuk menyorot
    /// segitiga saat men-debug, bukan untuk memilih.
    FaceHandle face = FaceHandle::Invalid;
    float distance = 0.0f;
    Vec3 position{0.0f};

    explicit operator bool() const { return hit; }
};

/// Sinar terhadap whitebox, di **ruang lokal meshnya**.
///
/// Ruang lokal, bukan dunia: yang memegang transform entity adalah pemanggil,
/// dan memindahkan sinarnya ke ruang lokal jauh lebih murah daripada memindahkan
/// setiap segitiga ke ruang dunia.
///
/// `direction` tidak perlu bernorma satu — ia dinormalkan di sini, karena arah
/// yang panjangnya bukan satu membuat `distance` berskala lain tanpa ada yang
/// menyebutkannya.
PolygonHit PickPolygon(const WhiteboxMesh& box, const Vec3& origin, const Vec3& direction,
                       float maxDistance = 1e30f);

}  // namespace sim::whitebox
