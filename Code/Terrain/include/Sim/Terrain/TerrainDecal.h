#pragma once

#include "Sim/Assets/MeshData.h"
#include "Sim/Terrain/Terrain.h"

/// Tanda yang diletakkan di atas tanah: jalan setapak, bekas ledakan, garis
/// batas.
///
/// **Digambar sebagai geometri, bukan diproyeksikan saat menggambar.** Decal
/// proyektif sejati menuntut membaca kembali buffer kedalaman dan menghitung
/// ulang posisi dunia tiap piksel — jalur yang belum ada di renderer ini.
/// Yang dikerjakan di sini: menyalin permukaan terrain di dalam jejak decal,
/// mengangkatnya sedikit, dan mewarnainya.
///
/// Itu bukan sekadar yang termurah. Ia punya sifat yang tidak dimiliki decal
/// proyektif: **ia mengikuti lereng dan lubang dengan sendirinya**, karena ia
/// memang permukaan itu — tanpa satu pun uji kemiringan yang harus disetel.
namespace sim::terrain {

struct DecalProjection {
    /// Pusat jejaknya di **ruang lokal terrain**, meter. Komponen `y` diabaikan:
    /// decal selalu mendarat di permukaan, bukan melayang di ketinggian yang
    /// kebetulan disetel seseorang.
    Vec3 center{0.0f};
    /// Setengah ukuran jejaknya sepanjang sumbu decal sendiri, meter.
    Vec2 halfSize{0.5f, 0.5f};
    /// Rotasi jejak terhadap sumbu Y, radian.
    float rotationY = 0.0f;
    /// Jarak angkat dari permukaan, meter.
    ///
    /// **Bukan nol**, dan bukan pula sesuatu yang bisa dilewatkan: dua permukaan
    /// yang sebidang persis menghasilkan z-fighting yang berkedip mengikuti
    /// gerakan kamera — cacat yang paling mudah dikira bug renderer.
    float lift = 0.02f;
    Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
    /// Batas jumlah petak per sumbu.
    ///
    /// Ada batasnya karena jejaknya bisa diskalakan sebesar apa pun oleh gizmo,
    /// dan decal selebar satu kilometer pada jarak sampel seperempat meter
    /// adalah enam belas juta segitiga untuk sebuah noda.
    int maxSteps = 64;
};

/// Permukaan terrain di dalam jejak sebuah decal, terangkat dan berwarna.
///
/// Mesh kosong bila jejaknya di luar peta, seluruhnya berlubang, atau ukurannya
/// nol.
///
/// **Tidak mewujudkan ubin mana pun**, dengan alasan yang sama seperti
/// `BuildTileMesh`: ia hanya membaca.
assets::MeshData BuildDecalMesh(const Terrain& terrain, const DecalProjection& decal);

}  // namespace sim::terrain
