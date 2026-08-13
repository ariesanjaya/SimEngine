#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Physics/PhysicsScene.h"
#include "Sim/Scene/Components.h"
#include "Sim/Scene/Serialization.h"
#include "Sim/Scene/World.h"

#include <doctest/doctest.h>

#include <cmath>
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
