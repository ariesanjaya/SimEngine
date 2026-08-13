#pragma once

#include "Sim/Physics/PhysicsWorld.h"
#include "Sim/Scene/World.h"

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

/// Jembatan antara scene yang disunting dan simulasi yang berjalan.
///
/// **Ada di `Sim::Physics`, bukan di `Sim::EditorFramework`.** Terlihat seperti
/// kode editor — ia membaca komponen dan menulis transform — tetapi
/// `Sim::EditorFramework` menarik `Sim::Render`, dan menaruhnya di sana berarti
/// tidak ada yang bisa mensimulasikan tanpa perangkat grafis. Server dedicated
/// dan `SimHeadless` butuh `Sim::Scene` dan modul ini saja.
///
/// Arahnya juga disengaja: `Sim::Physics` yang bergantung pada `Sim::Scene`,
/// bukan sebaliknya. `Sim::Scene` dipakai importir aset, serialisasi, dan tool
/// yang tidak pernah mensimulasikan apa pun; membalik arahnya membuat semuanya
/// ikut menautkan PhysX.
namespace sim::physics {

/// Ringkasan hasil `Build`, untuk log dan panel.
struct PhysicsSceneStats {
    std::size_t bodies = 0;
    /// Entity ber-`RigidBodyComponent` yang dilewati karena tidak punya
    /// `ColliderComponent`. Dihitung, bukan diabaikan: benda tanpa bentuk jatuh
    /// menembus segalanya, dan angka ini yang menjelaskan sebabnya.
    std::size_t skippedWithoutCollider = 0;
    /// Entity yang bentuknya diskalakan tidak seragam padahal bentuknya tidak
    /// mendukungnya (bola, kapsul).
    std::size_t nonUniformScale = 0;

    std::size_t vehicles = 0;
    /// Entity ber-`VehicleComponent` yang juga membawa `RigidBodyComponent`.
    /// Keduanya bersama berarti satu benda disimulasikan dua kali di tempat yang
    /// sama; yang dipakai adalah kendaraannya, dan yang lain dilaporkan.
    std::size_t vehiclesWithRigidBody = 0;

    /// Collider ber-`Whitebox` yang bentuknya tidak bisa diambil — asetnya
    /// hilang, atau tidak ada yang memasok bentuk pada build ini. Bentuknya
    /// mundur ke kotak, dan angka ini yang menjelaskan mengapa yang ditabrak
    /// tidak seperti yang digambar.
    std::size_t collidersWithoutGeometry = 0;
    /// Collider `Terrain` yang dipasang pada benda dinamis, atau yang kisinya
    /// tidak bisa dibaca. Kisi tinggi tidak bisa bergerak, jadi bendanya
    /// dilewatkan alih-alih dibuat dengan bentuk yang salah.
    std::size_t terrainNotStatic = 0;
    /// Whitebox cekung yang dipasang pada benda dinamis. Cekungannya terisi oleh
    /// selubung cembungnya — satu-satunya bentuk yang bisa bergerak menurut
    /// PhysX — jadi ruangannya tertutup dan lubangnya rata.
    std::size_t concaveDynamic = 0;

    std::size_t joints = 0;
    /// Sendi yang dilewati karena ujungnya tidak bisa dipakai — entity-nya
    /// sendiri bukan benda fisika, atau `connectedBody` menunjuk GUID yang tidak
    /// ada di level ini. Dihitung, bukan diabaikan: sendi yang hilang terlihat
    /// sebagai benda yang jatuh padahal seharusnya tergantung.
    std::size_t skippedJoints = 0;
};

/// Satu simulasi yang dibangun dari sebuah `scene::World`.
///
/// Alurnya: `Build` sekali saat Play ditekan, `Advance` tiap frame, `Clear` saat
/// Stop. Menyunting scene selama simulasi berjalan tidak tercermin — itu
/// disengaja, karena keadaan solver tidak bisa dibangun ulang di tengah jalan
/// tanpa membuang momentum setiap benda.
/// Geometri tabrakan yang datangnya dari aset, bukan dari komponen.
struct ColliderGeometry {
    /// Kisi tinggi, bila entity ini terrain. Yang mengisinya tidak mengisi
    /// `points`, dan sebaliknya.
    HeightFieldDesc heightField;
    std::vector<Vec3> points;
    /// Tiga indeks per segitiga. Kosong berarti hanya selubung cembungnya yang
    /// bisa dipakai.
    std::vector<uint32_t> indices;
    /// True bila bentuknya sudah cembung, sehingga selubungnya persis dan tidak
    /// ada yang perlu diperingatkan.
    bool convex = false;
};

/// Menjawab "bentuk tabrakan entity ini apa?" untuk collider yang bentuknya
/// tersimpan di aset.
///
/// **Modul ini tidak membuka berkas apa pun.** Yang bisa mengubah GUID menjadi
/// bentuk adalah yang memegang basis aset, dan itu editor atau pemuat level —
/// bukan fisika. Menariknya ke sini berarti `Sim::Physics` ikut bergantung pada
/// `Sim::Assets`, `Sim::Whitebox`, dan setiap format yang menyusul.
///
/// Mengembalikan false bila entity itu tidak punya bentuk untuk diberikan.
using ColliderGeometrySource = std::function<bool(scene::Entity, ColliderGeometry&)>;

class PhysicsScene {
public:
    /// Membangun benda dari seluruh entity ber-`RigidBodyComponent`.
    ///
    /// False bila PhysX tidak ada di build ini; `Error()` menyebutnya. Scene-nya
    /// sendiri tidak diubah sama sekali.
    bool Build(scene::World& world, const WorldDesc& desc = {},
               const ColliderGeometrySource& geometry = {});

    void Clear();
    bool IsValid() const { return simulation_.IsValid(); }
    const std::string& Error() const { return simulation_.Error(); }

    /// Menjalankan simulasi untuk waktu nyata yang berlalu, lalu menyalin
    /// hasilnya ke `world`. Mengembalikan banyaknya langkah tetap yang berjalan.
    uint32_t Advance(scene::World& world, float deltaSeconds);

    /// Menjalankan tepat `steps` langkah, mengabaikan akumulator. Untuk test dan
    /// untuk simulasi yang panjangnya ditentukan angka, bukan waktu.
    void Step(scene::World& world, uint32_t steps = 1);

    const PhysicsSceneStats& Stats() const { return stats_; }

    /// Simulasi di baliknya, untuk yang butuh lebih dari sinkronisasi transform.
    PhysicsWorld& Simulation() { return simulation_; }
    const PhysicsWorld& Simulation() const { return simulation_; }

    /// Handle benda milik sebuah entity, atau `BodyHandle::Invalid`.
    ///
    /// Untuk entity kendaraan, ini chassis-nya — roda bukan benda tegar dan
    /// karena itu tidak punya handle.
    BodyHandle BodyOf(scene::Entity entity) const;

    /// Handle kendaraan milik sebuah entity, atau `VehicleHandle::Invalid`.
    VehicleHandle VehicleOf(scene::Entity entity) const;

private:
    /// Mendorong transform entity kinematik ke solver. Arah scene → fisika.
    void PushKinematic(scene::World& world);
    /// Menyalin hasil solver ke transform entity dinamis. Arah fisika → scene.
    void PullDynamic(scene::World& world);

    struct Tracked {
        scene::Entity entity = scene::kNullEntity;
        BodyHandle body = BodyHandle::Invalid;
        /// Skala yang dipakai saat bentuknya dibangun. Disimpan supaya
        /// penulisan balik tidak menghapusnya dari `TransformComponent`.
        Vec3 scale{1.0f};
        bool kinematic = false;
        bool dynamic = false;
    };

    PhysicsWorld simulation_;
    std::vector<Tracked> tracked_;
    std::unordered_map<uint32_t, BodyHandle> byEntity_;
    std::unordered_map<uint32_t, VehicleHandle> vehiclesByEntity_;
    PhysicsSceneStats stats_;
};

}  // namespace sim::physics
