#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Core/Uuid.h"
#include "Sim/Reflect/TypeRegistry.h"
#include "Sim/Scene/AssetUsage.h"
#include "Sim/Scene/ComponentRegistry.h"
#include "Sim/Scene/Project.h"
#include "Sim/Scene/Serialization.h"
#include "Sim/Scene/World.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace sim;
using namespace sim::scene;

namespace {

/// Membangun dunia bertingkat: `roots` akar, masing-masing dengan rantai anak
/// sedalam `depth`, tiap tingkat punya `branch` anak.
std::size_t BuildTree(World& world, int roots, int depth, int branch) {
    std::size_t created = 0;
    std::vector<Entity> current;
    for (int r = 0; r < roots; ++r) {
        const Entity entity = world.Create("Root" + std::to_string(r));
        MeshRendererComponent mesh;
        mesh.receiveShadows = false;
        world.Add<MeshRendererComponent>(entity, mesh);
        current.push_back(entity);
        ++created;
    }
    for (int d = 0; d < depth; ++d) {
        std::vector<Entity> next;
        for (const Entity parent : current) {
            for (int b = 0; b < branch; ++b) {
                const Entity child =
                    world.Create("Node" + std::to_string(d) + "_" + std::to_string(b), parent);
                auto* transform = world.TryGet<TransformComponent>(child);
                transform->position = Vec3(static_cast<float>(b), static_cast<float>(d), 0.5f);
                if ((b % 3) == 0) {
                    world.Add<LightComponent>(child);
                }
                next.push_back(child);
                ++created;
            }
        }
        current = std::move(next);
    }
    return created;
}

}  // namespace

TEST_CASE("Uuid bolak-balik lewat teks tanpa berubah") {
    const Uuid id = Uuid::Generate();
    CHECK(id.IsValid());
    const std::string text = id.ToString();
    CHECK(text.size() == 36);
    CHECK(Uuid::Parse(text) == id);

    // Format yang salah harus menghasilkan Uuid tidak valid, bukan nilai ngawur.
    CHECK_FALSE(Uuid::Parse("").IsValid());
    CHECK_FALSE(Uuid::Parse("not-a-uuid").IsValid());
    CHECK_FALSE(Uuid::Parse(text.substr(0, 35)).IsValid());
    std::string broken = text;
    broken[8] = 'x';  // tanda hubung di posisi salah
    CHECK_FALSE(Uuid::Parse(broken).IsValid());
}

TEST_CASE("Semua tipe komponen lolos validasi reflection") {
    World world;  // konstruktornya yang mendaftarkan komponen
    std::vector<std::string> problems;
    const bool valid = reflect::TypeRegistry::Get().Validate(problems);
    for (const std::string& problem : problems) {
        MESSAGE(problem);
    }
    CHECK(valid);
    CHECK(ComponentRegistry::Get().All().size() >= 7);
}

TEST_CASE("Transform anak mengikuti saat induk berpindah") {
    World world;
    const Entity parent = world.Create("Parent");
    const Entity child = world.Create("Child", parent);

    world.TryGet<TransformComponent>(child)->position = Vec3(1.0f, 0.0f, 0.0f);
    world.MarkTransformDirty(child);

    Vec3 worldPos = Vec3(world.WorldMatrix(child)[3]);
    CHECK(worldPos.x == doctest::Approx(1.0f));

    // Induk bergeser: posisi dunia anak harus ikut, tanpa perlu disentuh.
    world.TryGet<TransformComponent>(parent)->position = Vec3(10.0f, 5.0f, 0.0f);
    world.MarkTransformDirty(parent);

    worldPos = Vec3(world.WorldMatrix(child)[3]);
    CHECK(worldPos.x == doctest::Approx(11.0f));
    CHECK(worldPos.y == doctest::Approx(5.0f));

    // Skala induk juga merambat.
    world.TryGet<TransformComponent>(parent)->scale = Vec3(2.0f, 2.0f, 2.0f);
    world.MarkTransformDirty(parent);
    worldPos = Vec3(world.WorldMatrix(child)[3]);
    CHECK(worldPos.x == doctest::Approx(12.0f));
}

TEST_CASE("Siklus parent ditolak") {
    World world;
    const Entity a = world.Create("A");
    const Entity b = world.Create("B", a);
    const Entity c = world.Create("C", b);

    // Menjadikan keturunan sebagai induk akan membuat hierarki jadi cincin.
    CHECK_FALSE(world.SetParent(a, c));
    CHECK_FALSE(world.SetParent(a, b));
    CHECK_FALSE(world.SetParent(a, a));
    // Hierarki harus tetap utuh setelah penolakan.
    CHECK(world.ParentOf(b) == a);
    CHECK(world.ParentOf(c) == b);
    CHECK(world.ParentOf(a) == kNullEntity);

    // Memindahkan ke atas tetap sah.
    CHECK(world.SetParent(c, a));
    CHECK(world.ParentOf(c) == a);
    CHECK(world.ChildrenOf(b).empty());
}

TEST_CASE("Menghapus induk ikut menghapus keturunannya") {
    World world;
    const Entity root = world.Create("Root");
    const Entity child = world.Create("Child", root);
    const Entity grandchild = world.Create("Grandchild", child);
    const Entity other = world.Create("Other");
    CHECK(world.Count() == 4);

    const Uuid childGuid = world.GuidOf(child);

    world.Destroy(root);
    CHECK_FALSE(world.IsAlive(root));
    CHECK_FALSE(world.IsAlive(child));
    CHECK_FALSE(world.IsAlive(grandchild));
    CHECK(world.IsAlive(other));
    CHECK(world.Count() == 1);
    // Indeks GUID ikut dibersihkan, kalau tidak pencarian akan mengembalikan
    // entity mati.
    CHECK(world.FindByGuid(childGuid) == kNullEntity);
    CHECK(world.Roots().size() == 1);
}

TEST_CASE("Level 5000 entity bolak-balik byte-per-byte sama") {
    World world;
    // 5 akar × 5 cabang sedalam 5 = 3905, ditambah akar tambahan agar > 5000.
    const std::size_t created = BuildTree(world, 8, 5, 4);
    MESSAGE("entities created: " << created);
    REQUIRE(created >= 5000);

    const std::string first = SaveLevelToString(world);

    World reloaded;
    const LevelIoResult load = LoadLevelFromString(reloaded, first);
    REQUIRE(load.ok);
    CHECK(load.entityCount == world.Count());
    CHECK_FALSE(load.migrated);

    const std::string second = SaveLevelToString(reloaded);
    // Byte-per-byte: GUID, urutan entity, urutan komponen, dan presisi angka
    // semuanya harus bertahan utuh.
    CHECK(first == second);
}

TEST_CASE("Hierarki dan nilai komponen bertahan lewat simpan-muat") {
    World world;
    const Entity parent = world.Create("Parent");
    const Entity child = world.Create("Child", parent);
    world.TryGet<TransformComponent>(child)->position = Vec3(1.5f, -2.25f, 3.125f);
    auto& light = world.Add<LightComponent>(child);
    light.type = LightType::Spot;
    light.color = Vec3(0.25f, 0.5f, 0.75f);
    light.intensity = 4.5f;

    const Uuid childGuid = world.GuidOf(child);
    const std::string text = SaveLevelToString(world);

    World reloaded;
    REQUIRE(LoadLevelFromString(reloaded, text).ok);

    const Entity loadedChild = reloaded.FindByGuid(childGuid);
    REQUIRE(IsValid(loadedChild));
    CHECK(reloaded.NameOf(loadedChild) == "Child");
    CHECK(reloaded.NameOf(reloaded.ParentOf(loadedChild)) == "Parent");

    const auto* transform = reloaded.TryGet<TransformComponent>(loadedChild);
    REQUIRE(transform != nullptr);
    CHECK(transform->position.x == doctest::Approx(1.5f));
    CHECK(transform->position.z == doctest::Approx(3.125f));

    const auto* loadedLight = reloaded.TryGet<LightComponent>(loadedChild);
    REQUIRE(loadedLight != nullptr);
    // Enum disimpan sebagai nama, jadi harus kembali sebagai nilai yang sama.
    CHECK(loadedLight->type == LightType::Spot);
    CHECK(loadedLight->intensity == doctest::Approx(4.5f));
    CHECK(loadedLight->color.y == doctest::Approx(0.5f));
}

TEST_CASE("Berkas skema versi 1 dimigrasikan, bukan ditolak") {
    // Rotasi versi 1 adalah sudut Euler derajat; versi 2 quaternion.
    const std::string legacy = R"({
  "schemaVersion": 1,
  "entities": [
    {
      "guid": "11111111-2222-4333-8444-555555555555",
      "components": {
        "Name": { "name": "Legacy" },
        "Transform": {
          "position": [1.0, 2.0, 3.0],
          "rotation": [0.0, 90.0, 0.0],
          "scale": [1.0, 1.0, 1.0]
        }
      }
    }
  ]
})";

    World world;
    const LevelIoResult result = LoadLevelFromString(world, legacy);
    REQUIRE(result.ok);
    CHECK(result.sourceVersion == 1);
    CHECK(result.migrated);
    CHECK(world.Count() == 1);

    const Entity entity = world.FindByGuid(Uuid::Parse("11111111-2222-4333-8444-555555555555"));
    REQUIRE(IsValid(entity));
    const auto* transform = world.TryGet<TransformComponent>(entity);
    REQUIRE(transform != nullptr);
    CHECK(transform->position.y == doctest::Approx(2.0f));

    // 90° di sekitar sumbu Y: memutar +Z menjadi +X.
    const Vec3 rotated = transform->rotation * Vec3(0.0f, 0.0f, 1.0f);
    CHECK(rotated.x == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(rotated.z == doctest::Approx(0.0f).epsilon(0.001));

    // Menyimpan ulang harus memakai skema terbaru. Versinya diambil dari
    // konstantanya, bukan ditulis sebagai angka: uji ini tentang "berkas lama
    // naik ke versi terbaru", dan mematoknya ke satu angka membuatnya gagal
    // setiap kali skema berubah karena alasan yang sama sekali tidak terkait.
    const std::string saved = SaveLevelToString(world);
    CHECK(saved.find("\"schemaVersion\": " + std::to_string(kLevelSchemaVersion)) !=
          std::string::npos);
}

TEST_CASE("Berkas rusak ditolak dengan pesan, bukan crash") {
    World world;
    CHECK_FALSE(LoadLevelFromString(world, "{").ok);
    CHECK_FALSE(LoadLevelFromString(world, "{}").ok);
    CHECK_FALSE(LoadLevelFromString(world, R"({"schemaVersion":2,"entities":[{}]})").ok);

    const LevelIoResult future =
        LoadLevelFromString(world, R"({"schemaVersion":9999,"entities":[]})");
    CHECK_FALSE(future.ok);
    CHECK(future.error.find("newer editor") != std::string::npos);
}

TEST_CASE("Komponen tak dikenal dilewati, sisanya tetap dimuat") {
    const std::string text = R"({
  "schemaVersion": 2,
  "entities": [
    {
      "guid": "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
      "components": {
        "Name": { "name": "Kept" },
        "FromSomePlugin": { "whatever": 1 }
      }
    }
  ]
})";
    World world;
    const LevelIoResult result = LoadLevelFromString(world, text);
    REQUIRE(result.ok);
    CHECK(world.Count() == 1);
    CHECK(world.NameOf(world.Roots().front()) == "Kept");
}

TEST_CASE("Prefab dibuat ulang dengan GUID baru tapi struktur sama") {
    World source;
    const Entity root = source.Create("Turret");
    source.TryGet<TransformComponent>(root)->position = Vec3(0.0f, 1.0f, 0.0f);
    source.Add<MeshRendererComponent>(root, MeshRendererComponent{});
    const Entity barrel = source.Create("Barrel", root);
    source.TryGet<TransformComponent>(barrel)->position = Vec3(0.0f, 0.5f, 0.0f);
    source.Add<LightComponent>(barrel);

    const auto path = std::filesystem::temp_directory_path() / "simengine-test.simprefab";
    REQUIRE(SavePrefab(source, root, path));

    World target;
    const Entity host = target.Create("Level");
    const Entity first = InstantiatePrefab(target, path, host);
    const Entity second = InstantiatePrefab(target, path, host);
    REQUIRE(IsValid(first));
    REQUIRE(IsValid(second));

    // Dua salinan harus bisa dibedakan, kalau tidak referensi ke salah satunya
    // akan menunjuk keduanya.
    CHECK(target.GuidOf(first) != target.GuidOf(second));
    CHECK(target.GuidOf(first) != source.GuidOf(root));

    // Struktur dan nilainya tetap sama.
    CHECK(target.NameOf(first) == "Turret");
    REQUIRE(target.ChildrenOf(first).size() == 1);
    const Entity copiedBarrel = target.ChildrenOf(first).front();
    CHECK(target.NameOf(copiedBarrel) == "Barrel");
    CHECK(target.TryGet<LightComponent>(copiedBarrel) != nullptr);
    CHECK(target.TryGet<MeshRendererComponent>(first)->mesh ==
          source.TryGet<MeshRendererComponent>(root)->mesh);
    CHECK(target.ParentOf(first) == host);

    std::filesystem::remove(path);
}

TEST_CASE("Project bolak-balik lewat berkas") {
    const auto dir = std::filesystem::temp_directory_path() / "simengine-test-project";
    std::filesystem::remove_all(dir);

    Project saved;
    saved.name = "Demo";
    saved.assetsPath = "Content";
    saved.startupLevel = "Levels/start.simlevel";
    REQUIRE(SaveProject(saved, dir));

    Project loaded;
    std::string error;
    REQUIRE(LoadProject(loaded, dir, error));
    CHECK(loaded.name == "Demo");
    CHECK(loaded.assetsPath == "Content");
    CHECK(loaded.AssetsDirectory() == dir / "Content");
    CHECK(loaded.StartupLevelPath() == dir / "Levels/start.simlevel");

    Project missing;
    CHECK_FALSE(LoadProject(missing, dir / "nope", error));
    CHECK_FALSE(error.empty());

    std::filesystem::remove_all(dir);
}

TEST_CASE("properti terekspos script ikut tersimpan bersama level") {
    World world;
    const Entity entity = world.Create("Turret");
    auto& component = world.Add<ScriptComponent>(entity);
    component.script = AssetRef{Uuid::Generate()};
    component.properties = {
        {"speed", ScriptPropertyKind::Number, 9.0f, false, {}},
        {"loop", ScriptPropertyKind::Bool, 0.0f, true, {}},
        {"label", ScriptPropertyKind::Text, 0.0f, false, "patrol"},
    };

    const std::string text = SaveLevelToString(world);
    World reloaded;
    REQUIRE(LoadLevelFromString(reloaded, text).ok);

    const Entity restored = reloaded.FindByGuid(world.GuidOf(entity));
    REQUIRE(reloaded.IsAlive(restored));
    const auto* after = reloaded.TryGet<ScriptComponent>(restored);
    REQUIRE(after != nullptr);
    REQUIRE(after->properties.size() == 3);
    // Urutannya ikut dipertahankan: ia menentukan susunan di Inspector.
    CHECK(after->properties[0].name == "speed");
    CHECK(after->properties[0].number == doctest::Approx(9.0f));
    CHECK(after->properties[1].kind == ScriptPropertyKind::Bool);
    CHECK(after->properties[1].flag);
    CHECK(after->properties[2].text == "patrol");
}

TEST_CASE("Pemakai aset ditemukan lewat reflection, termasuk yang bersarang") {
    RegisterCoreComponents();
    World world;

    const Uuid texture = Uuid::Generate();
    const Uuid unused = Uuid::Generate();

    const Entity plain = world.Create("Plain");
    const Entity user = world.Create("User");
    world.Add<MeshRendererComponent>(user).materials.push_back(AssetRef{texture});
    const Entity child = world.Create("Child", user);
    world.Add<MeshRendererComponent>(child).mesh = AssetRef{texture};

    // Bersarang di dalam vektor struct: ScriptComponent::properties tidak
    // memuat AssetRef, jadi yang diuji di sini adalah bahwa penelusurannya
    // menembus vektor tanpa tersandung.
    const Entity scripted = world.Create("Scripted");
    auto& script = world.Add<ScriptComponent>(scripted);
    script.script = AssetRef{unused};
    script.properties = {{"speed", ScriptPropertyKind::Number, 1.0f, false, {}}};

    const std::vector<Entity> users = EntitiesUsingAsset(world, texture);
    REQUIRE(users.size() == 2);
    // Urutannya mengikuti penelusuran hierarki dari akar, jadi induk lebih dulu.
    CHECK(world.NameOf(users[0]) == "User");
    CHECK(world.NameOf(users[1]) == "Child");

    // Yang tidak memakainya tidak ikut terbawa.
    CHECK(std::find(users.begin(), users.end(), plain) == users.end());
    CHECK(std::find(users.begin(), users.end(), scripted) == users.end());

    // Aset lain tetap ketemu lewat jalur yang sama.
    const std::vector<Entity> others = EntitiesUsingAsset(world, unused);
    REQUIRE(others.size() == 1);
    CHECK(world.NameOf(others[0]) == "Scripted");

    // GUID kosong tidak boleh cocok dengan AssetRef kosong milik entity mana
    // pun — kalau tidak, menanyakan "siapa memakai aset tak-valid" akan
    // menjawab "hampir semuanya".
    CHECK(EntitiesUsingAsset(world, Uuid{}).empty());
}

TEST_CASE("Berkas lama tanpa sourceRadius memakai nilai bawaannya") {
    // Menambah medan ke sebuah komponen tidak menaikkan versi skema: pembacaan
    // melewatkan medan yang tidak ada di JSON, jadi berkas lama mendapat nilai
    // bawaan konstruktornya. Test ini yang menjadikan itu janji, bukan
    // pengamatan — pembacaan yang suatu saat menolak medan yang hilang akan
    // membuat setiap level yang pernah disimpan gagal dibuka.
    const std::string legacy = R"({
  "schemaVersion": 2,
  "entities": [
    {
      "guid": "33333333-4444-4555-8666-777777777777",
      "components": {
        "Name": { "name": "Old Lamp" },
        "Light": { "type": "Point", "intensity": 3.0, "range": 25.0 }
      }
    }
  ]
})";

    World world;
    const LevelIoResult result = LoadLevelFromString(world, legacy);
    REQUIRE(result.ok);

    const Entity entity = world.FindByGuid(Uuid::Parse("33333333-4444-4555-8666-777777777777"));
    REQUIRE(IsValid(entity));
    const auto* light = world.TryGet<LightComponent>(entity);
    REQUIRE(light != nullptr);

    // Yang ada di berkas terbaca apa adanya...
    CHECK(light->intensity == doctest::Approx(3.0f));
    CHECK(light->range == doctest::Approx(25.0f));
    // ...dan yang tidak ada memakai bawaannya, yaitu satu sentimeter — angka
    // yang dulu tertanam di dalam shader dan kini bisa disunting.
    CHECK(light->sourceRadius == doctest::Approx(0.01f));
}

TEST_CASE("sourceRadius bolak-balik lewat berkas") {
    World world;
    const Entity entity = world.Create("Lamp");
    auto& light = world.Add<LightComponent>(entity);
    light.type = LightType::Point;
    light.sourceRadius = 0.35f;

    World reloaded;
    REQUIRE(LoadLevelFromString(reloaded, SaveLevelToString(world)).ok);
    const auto* loaded = reloaded.TryGet<LightComponent>(
        reloaded.FindByGuid(world.GuidOf(entity)));
    REQUIRE(loaded != nullptr);
    CHECK(loaded->sourceRadius == doctest::Approx(0.35f));
}
