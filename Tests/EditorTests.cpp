#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Editor/Actions.h"
#include "Sim/Editor/Command.h"
#include "Sim/Editor/PanelManager.h"
#include "Sim/Editor/Selection.h"
#include "Sim/Editor/SourceImport.h"

#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>
#include <cstdlib>

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
