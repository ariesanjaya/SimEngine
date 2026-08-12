#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Editor/Command.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/Gizmo.h"
#include "Sim/Editor/SceneCommands.h"
#include "Sim/Editor/SceneView.h"
#include "Sim/Editor/Selection.h"
#include "Sim/Editor/SkinnedPreview.h"
#include "Sim/Scene/Components.h"
#include "Sim/Scene/Serialization.h"
#include "Sim/Scene/World.h"

#include <doctest/doctest.h>

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
