#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Physics/PhysicsScene.h"
#include "Sim/Scene/Components.h"
#include "Sim/Scene/Serialization.h"
#include "Sim/Scene/World.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace sim;
using namespace sim::physics;

namespace {

/// Lantai statik tak hingga di y = 0.
scene::Entity AddGround(scene::World& world) {
    const scene::Entity entity = world.Create("Ground");
    auto& body = world.Add<scene::RigidBodyComponent>(entity);
    body.kind = scene::RigidBodyKind::Static;
    auto& collider = world.Add<scene::ColliderComponent>(entity);
    collider.shape = scene::ColliderShape::Plane;
    // Bidang PhysX menghadap +X; diputar +90° terhadap Z supaya normalnya ke atas.
    auto& transform = *world.TryGet<scene::TransformComponent>(entity);
    const float halfAngle = 0.25f * 3.14159265f;
    transform.rotation = Quat(std::cos(halfAngle), 0.0f, 0.0f, std::sin(halfAngle));
    world.MarkTransformDirty(entity);
    return entity;
}

scene::Entity AddBall(scene::World& world, const Vec3& position, float radius = 0.5f) {
    const scene::Entity entity = world.Create("Ball");
    world.Add<scene::RigidBodyComponent>(entity);
    auto& collider = world.Add<scene::ColliderComponent>(entity);
    collider.shape = scene::ColliderShape::Sphere;
    collider.radius = radius;
    world.TryGet<scene::TransformComponent>(entity)->position = position;
    world.MarkTransformDirty(entity);
    return entity;
}

const Vec3& PositionOf(scene::World& world, scene::Entity entity) {
    return world.TryGet<scene::TransformComponent>(entity)->position;
}

}  // namespace

TEST_CASE("tanpa PhysX, membangun gagal dan scene tidak tersentuh") {
    scene::World world;
    AddGround(world);
    const scene::Entity ball = AddBall(world, Vec3(0.0f, 5.0f, 0.0f));

    PhysicsScene physics;
    const bool built = physics.Build(world);

    if (Available()) {
        REQUIRE(built);
        CHECK(physics.Stats().bodies == 2);
        return;
    }

    CHECK_FALSE(built);
    CHECK(physics.Error().find("PhysX") != std::string::npos);
    // **Yang penting: scene-nya utuh.** Membangun simulasi yang gagal tidak
    // boleh meninggalkan level dalam keadaan setengah berubah — pengguna yang
    // menekan Play di build tanpa PhysX tetap menemukan pekerjaannya seperti
    // semula.
    CHECK(PositionOf(world, ball).y == doctest::Approx(5.0f));
    physics.Step(world, 60);
    CHECK(PositionOf(world, ball).y == doctest::Approx(5.0f));
    CHECK(physics.Advance(world, 1.0f) == 0);
}

TEST_CASE("bola jatuh lewat komponen dan berhenti di y = r") {
    if (!Available()) {
        return;
    }
    // Kriteria terima P1, kali ini menempuh seluruh jalurnya: komponen scene →
    // deskriptor → solver → kembali ke `TransformComponent`.
    scene::World world;
    AddGround(world);
    const float radius = 0.5f;
    const scene::Entity ball = AddBall(world, Vec3(0.0f, 5.0f, 0.0f), radius);

    PhysicsScene physics;
    REQUIRE(physics.Build(world));
    CHECK(physics.Stats().bodies == 2);
    CHECK(physics.Stats().skippedWithoutCollider == 0);

    physics.Step(world, 180);

    INFO("berhenti di y = " << PositionOf(world, ball).y);
    CHECK(PositionOf(world, ball).y == doctest::Approx(radius).epsilon(0.05));
    CHECK(PositionOf(world, ball).x == doctest::Approx(0.0f).epsilon(0.01));
}

TEST_CASE("skala entity ikut menentukan ukuran bentuk tabrakan") {
    if (!Available()) {
        return;
    }
    // **Bentuk tabrakan yang mengabaikan skala adalah cacat yang tidak terlihat
    // sebagai galat**: bendanya digambar besar tapi berhenti seolah kecil, dan
    // yang terlihat hanya benda yang tenggelam ke dalam lantai.
    scene::World world;
    AddGround(world);
    const scene::Entity ball = AddBall(world, Vec3(0.0f, 6.0f, 0.0f), 0.5f);
    world.TryGet<scene::TransformComponent>(ball)->scale = Vec3(3.0f);
    world.MarkTransformDirty(ball);

    PhysicsScene physics;
    REQUIRE(physics.Build(world));
    physics.Step(world, 240);

    // Jari-jari 0,5 diskalakan 3× menjadi 1,5 — angkanya diketahui sebelum
    // simulasinya dijalankan, bukan dibaca dari hasilnya.
    INFO("berhenti di y = " << PositionOf(world, ball).y);
    CHECK(PositionOf(world, ball).y == doctest::Approx(1.5f).epsilon(0.05));
}

TEST_CASE("entity berinduk ditulis balik di ruang induknya") {
    if (!Available()) {
        return;
    }
    // **Cacat yang hanya muncul di level sungguhan.** Entity fisika hampir selalu
    // ditata di dalam grup, dan menuliskan posisi dunia apa adanya ke transform
    // lokal membuat benda melompat sejauh transform induknya pada frame pertama.
    scene::World world;
    AddGround(world);

    const scene::Entity group = world.Create("Group");
    world.TryGet<scene::TransformComponent>(group)->position = Vec3(10.0f, 0.0f, -4.0f);
    world.MarkTransformDirty(group);

    const scene::Entity ball = world.Create("Ball", group);
    world.Add<scene::RigidBodyComponent>(ball);
    auto& collider = world.Add<scene::ColliderComponent>(ball);
    collider.shape = scene::ColliderShape::Sphere;
    collider.radius = 0.5f;
    // Lokal (0, 5, 0) di dalam grup, jadi dunia (10, 5, -4).
    world.TryGet<scene::TransformComponent>(ball)->position = Vec3(0.0f, 5.0f, 0.0f);
    world.MarkTransformDirty(ball);

    PhysicsScene physics;
    REQUIRE(physics.Build(world));

    // Satu langkah saja sudah cukup untuk memperlihatkan lompatannya: tanpa
    // konversi, x lokal akan melonjak dari 0 ke 10.
    physics.Step(world, 1);
    CHECK(PositionOf(world, ball).x == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(PositionOf(world, ball).z == doctest::Approx(0.0f).epsilon(0.001));

    physics.Step(world, 240);
    // Lantai ada di dunia y = 0, dan induknya tidak menggeser y — jadi bola
    // berhenti di lokal y = 0,5 sama seperti tanpa induk.
    INFO("lokal y = " << PositionOf(world, ball).y);
    CHECK(PositionOf(world, ball).y == doctest::Approx(0.5f).epsilon(0.05));
    CHECK(PositionOf(world, ball).x == doctest::Approx(0.0f).epsilon(0.01));

    // Dan posisi dunianya memang tetap di bawah grupnya.
    const Mat4 ballWorld = world.WorldMatrix(ball);
    CHECK(ballWorld[3].x == doctest::Approx(10.0f).epsilon(0.01));
    CHECK(ballWorld[3].z == doctest::Approx(-4.0f).epsilon(0.01));
}

TEST_CASE("kinematik digerakkan transform, dinamis didorong olehnya") {
    if (!Available()) {
        return;
    }
    scene::World world;
    const scene::Entity platform = world.Create("Platform");
    auto& body = world.Add<scene::RigidBodyComponent>(platform);
    body.kind = scene::RigidBodyKind::Kinematic;
    auto& collider = world.Add<scene::ColliderComponent>(platform);
    collider.halfExtents = Vec3(2.0f, 0.25f, 2.0f);
    world.TryGet<scene::TransformComponent>(platform)->position = Vec3(0.0f, 0.0f, 0.0f);
    world.MarkTransformDirty(platform);

    const scene::Entity ball = AddBall(world, Vec3(0.0f, 1.5f, 0.0f));

    PhysicsScene physics;
    REQUIRE(physics.Build(world));

    // Bola mendarat di atas platform: 0,25 (setengah tebal) + 0,5 (jari-jari).
    physics.Step(world, 180);
    INFO("bola di y = " << PositionOf(world, ball).y);
    CHECK(PositionOf(world, ball).y == doctest::Approx(0.75f).epsilon(0.05));

    // Platform dinaikkan lewat transform-nya, sedikit demi sedikit supaya
    // kontaknya terjaga — inilah yang membedakan kinematik dari teleport.
    for (int i = 0; i < 60; ++i) {
        world.TryGet<scene::TransformComponent>(platform)->position.y += 1.0f / 60.0f;
        world.MarkTransformDirty(platform);
        physics.Step(world, 1);
    }

    // Platform naik satu meter dan gravitasi tidak menyentuhnya sama sekali.
    CHECK(PositionOf(world, platform).y == doctest::Approx(1.0f).epsilon(0.01));
    // Dan bola ikut terangkat, bukan ditembus.
    INFO("bola di y = " << PositionOf(world, ball).y);
    CHECK(PositionOf(world, ball).y > 1.5f);
}

TEST_CASE("benda statis tidak pernah bergerak") {
    if (!Available()) {
        return;
    }
    scene::World world;
    const scene::Entity wall = world.Create("Wall");
    auto& body = world.Add<scene::RigidBodyComponent>(wall);
    body.kind = scene::RigidBodyKind::Static;
    world.Add<scene::ColliderComponent>(wall);
    world.TryGet<scene::TransformComponent>(wall)->position = Vec3(3.0f, 7.0f, -2.0f);
    world.MarkTransformDirty(wall);

    PhysicsScene physics;
    REQUIRE(physics.Build(world));
    physics.Step(world, 240);

    // Gravitasi tidak berlaku, dan tidak ada penulisan balik yang menggesernya.
    const Vec3& position = PositionOf(world, wall);
    CHECK(position.x == doctest::Approx(3.0f));
    CHECK(position.y == doctest::Approx(7.0f));
    CHECK(position.z == doctest::Approx(-2.0f));
}

TEST_CASE("rigid body tanpa collider dilewati dan dihitung") {
    if (!Available()) {
        return;
    }
    // Diam-diam melewatinya membuat benda jatuh menembus segalanya tanpa satu
    // pun petunjuk. Dihitung supaya editor bisa menyebutkan angkanya.
    scene::World world;
    AddGround(world);
    const scene::Entity orphan = world.Create("No Collider");
    world.Add<scene::RigidBodyComponent>(orphan);
    world.TryGet<scene::TransformComponent>(orphan)->position = Vec3(0.0f, 5.0f, 0.0f);
    world.MarkTransformDirty(orphan);

    PhysicsScene physics;
    REQUIRE(physics.Build(world));
    CHECK(physics.Stats().skippedWithoutCollider == 1);
    CHECK(physics.Stats().bodies == 1);
    CHECK(physics.BodyOf(orphan) == BodyHandle::Invalid);

    physics.Step(world, 60);
    // Tidak disimulasikan berarti benar-benar diam, bukan jatuh perlahan.
    CHECK(PositionOf(world, orphan).y == doctest::Approx(5.0f));
}

TEST_CASE("membangun ulang dari scene yang sama memberi hasil yang sama") {
    if (!Available()) {
        return;
    }
    // Determinisme lewat jalur komponen, termasuk urutan benda dimasukkan —
    // yang ikut menentukan hasil solver dan karena itu ditelusuri dari hierarki,
    // bukan dari urutan kolam komponen entt.
    const auto run = [] {
        scene::World world;
        AddGround(world);
        const scene::Entity group = world.Create("Stack");
        std::vector<scene::Entity> boxes;
        for (int i = 0; i < 6; ++i) {
            const scene::Entity box = world.Create("Box", group);
            world.Add<scene::RigidBodyComponent>(box);
            auto& collider = world.Add<scene::ColliderComponent>(box);
            collider.halfExtents = Vec3(0.25f);
            world.TryGet<scene::TransformComponent>(box)->position =
                Vec3(static_cast<float>(i) * 0.01f, 0.3f + static_cast<float>(i) * 0.6f, 0.0f);
            world.MarkTransformDirty(box);
            boxes.push_back(box);
        }

        PhysicsScene physics;
        physics.Build(world);
        physics.Step(world, 240);

        std::vector<Vec3> result;
        for (const scene::Entity box : boxes) {
            result.push_back(PositionOf(world, box));
        }
        return result;
    };

    const std::vector<Vec3> first = run();
    const std::vector<Vec3> second = run();
    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        INFO("balok " << i);
        CHECK(first[i].x == doctest::Approx(second[i].x));
        CHECK(first[i].y == doctest::Approx(second[i].y));
        CHECK(first[i].z == doctest::Approx(second[i].z));
    }
}

TEST_CASE("prefab Box dan Sphere memang jatuh ke atas prefab Ground") {
    // **Template yang tidak bisa dijatuhkan bukan template fisika.** Diuji lewat
    // berkasnya, bukan lewat entity yang dibangun test: yang ditempatkan panel
    // Prefab adalah berkas itu, dan komponen yang salah nama atau enum yang
    // salah eja hanya ketahuan di sini.
    const std::filesystem::path prefabs =
        std::filesystem::path(SIM_BUILTIN_DIR) / "Prefabs";

    const auto load = [&](const std::filesystem::path& path, scene::World& world) {
        std::ifstream stream(path);
        REQUIRE_MESSAGE(stream, "tidak bisa membuka ", path.string());
        const std::string text((std::istreambuf_iterator<char>(stream)),
                               std::istreambuf_iterator<char>());
        std::string rootGuid;
        REQUIRE(scene::RestoreSubtree(world, scene::RemapGuids(text, &rootGuid), Uuid{}));
        const scene::Entity entity = world.FindByGuid(Uuid::Parse(rootGuid));
        REQUIRE(entity != scene::kNullEntity);
        return entity;
    };

    scene::World world;
    const scene::Entity ground = load(prefabs / "Environment" / "Ground.simprefab", world);
    const scene::Entity box = load(prefabs / "Physics" / "Physics Box.simprefab", world);
    const scene::Entity sphere = load(prefabs / "Physics" / "Physics Sphere.simprefab", world);

    // Ketiganya harus benar-benar membawa komponen fisikanya.
    REQUIRE(world.TryGet<scene::RigidBodyComponent>(ground) != nullptr);
    REQUIRE(world.TryGet<scene::ColliderComponent>(box) != nullptr);
    REQUIRE(world.TryGet<scene::ColliderComponent>(sphere) != nullptr);
    CHECK(world.TryGet<scene::RigidBodyComponent>(box)->kind == scene::RigidBodyKind::Dynamic);
    CHECK(world.TryGet<scene::ColliderComponent>(sphere)->shape == scene::ColliderShape::Sphere);

    // Dipisahkan supaya keduanya tidak saling menimpa saat jatuh.
    world.TryGet<scene::TransformComponent>(box)->position = Vec3(-1.5f, 3.0f, 0.0f);
    world.TryGet<scene::TransformComponent>(sphere)->position = Vec3(1.5f, 3.0f, 0.0f);
    world.MarkTransformDirty(box);
    world.MarkTransformDirty(sphere);

    PhysicsScene physics;
    if (!Available()) {
        CHECK_FALSE(physics.Build(world));
        return;
    }

    REQUIRE(physics.Build(world));
    CHECK(physics.Stats().bodies == 3);
    CHECK(physics.Stats().skippedWithoutCollider == 0);

    physics.Step(world, 300);

    // Permukaan Ground tepat di y = 0, jadi keduanya berhenti di setengah
    // tingginya — angka yang diketahui sebelum simulasinya dijalankan.
    INFO("box y = " << PositionOf(world, box).y
                    << ", sphere y = " << PositionOf(world, sphere).y);
    CHECK(PositionOf(world, box).y == doctest::Approx(0.5f).epsilon(0.06));
    CHECK(PositionOf(world, sphere).y == doctest::Approx(0.5f).epsilon(0.06));
}

namespace {

/// Bola dinamis yang tidak boleh tertidur — bandul yang dianggap diam berhenti
/// berayun, dan itu terbaca sebagai sendi yang macet.
scene::Entity AddBob(scene::World& world, const char* name, const Vec3& position) {
    const scene::Entity entity = world.Create(name);
    auto& body = world.Add<scene::RigidBodyComponent>(entity);
    body.allowSleeping = false;
    auto& collider = world.Add<scene::ColliderComponent>(entity);
    collider.shape = scene::ColliderShape::Sphere;
    collider.radius = 0.2f;
    world.TryGet<scene::TransformComponent>(entity)->position = position;
    world.MarkTransformDirty(entity);
    return entity;
}

}  // namespace

TEST_CASE("JointComponent menggantungkan entity ke dunia") {
    if (!Available()) {
        return;
    }
    scene::World world;
    const scene::Entity bob = AddBob(world, "Bob", Vec3(2.0f, 0.0f, 0.0f));

    auto& joint = world.Add<scene::JointComponent>(bob);
    joint.type = scene::JointType::Revolute;
    // connectedBody dibiarkan kosong: tergantung pada dunia.
    joint.anchor = Vec3(-2.0f, 0.0f, 0.0f);
    const float halfAngle = 0.25f * 3.14159265f;
    joint.frame = Quat(std::cos(halfAngle), 0.0f, std::sin(halfAngle), 0.0f);

    PhysicsScene physics;
    REQUIRE(physics.Build(world));
    CHECK(physics.Stats().joints == 1);
    CHECK(physics.Stats().skippedJoints == 0);

    physics.Step(world, 400);

    // Berayun turun, tetapi tetap sejauh 2 m dari porosnya di titik asal — itu
    // yang membedakan tergantung dari sekadar jatuh.
    const Vec3& position = PositionOf(world, bob);
    const float radius = std::sqrt(position.x * position.x + position.y * position.y);
    INFO("di (" << position.x << ", " << position.y << ", " << position.z << ")");
    CHECK(radius == doctest::Approx(2.0f).epsilon(0.02));
    CHECK(position.y < -0.5f);
    CHECK(std::abs(position.z) < 0.01f);
}

TEST_CASE("JointComponent menyendikan dua entity lewat GUID") {
    if (!Available()) {
        return;
    }
    scene::World world;
    const scene::Entity anchor = world.Create("Anchor");
    {
        auto& body = world.Add<scene::RigidBodyComponent>(anchor);
        body.kind = scene::RigidBodyKind::Static;
        world.Add<scene::ColliderComponent>(anchor);
        world.TryGet<scene::TransformComponent>(anchor)->position = Vec3(0.0f, 5.0f, 0.0f);
        world.MarkTransformDirty(anchor);
    }

    const scene::Entity bob = AddBob(world, "Bob", Vec3(0.0f, 3.0f, 0.0f));
    auto& joint = world.Add<scene::JointComponent>(bob);
    joint.type = scene::JointType::Fixed;
    joint.connectedBody = world.GuidOf(anchor);

    PhysicsScene physics;
    REQUIRE(physics.Build(world));
    CHECK(physics.Stats().joints == 1);

    physics.Step(world, 300);

    // Dipaku ke benda statis: gravitasi tidak boleh memindahkannya sama sekali.
    const Vec3& position = PositionOf(world, bob);
    INFO("di y = " << position.y);
    CHECK(position.y == doctest::Approx(3.0f).epsilon(0.01));
}

TEST_CASE("sendi yang ujungnya tidak bisa dipakai dihitung, bukan diabaikan") {
    if (!Available()) {
        return;
    }
    scene::World world;

    SUBCASE("menunjuk GUID yang tidak ada di level ini") {
        const scene::Entity bob = AddBob(world, "Bob", Vec3(0.0f, 3.0f, 0.0f));
        auto& joint = world.Add<scene::JointComponent>(bob);
        joint.connectedBody = Uuid::Generate();

        PhysicsScene physics;
        REQUIRE(physics.Build(world));
        CHECK(physics.Stats().joints == 0);
        CHECK(physics.Stats().skippedJoints == 1);

        // Dan bendanya tetap jatuh bebas, bukan diam-diam tergantung ke dunia —
        // menebak "mungkin maksudnya dunia" akan menyembunyikan salah rujuk.
        physics.Step(world, 120);
        CHECK(PositionOf(world, bob).y < 2.0f);
    }

    SUBCASE("entity-nya sendiri bukan benda fisika") {
        const scene::Entity marker = world.Create("Marker");
        world.Add<scene::JointComponent>(marker);

        PhysicsScene physics;
        REQUIRE(physics.Build(world));
        CHECK(physics.Stats().joints == 0);
        CHECK(physics.Stats().skippedJoints == 1);
    }
}

TEST_CASE("menghapus entity yang dipegang sendi tidak meninggalkan aktor menggantung") {
    if (!Available()) {
        return;
    }
    // Kriteria terima P3 lewat jalur scene, diuji dengan benar-benar menghapus
    // entity. Kerusakannya tidak muncul saat penghapusan melainkan di langkah
    // berikutnya, jadi uji ini melangkah panjang sesudahnya.
    scene::World world;
    const scene::Entity anchor = world.Create("Anchor");
    {
        auto& body = world.Add<scene::RigidBodyComponent>(anchor);
        body.kind = scene::RigidBodyKind::Static;
        world.Add<scene::ColliderComponent>(anchor);
        world.TryGet<scene::TransformComponent>(anchor)->position = Vec3(0.0f, 5.0f, 0.0f);
        world.MarkTransformDirty(anchor);
    }
    const scene::Entity bob = AddBob(world, "Bob", Vec3(0.0f, 3.0f, 0.0f));
    auto& joint = world.Add<scene::JointComponent>(bob);
    joint.connectedBody = world.GuidOf(anchor);

    PhysicsScene physics;
    REQUIRE(physics.Build(world));
    REQUIRE(physics.Stats().joints == 1);
    physics.Step(world, 30);

    // Poros dihapus dari simulasi, seperti yang dilakukan skrip saat sesuatu
    // dihancurkan di tengah permainan.
    const BodyHandle anchorBody = physics.BodyOf(anchor);
    REQUIRE(anchorBody != BodyHandle::Invalid);
    physics.Simulation().RemoveBody(anchorBody);
    CHECK(physics.Simulation().JointCount() == 0);

    // Melangkah panjang: di sinilah aktor menggantung akan terlihat.
    physics.Step(world, 300);

    // Dan bebannya kini jatuh, karena yang menahannya sudah tidak ada.
    INFO("beban di y = " << PositionOf(world, bob).y);
    CHECK(PositionOf(world, bob).y < 0.0f);
}

TEST_CASE("VehicleComponent membangun kendaraan yang berdiri di suspensinya") {
    if (!Available()) {
        return;
    }
    scene::World world;
    AddGround(world);

    const scene::Entity car = world.Create("Car");
    world.Add<scene::VehicleComponent>(car);
    world.TryGet<scene::TransformComponent>(car)->position = Vec3(0.0f, 1.5f, 0.0f);
    world.MarkTransformDirty(car);

    PhysicsScene physics;
    REQUIRE(physics.Build(world));
    CHECK(physics.Stats().vehicles == 1);
    CHECK(physics.Stats().vehiclesWithRigidBody == 0);
    REQUIRE(physics.VehicleOf(car) != VehicleHandle::Invalid);
    // Chassis-nya terdaftar sebagai benda biasa, jadi sendi dan scene query bisa
    // menyebutnya. Roda tidak — roda bukan benda tegar.
    CHECK(physics.BodyOf(car) != BodyHandle::Invalid);

    physics.Step(world, 180);

    // Transform entity ikut ditulis balik seperti benda dinamis lain.
    const float restY = PositionOf(world, car).y;
    INFO("chassis berhenti di y = " << restY);
    CHECK(restY > 0.9f);
    CHECK(restY < 1.4f);

    VehicleState state;
    REQUIRE(physics.Simulation().ReadVehicleState(physics.VehicleOf(car), state));
    REQUIRE(state.wheels.size() == 4);
    for (const VehicleWheelState& wheel : state.wheels) {
        CHECK(wheel.onGround);
    }
}

TEST_CASE("Vehicle dan Rigid Body bersama dilaporkan, bukan didiamkan") {
    if (!Available()) {
        return;
    }
    // Dua benda tegar di tempat yang sama saling mendorong dengan cara yang tidak
    // bisa dijelaskan siapa pun. Kendaraannya tetap dibangun — itu yang jelas
    // diminta entity ini — dan yang lain disebutkan.
    scene::World world;
    AddGround(world);

    const scene::Entity car = world.Create("Car");
    world.Add<scene::VehicleComponent>(car);
    world.Add<scene::RigidBodyComponent>(car);
    world.Add<scene::ColliderComponent>(car);
    world.TryGet<scene::TransformComponent>(car)->position = Vec3(0.0f, 1.5f, 0.0f);
    world.MarkTransformDirty(car);

    PhysicsScene physics;
    REQUIRE(physics.Build(world));
    CHECK(physics.Stats().vehicles == 1);
    CHECK(physics.Stats().vehiclesWithRigidBody == 1);
}

TEST_CASE("prefab Vehicle memang bisa dikemudikan") {
    if (!Available()) {
        return;
    }
    // Diuji lewat berkasnya: nama komponen dan ejaan enum yang salah hanya
    // ketahuan di sini, bukan di entity yang dibangun test.
    const std::filesystem::path path = std::filesystem::path(SIM_BUILTIN_DIR) / "Prefabs" /
                                       "Physics" / "Vehicle.simprefab";
    std::ifstream stream(path);
    REQUIRE_MESSAGE(stream, "tidak bisa membuka ", path.string());
    const std::string text((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());

    scene::World world;
    AddGround(world);
    std::string rootGuid;
    REQUIRE(scene::RestoreSubtree(world, scene::RemapGuids(text, &rootGuid), Uuid{}));
    const scene::Entity car = world.FindByGuid(Uuid::Parse(rootGuid));
    REQUIRE(car != scene::kNullEntity);

    const auto* component = world.TryGet<scene::VehicleComponent>(car);
    REQUIRE(component != nullptr);
    CHECK(component->drive == scene::VehicleDriveKind::RearWheel);
    CHECK(component->wheelbase == doctest::Approx(3.0f));

    PhysicsScene physics;
    REQUIRE(physics.Build(world));
    REQUIRE(physics.Stats().vehicles == 1);
    const VehicleHandle handle = physics.VehicleOf(car);
    REQUIRE(handle != VehicleHandle::Invalid);

    physics.Step(world, 120);
    const Vec3 start = PositionOf(world, car);

    VehicleInput input;
    input.throttle = 1.0f;
    REQUIRE(physics.Simulation().SetVehicleInput(handle, input));
    physics.Step(world, 240);

    const Vec3 moved = PositionOf(world, car);
    const float travelled = glm::length(Vec3(moved.x - start.x, 0.0f, moved.z - start.z));
    INFO("menempuh " << travelled << " m dengan gas penuh");
    CHECK(travelled > 5.0f);
}

TEST_CASE("silinder berdiri di atas tutupnya pada ketinggian yang dihitung") {
    if (!Available()) {
        return;
    }
    // **PhysX tidak punya silinder primitif**, jadi ia dimasak menjadi convex
    // hull. Yang diuji di sini adalah bahwa hasil masakannya benar-benar
    // setinggi yang diminta — silinder yang dimasak salah tidak terlihat sebagai
    // galat melainkan sebagai benda yang tenggelam atau melayang.
    scene::World world;
    AddGround(world);

    const scene::Entity drum = world.Create("Drum");
    world.Add<scene::RigidBodyComponent>(drum);
    auto& collider = world.Add<scene::ColliderComponent>(drum);
    collider.shape = scene::ColliderShape::Cylinder;
    collider.radius = 0.5f;
    collider.halfHeight = 0.4f;
    // Sumbu silinder adalah +X lokal; diputar supaya berdiri tegak.
    const float halfAngle = 0.25f * 3.14159265f;
    auto& transform = *world.TryGet<scene::TransformComponent>(drum);
    transform.position = Vec3(0.0f, 3.0f, 0.0f);
    transform.rotation = Quat(std::cos(halfAngle), 0.0f, 0.0f, std::sin(halfAngle));
    world.MarkTransformDirty(drum);

    PhysicsScene physics;
    REQUIRE(physics.Build(world));
    CHECK(physics.Stats().bodies == 2);

    physics.Step(world, 300);

    // Berdiri di atas tutup datarnya: pusatnya tepat setengah-tinggi di atas
    // lantai. Angka ini diketahui sebelum simulasinya dijalankan — dan berbeda
    // dari kapsul, yang akan berhenti di 0,4 + 0,5 karena tudungnya membulat.
    INFO("berhenti di y = " << PositionOf(world, drum).y);
    CHECK(PositionOf(world, drum).y == doctest::Approx(0.4f).epsilon(0.05));
}

TEST_CASE("prefab Physics Cylinder jatuh ke atas Ground") {
    if (!Available()) {
        return;
    }
    const std::filesystem::path prefabs = std::filesystem::path(SIM_BUILTIN_DIR) / "Prefabs";
    const auto load = [&](const std::filesystem::path& path, scene::World& world) {
        std::ifstream stream(path);
        REQUIRE_MESSAGE(stream, "tidak bisa membuka ", path.string());
        const std::string text((std::istreambuf_iterator<char>(stream)),
                               std::istreambuf_iterator<char>());
        std::string rootGuid;
        REQUIRE(scene::RestoreSubtree(world, scene::RemapGuids(text, &rootGuid), Uuid{}));
        const scene::Entity entity = world.FindByGuid(Uuid::Parse(rootGuid));
        REQUIRE(entity != scene::kNullEntity);
        return entity;
    };

    scene::World world;
    load(prefabs / "Environment" / "Ground.simprefab", world);
    const scene::Entity cylinder =
        load(prefabs / "Physics" / "Physics Cylinder.simprefab", world);

    const auto* collider = world.TryGet<scene::ColliderComponent>(cylinder);
    REQUIRE(collider != nullptr);
    CHECK(collider->shape == scene::ColliderShape::Cylinder);

    PhysicsScene physics;
    REQUIRE(physics.Build(world));
    CHECK(physics.Stats().bodies == 2);
    CHECK(physics.Stats().skippedWithoutCollider == 0);

    physics.Step(world, 300);

    // Sumbunya +X dan prefabnya tidak diputar, jadi ia berbaring di sisinya:
    // beristirahat pada apotema segi-32, sekitar 0,5% di bawah jari-jari 0,5.
    const float rest = PositionOf(world, cylinder).y;
    INFO("berhenti di y = " << rest);
    CHECK(rest > 0.45f);
    CHECK(rest < 0.51f);
}

// ============================================================================
// W6 — collider whitebox
// ============================================================================

namespace {

/// Bentuk kubus satuan sebagai geometri collider, ditulis di sini alih-alih
/// diambil dari `Sim::Whitebox`.
///
/// **Uji ini menguji jembatannya, bukan pembangun bentuknya.** Mengambilnya dari
/// modul yang sama berarti satu bug pemetaan simpul lolos dari keduanya, karena
/// keduanya salah dengan cara yang sama.
ColliderGeometry UnitCubeGeometry() {
    ColliderGeometry shape;
    for (int i = 0; i < 8; ++i) {
        shape.points.push_back(Vec3((i & 1) != 0 ? 0.5f : -0.5f, (i & 2) != 0 ? 0.5f : -0.5f,
                                    (i & 4) != 0 ? 0.5f : -0.5f));
    }
    // Enam sisi, berlawanan arah jarum jam dilihat dari luar. Indeksnya:
    // bit 0 = +x, bit 1 = +y, bit 2 = +z.
    const uint32_t faces[6][4] = {
        {0, 2, 3, 1},  // -z
        {4, 5, 7, 6},  // +z
        {0, 4, 6, 2},  // -x
        {1, 3, 7, 5},  // +x
        {0, 1, 5, 4},  // -y
        {2, 6, 7, 3},  // +y
    };
    for (const auto& face : faces) {
        shape.indices.insert(shape.indices.end(),
                             {face[0], face[1], face[2], face[0], face[2], face[3]});
    }
    shape.convex = true;
    return shape;
}

scene::Entity AddWhiteboxBody(scene::World& world, scene::RigidBodyKind kind,
                              const Vec3& position, const Vec3& scale = Vec3(1.0f)) {
    const scene::Entity entity = world.Create("Blok");
    world.Add<scene::RigidBodyComponent>(entity).kind = kind;
    world.Add<scene::ColliderComponent>(entity).shape = scene::ColliderShape::Whitebox;
    auto& transform = *world.TryGet<scene::TransformComponent>(entity);
    transform.position = position;
    transform.scale = scale;
    world.MarkTransformDirty(entity);
    return entity;
}

}  // namespace

TEST_CASE("W6: whitebox statik memakai segitiganya, sehingga palungnya tetap berlubang") {
    if (!Available()) {
        return;
    }
    // **Inilah yang membedakan mesh segitiga dari selubung cembung**, dan tanpa
    // uji ini kedua pilihan itu tak terbedakan: seluruh uji lain memakai bentuk
    // cembung, yang selubungnya sama persis dengan segitiganya.
    //
    // Sebuah palung: lantai di y = 1 selebar x ∈ [−1, 1], dengan bibir di y = 2
    // di kedua sisinya. Selubung cembungnya menutup palung itu rata di y = 2.
    const auto addQuad = [](ColliderGeometry& shape, const Vec3& a, const Vec3& b, const Vec3& c,
                            const Vec3& d) {
        const uint32_t base = static_cast<uint32_t>(shape.points.size());
        shape.points.insert(shape.points.end(), {a, b, c, d});
        shape.indices.insert(shape.indices.end(),
                             {base, base + 1, base + 2, base, base + 2, base + 3});
    };
    ColliderGeometry trough;
    // Lantai palung.
    addQuad(trough, Vec3(-1.0f, 1.0f, 2.0f), Vec3(1.0f, 1.0f, 2.0f), Vec3(1.0f, 1.0f, -2.0f),
            Vec3(-1.0f, 1.0f, -2.0f));
    // Bibir kiri dan kanan.
    addQuad(trough, Vec3(-2.0f, 2.0f, 2.0f), Vec3(-1.0f, 2.0f, 2.0f), Vec3(-1.0f, 2.0f, -2.0f),
            Vec3(-2.0f, 2.0f, -2.0f));
    addQuad(trough, Vec3(1.0f, 2.0f, 2.0f), Vec3(2.0f, 2.0f, 2.0f), Vec3(2.0f, 2.0f, -2.0f),
            Vec3(1.0f, 2.0f, -2.0f));
    // Dinding dalam.
    addQuad(trough, Vec3(-1.0f, 1.0f, -2.0f), Vec3(-1.0f, 2.0f, -2.0f), Vec3(-1.0f, 2.0f, 2.0f),
            Vec3(-1.0f, 1.0f, 2.0f));
    addQuad(trough, Vec3(1.0f, 1.0f, 2.0f), Vec3(1.0f, 2.0f, 2.0f), Vec3(1.0f, 2.0f, -2.0f),
            Vec3(1.0f, 1.0f, -2.0f));
    trough.convex = false;

    scene::World world;
    AddWhiteboxBody(world, scene::RigidBodyKind::Static, Vec3(0.0f));
    const scene::Entity ball = AddBall(world, Vec3(0.0f, 6.0f, 0.0f), 0.25f);

    PhysicsScene physics;
    REQUIRE(physics.Build(world, {}, [&](scene::Entity, ColliderGeometry& out) {
        out = trough;
        return true;
    }));
    // Yang statik tidak diperingatkan cekung: segitiganya dipakai apa adanya.
    CHECK(physics.Stats().concaveDynamic == 0);

    physics.Step(world, 400);

    // Jatuh ke dasar palung: 1 + 0,25. Selubung cembung akan menahannya di
    // 2 + 0,25, dan selisih satu meter itu yang membuat uji ini berarti.
    INFO("berhenti di y = " << PositionOf(world, ball).y);
    CHECK(PositionOf(world, ball).y == doctest::Approx(1.25f).epsilon(0.05));
}

TEST_CASE("W6: whitebox statik menahan benda pada tinggi yang dihitung") {
    if (!Available()) {
        return;
    }
    scene::World world;
    // Kubus satuan diregangkan 4 × 3 × 4. **Ketiga sumbunya diuji**, dan itu
    // disengaja: skala yang diabaikan meninggalkan permukaan di y = 0,5 alih-alih
    // 1,5, dan pelat selebar satu meter alih-alih empat.
    AddWhiteboxBody(world, scene::RigidBodyKind::Static, Vec3(0.0f), Vec3(4.0f, 3.0f, 4.0f));
    // Di tengah: menguji tingginya.
    const scene::Entity middle = AddBall(world, Vec3(0.0f, 5.0f, 0.0f), 0.25f);
    // Di dekat tepinya: menguji lebarnya. x = 1,5 masih di atas pelat yang
    // diregangkan (setengah-lebar 2), dan melayang di luar yang tidak (0,5).
    const scene::Entity edge = AddBall(world, Vec3(1.5f, 5.0f, 1.5f), 0.25f);

    PhysicsScene physics;
    REQUIRE(physics.Build(world, {}, [](scene::Entity, ColliderGeometry& out) {
        out = UnitCubeGeometry();
        return true;
    }));
    CHECK(physics.Stats().bodies == 3);
    CHECK(physics.Stats().collidersWithoutGeometry == 0);
    CHECK(physics.Stats().concaveDynamic == 0);
    // Skala tak seragam **tidak** dikeluhkan untuk whitebox: sebuah titik bisa
    // diregangkan ke satu arah, sebuah jari-jari tidak.
    CHECK(physics.Stats().nonUniformScale == 0);

    physics.Step(world, 300);
    INFO("tengah berhenti di y = " << PositionOf(world, middle).y);
    CHECK(PositionOf(world, middle).y == doctest::Approx(1.75f).epsilon(0.05));
    INFO("tepi berhenti di y = " << PositionOf(world, edge).y);
    CHECK(PositionOf(world, edge).y == doctest::Approx(1.75f).epsilon(0.05));
}

TEST_CASE("W6: tanpa pemasok bentuk, collider whitebox mundur ke kotak dan dilaporkan") {
    if (!Available()) {
        return;
    }
    // **Mundur, bukan hilang.** Benda yang dilewatkan simulasi terlihat sebagai
    // benda yang jatuh menembus lantai — gejala yang mengarahkan orang mencari
    // bug solver. Kotak yang salah ukuran terlihat sebagai kotak yang salah
    // ukuran, dan angka di Stats menyebut sebabnya.
    scene::World world;
    AddGround(world);
    const scene::Entity block =
        AddWhiteboxBody(world, scene::RigidBodyKind::Dynamic, Vec3(0.0f, 3.0f, 0.0f));

    PhysicsScene physics;
    REQUIRE(physics.Build(world));
    CHECK(physics.Stats().bodies == 2);
    CHECK(physics.Stats().collidersWithoutGeometry == 1);

    physics.Step(world, 300);
    // Kotak bawaan setengah-ukuran 0,5, jadi ia beristirahat di y = 0,5.
    CHECK(PositionOf(world, block).y == doctest::Approx(0.5f).epsilon(0.05));
}

TEST_CASE("W6: whitebox cekung yang dinamis dilaporkan, bukan didiamkan") {
    if (!Available()) {
        return;
    }
    scene::World world;
    AddGround(world);
    AddWhiteboxBody(world, scene::RigidBodyKind::Dynamic, Vec3(0.0f, 3.0f, 0.0f));
    AddWhiteboxBody(world, scene::RigidBodyKind::Static, Vec3(6.0f, 0.0f, 0.0f));

    PhysicsScene physics;
    REQUIRE(physics.Build(world, {}, [](scene::Entity, ColliderGeometry& out) {
        out = UnitCubeGeometry();
        out.convex = false;  // seolah bloknya berbentuk L
        return true;
    }));

    // Yang dinamis dihitung, yang statik tidak: yang statik memakai segitiganya
    // apa adanya, jadi cekungannya memang terjaga dan tidak ada yang perlu
    // diperingatkan.
    CHECK(physics.Stats().concaveDynamic == 1);
    CHECK(physics.Stats().bodies == 3);
}

// ============================================================================
// L5 / P4 — collider heightfield
// ============================================================================

namespace {

/// Kisi tinggi berteras: datar per delapan sampel, naik lima meter tiap teras.
///
/// **Berteras, bukan menanjak.** Versi pertama uji ini memakai lereng tetap, dan
/// setiap bola yang mendarat menggelinding turun lalu jatuh dari tepi peta —
/// yang terbaca sebagai "collidernya tidak ada" padahal ia bekerja dengan benar.
/// Permukaan datar membuat angka akhirnya berarti.
///
/// Tingginya bergantung pada X saja. Itu yang membuat sumbu yang tertukar
/// terlihat: dengan baris dan kolom terbalik, tiga bola pada X berbeda akan
/// membaca tinggi yang sama.
///
/// Ditulis di sini, bukan diambil dari `Sim::Terrain`. Uji ini menguji
/// jembatannya, dan mengambil kisinya dari modul yang sama berarti satu bug
/// pemetaan lolos dari keduanya karena keduanya salah dengan cara yang sama.
ColliderGeometry TerraceField(int side = 33, float spacing = 1.0f) {
    ColliderGeometry shape;
    HeightFieldDesc& field = shape.heightField;
    field.width = side;
    field.depth = side;
    field.spacing = spacing;
    field.minHeight = 0.0f;
    field.maxHeight = 100.0f;
    field.samples.resize(static_cast<std::size_t>(side) * side);

    for (int z = 0; z < side; ++z) {
        for (int x = 0; x < side; ++x) {
            const float meters = 10.0f + 5.0f * static_cast<float>(x / 8);
            const float unit = (meters - field.minHeight) / (field.maxHeight - field.minHeight);
            field.samples[static_cast<std::size_t>(z) * side + x] =
                static_cast<uint16_t>(unit * 65535.0f + 0.5f);
        }
    }
    return shape;
}

/// Tinggi yang dijanjikan kisi di atas, dalam meter.
float TerraceHeight(float worldX) {
    return 10.0f + 5.0f * static_cast<float>(static_cast<int>(worldX) / 8);
}

scene::Entity AddTerrainBody(scene::World& world, scene::RigidBodyKind kind,
                             const Vec3& scale = Vec3(1.0f)) {
    const scene::Entity entity = world.Create("Lanskap");
    world.Add<scene::RigidBodyComponent>(entity).kind = kind;
    world.Add<scene::ColliderComponent>(entity).shape = scene::ColliderShape::Terrain;
    world.TryGet<scene::TransformComponent>(entity)->scale = scale;
    world.MarkTransformDirty(entity);
    return entity;
}

}  // namespace

TEST_CASE("L5: benda berhenti pada ketinggian yang dijanjikan kisi tingginya") {
    if (!Available()) {
        return;
    }
    // **Kriteria terima P4.** Bukan "sesuatu tertahan" melainkan "tertahan
    // persis di tinggi yang dilaporkan datanya" — dan diperiksa di tiga teras
    // berbeda, karena satu titik bisa kebetulan benar pada kisi yang sumbunya
    // tertukar.
    scene::World world;
    AddTerrainBody(world, scene::RigidBodyKind::Static);

    struct Probe {
        float x;
        scene::Entity ball;
    };
    std::vector<Probe> probes;
    for (const float x : {4.0f, 12.0f, 20.0f}) {
        // **Dijatuhkan dari lima meter, bukan dari empat puluh lima.** Kisi
        // tinggi tidak punya tebal: ia sebuah permukaan, bukan benda pejal.
        // Bola berjari-jari 0,25 m yang jatuh dari 45 m bergerak 0,44 m tiap
        // langkah — hampir sepanjang diameternya — dan deteksi tabrakan diskret
        // melewatkannya. Itu sifat kisi tinggi di mesin fisika mana pun, bukan
        // cacat yang bisa dibetulkan di sini; yang butuh benda cepat menyalakan
        // CCD. Versi pertama uji ini menjatuhkannya dari 45 m dan lulus hanya
        // pada teras tertinggi — yang jatuhnya paling pendek, dan karena itu
        // paling lambat saat menyentuh.
        probes.push_back(Probe{x, AddBall(world, Vec3(x, TerraceHeight(x) + 5.0f, 16.0f), 0.25f)});
    }

    PhysicsScene physics;
    REQUIRE(physics.Build(world, {}, [](scene::Entity, ColliderGeometry& out) {
        out = TerraceField();
        return true;
    }));
    CHECK(physics.Stats().terrainNotStatic == 0);
    CHECK(physics.Stats().bodies == 4);

    physics.Step(world, 400);

    for (const Probe& probe : probes) {
        const float expected = TerraceHeight(probe.x) + 0.25f;
        INFO("di x = " << probe.x << " berhenti di y = " << PositionOf(world, probe.ball).y
                       << ", diharapkan " << expected);
        CHECK(PositionOf(world, probe.ball).y == doctest::Approx(expected).epsilon(0.02));
        // Dan tetap di tempatnya: teras yang datar tidak menggelindingkan apa
        // pun, jadi bola yang bergeser berarti permukaannya miring padahal
        // datanya rata.
        CHECK(PositionOf(world, probe.ball).x == doctest::Approx(probe.x).epsilon(0.05));
    }
}

TEST_CASE("L5: sampel enam belas bit berpindah tanpa pembulatan") {
    if (!Available()) {
        return;
    }
    // Kisi terrain dan kisi PhysX sama-sama enam belas bit, jadi perpindahannya
    // penggeseran titik nol — bukan pembulatan. Yang melewatkan float di
    // antaranya menghasilkan collider yang tidak pernah persis sama dengan yang
    // digambar, dengan selisih yang terlalu kecil untuk dicari dan terlalu besar
    // untuk diabaikan ketika sebuah benda berhenti setengah tenggelam.
    //
    // Tinggi yang **tidak** bulat dipilih sengaja: 37,5 m pada rentang 0..100 m
    // adalah 24575,6 langkah, yang tidak mendarat tepat di sebuah sampel.
    scene::World world;
    AddTerrainBody(world, scene::RigidBodyKind::Static);
    // Lima meter di atas permukaannya, dengan alasan yang sama seperti di atas.
    const scene::Entity ball = AddBall(world, Vec3(8.0f, 42.5f, 8.0f), 0.5f);

    ColliderGeometry flat;
    HeightFieldDesc& field = flat.heightField;
    field.width = 17;
    field.depth = 17;
    field.spacing = 1.0f;
    field.minHeight = 0.0f;
    field.maxHeight = 100.0f;
    const uint16_t raw = static_cast<uint16_t>(37.5f / 100.0f * 65535.0f + 0.5f);
    field.samples.assign(static_cast<std::size_t>(17) * 17, raw);

    PhysicsScene physics;
    REQUIRE(physics.Build(world, {}, [&](scene::Entity, ColliderGeometry& out) {
        out = flat;
        return true;
    }));
    physics.Step(world, 400);

    // Tinggi yang benar-benar tersimpan di kisi enam belas bit, bukan 37,5
    // bulat: yang membandingkannya dengan 37,5 sedang menguji pembulatan
    // ujinya sendiri.
    const float stored = static_cast<float>(raw) / 65535.0f * 100.0f;
    INFO("tersimpan " << stored << " m, berhenti di " << PositionOf(world, ball).y);
    CHECK(PositionOf(world, ball).y == doctest::Approx(stored + 0.5f).epsilon(0.002));
}

TEST_CASE("L5: sampel hole benar-benar berlubang") {
    if (!Available()) {
        return;
    }
    scene::World world;
    AddTerrainBody(world, scene::RigidBodyKind::Static);
    // Satu bola di atas lubang, satu di atas tanah utuh — yang kedua yang
    // membuktikan lubangnya tidak melubangi seluruh peta.
    const scene::Entity overHole = AddBall(world, Vec3(4.5f, 15.0f, 4.5f), 0.25f);
    const scene::Entity overSolid = AddBall(world, Vec3(28.5f, 30.0f, 4.5f), 0.25f);

    ColliderGeometry field = TerraceField();
    field.heightField.holes.assign(field.heightField.samples.size(), 0);
    // Petak 3×3 di dalam teras pertama, jauh dari tangganya: satu petak saja
    // terlalu sempit untuk dijatuhi bola tanpa menyerempet tepinya.
    for (int z = 3; z <= 5; ++z) {
        for (int x = 3; x <= 5; ++x) {
            field.heightField.holes[static_cast<std::size_t>(z) * 33 + x] = 1;
        }
    }

    PhysicsScene physics;
    REQUIRE(physics.Build(world, {}, [&](scene::Entity, ColliderGeometry& out) {
        out = field;
        return true;
    }));
    CHECK(physics.Stats().bodies == 3);
    physics.Step(world, 400);

    INFO("di atas lubang: y = " << PositionOf(world, overHole).y);
    CHECK(PositionOf(world, overHole).y < -50.0f);
    INFO("di atas tanah: y = " << PositionOf(world, overSolid).y);
    CHECK(PositionOf(world, overSolid).y ==
          doctest::Approx(TerraceHeight(28.5f) + 0.25f).epsilon(0.02));
}

TEST_CASE("L5: terrain yang dinamis dilewatkan beserta sebabnya") {
    if (!Available()) {
        return;
    }
    // **Dilewatkan, bukan dimundurkan ke kotak seperti whitebox.** Kotak
    // setengah-ukuran 0,5 di tempat terrain empat kilometer bukan hampiran
    // melainkan sesuatu yang lain sama sekali, dan benda yang bertumpu padanya
    // melayang di udara.
    //
    // Kedua bagian dijalankan berurutan, bukan berdampingan: PhysX hanya
    // mengizinkan satu foundation per proses, jadi dua `PhysicsScene` yang
    // hidup bersamaan menggagalkan yang kedua karena sebab yang tidak ada
    // hubungannya dengan yang diuji.
    {
        scene::World world;
        AddTerrainBody(world, scene::RigidBodyKind::Dynamic);

        PhysicsScene physics;
        REQUIRE(physics.Build(world, {}, [](scene::Entity, ColliderGeometry& out) {
            out = TerraceField();
            return true;
        }));
        CHECK(physics.Stats().terrainNotStatic == 1);
        CHECK(physics.Stats().bodies == 0);
    }
    {
        // Tanpa pemasok bentuk sama sekali, hasilnya sama.
        scene::World world;
        AddTerrainBody(world, scene::RigidBodyKind::Static);
        PhysicsScene physics;
        REQUIRE(physics.Build(world));
        CHECK(physics.Stats().terrainNotStatic == 1);
        CHECK(physics.Stats().bodies == 0);
    }
}

TEST_CASE("L5: skala entity meregangkan kisinya, bukan sampelnya") {
    if (!Available()) {
        return;
    }
    // Kisi beraturan menyimpan jarak sampelnya sebagai satu angka, jadi
    // menskalakannya adalah mengalikan satu angka — bukan menyentuh jutaan
    // sampel yang isinya tidak berubah.
    scene::World world;
    AddTerrainBody(world, scene::RigidBodyKind::Static, Vec3(2.0f, 3.0f, 2.0f));
    // Pada skala 2 di XZ, dunia x = 12 adalah sampel x = 6 — teras pertama.
    // Tingginya dikalikan 3.
    const scene::Entity ball = AddBall(world, Vec3(12.0f, 35.0f, 12.0f), 0.25f);

    PhysicsScene physics;
    REQUIRE(physics.Build(world, {}, [](scene::Entity, ColliderGeometry& out) {
        out = TerraceField();
        return true;
    }));
    physics.Step(world, 500);

    const float expected = TerraceHeight(6.0f) * 3.0f + 0.25f;
    INFO("berhenti di y = " << PositionOf(world, ball).y << ", diharapkan " << expected);
    CHECK(PositionOf(world, ball).y == doctest::Approx(expected).epsilon(0.02));
}


TEST_CASE("Gizmo collider melaporkan ukuran yang sama dengan yang disimulasikan") {
    // **Gizmo yang menghitung ukurannya sendiri akan berbohong, dan justru di
    // kasus yang orang buka gizmo untuk memeriksanya.** Skala entity masuk ke
    // ukuran bentuk dengan aturan yang berbeda per bentuk — per sumbu untuk
    // kotak, sumbu terbesar untuk bola — dan aritmetika kedua yang "kira-kira
    // sama" akan sepakat pada skala seragam lalu menyimpang pada yang lain.
    //
    // Uji ini mengikatnya ke satu-satunya kebenaran yang tidak bisa dibantah:
    // di mana bolanya benar-benar berhenti.
    scene::World world;
    AddGround(world);
    const scene::Entity ball = AddBall(world, Vec3(0.0f, 6.0f, 0.0f), 0.5f);
    world.TryGet<scene::TransformComponent>(ball)->scale = Vec3(3.0f);
    world.MarkTransformDirty(ball);

    ColliderPlacement placement;
    REQUIRE(DescribeCollider(world, ball, placement));
    CHECK(placement.shape.kind == ShapeKind::Sphere);
    CHECK(placement.shape.radius == doctest::Approx(1.5f));
    CHECK(placement.simulated);
    CHECK_FALSE(placement.nonUniformScale);

    if (!Available()) {
        return;
    }
    PhysicsScene physics;
    REQUIRE(physics.Build(world));
    physics.Step(world, 240);
    // Bola berhenti setinggi jari-jarinya. Angka yang dibandingkan di sini
    // adalah yang dilaporkan gizmo, bukan angka yang ditulis ulang di uji —
    // jadi gizmo yang menyimpang menggagalkan baris ini.
    INFO("digambar r = " << placement.shape.radius << ", berhenti di y = "
                         << PositionOf(world, ball).y);
    CHECK(PositionOf(world, ball).y ==
          doctest::Approx(placement.shape.radius).epsilon(0.05));
}

TEST_CASE("Gizmo collider mengikuti aturan skala tiap bentuk, dan menyebut yang tidak muat") {
    // Tidak menuntut PhysX: ini aritmetika penempatan, dan build tanpa PhysX
    // tetap berhak menggambar collider yang sedang disetel orang.
    scene::World world;

    const scene::Entity box = world.Create("Box");
    {
        auto& collider = world.Add<scene::ColliderComponent>(box);
        collider.shape = scene::ColliderShape::Box;
        collider.halfExtents = Vec3(1.0f, 2.0f, 3.0f);
        collider.offset = Vec3(0.0f, 1.0f, 0.0f);
        auto& transform = *world.TryGet<scene::TransformComponent>(box);
        transform.scale = Vec3(2.0f, 3.0f, 4.0f);
        world.MarkTransformDirty(box);
    }

    ColliderPlacement placement;
    REQUIRE(DescribeCollider(world, box, placement));
    // Kotak boleh diregangkan per sumbu; setiap sumbu memakai skalanya sendiri.
    CHECK(placement.shape.halfExtents.x == doctest::Approx(2.0f));
    CHECK(placement.shape.halfExtents.y == doctest::Approx(6.0f));
    CHECK(placement.shape.halfExtents.z == doctest::Approx(12.0f));
    // Geserannya ikut berskala — kalau tidak, kapsul karakter yang asalnya di
    // telapak kaki akan melayang begitu entity-nya diperbesar.
    CHECK(placement.shape.localPosition.y == doctest::Approx(3.0f));
    CHECK(placement.scale.y == doctest::Approx(3.0f));
    // **Collider tanpa RigidBody tidak pernah sampai ke solver.** `Build`
    // menelusuri benda tegar, bukan collider — dan yang menggambarnya harus bisa
    // membedakan bentuk yang disimulasikan dari bentuk yang hanya digambar.
    CHECK_FALSE(placement.simulated);
    CHECK_FALSE(placement.nonUniformScale);

    const scene::Entity capsule = world.Create("Capsule");
    {
        auto& collider = world.Add<scene::ColliderComponent>(capsule);
        collider.shape = scene::ColliderShape::Capsule;
        collider.radius = 0.4f;
        collider.halfHeight = 0.6f;
        world.Add<scene::RigidBodyComponent>(capsule);
        auto& transform = *world.TryGet<scene::TransformComponent>(capsule);
        transform.scale = Vec3(1.0f, 2.0f, 1.0f);
        world.MarkTransformDirty(capsule);
    }
    REQUIRE(DescribeCollider(world, capsule, placement));
    // Jari-jari tidak bisa diregangkan ke satu arah, jadi sumbu terbesar yang
    // dipakai — dan itu dilaporkan, bukan didiamkan: yang tergambar memang
    // bukan yang terlihat.
    CHECK(placement.shape.radius == doctest::Approx(0.8f));
    // Setengah-tinggi bagian silinder tinggal di `halfExtents.x`, konvensi
    // PhysX — dan itu juga sumbu yang dipakai rangka kawatnya.
    CHECK(placement.shape.halfExtents.x == doctest::Approx(1.2f));
    CHECK(placement.simulated);
    CHECK(placement.nonUniformScale);

    // Entity tanpa collider tidak punya apa pun untuk digambar.
    const scene::Entity empty = world.Create("Empty");
    CHECK_FALSE(DescribeCollider(world, empty, placement));
}
