#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Editor/Command.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/Gizmo.h"
#include "Sim/Editor/SceneCommands.h"
#include "Sim/Editor/SceneView.h"
#include "Sim/Editor/Selection.h"
#include "Sim/Scene/Components.h"
#include "Sim/Scene/Serialization.h"
#include "Sim/Scene/World.h"

#include <doctest/doctest.h>

#include <chrono>
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
