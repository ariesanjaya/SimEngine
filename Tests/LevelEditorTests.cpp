#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Editor/Command.h"
#include "Sim/Editor/EditorApp.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/Gizmo.h"
#include "Sim/Editor/SceneCommands.h"
#include "Sim/Editor/SceneView.h"
#include "Sim/Editor/ProjectLibrary.h"
#include "Sim/Editor/Selection.h"
#include "Sim/Editor/SkinnedPreview.h"
#include "Sim/Editor/WhiteboxCommands.h"
#include "Sim/Editor/WhiteboxStore.h"
#include "Sim/Physics/PhysicsScene.h"
#include "Sim/Whitebox/WhiteboxIo.h"
#include "Sim/Scene/Components.h"

#include <cmath>
#include "Sim/Scene/Serialization.h"
#include "Sim/Scene/World.h"

#include "Sim/Scene/Project.h"
#include <doctest/doctest.h>
#include <array>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace sim;
using namespace sim::editor;

namespace {

/// Registrasi tipe komponen sekali untuk seluruh berkas test.
struct ComponentTypes {
    ComponentTypes() { scene::RegisterCoreComponents(); }
};
const ComponentTypes kComponentTypes;

scene::Entity MakeBox(scene::World& world, const std::string& name, const Vec3& position,
                      scene::Entity parent = scene::kNullEntity) {
    const scene::Entity entity = world.Create(name, parent);
    world.TryGet<scene::TransformComponent>(entity)->position = position;
    world.Add<scene::MeshRendererComponent>(entity, scene::MeshRendererComponent{});
    world.MarkTransformDirty(entity);
    return entity;
}

/// Kamera yang memandang lurus ke -Z dari +Z, dipakai uji picking.
Mat4 TestView() {
    return LookAt(Vec3(0.0f, 0.0f, 10.0f), Vec3(0.0f), kUp);
}
Mat4 TestProjection() {
    return Perspective(60.0f * kDegToRad, 16.0f / 9.0f, 0.1f, 1000.0f);
}

}  // namespace

TEST_CASE("picking memilih objek terdepan di antara yang bertumpuk") {
    scene::World world;
    Selection selection;

    // Dua kotak persis segaris pandang: yang dekat harus menang, dan itu tidak
    // boleh bergantung pada urutan penyusunan daftar.
    const scene::Entity far = MakeBox(world, "Far", Vec3(0.0f, 0.0f, -5.0f));
    const scene::Entity near = MakeBox(world, "Near", Vec3(0.0f, 0.0f, 2.0f));

    SceneView view;
    view.Build(world, selection);
    CHECK(view.Pickables().size() == 2);

    const Ray ray{Vec3(0.0f, 0.0f, 10.0f), Vec3(0.0f, 0.0f, -1.0f)};
    CHECK(view.Raycast(ray) == near);

    // Dari sisi berlawanan, pemenangnya harus ikut berbalik.
    const Ray back{Vec3(0.0f, 0.0f, -10.0f), Vec3(0.0f, 0.0f, 1.0f)};
    CHECK(view.Raycast(back) == far);
}

TEST_CASE("picking menghormati rotasi dan skala objek") {
    scene::World world;
    Selection selection;

    const scene::Entity entity = MakeBox(world, "Rotated", Vec3(0.0f));
    auto* transform = world.TryGet<scene::TransformComponent>(entity);
    transform->rotation = Quat(Vec3(0.0f, 45.0f * kDegToRad, 0.0f));
    world.MarkTransformDirty(entity);

    SceneView view;
    view.Build(world, selection);

    // Sudut kubus satuan yang diputar 45° mencapai ~0.707 di sumbu X, jadi
    // sinar pada x = 0.6 mengenainya sedangkan pada x = 0.9 tidak. Uji ini
    // gagal kalau picking memakai AABB dunia alih-alih AABB ruang lokal.
    const Ray hit{Vec3(0.6f, 0.0f, 10.0f), Vec3(0.0f, 0.0f, -1.0f)};
    const Ray miss{Vec3(0.9f, 0.0f, 10.0f), Vec3(0.0f, 0.0f, -1.0f)};
    CHECK(view.Raycast(hit) == entity);
    CHECK(view.Raycast(miss) == scene::kNullEntity);
}

TEST_CASE("entity terkunci tetap terlihat tapi tidak bisa dipilih") {
    scene::World world;
    Selection selection;

    const scene::Entity entity = MakeBox(world, "Locked", Vec3(0.0f));
    world.Add<scene::VisibilityComponent>(entity, scene::VisibilityComponent{true, true});

    SceneView view;
    view.Build(world, selection);

    CHECK(view.Scene().meshes.size() == 1);  // masih digambar
    CHECK(view.Pickables().empty());         // tapi tidak bisa diklik
    CHECK(view.Raycast(Ray{Vec3(0.0f, 0.0f, 10.0f), Vec3(0.0f, 0.0f, -1.0f)}) ==
          scene::kNullEntity);
}

TEST_CASE("entity tersembunyi tidak digambar dan tidak bisa dipilih") {
    scene::World world;
    Selection selection;

    const scene::Entity entity = MakeBox(world, "Hidden", Vec3(0.0f));
    world.Add<scene::VisibilityComponent>(entity, scene::VisibilityComponent{false, false});

    SceneView view;
    view.Build(world, selection);
    CHECK(view.Scene().meshes.empty());
    CHECK(view.Pickables().empty());
}

TEST_CASE("seleksi kotak memilih yang di dalam dan melewatkan yang di luar") {
    scene::World world;
    Selection selection;

    const scene::Entity inside = MakeBox(world, "Inside", Vec3(0.0f, 0.0f, 0.0f));
    MakeBox(world, "Outside", Vec3(40.0f, 0.0f, 0.0f));

    SceneView view;
    view.Build(world, selection);

    const Vec2 size(1600.0f, 900.0f);
    const Mat4 viewProjection = TestProjection() * TestView();

    // Kotak kecil di tengah layar: hanya objek di titik nol yang tercakup.
    const ScreenRect rect = ScreenRect::FromCorners(Vec2(700.0f, 380.0f), Vec2(900.0f, 520.0f));
    const std::vector<scene::Entity> hits =
        view.RectSelect(viewProjection, Vec2(0.0f), size, rect);

    CHECK(hits.size() == 1);
    CHECK(hits.front() == inside);
}

TEST_CASE("sinar dari titik layar mengarah ke tempat yang ditunjuk") {
    scene::World world;
    Selection selection;
    const scene::Entity entity = MakeBox(world, "Center", Vec3(0.0f));

    SceneView view;
    view.Build(world, selection);

    const Vec2 size(1600.0f, 900.0f);
    const Ray center = ScreenPointToRay(TestView(), TestProjection(), size, size * 0.5f);
    CHECK(view.Raycast(center) == entity);

    // Sudut layar tidak boleh mengenai objek di tengah.
    const Ray corner = ScreenPointToRay(TestView(), TestProjection(), size, Vec2(4.0f, 4.0f));
    CHECK(view.Raycast(corner) == scene::kNullEntity);
}

TEST_CASE("satu seretan gizmo menghasilkan tepat satu entri undo") {
    scene::World world;
    Selection selection;
    CommandHistory history;

    const scene::Entity entity = MakeBox(world, "Dragged", Vec3(0.0f));
    const Uuid guid = world.GuidOf(entity);
    const scene::TransformComponent before = *world.TryGet<scene::TransformComponent>(entity);

    // Seretan nyata menghasilkan satu perintah per frame. Semuanya harus
    // menyatu menjadi satu entri, kalau tidak membatalkannya butuh puluhan
    // Ctrl+Z — dan itu yang dirasakan pengguna sebagai undo yang rusak.
    for (int frame = 1; frame <= 40; ++frame) {
        scene::TransformComponent after = before;
        after.position = Vec3(static_cast<float>(frame) * 0.1f, 0.0f, 0.0f);
        history.Execute(std::make_unique<SetTransformsCommand>(
            &world, std::vector<SetTransformsCommand::Item>{{guid, before, after}}, "Move"));
    }
    history.CloseMergeGroup();

    CHECK(history.Entries().size() == 1);
    CHECK(world.TryGet<scene::TransformComponent>(entity)->position.x == doctest::Approx(4.0f));

    CHECK(history.Undo());
    CHECK(world.TryGet<scene::TransformComponent>(entity)->position.x == doctest::Approx(0.0f));
    CHECK(history.Redo());
    CHECK(world.TryGet<scene::TransformComponent>(entity)->position.x == doctest::Approx(4.0f));
}

TEST_CASE("seretan atas seleksi berbeda tidak digabung") {
    scene::World world;
    Selection selection;
    CommandHistory history;

    const scene::Entity first = MakeBox(world, "First", Vec3(0.0f));
    const scene::Entity second = MakeBox(world, "Second", Vec3(5.0f, 0.0f, 0.0f));
    const scene::TransformComponent before{};

    scene::TransformComponent after = before;
    after.position = Vec3(1.0f, 0.0f, 0.0f);

    history.Execute(std::make_unique<SetTransformsCommand>(
        &world, std::vector<SetTransformsCommand::Item>{{world.GuidOf(first), before, after}},
        "Move"));
    history.Execute(std::make_unique<SetTransformsCommand>(
        &world, std::vector<SetTransformsCommand::Item>{{world.GuidOf(second), before, after}},
        "Move"));

    // Menggabungkannya akan membuat "sebelum" milik objek pertama dipakai untuk
    // membatalkan perpindahan objek kedua.
    CHECK(history.Entries().size() == 2);
}

TEST_CASE("snapping menghasilkan kelipatan persis tanpa galat menumpuk") {
    // Membulatkan nilai akhir, bukan selisih tiap frame. Uji ini meniru itu:
    // seratus langkah kecil berturut-turut harus tetap mendarat pada kelipatan
    // yang persis, bukan pada 2.4999998.
    const float step = 0.5f;
    float value = 0.0f;
    for (int i = 0; i < 100; ++i) {
        value += 0.137f;  // pergerakan yang tidak selaras dengan grid
        const float snapped = std::round(value / step) * step;
        CHECK(std::fmod(snapped, step) == 0.0f);
        value = snapped;
    }
}

TEST_CASE("decompose dan compose transform saling membalik") {
    const Vec3 position(1.5f, -2.25f, 7.0f);
    const Quat rotation = Quat(Vec3(0.3f, -1.1f, 0.7f));
    const Vec3 scale(2.0f, 0.5f, 3.0f);

    Vec3 outPosition;
    Quat outRotation;
    Vec3 outScale;
    DecomposeTransform(ComposeTransform(position, rotation, scale), outPosition, outRotation,
                       outScale);

    CHECK(outPosition.x == doctest::Approx(position.x));
    CHECK(outPosition.y == doctest::Approx(position.y));
    CHECK(outPosition.z == doctest::Approx(position.z));
    CHECK(outScale.x == doctest::Approx(scale.x));
    CHECK(outScale.y == doctest::Approx(scale.y));
    CHECK(outScale.z == doctest::Approx(scale.z));
    // Quaternion q dan -q mewakili rotasi yang sama, jadi yang dibandingkan
    // nilai mutlak hasil dot-nya.
    CHECK(std::abs(glm::dot(rotation, outRotation)) == doctest::Approx(1.0f));
}

TEST_CASE("hapus banyak entity bisa dibatalkan beserta hierarkinya") {
    scene::World world;
    Selection selection;
    CommandHistory history;

    const scene::Entity root = world.Create("Root");
    const scene::Entity child = MakeBox(world, "Child", Vec3(1.0f, 0.0f, 0.0f), root);
    const scene::Entity grandchild = MakeBox(world, "Grandchild", Vec3(0.0f, 1.0f, 0.0f), child);
    const scene::Entity other = MakeBox(world, "Other", Vec3(9.0f, 0.0f, 0.0f));

    const Uuid rootGuid = world.GuidOf(root);
    const Uuid grandchildGuid = world.GuidOf(grandchild);
    const Uuid otherGuid = world.GuidOf(other);
    const std::size_t before = world.Count();

    history.Execute(std::make_unique<DeleteEntitiesCommand>(
        &world, &selection, std::vector<Uuid>{rootGuid, otherGuid}));
    CHECK(world.Count() == before - 4);  // root + 2 keturunan + other

    CHECK(history.Undo());
    CHECK(world.Count() == before);

    const scene::Entity restored = world.FindByGuid(grandchildGuid);
    REQUIRE(scene::IsValid(restored));
    // Hierarki harus utuh, bukan sekadar jumlahnya kembali.
    CHECK(world.IsDescendantOf(restored, world.FindByGuid(rootGuid)));
    CHECK(world.TryGet<scene::TransformComponent>(restored)->position.y == doctest::Approx(1.0f));
}

TEST_CASE("duplikasi memberi GUID baru dan redo mempertahankannya") {
    scene::World world;
    Selection selection;
    CommandHistory history;

    const scene::Entity source = world.Create("Source");
    MakeBox(world, "Child", Vec3(2.0f, 0.0f, 0.0f), source);
    const Uuid sourceGuid = world.GuidOf(source);

    const std::vector<std::string> subtrees = CopySubtrees(world, {sourceGuid});
    REQUIRE(subtrees.size() == 1);

    auto command = std::make_unique<PasteEntitiesCommand>(&world, &selection, subtrees, Uuid{},
                                                          "Duplicate");
    PasteEntitiesCommand* paste = command.get();
    history.Execute(std::move(command));

    REQUIRE(paste->CreatedRoots().size() == 1);
    const Uuid copyGuid = paste->CreatedRoots().front();
    CHECK(copyGuid != sourceGuid);
    CHECK(world.Count() == 4);

    CHECK(history.Undo());
    CHECK(world.Count() == 2);

    // Redo harus menghidupkan kembali GUID yang sama. Kalau ditukar ulang tiap
    // kali, perintah lain yang menunjuk hasil duplikasi jadi tidak sah setelah
    // satu putaran undo-redo.
    CHECK(history.Redo());
    CHECK(paste->CreatedRoots().front() == copyGuid);
    CHECK(scene::IsValid(world.FindByGuid(copyGuid)));
}

TEST_CASE("menyalin melewatkan entity yang leluhurnya ikut terpilih") {
    scene::World world;
    const scene::Entity parent = world.Create("Parent");
    const scene::Entity child = world.Create("Child", parent);

    // Memilih induk dan anaknya sekaligus lalu menyalin keduanya akan
    // menghasilkan anak yang tersalin dua kali.
    const std::vector<std::string> subtrees =
        CopySubtrees(world, {world.GuidOf(parent), world.GuidOf(child)});
    CHECK(subtrees.size() == 1);
}

TEST_CASE("level 10+ entity tiga tingkat kembali identik setelah putaran simpan-muat") {
    scene::World world;
    const scene::Entity root = world.Create("Level");
    for (int i = 0; i < 4; ++i) {
        const scene::Entity group =
            MakeBox(world, "Group" + std::to_string(i), Vec3(static_cast<float>(i), 0.0f, 0.0f),
                    root);
        for (int j = 0; j < 3; ++j) {
            const scene::Entity leaf =
                MakeBox(world, "Leaf" + std::to_string(i) + "_" + std::to_string(j),
                        Vec3(0.0f, static_cast<float>(j) * 0.5f, 1.0f), group);
            world.TryGet<scene::TransformComponent>(leaf)->rotation =
                Quat(Vec3(0.1f * static_cast<float>(j), 0.2f, 0.3f));
            world.MarkTransformDirty(leaf);
        }
    }
    CHECK(world.Count() >= 10);

    const std::string first = scene::SaveLevelToString(world);

    scene::World reloaded;
    REQUIRE(scene::LoadLevelFromString(reloaded, first).ok);
    const std::string second = scene::SaveLevelToString(reloaded);

    CHECK(first == second);
    CHECK(reloaded.Count() == world.Count());
}

TEST_CASE("memindahkan 100 entity sekaligus tetap jauh di bawah anggaran satu frame") {
    scene::World world;
    Selection selection;
    CommandHistory history;

    std::vector<SetTransformsCommand::Item> items;
    for (int i = 0; i < 100; ++i) {
        const scene::Entity entity =
            MakeBox(world, "Box" + std::to_string(i), Vec3(static_cast<float>(i), 0.0f, 0.0f));
        scene::TransformComponent before = *world.TryGet<scene::TransformComponent>(entity);
        scene::TransformComponent after = before;
        after.position += Vec3(0.0f, 1.0f, 0.0f);
        items.push_back({world.GuidOf(entity), before, after});
    }

    // 120 frame seretan atas 100 entity. Anggaran satu frame pada 60 Hz adalah
    // 16,6 ms; seluruh rangkaian ini harus selesai jauh di bawah itu supaya
    // menyeret seleksi besar tidak menurunkan laju frame.
    const auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < 120; ++frame) {
        for (auto& item : items) {
            item.after.position.y = static_cast<float>(frame) * 0.01f;
        }
        history.Execute(std::make_unique<SetTransformsCommand>(&world, items, "Move"));
    }
    history.CloseMergeGroup();
    const auto elapsed = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - start)
                             .count();

    // Ambangnya sengaja 1 ms per frame, bukan 16,6 ms. Anggaran satu frame
    // harus dibagi dengan menggambar seluruh UI dan scene; kalau memindahkan
    // seleksi saja sudah menghabiskannya, laju frame pasti turun. Batas yang
    // longgar akan lulus bahkan ketika kinerjanya sudah 16 kali lebih buruk.
    const double perFrameMs = elapsed / 120.0;
    INFO("120 frame x 100 entity: " << elapsed << " ms total, " << perFrameMs << " ms per frame");
    CHECK(perFrameMs < 1.0);
    CHECK(history.Entries().size() == 1);
}

// --- penerjemahan lampu ke ruang dunia ---------------------------------------

namespace {

scene::Entity MakeLight(scene::World& world, const char* name, scene::LightType type,
                        const Quat& rotation = Quat(1.0f, 0.0f, 0.0f, 0.0f)) {
    const scene::Entity entity = world.Create(name);
    auto& transform = world.Add<scene::TransformComponent>(entity);
    transform.position = Vec3(1.0f, 2.0f, 3.0f);
    transform.rotation = rotation;
    world.MarkTransformDirty(entity);
    auto& light = world.Add<scene::LightComponent>(entity);
    light.type = type;
    return entity;
}

}  // namespace

/// Lampu pertama berjenis tertentu. Dicari, bukan diindeks: urutan iterasi
/// `entt` bukan bagian dari kontrak SceneView, dan test yang mengandaikannya
/// akan merah karena alasan yang tidak ada hubungannya dengan yang diujinya.
const render::LightInstance* FindLight(const render::ViewportScene& scene,
                                       render::LightKind kind) {
    for (const render::LightInstance& light : scene.lights) {
        if (light.kind == kind) {
            return &light;
        }
    }
    return nullptr;
}

TEST_CASE("Arah lampu directional dibalik, arah spot tidak") {
    scene::World world;
    Selection selection;

    // Tanpa rotasi, -Z lokal menghadap ke -Z dunia.
    MakeLight(world, "Sun", scene::LightType::Directional);
    MakeLight(world, "Beam", scene::LightType::Spot);
    MakeLight(world, "Bulb", scene::LightType::Point);

    SceneView view;
    view.Build(world, selection);
    const render::ViewportScene scene = view.Scene();
    REQUIRE(scene.lights.size() == 3);

    const render::LightInstance* sun = FindLight(scene, render::LightKind::Directional);
    const render::LightInstance* beam = FindLight(scene, render::LightKind::Spot);
    REQUIRE(sun != nullptr);
    REQUIRE(beam != nullptr);
    REQUIRE(FindLight(scene, render::LightKind::Point) != nullptr);

    // **Dua konvensi yang berlawanan, dan keduanya sengaja.** Directional
    // menyimpan arah KE cahaya — itulah yang dipakai `n·l` dan cascade bayangan.
    // Spot menyimpan arah pancarnya. Menyamakan keduanya berarti satu tanda yang
    // harus diingat di setiap pemakaian, dan yang lupa mendapat adegan gelap
    // atau kerucut yang menyorot ke belakang.
    CHECK(sun->direction.z == doctest::Approx(1.0f));
    CHECK(beam->direction.z == doctest::Approx(-1.0f));
}

TEST_CASE("Rotasi entity memutar arah lampu") {
    scene::World world;
    Selection selection;
    // Yaw 90 derajat: -Z lokal menjadi -X dunia.
    MakeLight(world, "Beam", scene::LightType::Spot,
              Quat(Vec3(0.0f, 90.0f * kDegToRad, 0.0f)));

    SceneView view;
    view.Build(world, selection);
    const render::ViewportScene scene = view.Scene();
    REQUIRE(scene.lights.size() == 1);
    CHECK(scene.lights[0].direction.x == doctest::Approx(-1.0f).epsilon(0.001));
    CHECK(std::abs(scene.lights[0].direction.z) < 0.001f);
    // Posisi diambil dari kolom translasi matriks dunia, bukan dari komponen.
    CHECK(scene.lights[0].position.y == doctest::Approx(2.0f));
}

TEST_CASE("Sudut kerucut yang tertukar dibetulkan, bukan diteruskan") {
    scene::World world;
    Selection selection;
    const scene::Entity entity = MakeLight(world, "Beam", scene::LightType::Spot);
    auto* light = world.TryGet<scene::LightComponent>(entity);
    // Dalam lebih besar daripada luar — sah diketik pengguna, tidak sah dipakai.
    light->innerAngleRadians = 1.0f;
    light->outerAngleRadians = 0.4f;

    SceneView view;
    view.Build(world, selection);
    const render::ViewportScene scene = view.Scene();
    REQUIRE(scene.lights.size() == 1);

    // Kosinus mengecil saat sudut membesar, jadi kerucut dalam harus punya
    // kosinus yang LEBIH BESAR. Kalau keduanya diteruskan apa adanya, pembagi
    // di shader menjadi negatif dan tepi berkasnya menyala alih-alih memudar.
    CHECK(scene.lights[0].cosInner > scene.lights[0].cosOuter);
    CHECK(scene.lights[0].cosOuter == doctest::Approx(std::cos(1.0f)));
    CHECK(scene.lights[0].cosInner == doctest::Approx(std::cos(0.4f)));
}

TEST_CASE("Jari-jari sumber sampai ke renderer") {
    scene::World world;
    Selection selection;
    const scene::Entity entity = MakeLight(world, "Bulb", scene::LightType::Point);
    world.TryGet<scene::LightComponent>(entity)->sourceRadius = 0.5f;

    SceneView view;
    view.Build(world, selection);
    const render::ViewportScene scene = view.Scene();
    REQUIRE(scene.lights.size() == 1);
    CHECK(scene.lights[0].sourceRadius == doctest::Approx(0.5f));
}

TEST_CASE("Bendera bayangan mesh sampai ke renderer") {
    scene::World world;
    Selection selection;

    const scene::Entity caster = MakeBox(world, "Caster", Vec3(0.0f));
    const scene::Entity ghost = MakeBox(world, "Ghost", Vec3(4.0f, 0.0f, 0.0f));
    world.TryGet<scene::MeshRendererComponent>(ghost)->castShadows = false;
    world.TryGet<scene::MeshRendererComponent>(ghost)->receiveShadows = false;

    SceneView view;
    view.Build(world, selection);
    const render::ViewportScene scene = view.Scene();
    REQUIRE(scene.meshes.size() == 2);

    // **Bendera yang muncul di Inspector tapi tidak sampai ke renderer adalah
    // antarmuka yang berbohong**, dan itu lebih buruk daripada tombol yang belum
    // ada: pemakainya mengira sudah mematikan sesuatu. Keduanya sempat begitu.
    int casters = 0;
    int receivers = 0;
    for (const render::MeshInstance& mesh : scene.meshes) {
        casters += mesh.castShadows ? 1 : 0;
        receivers += mesh.receiveShadows ? 1 : 0;
    }
    CHECK(casters == 1);
    CHECK(receivers == 1);
    (void)caster;
}

TEST_CASE("Warna, intensitas, dan bendera bayangan lampu sampai ke renderer") {
    scene::World world;
    Selection selection;
    const scene::Entity entity = MakeLight(world, "Sun", scene::LightType::Directional);
    auto* light = world.TryGet<scene::LightComponent>(entity);
    light->color = Vec3(1.0f, 0.5f, 0.25f);
    light->intensity = 3.0f;
    light->castShadows = false;

    SceneView view;
    view.Build(world, selection);
    const render::ViewportScene scene = view.Scene();
    REQUIRE(scene.lights.size() == 1);

    // Warna dan intensitas dibawa terpisah, tidak dikalikan di sini: yang
    // mengalikannya renderer, bersama eksposur — dan mengalikan dua kali adalah
    // kesalahan yang tampak seperti "lampunya memang terlalu terang".
    CHECK(scene.lights[0].color.g == doctest::Approx(0.5f));
    CHECK(scene.lights[0].intensity == doctest::Approx(3.0f));
    CHECK(scene.lights[0].castShadows == false);
}

// --- palet kulit ---------------------------------------------------------------

namespace {

/// Perender palsu yang hanya menjawab pertanyaan "berapa bone mesh ini".
///
/// **Palsu, bukan `StubRenderer`.** Yang diuji di sini adalah sisi editor dari
/// seam skinning — apakah `SceneView` menyusun palet dan menunjuknya dengan
/// benar — dan itu menuntut sebuah `AcquireMesh` yang bisa dibuat menjawab apa
/// saja. Perender sungguhan menuntut GPU; `StubRenderer` selalu menjawab nol.
class BoneCountRenderer final : public render::IViewportRenderer {
public:
    explicit BoneCountRenderer(uint32_t boneCount) : boneCount_(boneCount) {}

    render::MeshAsset AcquireMesh(std::string_view) override {
        render::MeshAsset asset;
        asset.handle = 1;
        asset.loaded = true;
        asset.boneCount = boneCount_;
        return asset;
    }

    void Resize(uint32_t, uint32_t) override {}
    void Render(const render::ViewportDesc&, const render::ViewportScene&) override {}
    render::TextureHandle ColorTarget() const override { return render::kInvalidTexture; }
    Vec2 ColorTargetUvMax() const override { return Vec2(1.0f); }
    uint32_t Width() const override { return 1; }
    uint32_t Height() const override { return 1; }
    const char* Name() const override { return "BoneCountRenderer"; }

private:
    uint32_t boneCount_ = 0;
};

/// Folder aset sementara berisi satu berkas mesh, beserta database-nya.
struct MeshFixture {
    MeshFixture() {
        static std::atomic<int> counter{0};
        root = std::filesystem::temp_directory_path() /
               ("simskin_" + std::to_string(counter.fetch_add(1)) + "_" +
                std::to_string(::getpid()));
        std::filesystem::create_directories(root);
        std::ofstream(root / "rig.fbx") << "bukan fbx sungguhan";
        database.Initialize({root, nullptr, 1.0f});
    }
    ~MeshFixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    MeshFixture(const MeshFixture&) = delete;
    MeshFixture& operator=(const MeshFixture&) = delete;

    /// GUID berkas mesh-nya, atau tidak sah kalau pemindaian gagal.
    Uuid Guid() const {
        const assets::AssetRecord* record = database.FindByRelativePath("rig.fbx");
        return record != nullptr ? record->guid : Uuid{};
    }

    std::filesystem::path root;
    assets::AssetDatabase database;
};

scene::Entity MakeSkinnedMesh(scene::World& world, const char* name, const Uuid& mesh) {
    const scene::Entity entity = world.Create(name);
    scene::MeshRendererComponent renderer;
    renderer.mesh = AssetRef{mesh};
    world.Add<scene::MeshRendererComponent>(entity, renderer);
    return entity;
}

}  // namespace

TEST_CASE("Tiap karakter mendapat ruas paletnya sendiri, berurutan tanpa tumpang tindih") {
    MeshFixture fixture;
    const Uuid mesh = fixture.Guid();
    REQUIRE(mesh.IsValid());

    scene::World world;
    Selection selection;
    MakeSkinnedMesh(world, "Hero", mesh);
    MakeSkinnedMesh(world, "Villain", mesh);

    // Tiga bone, dua karakter. **Ruas yang tumpang tindih adalah dua karakter
    // yang memakai pose yang sama**, dan itu terlihat sebagai kembar yang
    // bergerak serempak — bukan sebagai galat.
    BoneCountRenderer renderer(3);
    SceneView view;
    view.Build(world, selection, &fixture.database, &renderer);
    const render::ViewportScene scene = view.Scene();

    REQUIRE(scene.meshes.size() == 2);
    CHECK(scene.skinMatrices.size() == 6);
    CHECK(scene.meshes[0].skinCount == 3);
    CHECK(scene.meshes[1].skinCount == 3);
    CHECK(scene.meshes[0].skinFirst != scene.meshes[1].skinFirst);
    for (const render::MeshInstance& instance : scene.meshes) {
        CHECK(instance.skinFirst + instance.skinCount <= scene.skinMatrices.size());
    }

    // Isinya bind pose, yaitu matriks satuan: matriks kulit adalah
    // `global × invers bind`, dan pada bind pose keduanya saling meniadakan.
    for (const Mat4& matrix : scene.skinMatrices) {
        CHECK(matrix == Mat4(1.0f));
    }

    // Dibangun ulang tiap frame, jadi paletnya tidak boleh menumpuk.
    view.Build(world, selection, &fixture.database, &renderer);
    CHECK(view.Scene().skinMatrices.size() == 6);
}

TEST_CASE("Mesh tanpa rangka tidak menyita satu matriks pun") {
    MeshFixture fixture;
    const Uuid mesh = fixture.Guid();
    REQUIRE(mesh.IsValid());

    scene::World world;
    Selection selection;
    MakeSkinnedMesh(world, "Batu", mesh);

    BoneCountRenderer renderer(0);
    SceneView view;
    view.Build(world, selection, &fixture.database, &renderer);
    const render::ViewportScene scene = view.Scene();

    REQUIRE(scene.meshes.size() == 1);
    CHECK(scene.skinMatrices.empty());
    // Nol berarti "digambar tanpa kulit", dan itulah jalur seluruh adegan statis.
    CHECK(scene.meshes[0].skinCount == 0);
}

// --- pemutaran klip di viewport -------------------------------------------------

TEST_CASE("Animator memutar klip FBX pada mesh ber-rig dan paletnya berubah terhadap waktu") {
    // Rig dan klipnya tidak ikut di repo:
    //   SIM_RIG_FBX=/path/rig.fbx SIM_CLIP_FBX=/path/Running.fbx ctest
    const char* rigPath = std::getenv("SIM_RIG_FBX");
    const char* clipPath = std::getenv("SIM_CLIP_FBX");
    if (rigPath == nullptr || clipPath == nullptr || !std::filesystem::exists(rigPath) ||
        !std::filesystem::exists(clipPath)) {
        return;
    }

    // Folder aset sementara berisi keduanya. Disalin, bukan ditunjuk: AssetDatabase
    // memberi GUID lewat berkas `.meta` di sebelah asetnya, dan menaburi folder
    // sumber orang dengan berkas itu bukan yang boleh dilakukan sebuah test.
    static std::atomic<int> counter{0};
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("simanim_" + std::to_string(counter.fetch_add(1)) + "_" + std::to_string(::getpid()));
    std::filesystem::create_directories(root);
    std::error_code copyError;
    std::filesystem::copy_file(rigPath, root / "rig.fbx", copyError);
    std::filesystem::copy_file(clipPath, root / "clip.fbx", copyError);
    REQUIRE(!copyError);

    assets::AssetDatabase database;
    REQUIRE(database.Initialize({root, nullptr, 1.0f}));
    const assets::AssetRecord* rigRecord = database.FindByRelativePath("rig.fbx");
    const assets::AssetRecord* clipRecord = database.FindByRelativePath("clip.fbx");
    REQUIRE(rigRecord != nullptr);
    REQUIRE(clipRecord != nullptr);

    scene::World world;
    const scene::Entity entity = world.Create("Karakter");
    scene::MeshRendererComponent renderer;
    renderer.mesh = AssetRef{rigRecord->guid};
    world.Add<scene::MeshRendererComponent>(entity, renderer);
    scene::AnimatorComponent animator;
    animator.clip = AssetRef{clipRecord->guid};
    world.Add<scene::AnimatorComponent>(entity, animator);

    SkinnedPreview preview;
    preview.Update(world, &database, 0.0f);
    CHECK(preview.AnimatedCount() == 1);
    const std::vector<Mat4> first(preview.PaletteFor(entity).begin(),
                                  preview.PaletteFor(entity).end());
    REQUIRE(first.size() > 30);  // rig Mixamo: 65 bone

    // **Pose awal bukan bind pose.** Palet yang seluruhnya matriks satuan berarti
    // klipnya tidak sampai ke bone mana pun — dan itu terlihat persis sama dengan
    // animasi yang berjalan benar pada frame pertama, jadi ia harus diperiksa.
    int moved = 0;
    for (const Mat4& matrix : first) {
        if (matrix != Mat4(1.0f)) {
            ++moved;
        }
    }
    CHECK(moved > 10);

    // Maju seperempat detik: paletnya harus berubah.
    preview.Update(world, &database, 0.25f);
    const std::span<const Mat4> second = preview.PaletteFor(entity);
    REQUIRE(second.size() == first.size());
    int changed = 0;
    for (std::size_t i = 0; i < first.size(); ++i) {
        if (first[i] != second[i]) {
            ++changed;
        }
    }
    CHECK(changed > 10);
    CHECK(world.TryGet<scene::AnimatorComponent>(entity)->time == doctest::Approx(0.25f));

    // Dijeda berarti benar-benar diam — bukan sekadar lebih lambat.
    world.TryGet<scene::AnimatorComponent>(entity)->playing = false;
    const std::vector<Mat4> held(second.begin(), second.end());
    preview.Update(world, &database, 0.5f);
    const std::span<const Mat4> after = preview.PaletteFor(entity);
    REQUIRE(after.size() == held.size());
    for (std::size_t i = 0; i < held.size(); ++i) {
        REQUIRE(held[i] == after[i]);
    }

    // Waktu dibungkus di dalam durasi klipnya, bukan tumbuh tanpa batas: waktu
    // yang membesar terus kehilangan presisi float-nya sesudah beberapa jam, dan
    // yang terlihat adalah animasi yang makin tersendat.
    world.TryGet<scene::AnimatorComponent>(entity)->playing = true;
    for (int i = 0; i < 200; ++i) {
        preview.Update(world, &database, 0.1f);
    }
    const float wrapped = world.TryGet<scene::AnimatorComponent>(entity)->time;
    CHECK(wrapped >= 0.0f);
    CHECK(wrapped < 10.0f);

    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);
}

TEST_CASE("Animator tanpa mesh ber-rig tidak menghasilkan palet") {
    scene::World world;
    const scene::Entity entity = world.Create("Kotak");
    world.Add<scene::MeshRendererComponent>(entity, scene::MeshRendererComponent{});
    world.Add<scene::AnimatorComponent>(entity, scene::AnimatorComponent{});

    SkinnedPreview preview;
    preview.Update(world, nullptr, 0.016f);
    CHECK(preview.AnimatedCount() == 0);
    CHECK(preview.PaletteFor(entity).empty());
}

// --- project manager -----------------------------------------------------------

namespace {

/// Folder sementara yang membersihkan dirinya sendiri.
struct ScratchDir {
    ScratchDir() {
        static std::atomic<int> counter{0};
        path = std::filesystem::temp_directory_path() /
               ("simproj_" + std::to_string(counter.fetch_add(1)) + "_" +
                std::to_string(::getpid()));
        std::filesystem::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    std::filesystem::path path;
};

}  // namespace

TEST_CASE("Nama project menjadi nama folder yang sah di sistem berkas mana pun") {
    // Yang dibuang adalah aksara yang ditolak Windows, bukan hanya yang ditolak
    // Linux: project dibuat di satu mesin dan dibuka di mesin lain, dan folder
    // bernama "Level: 2" tidak bisa di-checkout di sana sama sekali.
    CHECK(ProjectLibrary::SanitizeFolderName("Kota Tua") == "Kota Tua");
    CHECK(ProjectLibrary::SanitizeFolderName("Level: 2") == "Level_ 2");
    CHECK(ProjectLibrary::SanitizeFolderName("a/b\\c") == "a_b_c");
    // Titik di awal ikut dibuang: sebuah nama project tidak boleh bisa menjadi
    // folder tersembunyi, dan tidak boleh bisa menjadi `..`.
    CHECK(ProjectLibrary::SanitizeFolderName("../rahasia") == "_rahasia");
    CHECK(ProjectLibrary::SanitizeFolderName("..") .empty());
    CHECK(ProjectLibrary::SanitizeFolderName(".git") == "git");
    // Spasi dan titik di ujung dihapus Windows diam-diam, jadi nama yang diminta
    // dan nama yang jadi akan berbeda — dan project yang foldernya bukan yang
    // tertulis di daftar tidak bisa dibuka lagi dari sana.
    CHECK(ProjectLibrary::SanitizeFolderName("  Arena . ") == "Arena");
    CHECK(ProjectLibrary::SanitizeFolderName("...") .empty());
    CHECK(ProjectLibrary::SanitizeFolderName("").empty());
}

TEST_CASE("Project baru lahir lengkap dengan foldernya, dan menolak menimpa yang sudah ada") {
    ScratchDir scratch;
    ProjectLibrary library;

    scene::Project project;
    std::string error;
    REQUIRE(library.Create(scratch.path, "Kota Tua", project, error));
    CHECK(project.name == "Kota Tua");
    CHECK(project.root == scratch.path / "Kota Tua");
    CHECK(std::filesystem::exists(project.root / "project.simproj"));
    CHECK(std::filesystem::is_directory(project.AssetsDirectory()));
    CHECK(std::filesystem::is_directory(project.LevelsDirectory()));
    CHECK(std::filesystem::is_directory(project.PrefabsDirectory()));

    // **Menolak menimpa, dan itu bukan kehati-hatian berlebihan.** Menimpa
    // folder yang sudah berisi adalah satu-satunya cara alat seperti ini bisa
    // menghapus pekerjaan orang.
    scene::Project again;
    CHECK_FALSE(library.Create(scratch.path, "Kota Tua", again, error));
    CHECK_FALSE(error.empty());

    // Dibuka kembali menghasilkan project yang sama.
    scene::Project opened;
    REQUIRE(library.Open(project.root, opened, error));
    CHECK(opened.name == "Kota Tua");
    CHECK(opened.AssetsDirectory() == project.AssetsDirectory());

    // Folder yang hilang — git tidak menyimpan folder kosong — dibuat lagi saat
    // dibuka, bukan menjadi alasan menolak.
    std::error_code removeError;
    std::filesystem::remove(opened.PrefabsDirectory(), removeError);
    REQUIRE_FALSE(std::filesystem::exists(opened.PrefabsDirectory()));
    scene::Project reopened;
    REQUIRE(library.Open(project.root, reopened, error));
    CHECK(std::filesystem::is_directory(reopened.PrefabsDirectory()));
}

TEST_CASE("Daftar project terakhir dibuka bertahan, terurut, dan tidak menggandakan diri") {
    ScratchDir scratch;
    ProjectLibrary library;
    std::string error;

    scene::Project first;
    scene::Project second;
    REQUIRE(library.Create(scratch.path, "Satu", first, error));
    REQUIRE(library.Create(scratch.path, "Dua", second, error));

    library.Remember(first, 100);
    library.Remember(second, 200);
    REQUIRE(library.Recent().size() == 2);
    CHECK(library.Recent()[0].name == "Dua");

    // Membuka yang lama lagi memindahkannya ke depan, bukan menambah entri.
    library.Remember(first, 300);
    REQUIRE(library.Recent().size() == 2);
    CHECK(library.Recent()[0].name == "Satu");

    const std::filesystem::path listFile = scratch.path / "projects.json";
    REQUIRE(library.Save(listFile));

    ProjectLibrary loaded;
    loaded.Load(listFile);
    REQUIRE(loaded.Recent().size() == 2);
    CHECK(loaded.Recent()[0].name == "Satu");
    CHECK(loaded.Recent()[1].name == "Dua");
    CHECK(loaded.Recent()[0].Exists());

    // Project yang berkasnya lenyap tetap tercatat, tapi ditandai: yang hilang
    // dari daftar tanpa penjelasan terbaca sebagai editor yang lupa.
    std::error_code removeError;
    std::filesystem::remove_all(second.root, removeError);
    ProjectLibrary afterDelete;
    afterDelete.Load(listFile);
    REQUIRE(afterDelete.Recent().size() == 2);
    CHECK_FALSE(afterDelete.Recent()[1].Exists());

    CHECK(afterDelete.Forget(second.root));
    CHECK(afterDelete.Recent().size() == 1);
    CHECK_FALSE(afterDelete.Forget(second.root));
}

TEST_CASE("Daftar project yang belum pernah ada bukan galat") {
    ScratchDir scratch;
    ProjectLibrary library;
    // Keadaan pemakaian pertama. Editor yang menganggapnya galat akan menyapa
    // pemakai barunya dengan pesan kesalahan.
    library.Load(scratch.path / "belum-ada.json");
    CHECK(library.Recent().empty());
}

TEST_CASE("EditorApp::CreateProject benar-benar membuat foldernya di lokasi bawaan") {
    // Dilaporkan: menekan "Buat" tidak menghasilkan folder apa pun di bawah
    // ~/Documents/SimEngine. Uji ini menjalankan jalur yang sama persis dengan
    // yang dipanggil tombolnya, tanpa lapisan UI — supaya jelas sisi mana yang
    // salah.
    ScratchDir scratch;
    const std::filesystem::path configDir = scratch.path / "config";
    const std::filesystem::path projectsRoot = scratch.path / "Documents" / "SimEngine";

    EditorApp app;
    EditorApp::Config config;
    config.configDir = configDir;
    config.projectsRoot = projectsRoot;
    REQUIRE(app.Initialize(config));
    CHECK_FALSE(app.HasProject());

    REQUIRE(app.CreateProject(projectsRoot, "Arena"));
    CHECK(app.HasProject());
    CHECK(app.CurrentProject().name == "Arena");
    CHECK(std::filesystem::is_directory(projectsRoot / "Arena"));
    CHECK(std::filesystem::exists(projectsRoot / "Arena" / "project.simproj"));
    CHECK(std::filesystem::is_directory(projectsRoot / "Arena" / "Assets"));
    CHECK(std::filesystem::exists(configDir / "projects.json"));

    app.Shutdown();
}

TEST_CASE("Warna instance datang dari material, bukan lagi dari komponen") {
    // `MeshRendererComponent::baseColor` sudah tidak ada. Yang menggantikannya:
    // material yang ditetapkan entity, dan material bawaan editor untuk yang
    // tidak menetapkannya.
    ScratchDir scratch;
    const std::filesystem::path builtinRoot = scratch.path / "Resources";
    std::filesystem::create_directories(builtinRoot / "Materials");
    std::filesystem::copy_file(
        std::filesystem::path(SIM_BUILTIN_DIR) / "Materials" / "Default.simmat",
        builtinRoot / "Materials" / "Default.simmat");

    assets::AssetDatabase builtin;
    REQUIRE(builtin.Initialize({builtinRoot, nullptr, 1.0f}));
    REQUIRE(builtin.FindByRelativePath("Materials/Default.simmat") != nullptr);

    scene::World world;
    Selection selection;
    const scene::Entity entity = world.Create("Kotak");
    world.Add<scene::MeshRendererComponent>(entity, scene::MeshRendererComponent{});

    SceneView view;
    view.Build(world, selection, nullptr, nullptr, nullptr, &builtin);
    const render::ViewportScene scene = view.Scene();
    REQUIRE(scene.meshes.size() == 1);

    // Warna material bawaan, bukan warna apa pun yang kebetulan menjadi nilai
    // awal `MeshInstance`.
    CHECK(scene.meshes[0].color.r == doctest::Approx(0.62f));
    CHECK(scene.meshes[0].color.g == doctest::Approx(0.65f));
    CHECK(scene.meshes[0].color.b == doctest::Approx(0.70f));

    // Slot material yang kosong dikirim ber-alpha nol, bukan diisi warna bawaan:
    // nol berarti "tidak ditetapkan", dan renderer lalu memakai material yang
    // tertulis di berkas mesh — yang tidak diketahui editor. Mengisinya di sini
    // akan menimpa model yang membawa warnanya sendiri.
    const render::ViewportScene withSlots = view.Scene();
    for (const Vec4& color : withSlots.partColors) {
        CHECK(color.a == doctest::Approx(0.0f));
    }

    // Tanpa pustaka bawaan sama sekali editor tetap menggambar sesuatu, bukan
    // hitam: entity yang tidak terlihat adalah entity yang tidak bisa dipilih.
    SceneView bare;
    bare.Build(world, selection);
    REQUIRE(bare.Scene().meshes.size() == 1);
    CHECK(bare.Scene().meshes[0].color.a > 0.0f);
}

// --- Panel Prefab: template bawaan -------------------------------------------

TEST_CASE("setiap template prefab bawaan bisa dimuat dan berisi yang dijanjikannya") {
    // **Template yang rusak tidak terlihat sebagai galat.** Ia muncul di panel,
    // bisa diklik, dan yang mendarat di scene adalah entity kosong — yang
    // terbaca sebagai "prefabnya tidak bekerja" alih-alih "berkasnya salah".
    // Uji ini membaca berkas yang benar-benar dikirim, bukan salinannya.
    const std::filesystem::path root =
        std::filesystem::path(SIM_BUILTIN_DIR) / "Prefabs";
    REQUIRE(std::filesystem::is_directory(root));

    struct Expected {
        const char* group;
        const char* file;
        const char* name;
        const char* component;
    };
    const std::array<Expected, 8> kTemplates{{
        {"Actors", "Actor", "Actor", "MeshRenderer"},
        {"Actors", "Shader Ball", "Shader Ball", "MeshRenderer"},
        {"Lights", "Directional Light", "Directional Light", "Light"},
        {"Lights", "Point Light", "Point Light", "Light"},
        {"Lights", "Spot Light", "Spot Light", "Light"},
        {"Cameras", "Camera", "Camera", "Camera"},
        {"Environment", "Sky Dome", "Sky Dome", "Sky"},
        {"Environment", "Ground", "Ground", "MeshRenderer"},
    }};

    for (const Expected& expected : kTemplates) {
        const std::filesystem::path path =
            root / expected.group / (std::string(expected.file) + ".simprefab");
        INFO("template " << path.string());
        REQUIRE(std::filesystem::exists(path));

        // Dimuat lewat jalur yang sama persis dengan yang dipakai panel:
        // teksnya di-remap GUID-nya lalu dipulihkan sebagai sub-pohon.
        std::ifstream stream(path);
        REQUIRE(stream);
        const std::string text((std::istreambuf_iterator<char>(stream)),
                               std::istreambuf_iterator<char>());
        REQUIRE_FALSE(text.empty());

        scene::World world;
        std::string rootGuid;
        const std::string remapped = scene::RemapGuids(text, &rootGuid);
        INFO("remap menghasilkan " << remapped.size() << " byte");
        REQUIRE_FALSE(remapped.empty());
        REQUIRE(scene::RestoreSubtree(world, remapped, Uuid{}));

        const scene::Entity entity = world.FindByGuid(Uuid::Parse(rootGuid));
        REQUIRE(entity != scene::kNullEntity);
        CHECK(world.NameOf(entity) == expected.name);

        // Komponen yang menjadi alasan template ini ada. Sebuah "Directional
        // Light" tanpa LightComponent adalah entity kosong bernama lampu.
        const std::string component = expected.component;
        if (component == "MeshRenderer") {
            CHECK(world.Has<scene::MeshRendererComponent>(entity));
        } else if (component == "Light") {
            REQUIRE(world.Has<scene::LightComponent>(entity));
            const auto* light = world.TryGet<scene::LightComponent>(entity);
            REQUIRE(light != nullptr);
            CHECK(light->intensity > 0.0f);
        } else if (component == "Camera") {
            REQUIRE(world.Has<scene::CameraComponent>(entity));
            const auto* camera = world.TryGet<scene::CameraComponent>(entity);
            REQUIRE(camera != nullptr);
            CHECK(camera->farZ > camera->nearZ);
        } else if (component == "Sky") {
            // **Sky Dome bukan mesh.** Ia yang menyalakan langit: level tanpa
            // entity ini tidak menggambar langit sama sekali, dan itu yang
            // membuat adegan interior berhenti membayar pass yang tidak
            // terlihat. Sebuah kubus raksasa di sini akan terlihat benar di
            // panel dan salah di setiap level yang memakainya.
            REQUIRE(world.Has<scene::SkyComponent>(entity));
            const auto* sky = world.TryGet<scene::SkyComponent>(entity);
            REQUIRE(sky != nullptr);
            CHECK(sky->intensity > 0.0f);
            CHECK(sky->source == scene::SkySourceKind::Atmosphere);
            CHECK_FALSE(world.Has<scene::MeshRendererComponent>(entity));
        }
        // Transform selalu ada; tanpanya prefab tidak bisa ditempatkan.
        CHECK(world.Has<scene::TransformComponent>(entity));
    }
}

TEST_CASE("jenis lampu di template benar-benar berbeda") {
    // Ketiga lampu memakai komponen yang sama; yang membedakannya cuma field
    // `type`. Salah menuliskannya menghasilkan tiga template yang kelihatan
    // berbeda di panel dan berperilaku sama di scene.
    const std::filesystem::path root =
        std::filesystem::path(SIM_BUILTIN_DIR) / "Prefabs" / "Lights";

    const std::array<std::pair<const char*, scene::LightType>, 3> kExpected{{
        {"Directional Light", scene::LightType::Directional},
        {"Point Light", scene::LightType::Point},
        {"Spot Light", scene::LightType::Spot},
    }};

    for (const auto& [file, type] : kExpected) {
        const std::filesystem::path path = root / (std::string(file) + ".simprefab");
        std::ifstream stream(path);
        REQUIRE(stream);
        const std::string text((std::istreambuf_iterator<char>(stream)),
                               std::istreambuf_iterator<char>());

        scene::World world;
        std::string rootGuid;
        REQUIRE(scene::RestoreSubtree(world, scene::RemapGuids(text, &rootGuid), Uuid{}));
        const auto* light =
            world.TryGet<scene::LightComponent>(world.FindByGuid(Uuid::Parse(rootGuid)));
        INFO("template " << file);
        REQUIRE(light != nullptr);
        CHECK(light->type == type);
    }
}

TEST_CASE("level bawaan disusun dari template, bukan ditulis ulang di kode") {
    // **Yang dijaga di sini adalah satu definisi, bukan dua.** Sebelumnya level
    // contoh membangun entitynya sendiri, jadi menyunting prefab "Shader Ball"
    // tidak mengubah level baru sama sekali — dan dua definisi yang bergeser
    // sendiri-sendiri baru ketahuan ketika seseorang membandingkannya.
    //
    // Diuji lewat berkasnya, karena itulah yang benar-benar dibaca
    // `CreateStarterLevel`: setiap bagian level bawaan harus punya template
    // yang bisa menghasilkannya.
    const std::filesystem::path prefabs =
        std::filesystem::path(SIM_BUILTIN_DIR) / "Prefabs";

    struct Piece {
        const char* group;
        const char* file;
    };
    const std::array<Piece, 5> kStarter{{
        {"Environment", "Ground"},
        {"Environment", "Sky Dome"},
        {"Lights", "Directional Light"},
        {"Actors", "Shader Ball"},
        {"Cameras", "Camera"},
    }};

    scene::World world;
    for (const Piece& piece : kStarter) {
        const std::filesystem::path path =
            prefabs / piece.group / (std::string(piece.file) + ".simprefab");
        INFO("bagian " << path.string());
        REQUIRE(std::filesystem::exists(path));

        std::ifstream stream(path);
        REQUIRE(stream);
        const std::string text((std::istreambuf_iterator<char>(stream)),
                               std::istreambuf_iterator<char>());
        std::string rootGuid;
        REQUIRE(scene::RestoreSubtree(world, scene::RemapGuids(text, &rootGuid), Uuid{}));
    }

    // Kelimanya berdampingan di satu dunia, seperti di level bawaan.
    CHECK(world.Registry().view<scene::SkyComponent>().size() == 1);
    CHECK(world.Registry().view<scene::CameraComponent>().size() == 1);
    CHECK(world.Registry().view<scene::LightComponent>().size() == 1);
    // Ground dan Shader Ball: dua mesh.
    std::size_t meshes = 0;
    for (const auto raw : world.Registry().view<scene::MeshRendererComponent>()) {
        (void)raw;
        ++meshes;
    }
    CHECK(meshes == 2);
}

TEST_CASE("Ground adalah kotak yang dipipihkan, bukan kubus") {
    // Tebalnya yang membuatnya lantai. Kubus 20x20x20 akan menelan seluruh
    // adegan contoh, dan itu terbaca sebagai "kameranya di dalam sesuatu"
    // alih-alih sebagai prefab yang salah skala.
    const std::filesystem::path path =
        std::filesystem::path(SIM_BUILTIN_DIR) / "Prefabs" / "Environment" / "Ground.simprefab";
    std::ifstream stream(path);
    REQUIRE(stream);
    const std::string text((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());

    scene::World world;
    std::string rootGuid;
    REQUIRE(scene::RestoreSubtree(world, scene::RemapGuids(text, &rootGuid), Uuid{}));
    const scene::Entity ground = world.FindByGuid(Uuid::Parse(rootGuid));
    REQUIRE(ground != scene::kNullEntity);

    const auto* transform = world.TryGet<scene::TransformComponent>(ground);
    REQUIRE(transform != nullptr);
    CHECK(transform->scale.x > 10.0f);
    CHECK(transform->scale.z > 10.0f);
    CHECK(transform->scale.y < 1.0f);

    // Tidak menjatuhkan bayangan: permukaan tanah yang membayangi dirinya
    // sendiri membayar satu pass untuk bayangan yang tak pernah terlihat.
    const auto* mesh = world.TryGet<scene::MeshRendererComponent>(ground);
    REQUIRE(mesh != nullptr);
    CHECK_FALSE(mesh->castShadows);
    CHECK(mesh->receiveShadows);

    // **Dan ia bisa dipijak.** Tanpa collider, menekan Play di level bawaan
    // menjatuhkan segalanya menembus lantai — cacat pertama yang akan ditemui
    // siapa pun yang mencoba fisika untuk pertama kali.
    //
    // Bentuknya kotak, bukan mesh: lantainya memang kubus yang dipipihkan, jadi
    // kotak menggambarkannya persis. Triangle mesh akan lebih mahal untuk
    // jawaban yang sama, dan bentuk itu baru ada sesudah cooking di P4.
    const auto* body = world.TryGet<scene::RigidBodyComponent>(ground);
    REQUIRE(body != nullptr);
    CHECK(body->kind == scene::RigidBodyKind::Static);
    const auto* collider = world.TryGet<scene::ColliderComponent>(ground);
    REQUIRE(collider != nullptr);
    CHECK(collider->shape == scene::ColliderShape::Box);

    // Permukaan atasnya harus tepat di y = 0, kalau tidak benda berhenti
    // melayang atau setengah tenggelam. Setengah-ukuran collider ikut diskalakan
    // transform, jadi angkanya: -0,05 + (0,5 × 0,1) = 0.
    const float topY = transform->position.y + collider->halfExtents.y * transform->scale.y;
    INFO("permukaan atas di y = " << topY);
    CHECK(topY == doctest::Approx(0.0f).epsilon(0.001));
}

TEST_CASE("Play menjalankan fisika, Stop mengembalikan level seperti semula") {
    // **Play dulu tidak berjalan sama sekali tanpa skrip** — seluruh fungsinya
    // dipagari `#if SIM_WITH_LUA` dan langsung kembali bila `scripts` null. Uji
    // ini menjaga perbaikannya: fisika dan Lua adalah dua fitur opsional yang
    // tidak boleh saling mengunci, dan `config.scripts` di bawah memang tidak
    // pernah diisi.
    ScratchDir scratch;
    EditorApp app;
    EditorApp::Config config;
    config.configDir = scratch.path / "config";
    config.projectsRoot = scratch.path / "Documents" / "SimEngine";
    REQUIRE(app.Initialize(config));

    scene::World& world = app.GetWorld();
    const scene::Entity ground = world.Create("Ground");
    {
        auto& body = world.Add<scene::RigidBodyComponent>(ground);
        body.kind = scene::RigidBodyKind::Static;
        auto& collider = world.Add<scene::ColliderComponent>(ground);
        collider.shape = scene::ColliderShape::Plane;
        const float halfAngle = 0.25f * 3.14159265f;
        world.TryGet<scene::TransformComponent>(ground)->rotation =
            Quat(std::cos(halfAngle), 0.0f, 0.0f, std::sin(halfAngle));
        world.MarkTransformDirty(ground);
    }

    const scene::Entity ball = world.Create("Ball");
    {
        world.Add<scene::RigidBodyComponent>(ball);
        auto& collider = world.Add<scene::ColliderComponent>(ball);
        collider.shape = scene::ColliderShape::Sphere;
        collider.radius = 0.5f;
        world.TryGet<scene::TransformComponent>(ball)->position = Vec3(0.0f, 4.0f, 0.0f);
        world.MarkTransformDirty(ball);
    }
    const Uuid ballGuid = world.GuidOf(ball);
    const Uuid groundGuid = world.GuidOf(ground);

    app.Play();
    CHECK(app.IsPlaying());

    if (physics::Available()) {
        REQUIRE(app.GetPhysics().IsValid());
        CHECK(app.GetPhysics().Stats().bodies == 2);

        // Langkah yang sama dengan yang dijalankan `DrawFrame`, tanpa ImGui.
        app.GetPhysics().Step(app.GetWorld(), 180);
        const scene::Entity playBall = app.GetWorld().FindByGuid(ballGuid);
        REQUIRE(playBall != scene::kNullEntity);
        const float restY =
            app.GetWorld().TryGet<scene::TransformComponent>(playBall)->position.y;
        INFO("berhenti di y = " << restY);
        CHECK(restY == doctest::Approx(0.5f).epsilon(0.05));
    }

    app.Stop();
    CHECK_FALSE(app.IsPlaying());
    CHECK_FALSE(app.GetPhysics().IsValid());

    // **Yang dijatuhkan fisika ikut dikembalikan.** Cuplikan sebelum Play sudah
    // ada sebelum fisika ada; yang diuji di sini adalah bahwa ia tetap menutupi
    // perubahan yang kali ini datang dari solver, bukan dari skrip.
    const scene::Entity restoredBall = app.GetWorld().FindByGuid(ballGuid);
    REQUIRE(restoredBall != scene::kNullEntity);
    CHECK(app.GetWorld().TryGet<scene::TransformComponent>(restoredBall)->position.y ==
          doctest::Approx(4.0f));

    // Dan komponennya ikut kembali utuh. Ini menguji serialisasi keduanya
    // sekaligus: cuplikan Play menempuh jalur simpan-muat yang sama dengan
    // berkas level, jadi field yang tidak terpantul akan hilang di sini —
    // termasuk enum `shape` dan `Vec3 offset`, dua bentuk yang paling mudah
    // luput dari serialisasi yang digerakkan refleksi.
    const auto* restoredBody = app.GetWorld().TryGet<scene::RigidBodyComponent>(restoredBall);
    REQUIRE(restoredBody != nullptr);
    CHECK(restoredBody->kind == scene::RigidBodyKind::Dynamic);
    const auto* restoredCollider =
        app.GetWorld().TryGet<scene::ColliderComponent>(restoredBall);
    REQUIRE(restoredCollider != nullptr);
    CHECK(restoredCollider->shape == scene::ColliderShape::Sphere);
    CHECK(restoredCollider->radius == doctest::Approx(0.5f));

    const scene::Entity restoredGround = app.GetWorld().FindByGuid(groundGuid);
    REQUIRE(restoredGround != scene::kNullEntity);
    const auto* groundBody = app.GetWorld().TryGet<scene::RigidBodyComponent>(restoredGround);
    REQUIRE(groundBody != nullptr);
    CHECK(groundBody->kind == scene::RigidBodyKind::Static);

    app.Shutdown();
}

TEST_CASE("satu seretan whitebox menghasilkan tepat satu entri undo") {
    // **Kriteria terima W5**, aturan yang sama dengan seretan gizmo transform:
    // seretan nyata menghasilkan satu perintah per frame, dan semuanya harus
    // menyatu. Kalau tidak, membatalkannya butuh puluhan Ctrl+Z — dan itu yang
    // dirasakan pengguna sebagai undo yang rusak.
    // Lewat store, karena itulah yang dipegang perintahnya sekarang: yang
    // membatalkan bentuk juga harus menaikkan versi yang dipakai viewport.
    WhiteboxStore store;
    const Uuid guid = Uuid::Generate();
    whitebox::WhiteboxMesh& box = store.Adopt(guid, whitebox::WhiteboxMesh::MakeCube());
    const whitebox::WhiteboxData start = box.ToData();
    const uint64_t versionAtStart = store.Version(guid);

    CommandHistory history;
    const std::vector<whitebox::PolygonHandle> sides = box.Polygons().Polygons();
    REQUIRE(sides.size() == 6);

    // Empat puluh frame seretan: tiap frame mendorong sedikit lebih jauh dari
    // keadaan awal, persis seperti gizmo yang dipegang.
    for (int frame = 1; frame <= 40; ++frame) {
        whitebox::WhiteboxMesh preview;
        std::string error;
        REQUIRE(whitebox::WhiteboxMesh::Build(preview, start, error));
        const whitebox::PolygonHandle top =
            preview.Polygons().Polygons()[static_cast<std::size_t>(sides.front())];
        const whitebox::WhiteboxData before = box.ToData();
        REQUIRE(preview.Extrude(top, static_cast<float>(frame) * 0.05f).ok);

        history.Execute(std::make_unique<WhiteboxEditCommand>(&store, guid, before,
                                                              preview.ToData(), "Extrude"));
    }
    history.CloseMergeGroup();

    INFO(history.Entries().size() << " entri");
    CHECK(history.Entries().size() == 1);

    // Membatalkannya sekali mengembalikan bentuk semula **persis**.
    REQUIRE(history.Undo());
    // Dan menandainya berubah, supaya yang tergambar ikut kembali. Undo yang
    // membetulkan data tetapi meninggalkan layar adalah undo yang tidak
    // dipercaya orang.
    CHECK(store.Version(guid) > versionAtStart);
    CHECK(whitebox::SaveToString(box) ==
          [&] {
              whitebox::WhiteboxMesh original;
              std::string error;
              whitebox::WhiteboxMesh::Build(original, start, error);
              return whitebox::SaveToString(original);
          }());

    // Dan mengulanginya membawa kembali ke ujung seretan, bukan ke tengahnya.
    REQUIRE(history.Redo());
    CHECK(box.Mesh().FaceCount() == 10);
    CHECK(box.CheckInvariants().ok);
}

TEST_CASE("menetapkan material sisi bisa dibatalkan, dan sisi berbeda tidak digabung") {
    WhiteboxStore store;
    const Uuid guid = Uuid::Generate();
    whitebox::WhiteboxMesh& box = store.Adopt(guid, whitebox::WhiteboxMesh::MakeCube());
    const std::vector<whitebox::PolygonHandle> sides = box.Polygons().Polygons();
    REQUIRE(sides.size() == 6);

    CommandHistory history;
    history.Execute(std::make_unique<SetPolygonMaterialCommand>(&store, guid, sides[0], 2));
    history.Execute(std::make_unique<SetPolygonMaterialCommand>(&store, guid, sides[0], 5));
    // Sisi yang sama, satu gerakan: menyatu.
    CHECK(history.Entries().size() == 1);
    CHECK(box.PolygonMaterial(sides[0]) == 5);

    // **Sisi berbeda adalah keputusan baru pengguna.** Menggabungkannya berarti
    // satu Ctrl+Z membatalkan dua penetapan yang tidak berhubungan.
    history.Execute(std::make_unique<SetPolygonMaterialCommand>(&store, guid, sides[1], 3));
    CHECK(history.Entries().size() == 2);

    REQUIRE(history.Undo());
    CHECK(box.PolygonMaterial(sides[1]) == whitebox::kNoMaterial);
    CHECK(box.PolygonMaterial(sides[0]) == 5);

    REQUIRE(history.Undo());
    CHECK(box.PolygonMaterial(sides[0]) == whitebox::kNoMaterial);
}
