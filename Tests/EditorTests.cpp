#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Editor/Actions.h"
#include "Sim/Render/IViewportRenderer.h"
#include "Sim/Core/TaskPool.h"
#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Editor/MaterialPrograms.h"
#include "Sim/Material/MaterialGraph.h"
#include "Sim/Material/MaterialNodeCatalog.h"
#include "Sim/Editor/Command.h"
#include "Sim/Editor/DockLayout.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelManager.h"
#include "Sim/Editor/Selection.h"
#include "Sim/Editor/ToolApproval.h"
#include "Sim/Editor/SourceImport.h"

#include <doctest/doctest.h>
#include <imgui_internal.h>

#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>
#include <future>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <unistd.h>
#include <vector>
#include <cstdlib>

using namespace sim;
using namespace sim::editor;

namespace {

/// Command uji: menyetel sebuah int, dan bisa digabung dengan command lain
/// yang menyasar variabel yang sama.
class SetIntCommand final : public ICommand {
public:
    SetIntCommand(int* target, int before, int after)
        : target_(target), before_(before), after_(after) {}

    void Do() override { *target_ = after_; }
    void Undo() override { *target_ = before_; }
    std::string Name() const override { return "Set " + std::to_string(after_); }

    bool MergeWith(const ICommand& next) override {
        const auto* other = dynamic_cast<const SetIntCommand*>(&next);
        if (other == nullptr || other->target_ != target_) {
            return false;
        }
        after_ = other->after_;
        return true;
    }

    std::size_t MemoryCost() const override { return 128; }

private:
    int* target_;
    int before_;
    int after_;
};

}  // namespace

TEST_CASE("Undo dan redo mengembalikan nilai persis") {
    CommandHistory history;
    int value = 0;

    history.Execute<SetIntCommand>(&value, 0, 10);
    history.CloseMergeGroup();
    history.Execute<SetIntCommand>(&value, 10, 20);
    CHECK(value == 20);
    CHECK(history.Entries().size() == 2);

    CHECK(history.Undo());
    CHECK(value == 10);
    CHECK(history.Undo());
    CHECK(value == 0);
    CHECK_FALSE(history.Undo());

    CHECK(history.Redo());
    CHECK(value == 10);
    CHECK(history.Redo());
    CHECK(value == 20);
    CHECK_FALSE(history.Redo());
}

TEST_CASE("Seretan panjang menjadi satu entri undo") {
    CommandHistory history;
    int value = 0;

    // Meniru satu gerakan seret: puluhan perubahan berturut-turut tanpa
    // kelompok penggabungan ditutup di antaranya.
    for (int step = 1; step <= 50; ++step) {
        history.Execute<SetIntCommand>(&value, 0, step);
    }
    CHECK(value == 50);
    REQUIRE(history.Entries().size() == 1);

    // Satu undo harus mengembalikan seluruh seretan, bukan satu langkah.
    CHECK(history.Undo());
    CHECK(value == 0);

    // Setelah kelompok ditutup, perubahan berikutnya membuat entri baru.
    history.Redo();
    history.CloseMergeGroup();
    history.Execute<SetIntCommand>(&value, 50, 99);
    CHECK(history.Entries().size() == 2);
}

TEST_CASE("Command baru memangkas cabang redo") {
    CommandHistory history;
    int value = 0;

    history.Execute<SetIntCommand>(&value, 0, 1);
    history.CloseMergeGroup();
    history.Execute<SetIntCommand>(&value, 1, 2);
    history.CloseMergeGroup();
    history.Undo();
    CHECK(history.CanRedo());

    history.Execute<SetIntCommand>(&value, 1, 7);
    CHECK_FALSE(history.CanRedo());
    CHECK(history.Entries().size() == 2);
    CHECK(value == 7);
}

TEST_CASE("Transaksi menjadi satu entri dan dibatalkan terbalik") {
    CommandHistory history;
    int a = 0;
    int b = 0;

    history.BeginTransaction("Move two things");
    history.Execute<SetIntCommand>(&a, 0, 5);
    history.Execute<SetIntCommand>(&b, 0, 9);
    history.EndTransaction();

    REQUIRE(history.Entries().size() == 1);
    CHECK(history.Entries().front().name == "Move two things");
    CHECK(a == 5);
    CHECK(b == 9);

    history.Undo();
    CHECK(a == 0);
    CHECK(b == 0);
    history.Redo();
    CHECK(a == 5);
    CHECK(b == 9);
}

TEST_CASE("JumpTo mencapai titik mana pun di history") {
    CommandHistory history;
    int value = 0;
    for (int step = 1; step <= 4; ++step) {
        history.Execute<SetIntCommand>(&value, step - 1, step);
        history.CloseMergeGroup();
    }
    CHECK(value == 4);

    history.JumpTo(2);
    CHECK(value == 2);
    history.JumpTo(0);
    CHECK(value == 0);
    history.JumpTo(4);
    CHECK(value == 4);
}

TEST_CASE("Batas memori membuang entri terlama") {
    CommandHistory history;
    history.SetMemoryBudget(500);  // 128 byte per command, jadi muat ~3
    int value = 0;

    for (int step = 1; step <= 10; ++step) {
        history.Execute<SetIntCommand>(&value, step - 1, step);
        history.CloseMergeGroup();
    }
    CHECK(history.MemoryUsed() <= 500);
    CHECK(history.Entries().size() < 10);
    // Entri yang tersisa harus yang terbaru: yang lama justru paling tidak
    // mungkin dibutuhkan lagi.
    CHECK(history.Entries().back().name == "Set 10");
}

TEST_CASE("Dirty flag mengikuti titik simpan") {
    CommandHistory history;
    int value = 0;
    CHECK_FALSE(history.IsDirty());

    history.Execute<SetIntCommand>(&value, 0, 1);
    CHECK(history.IsDirty());

    history.MarkSaved();
    CHECK_FALSE(history.IsDirty());

    history.Undo();
    CHECK(history.IsDirty());
    history.Redo();
    CHECK_FALSE(history.IsDirty());
}

TEST_CASE("Seleksi mempertahankan urutan dan melaporkan objek acuan") {
    Selection selection;
    CHECK(selection.Empty());
    CHECK(selection.Primary() == kInvalidSelection);

    selection.Add(7);
    selection.Add(3);
    selection.Add(9);
    CHECK(selection.Count() == 3);
    // Primary = yang terakhir ditambahkan, bukan yang terkecil atau pertama.
    CHECK(selection.Primary() == 9);

    selection.Add(3);  // duplikat diabaikan
    CHECK(selection.Count() == 3);

    selection.Toggle(3);
    CHECK_FALSE(selection.Contains(3));
    CHECK(selection.Count() == 2);

    const uint64_t version = selection.Version();
    selection.Remove(1234);  // tidak ada, tidak boleh menaikkan versi
    CHECK(selection.Version() == version);

    selection.SelectOnly(42);
    CHECK(selection.Count() == 1);
    CHECK(selection.Primary() == 42);
    CHECK(selection.Version() > version);
}

TEST_CASE("Teks pintasan bolak-balik tanpa berubah") {
    struct Case {
        ImGuiKeyChord chord;
        const char* text;
    };
    const Case cases[] = {
        {ImGuiMod_Ctrl | ImGuiKey_Z, "Ctrl+Z"},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, "Ctrl+Shift+Z"},
        {ImGuiMod_Alt | ImGuiKey_F4, "Alt+F4"},
        {ImGuiKey_Delete, "Delete"},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiMod_Alt | ImGuiKey_5, "Ctrl+Shift+Alt+5"},
    };
    for (const Case& item : cases) {
        CHECK(ActionRegistry::ChordToString(item.chord) == item.text);
        CHECK(ActionRegistry::ChordFromString(item.text) == item.chord);
    }

    // Teks yang tidak dikenal harus ditolak, bukan menghasilkan chord ngawur.
    CHECK(ActionRegistry::ChordFromString("Ctrl+Nonsense") == ImGuiKey_None);
    CHECK(ActionRegistry::ChordFromString("Hyper+A") == ImGuiKey_None);
}

TEST_CASE("Pintasan yang diubah bertahan lewat berkas config") {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "simengine-test-shortcuts.json";
    std::filesystem::remove(path);

    auto makeRegistry = []() {
        auto registry = std::make_unique<ActionRegistry>();
        registry->Register(Action{"edit.undo", "Undo", "Edit", "", ImGuiMod_Ctrl | ImGuiKey_Z,
                                  []() {}, {}});
        registry->Register(Action{"edit.redo", "Redo", "Edit", "", ImGuiMod_Ctrl | ImGuiKey_Y,
                                  []() {}, {}});
        return registry;
    };

    {
        auto registry = makeRegistry();
        registry->SetShortcut("edit.redo", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z);
        REQUIRE(registry->Save(path));
    }
    {
        auto registry = makeRegistry();
        REQUIRE(registry->Load(path));
        CHECK(registry->Shortcut("edit.redo") == (ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z));
        // Yang tidak diubah tetap memakai bawaan.
        CHECK(registry->Shortcut("edit.undo") == (ImGuiMod_Ctrl | ImGuiKey_Z));
    }

    std::filesystem::remove(path);
}

TEST_CASE("Bentrokan pintasan terdeteksi") {
    ActionRegistry registry;
    registry.Register(
        Action{"a.one", "One", "A", "", ImGuiMod_Ctrl | ImGuiKey_S, []() {}, {}});
    registry.Register(Action{"a.two", "Two", "A", "", ImGuiKey_None, []() {}, {}});

    CHECK(registry.FindConflict(ImGuiMod_Ctrl | ImGuiKey_S, "a.two") == "a.one");
    // Aksi yang sama tidak dihitung bentrok dengan dirinya sendiri.
    CHECK(registry.FindConflict(ImGuiMod_Ctrl | ImGuiKey_S, "a.one").empty());
    CHECK(registry.FindConflict(ImGuiMod_Ctrl | ImGuiKey_Q, "a.two").empty());
}

TEST_CASE("Aksi tidak aktif tidak bisa dipanggil") {
    ActionRegistry registry;
    int calls = 0;
    bool allowed = false;
    registry.Register(Action{"test.gated", "Gated", "Test", "", ImGuiKey_None,
                             [&calls]() { ++calls; }, [&allowed]() { return allowed; }});

    CHECK_FALSE(registry.Invoke("test.gated"));
    CHECK(calls == 0);

    allowed = true;
    CHECK(registry.Invoke("test.gated"));
    CHECK(calls == 1);
}

namespace {

/// Panel uji: PanelManager tidak menggambarnya di test, jadi OnDraw kosong.
class DummyPanel final : public Panel {
public:
    explicit DummyPanel(std::string id)
        : Panel(std::move(id), "Dummy", PanelCategory::Authoring) {}
    void OnDraw(EditorContext& /*context*/) override {}
};

}  // namespace

TEST_CASE("Keadaan panel yang didaftarkan setelah LoadState tetap dipulihkan") {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "simengine-test-panels.json";
    std::filesystem::remove(path);

    {
        PanelManager panels;
        panels.Add(std::make_unique<DummyPanel>("lua.notes"));
        panels.Find("lua.notes")->SetOpen(false);
        REQUIRE(panels.SaveState(path));
    }
    {
        // Urutan yang sebenarnya terjadi di editor: keadaan dibaca saat
        // startup, sementara panel Lua baru lahir di frame pertama — setelah
        // berkas skripnya dijalankan.
        PanelManager panels;
        REQUIRE(panels.LoadState(path));
        panels.Add(std::make_unique<DummyPanel>("lua.notes"));
        CHECK_FALSE(panels.Find("lua.notes")->IsOpen());

        // Yang tidak pernah tersimpan tetap lahir terbuka.
        panels.Add(std::make_unique<DummyPanel>("lua.baru"));
        CHECK(panels.Find("lua.baru")->IsOpen());
    }

    std::filesystem::remove(path);
}

// --- impor berkas sumber menjadi beberapa aset ---------------------------------

namespace {

/// Folder sementara yang membersihkan dirinya sendiri.
class ImportDir {
public:
    ImportDir() {
        static int counter = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("simimport_" + std::to_string(counter++) + "_" + std::to_string(::getpid()));
        std::filesystem::create_directories(path_);
    }
    ~ImportDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    const std::filesystem::path& Path() const { return path_; }

private:
    std::filesystem::path path_;
};

bool Wrote(const std::vector<std::string>& written, std::string_view suffix) {
    for (const std::string& name : written) {
        if (name.size() >= suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("Satu berkas mesh menjadi beberapa aset sekaligus") {
    const std::filesystem::path source = std::filesystem::path(SIM_MESH_DIR) / "shaderBall.fbx";
    if (!std::filesystem::exists(source)) {
        return;  // aset opsional
    }

    // shaderBall punya geometri dan lima material, tapi tidak punya rangka
    // maupun take animasi — dan itu justru yang diuji di sini: bagian yang tidak
    // ada tidak boleh menghasilkan berkas kosong.
    const SourceContents contents = InspectSource(source);
    CHECK(contents.hasMesh);
    CHECK(contents.materialCount == 5);
    CHECK(contents.boneCount == 0);
    CHECK(contents.clipCount == 0);

    ImportDir folder;
    const SourceImportResult result = ImportSource(source, folder.Path(), SourceImportOptions{});
    INFO("galat impor: " << result.error);
    REQUIRE(result.ok);

    // Mesh-nya sendiri ditambah lima material; tidak ada .simskel maupun .simanim.
    CHECK(result.written.size() == 6);
    CHECK(Wrote(result.written, "shaderBall.fbx"));
    CHECK_FALSE(Wrote(result.written, ".simskel"));
    CHECK_FALSE(Wrote(result.written, ".simanim"));
    int materials = 0;
    for (const std::string& name : result.written) {
        if (name.size() > 12 && name.rfind(".simmatinst") == name.size() - 11) {
            ++materials;
        }
        INFO("berkas " << name);
        CHECK(std::filesystem::exists(folder.Path() / name));
    }
    CHECK(materials == 5);
}

TEST_CASE("Impor bisa dibatasi ke salah satu bagian berkasnya") {
    // Rig lengkap — geometri, rangka, dan animasi — tidak ikut di repo:
    //   SIM_RIG_FBX=/path/rig.fbx ctest
    const char* rigPath = std::getenv("SIM_RIG_FBX");
    if (rigPath == nullptr || !std::filesystem::exists(rigPath)) {
        return;
    }

    const SourceContents contents = InspectSource(rigPath);
    CHECK(contents.hasMesh);
    CHECK(contents.boneCount > 0);
    CHECK(contents.clipCount > 0);

    SUBCASE("semuanya sekaligus") {
        ImportDir folder;
        const SourceImportResult all = ImportSource(rigPath, folder.Path(), SourceImportOptions{});
        REQUIRE(all.ok);
        CHECK(Wrote(all.written, ".fbx"));
        CHECK(Wrote(all.written, ".simskel"));
        CHECK(Wrote(all.written, ".simanim"));
        CHECK(Wrote(all.written, ".simmatinst"));
    }

    SUBCASE("mesh saja") {
        ImportDir folder;
        SourceImportOptions options;
        options.skeleton = false;
        options.animation = false;
        options.materials = false;
        const SourceImportResult only = ImportSource(rigPath, folder.Path(), options);
        REQUIRE(only.ok);
        CHECK(only.written.size() == 1);
        CHECK(Wrote(only.written, ".fbx"));
    }

    SUBCASE("animasi saja") {
        ImportDir folder;
        SourceImportOptions options;
        options.mesh = false;
        options.skeleton = false;
        options.materials = false;
        const SourceImportResult only = ImportSource(rigPath, folder.Path(), options);
        REQUIRE(only.ok);
        // **Berkas sumbernya tidak ikut disalin.** Klip yang dihasilkan berdiri
        // sendiri — ia tidak menunjuk kembali ke FBX-nya — jadi menyalin sebelas
        // megabyte FBX untuk sesuatu yang tidak merujuknya hanya menumpuk berkas.
        CHECK_FALSE(Wrote(only.written, ".fbx"));
        CHECK_FALSE(Wrote(only.written, ".simskel"));
        CHECK(Wrote(only.written, ".simanim"));
    }
}

TEST_CASE("Impor yang tidak menghasilkan apa pun dilaporkan sebagai galat") {
    const std::filesystem::path source = std::filesystem::path(SIM_MESH_DIR) / "shaderBall.fbx";
    if (!std::filesystem::exists(source)) {
        return;
    }
    ImportDir folder;
    SourceImportOptions options;
    // shaderBall tidak punya rangka maupun animasi; meminta hanya keduanya
    // berarti tidak ada satu pun berkas yang ditulis.
    options.mesh = false;
    options.materials = false;
    const SourceImportResult result = ImportSource(source, folder.Path(), options);
    CHECK_FALSE(result.ok);
    CHECK_FALSE(result.error.empty());
    CHECK(result.written.empty());

    const SourceImportResult missing =
        ImportSource("/tidak/ada/berkas.fbx", folder.Path(), SourceImportOptions{});
    CHECK_FALSE(missing.ok);
    CHECK(missing.error == "file not found");
}

// ---------------------------------------------------------------------------
// E8.4 #4 — shader material dikompilasi di TaskPool
// ---------------------------------------------------------------------------

namespace {

/// Folder sementara yang membersihkan dirinya sendiri.
class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("simeditor_" + std::to_string(counter.fetch_add(1)) + "_" +
                 std::to_string(::getpid()));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    const std::filesystem::path& Path() const { return path_; }

private:
    std::filesystem::path path_;
};

/// Perender tiruan yang hanya mencatat apa yang diserahkan kepadanya.
///
/// **Cukup untuk menguji seluruh jalur kecuali Vulkan-nya**, dan itu memang
/// batas yang benar: yang diuji di sini adalah bahwa kompilasi berjalan di luar
/// thread pemanggil, bahwa hasilnya sampai utuh, dan bahwa ia terjadi sekali.
class RecordingRenderer final : public render::IViewportRenderer {
public:
    void Resize(uint32_t, uint32_t) override {}
    render::MeshAsset AcquireMesh(std::string_view) override { return {}; }
    void Render(const render::ViewportDesc&, const render::ViewportScene&) override {}
    render::TextureHandle ColorTarget() const override { return 0; }
    Vec2 ColorTargetUvMax() const override { return Vec2(1.0f); }
    uint32_t Width() const override { return 0; }
    uint32_t Height() const override { return 0; }
    const char* Name() const override { return "recording"; }

    render::MaterialHandle AcquireMaterial(std::string_view key,
                                           const MaterialProgram& program) override {
        ++acquires;
        lastKey = std::string(key);
        spirvWords = program.fragmentSpirv.size();
        parameterBytes = program.parameters.size();
        textureCount = program.textures.size();
        return static_cast<render::MaterialHandle>(acquires);
    }

    int acquires = 0;
    std::string lastKey;
    std::size_t spirvWords = 0;
    std::size_t parameterBytes = 0;
    std::size_t textureCount = 0;
};

/// Project sementara berisi satu material bawaan yang disalin dari repo.
std::filesystem::path CopyBuiltinMaterial(const std::filesystem::path& assetsDir) {
    std::error_code error;
    std::filesystem::create_directories(assetsDir, error);
    const std::filesystem::path source =
        std::filesystem::path(SIM_BUILTIN_DIR) / "Materials" / "Default.simmat";
    const std::filesystem::path target = assetsDir / "Default.simmat";
    std::filesystem::copy_file(source, target,
                               std::filesystem::copy_options::overwrite_existing, error);
    return error ? std::filesystem::path{} : target;
}

}  // namespace

TEST_CASE("E8.4: shader material dikompilasi di TaskPool, bukan di thread pemanggil") {
    // **Inilah alasan MaterialPrograms ada.** Satu panggilan slangc adalah
    // detik, dan detik di dalam `SceneView::Build` berarti editor membeku tepat
    // pada frame sebuah level dibuka.
    TempDir temp;
    const std::filesystem::path assetsDir = temp.Path() / "Assets";
    const std::filesystem::path material = CopyBuiltinMaterial(assetsDir);
    if (material.empty()) {
        MESSAGE("material bawaan tidak terbaca — uji dilewati");
        return;
    }

    TaskPool tasks;
    assets::AssetDatabase database;
    database.Initialize({assetsDir, &tasks, 0.0f});
    tasks.WaitIdle();
    database.Update(0.0f);

    const assets::AssetRecord* record = database.FindByRelativePath("Default.simmat");
    REQUIRE_MESSAGE(record != nullptr, "material tidak masuk indeks");
    const Uuid guid = record->guid;

    MaterialPrograms programs(temp.Path() / "ShaderCache", SIM_SHADER_DIR, &tasks);
    if (!programs.Usable()) {
        MESSAGE("slangc tidak ditemukan — uji dilewati");
        return;
    }

    RecordingRenderer renderer;
    const auto resolve = [](const Uuid&) {
        return ResolvedMaterialTexture{render::kInvalidTexture, true};
    };

    // Frame pertama: belum siap, dan **tidak menunggu**.
    const MaterialProgramRef first =
        programs.Request(database, guid, renderer, resolve);
    CHECK(first.state == MaterialProgramState::Pending);
    CHECK(first.handle == render::kInvalidMaterial);
    CHECK(renderer.acquires == 0);

    // Permintaan berulang pada frame yang sama tidak mengantre tugas kedua.
    CHECK(programs.Request(database, guid, renderer, resolve).state ==
          MaterialProgramState::Pending);
    CHECK(programs.Request(database, guid, renderer, resolve).state ==
          MaterialProgramState::Pending);

    tasks.WaitIdle();
    // **Sekali, bukan tiga kali.** Tiga permintaan pada frame yang sama untuk
    // material yang sama menghasilkan SPIR-V yang identik, jadi tugas kedua dan
    // ketiga tidak punya satu pun akibat yang terlihat — yang terbuang hanya
    // beberapa detik inti, diam-diam.
    CHECK(programs.CompileCount() == 1);

    // Frame berikutnya: pipeline dibangun **di thread ini**, karena hanya main
    // thread yang boleh menyentuh renderer.
    const MaterialProgramRef ready =
        programs.Request(database, guid, renderer, resolve);
    CHECK(ready.state == MaterialProgramState::Ready);
    CHECK(ready.handle != render::kInvalidMaterial);
    CHECK(renderer.acquires == 1);
    CHECK(renderer.spirvWords > 0);
    CHECK(programs.PendingCount() == 0);

    // Dan sesudahnya tidak dibangun lagi, betapapun sering ia diminta.
    for (int i = 0; i < 5; ++i) {
        CHECK(programs.Request(database, guid, renderer, resolve).handle == ready.handle);
    }
    CHECK(renderer.acquires == 1);
    CHECK(programs.CompileCount() == 1);

    // Kuncinya memuat hash SPIR-V-nya, bukan hanya GUID-nya: material yang
    // disunting menghasilkan shader yang lain, dan kunci yang cuma GUID membuat
    // renderer mengembalikan pipeline yang lama.
    CHECK(renderer.lastKey.find(guid.ToString()) == 0);
    CHECK(renderer.lastKey.size() > guid.ToString().size() + 1);
}

TEST_CASE("E8.4: material yang tidak ada di indeks gagal, dan tidak dicoba lagi") {
    TempDir temp;
    TaskPool tasks;
    assets::AssetDatabase database;
    database.Initialize({temp.Path() / "Assets", &tasks, 0.0f});
    tasks.WaitIdle();

    MaterialPrograms programs(temp.Path() / "ShaderCache", SIM_SHADER_DIR, &tasks);
    if (!programs.Usable()) {
        MESSAGE("slangc tidak ditemukan — uji dilewati");
        return;
    }

    RecordingRenderer renderer;
    const auto resolve = [](const Uuid&) {
        return ResolvedMaterialTexture{render::kInvalidTexture, true};
    };
    const Uuid missing = Uuid::Generate();

    CHECK(programs.Request(database, missing, renderer, resolve).state ==
          MaterialProgramState::Failed);
    CHECK(programs.Request(database, missing, renderer, resolve).state ==
          MaterialProgramState::Failed);
    CHECK(renderer.acquires == 0);
    CHECK(programs.PendingCount() == 0);
}

TEST_CASE("E8.4: material rusak diingat gagal sampai dilupakan") {
    TempDir temp;
    const std::filesystem::path assetsDir = temp.Path() / "Assets";
    std::error_code error;
    std::filesystem::create_directories(assetsDir, error);
    {
        std::ofstream broken(assetsDir / "Rusak.simmat");
        broken << "ini bukan material";
    }

    TaskPool tasks;
    assets::AssetDatabase database;
    database.Initialize({assetsDir, &tasks, 0.0f});
    tasks.WaitIdle();
    database.Update(0.0f);
    const assets::AssetRecord* record = database.FindByRelativePath("Rusak.simmat");
    REQUIRE(record != nullptr);
    const Uuid guid = record->guid;

    MaterialPrograms programs(temp.Path() / "ShaderCache", SIM_SHADER_DIR, &tasks);
    if (!programs.Usable()) {
        MESSAGE("slangc tidak ditemukan — uji dilewati");
        return;
    }
    RecordingRenderer renderer;
    const auto resolve = [](const Uuid&) {
        return ResolvedMaterialTexture{render::kInvalidTexture, true};
    };

    CHECK(programs.Request(database, guid, renderer, resolve).state ==
          MaterialProgramState::Pending);
    tasks.WaitIdle();
    CHECK(programs.Request(database, guid, renderer, resolve).state ==
          MaterialProgramState::Failed);
    CHECK(programs.CompileCount() == 1);

    // **Dan ia benar-benar diingat, bukan sekadar gagal lagi dengan cara yang
    // sama.** Berkasnya diganti dengan yang sah; jawabannya tetap gagal, dan
    // tidak ada satu pun panggilan slangc yang bertambah — itu yang mencegah
    // material rusak dikompilasi enam puluh kali per detik.
    std::filesystem::copy_file(
        std::filesystem::path(SIM_BUILTIN_DIR) / "Materials" / "Default.simmat",
        assetsDir / "Rusak.simmat", std::filesystem::copy_options::overwrite_existing, error);
    REQUIRE_MESSAGE(!error, error.message());
    CHECK(programs.Request(database, guid, renderer, resolve).state ==
          MaterialProgramState::Failed);
    CHECK(programs.CompileCount() == 1);

    // Dan `Invalidate` itulah jalan keluarnya — yang dipanggil ketika berkasnya
    // berubah.
    programs.Invalidate(guid);
    CHECK(programs.Request(database, guid, renderer, resolve).state ==
          MaterialProgramState::Pending);
    tasks.WaitIdle();
    CHECK(programs.Request(database, guid, renderer, resolve).state ==
          MaterialProgramState::Ready);
    CHECK(programs.CompileCount() == 2);
}

TEST_CASE("E8.4: tanpa TaskPool, kompilasi dikerjakan di tempat dan langsung siap") {
    // **Jalur ini tidak pernah dilewati editor**, jadi tidak ada yang akan
    // menemukan kesalahannya di sana. Ia sempat mengunci dirinya sendiri —
    // `Compile` mengambil kunci yang masih dipegang pemanggilnya — dan yang
    // menemukannya sebuah mutasi, bukan sebuah sesi.
    TempDir temp;
    const std::filesystem::path assetsDir = temp.Path() / "Assets";
    const std::filesystem::path material = CopyBuiltinMaterial(assetsDir);
    if (material.empty()) {
        MESSAGE("material bawaan tidak terbaca — uji dilewati");
        return;
    }

    TaskPool tasks;
    assets::AssetDatabase database;
    database.Initialize({assetsDir, &tasks, 0.0f});
    tasks.WaitIdle();
    database.Update(0.0f);
    const assets::AssetRecord* record = database.FindByRelativePath("Default.simmat");
    REQUIRE(record != nullptr);

    MaterialPrograms programs(temp.Path() / "ShaderCache", SIM_SHADER_DIR, /*tasks=*/nullptr);
    if (!programs.Usable()) {
        MESSAGE("slangc tidak ditemukan — uji dilewati");
        return;
    }
    RecordingRenderer renderer;
    const auto resolve = [](const Uuid&) {
        return ResolvedMaterialTexture{render::kInvalidTexture, true};
    };

    // Sekali panggil, langsung siap — tanpa frame kedua dan tanpa `WaitIdle`.
    const MaterialProgramRef ref = programs.Request(database, record->guid, renderer, resolve);
    CHECK(ref.state == MaterialProgramState::Ready);
    CHECK(ref.handle != render::kInvalidMaterial);
    CHECK(renderer.acquires == 1);
    CHECK(programs.CompileCount() == 1);
}

TEST_CASE("E8.4: material menunggu teksturnya siap sebelum pipeline-nya dibangun") {
    // **Descriptor set material ditulis sekali dan tidak pernah ditinjau lagi.**
    // Membangunnya sementara sebuah teksturnya masih di-bake berarti mengunci
    // placeholder ke dalamnya selamanya — dan yang terlihat bukan objek yang
    // sedang menunggu melainkan objek magenta yang tidak pernah berubah. Itu
    // persis yang sempat terjadi, dan uji ini yang menjaganya tidak terulang.
    TempDir temp;
    const std::filesystem::path assetsDir = temp.Path() / "Assets";
    std::error_code error;
    std::filesystem::create_directories(assetsDir, error);

    // Material bawaan tidak punya slot tekstur sama sekali, jadi ia tidak bisa
    // memperlihatkan apa pun tentang penungguan ini. Yang dipakai di sini graph
    // yang memang menyampel sebuah tekstur — bentuk yang sama dengan yang
    // dihasilkan Material Editor saat sebuah tekstur dijatuhkan ke kanvasnya.
    material::MaterialGraph graph;
    {
        material::MaterialNode output;
        output.guid = Uuid::Generate();
        output.type = std::string(material::kSurfaceOutputType);
        material::MaterialNode texture;
        texture.guid = Uuid::Generate();
        texture.type = "input.texture";
        texture.settings["name"] = "Albedo";
        texture.settings["texture"] = Uuid::Generate().ToString();
        material::MaterialNode sample;
        sample.guid = Uuid::Generate();
        sample.type = "input.sample";

        material::MaterialLink toSample;
        toSample.guid = Uuid::Generate();
        toSample.fromNode = texture.guid;
        toSample.fromPin = "texture";
        toSample.toNode = sample.guid;
        toSample.toPin = "texture";
        material::MaterialLink toColor;
        toColor.guid = Uuid::Generate();
        toColor.fromNode = sample.guid;
        toColor.fromPin = "rgb";
        toColor.toNode = output.guid;
        toColor.toPin = "baseColor";

        graph.nodes.push_back(std::move(output));
        graph.nodes.push_back(std::move(texture));
        graph.nodes.push_back(std::move(sample));
        graph.links.push_back(std::move(toSample));
        graph.links.push_back(std::move(toColor));
    }
    REQUIRE(material::SaveMaterialToFile(graph, assetsDir / "Bertekstur.simmat").ok);

    TaskPool tasks;
    assets::AssetDatabase database;
    database.Initialize({assetsDir, &tasks, 0.0f});
    tasks.WaitIdle();
    database.Update(0.0f);
    const assets::AssetRecord* record = database.FindByRelativePath("Bertekstur.simmat");
    REQUIRE(record != nullptr);

    MaterialPrograms programs(temp.Path() / "ShaderCache", SIM_SHADER_DIR, &tasks);
    if (!programs.Usable()) {
        MESSAGE("slangc tidak ditemukan — uji dilewati");
        return;
    }
    RecordingRenderer renderer;

    bool baked = false;
    const auto resolve = [&baked](const Uuid&) {
        // Handle-nya selalu ada — placeholder selama belum di-bake, persis
        // seperti yang dilakukan SceneView. Yang membedakan hanya `ready`.
        return ResolvedMaterialTexture{7, baked};
    };

    programs.Request(database, record->guid, renderer, resolve);
    tasks.WaitIdle();

    // Sudah dikompilasi, tetapi teksturnya belum. Pipeline **tidak** dibangun.
    for (int frame = 0; frame < 3; ++frame) {
        CHECK(programs.Request(database, record->guid, renderer, resolve).state ==
              MaterialProgramState::Pending);
    }
    CHECK(renderer.acquires == 0);
    // Dan tidak dikompilasi ulang berkali-kali sambil menunggu.
    CHECK(programs.CompileCount() == 1);

    baked = true;
    const MaterialProgramRef ready =
        programs.Request(database, record->guid, renderer, resolve);
    CHECK(ready.state == MaterialProgramState::Ready);
    CHECK(renderer.acquires == 1);
    CHECK(renderer.textureCount == 1);
    CHECK(programs.CompileCount() == 1);
}

// Panel yang tidak disebut BuildLayout tidak "tersembunyi" — ia mengambang di
// atas host dockspace dan menutupi apa pun yang ter-dock di bawahnya. Prefabs
// terkena itu: begitu dibuka, ia menutupi seluruh kolom kiri, dan Asset Browser
// di baliknya tetap tergambar tapi tak terlihat.
//
// ImGui menyimpan hasil DockBuilderDockWindow untuk jendela yang belum pernah
// ada ke ImGuiWindowSettings, jadi seluruh pemeriksaan ini jalan tanpa satu pun
// frame digambar dan tanpa backend grafis.
TEST_CASE("Setiap panel yang disusun layout benar-benar mendapat node dock") {
    IMGUI_CHECKVERSION();
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.DisplaySize = ImVec2(1920.0f, 1080.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.Fonts->AddFontDefault();
    io.Fonts->Build();
    // Backend biasanya yang mengisi ini; tanpanya NewFrame menolak jalan.
    io.Fonts->TexID = static_cast<ImTextureID>(1);

    ImGui::NewFrame();

    const ImGuiID dockspaceId = ImGui::GetID("##TestDockspace");

    // Judul panel boleh berubah; id-nya tidak. Kunci settings-nya dihitung dari
    // "###id" persis seperti yang dilakukan DockLayout.
    const auto dockOf = [](const char* panelId) -> ImGuiID {
        const std::string key = std::string("###") + panelId;
        ImGuiWindowSettings* settings = ImGui::FindWindowSettingsByID(ImHashStr(key.c_str()));
        return settings != nullptr ? settings->DockId : 0;
    };

    struct Placement {
        ImGuiID prefabs;
        ImGuiID assetBrowser;
        ImGuiID viewport;
        ImGuiID console;
    };
    const auto place = [&](Workspace workspace) {
        BuildLayout(dockspaceId, workspace);
        return Placement{dockOf(panel_id::kPrefabs), dockOf(panel_id::kAssetBrowser),
                         dockOf(panel_id::kViewport), dockOf(panel_id::kConsole)};
    };

    // Diperiksa per workspace dengan pesan literal: argumen pesan doctest yang
    // berupa `const char*` non-literal diubah jadi bool, dan `std::string` tidak
    // bisa di-stream sama sekali — nama workspace hanya akan tercetak "1".
    const Placement level = place(Workspace::Level);
    const Placement authoring = place(Workspace::Authoring);
    const Placement debug = place(Workspace::Debug);

    CHECK_MESSAGE(level.prefabs != 0, "Prefabs mengambang di workspace Level");
    CHECK_MESSAGE(authoring.prefabs != 0, "Prefabs mengambang di workspace Authoring");
    CHECK_MESSAGE(debug.prefabs != 0, "Prefabs mengambang di workspace Debug");

    // Pasangannya: Prefabs katalog seperti Asset Browser, jadi keduanya harus
    // mendarat di node yang sama, bukan sekadar ter-dock di mana pun.
    CHECK_MESSAGE(level.prefabs == level.assetBrowser, "Prefabs lepas dari Asset Browser: Level");
    CHECK_MESSAGE(authoring.prefabs == authoring.assetBrowser,
                  "Prefabs lepas dari Asset Browser: Authoring");
    CHECK_MESSAGE(debug.prefabs == debug.assetBrowser, "Prefabs lepas dari Asset Browser: Debug");

    // Penjaga agar uji ini tidak lulus karena alasan yang salah: kalau dockOf
    // selalu mengembalikan nilai yang sama, seluruh CHECK di atas lulus palsu.
    CHECK(level.viewport != level.console);
    CHECK(authoring.viewport != authoring.console);
    CHECK(debug.viewport != debug.console);

    // Aturan yang sebenarnya berlaku untuk semua panel: **sebuah panel boleh
    // tidak di-dock, atau boleh terbuka sejak awal, tapi tidak keduanya.**
    // Panel yang terbuka tanpa node dock akan mengambang menutupi kolom di
    // bawahnya, dan pengguna baru tidak punya petunjuk kenapa panelnya hilang.
    //
    // Daftar id-nya dibaca dari PanelIds.h, bukan disalin ke sini: id yang
    // ditambahkan nanti ikut terperiksa tanpa ada yang perlu ingat.
    BuildLayout(dockspaceId, Workspace::Level);

    const std::filesystem::path codeDir = SIM_CODE_DIR;
    const auto readFile = [](const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };

    const std::string panelIds =
        readFile(codeDir / "EditorFramework" / "include" / "Sim" / "Editor" / "PanelIds.h");
    REQUIRE_FALSE(panelIds.empty());

    // Sumber panel disatukan jadi satu teks: yang dicari hanya "berkas mana
    // yang menyebut id ini", dan tiap panel tinggal di satu berkas.
    std::vector<std::pair<std::string, std::string>> sources;  // (nama berkas, isi)
    for (const auto& entry : std::filesystem::directory_iterator(codeDir / "Editor" / "src")) {
        if (entry.path().extension() == ".cpp") {
            sources.emplace_back(entry.path().filename().string(), readFile(entry.path()));
        }
    }
    REQUIRE_FALSE(sources.empty());

    int checked = 0;
    int floating = 0;
    for (std::size_t at = panelIds.find("inline constexpr const char* k");
         at != std::string::npos;
         at = panelIds.find("inline constexpr const char* k", at + 1)) {
        const std::size_t nameStart = at + std::strlen("inline constexpr const char* ");
        const std::size_t nameEnd = panelIds.find(' ', nameStart);
        const std::size_t idStart = panelIds.find('"', nameEnd) + 1;
        const std::size_t idEnd = panelIds.find('"', idStart);
        REQUIRE(idEnd != std::string::npos);

        const std::string constant = panelIds.substr(nameStart, nameEnd - nameStart);
        const std::string id = panelIds.substr(idStart, idEnd - idStart);
        ++checked;
        if (dockOf(id.c_str()) != 0) {
            continue;
        }
        ++floating;

        // Tidak di-dock. Maka berkasnya wajib menutup panelnya sendiri.
        const std::string needle = "panel_id::" + constant;
        bool declaredClosed = false;
        bool found = false;
        for (const auto& [name, text] : sources) {
            if (text.find(needle) == std::string::npos) {
                continue;
            }
            found = true;
            declaredClosed = declaredClosed || text.find("SetOpen(false)") != std::string::npos;
        }
        // Dibungkus doctest::String: `const char*` non-literal diubah jadi bool
        // oleh MessageBuilder, dan `std::string` tidak bisa di-stream sama sekali.
        INFO("panel ", doctest::String(id.c_str()));
        CHECK_MESSAGE(found, "Id panel tidak dipakai panel mana pun di Code/Editor/src");
        CHECK_MESSAGE(declaredClosed,
                      "Panel ini tidak di-dock BuildLayout tapi juga tidak SetOpen(false), "
                      "jadi ia akan mengambang menutupi panel lain sejak editor pertama dibuka");
    }

    // Penjaga terakhir: kalau pembacaan PanelIds.h meleset, loop di atas tidak
    // memeriksa apa pun dan uji ini lulus tanpa melakukan apa-apa.
    CHECK(checked >= 20);
    CHECK(floating > 0);

    ImGui::EndFrame();
    ImGui::DestroyContext(context);
}

// Gerbang persetujuan menyeberangkan satu pertanyaan dari thread jaringan ke
// main thread dan menahan yang bertanya sampai dijawab. Yang diuji di sini
// adalah perilaku threading-nya; dialognya sendiri hanya menggambar apa yang
// dilaporkan `Pending`.
TEST_CASE("Gerbang persetujuan menahan penanya sampai dijawab") {
    ToolApprovalGate gate;

    SUBCASE("jawaban ya diteruskan") {
        std::atomic<bool> answered{false};
        std::future<bool> asking = std::async(std::launch::async, [&gate] {
            return gate.Ask({"entity.create", "write", R"({"name":"Kubus"})"},
                            std::chrono::seconds(5));
        });

        // Meniru main thread: menunggu pertanyaannya muncul, lalu menjawabnya.
        ToolApprovalGate::Question seen;
        for (int spin = 0; spin < 500 && !gate.Pending(seen); ++spin) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(seen.tool == "entity.create");
        CHECK(seen.permission == "write");
        // Argumennya ikut menyeberang: persetujuan tanpa melihat apa yang
        // disetujui bukan persetujuan.
        CHECK(seen.arguments.find("Kubus") != std::string::npos);
        gate.Answer(true);
        answered = true;

        CHECK(asking.get());
        CHECK(answered.load());
    }

    SUBCASE("jawaban tidak diteruskan") {
        std::future<bool> asking = std::async(std::launch::async, [&gate] {
            return gate.Ask({"file.write", "dangerous", "{}"}, std::chrono::seconds(5));
        });
        ToolApprovalGate::Question seen;
        for (int spin = 0; spin < 500 && !gate.Pending(seen); ++spin) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        gate.Answer(false);
        CHECK_FALSE(asking.get());
    }

    SUBCASE("tenggat habis berarti menolak, bukan menyetujui") {
        // Menyetujui sesuatu karena tidak ada yang menjawab adalah kebalikan
        // dari yang diminta mode `ask`.
        const auto started = std::chrono::steady_clock::now();
        CHECK_FALSE(gate.Ask({"entity.delete", "write", "{}"},
                             std::chrono::milliseconds(120)));
        CHECK(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(100));
        // Dan gerbangnya bersih sesudahnya: pertanyaan yang kedaluwarsa tidak
        // boleh tertinggal sebagai dialog yang tidak pernah hilang.
        ToolApprovalGate::Question stale;
        CHECK_FALSE(gate.Pending(stale));
    }

    SUBCASE("dua penanya dilayani bergiliran, bukan bersamaan") {
        // Dua dialog yang saling menimpa membuat yang diklik orang menjadi
        // tidak jelas milik yang mana.
        std::atomic<int> concurrent{0};
        std::atomic<int> peak{0};
        const auto ask = [&](const char* name) {
            return std::async(std::launch::async, [&gate, name] {
                return gate.Ask({name, "write", "{}"}, std::chrono::seconds(5));
            });
        };
        std::future<bool> first = ask("test.a");
        std::future<bool> second = ask("test.b");

        std::vector<std::string> order;
        for (int answered = 0; answered < 2;) {
            ToolApprovalGate::Question seen;
            if (!gate.Pending(seen)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            // Hanya satu yang pernah terlihat menunggu pada satu waktu.
            concurrent.fetch_add(1);
            peak.store(std::max(peak.load(), concurrent.load()));
            order.push_back(seen.tool);
            gate.Answer(true);
            concurrent.fetch_sub(1);
            ++answered;
        }
        CHECK(first.get());
        CHECK(second.get());
        CHECK(peak.load() == 1);
        REQUIRE(order.size() == 2);
        CHECK(order[0] != order[1]);
    }

    SUBCASE("Shutdown melepaskan yang sedang menunggu, dengan menolaknya") {
        std::future<bool> asking = std::async(std::launch::async, [&gate] {
            // Tenggat panjang: yang mengakhirinya harus Shutdown, bukan waktu.
            return gate.Ask({"lua.eval", "dangerous", "{}"}, std::chrono::seconds(30));
        });
        ToolApprovalGate::Question seen;
        for (int spin = 0; spin < 500 && !gate.Pending(seen); ++spin) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const auto started = std::chrono::steady_clock::now();
        gate.Shutdown();
        CHECK_FALSE(asking.get());
        // Penutupan editor tidak boleh memakan waktu setenggat penuh.
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));

        // Dan sesudah ditutup, pertanyaan baru langsung ditolak.
        CHECK_FALSE(gate.Ask({"entity.create", "write", "{}"}, std::chrono::seconds(30)));
    }
}
