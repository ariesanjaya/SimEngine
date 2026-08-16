#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/AIBridge/ResourceRegistry.h"
#include "Sim/AIBridge/ToolRegistry.h"
#include "Sim/Editor/AiTools.h"
#include "Sim/Editor/Command.h"
#include "Sim/Editor/EditorApp.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/Selection.h"
#include "Sim/Scene/Serialization.h"
#include "Sim/Scene/World.h"

#include <nlohmann/json.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <unistd.h>

// Tool MCP diuji di tingkat semantiknya: apa yang dilakukan sebuah tool call
// terhadap `World`, `Selection`, dan `CommandHistory`.
//
// **Lapisan kawatnya sengaja tidak diulang di sini.** `SimAIBridgeTests` sudah
// menembak lewat soket sungguhan dan menutup JSON-RPC, kode status, dan
// marshaling; mengulangnya berarti dua uji yang gagal bersamaan untuk sebab yang
// sama dan tidak pernah gagal sendirian untuk sebab yang berbeda.
//
// Yang dikunci berkas ini adalah kelas bug yang benar-benar lolos selama A1, dan
// semuanya punya bentuk yang sama: sebuah tool melapor berhasil untuk sesuatu
// yang tidak terjadi.

using namespace sim;
using namespace sim::editor;
using nlohmann::json;

namespace {

struct ScratchDir {
    std::filesystem::path path;

    ScratchDir() {
        static std::atomic<int> counter{0};
        path = std::filesystem::temp_directory_path() /
               ("simai_" + std::to_string(counter.fetch_add(1)) + "_" +
                std::to_string(::getpid()));
        std::filesystem::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
};

/// Editor tanpa GPU maupun jendela, dengan satu project dan satu level terbuka,
/// dan seluruh tool AI terdaftar.
struct Harness {
    ScratchDir scratch;
    EditorApp app;
    ai::ToolRegistry tools;
    ai::ResourceRegistry resources;

    Harness() {
        EditorApp::Config config;
        config.configDir = scratch.path / "config";
        config.projectsRoot = scratch.path / "projects";
        REQUIRE(app.Initialize(config));
        REQUIRE(app.CreateProject(config.projectsRoot, "Uji"));
        REQUIRE(app.CreateLevelFile("scene"));

        // Tanpa `ScreenshotFn`: tool yang menangkap layar menuntut swapchain,
        // dan uji ini memang tidak punya satu pun. Yang tidak didaftarkan tidak
        // bisa dipanggil, dan itu perilaku yang juga diuji di bawah.
        RegisterEditorTools(tools, resources, app);
        RegisterSceneTools(tools, resources, app);
        RegisterEntityTools(tools, app);

        // **Level baru tidak kosong** — ia sudah memuat sebuah entity
        // "Environment". Uji yang menghitung jumlah mutlak karena itu menguji
        // isi level contoh, bukan apa yang dilakukan tool-nya; yang berarti di
        // sini adalah selisihnya.
        baseline = app.Context().world->Count();
    }

    std::size_t baseline = 0;

    ~Harness() { app.Shutdown(); }

    ai::ToolResult Call(const std::string& name, const json& arguments = json::object()) {
        const ai::ToolDefinition* tool = tools.Find(name);
        REQUIRE_MESSAGE(tool != nullptr, "no tool called ", name);
        return tool->handler(arguments.dump());
    }

    /// Hasil sebuah tool sebagai JSON. Gagal bila tool-nya melaporkan galat.
    json CallOk(const std::string& name, const json& arguments = json::object()) {
        const ai::ToolResult result = Call(name, arguments);
        REQUIRE_MESSAGE(!result.isError, result.text.c_str());
        json parsed = json::parse(result.text, nullptr, /*allow_exceptions=*/false);
        REQUIRE_FALSE(parsed.is_discarded());
        return parsed;
    }

    scene::World& World() { return *app.Context().world; }
    CommandHistory& History() { return app.History(); }

    std::string CreateEntity(const std::string& name) {
        return CallOk("entity.create", json{{"name", name}}).at("created")[0].get<std::string>();
    }
};

}  // namespace

TEST_CASE("Level yang dimuat menurunkan layar pemilihnya") {
    // **Regresi.** `LoadLevel` sempat tidak menyentuh `awaitingLevelChoice_`,
    // jadi memuat level dari mana pun selain pemilihnya sendiri menukar isi
    // dunia diam-diam sementara editor tetap menampilkan pemilih. Yang terlihat
    // adalah editor yang mengabaikan permintaan; yang terjadi adalah level yang
    // termuat penuh di balik layar yang menutupinya.
    Harness harness;
    CHECK_FALSE(harness.app.IsAwaitingLevelChoice());

    harness.CreateEntity("Kubus");
    REQUIRE(harness.app.SaveLevel(harness.app.LevelsDirectory() / "kedua.simlevel"));

    // Kembali ke keadaan menunggu, seperti sesudah sebuah project dibuka.
    REQUIRE(harness.app.CreateProject(harness.scratch.path / "projects", "Kedua"));
    CHECK(harness.app.IsAwaitingLevelChoice());

    REQUIRE(harness.app.CreateLevelFile("apa saja"));
    CHECK_FALSE(harness.app.IsAwaitingLevelChoice());
}

TEST_CASE("Satu tool call adalah satu entri undo, dan namanya menyebut asalnya") {
    Harness harness;
    const int before = harness.History().Cursor();

    harness.CallOk("entity.create_many",
                   json{{"entities", json::array({json{{"name", "A"}}, json{{"name", "B"}},
                                                  json{{"name", "C"}}})}});

    // Tiga entity, satu entri. Tanpa transaksi ini, "batalkan yang barusan
    // dilakukan agen" menuntut manusia menekan Ctrl+Z sebanyak entity yang
    // dibuatnya.
    CHECK(harness.History().Cursor() == before + 1);
    REQUIRE(harness.History().Entries().size() >= 1);
    CHECK(harness.History().Entries().back().name == "AI: entity.create_many");
    CHECK(harness.World().Count() == harness.baseline + 3);

    REQUIRE(harness.History().Undo());
    CHECK(harness.World().Count() == harness.baseline);
}

TEST_CASE("entity.modify menambahkan komponen yang belum ada") {
    // **Regresi, dan yang ini mematikan editor.**
    // `SetComponentsCommand::Apply` melewati entity yang belum punya
    // komponennya — dengan sengaja — jadi menyerahkan nilai kepadanya tanpa
    // menambahkan komponennya lebih dulu tidak menghasilkan galat apa pun; ia
    // hanya diam-diam tidak melakukan apa-apa, dan pembacaan hasilnya lalu
    // men-dereference nullptr.
    Harness harness;
    const std::string guid = harness.CreateEntity("Kubus");

    const json applied = harness.CallOk(
        "entity.modify",
        json{{"entity", guid}, {"component", "Visibility"}, {"values", {{"visible", false}}}});

    CHECK(applied.at("applied").at("visible") == false);
    // Field yang tidak disebut tetap bernilai bawaannya, bukan dikosongkan.
    CHECK(applied.at("applied").at("locked") == false);

    const scene::Entity entity = harness.World().FindByGuid(Uuid::Parse(guid));
    REQUIRE(scene::IsValid(entity));
    CHECK(harness.World().Has<scene::VisibilityComponent>(entity));

    // Menambah komponen dan menyetel nilainya tetap satu entri undo.
    REQUIRE(harness.History().Undo());
    CHECK_FALSE(harness.World().Has<scene::VisibilityComponent>(entity));
}

TEST_CASE("entity.modify menyimpan field yang tidak disebut") {
    Harness harness;
    const std::string guid = harness.CreateEntity("Kubus");
    harness.CallOk("entity.modify", json{{"entity", guid},
                                         {"component", "Transform"},
                                         {"values", {{"position", {1.0, 2.0, 3.0}}}}});
    const json applied =
        harness.CallOk("entity.modify",
                       json{{"entity", guid}, {"component", "Transform"},
                            {"values", {{"scale", {2.0, 2.0, 2.0}}}}});
    // merge_patch, bukan penggantian: menyebut `scale` bukan permintaan supaya
    // `position` kembali ke nol.
    CHECK(applied.at("applied").at("position")[1] == 2.0);
    CHECK(applied.at("applied").at("scale")[0] == 2.0);
}

TEST_CASE("entity.reparent menolak menjadikan entity induk dirinya sendiri") {
    // **Regresi.** `IsDescendantOf(x, x)` bernilai false, jadi penjagaan siklus
    // saja meloloskan kasus paling sederhana — dan `World::SetParent`
    // menolaknya diam-diam, sehingga tool melaporkan berhasil sambil mencatat
    // satu entri undo untuk sesuatu yang tidak pernah terjadi.
    Harness harness;
    const std::string guid = harness.CreateEntity("Kubus");
    const int before = harness.History().Cursor();

    const ai::ToolResult result =
        harness.Call("entity.reparent", json{{"entity", guid}, {"parent", guid}});
    CHECK(result.isError);
    CHECK(result.text.find("its own parent") != std::string::npos);
    // Yang ditolak tidak boleh meninggalkan jejak di history.
    CHECK(harness.History().Cursor() == before);
}

TEST_CASE("entity.reparent menolak siklus lewat keturunan") {
    Harness harness;
    const std::string induk = harness.CreateEntity("Induk");
    const std::string anak = harness.CreateEntity("Anak");
    harness.CallOk("entity.reparent", json{{"entity", anak}, {"parent", induk}});

    const int before = harness.History().Cursor();
    const ai::ToolResult result =
        harness.Call("entity.reparent", json{{"entity", induk}, {"parent", anak}});
    CHECK(result.isError);
    CHECK(harness.History().Cursor() == before);
}

TEST_CASE("history.rollback mengembalikan level persis") {
    // Kriteria terima A1 nomor 3, dikunci di sini supaya ia tetap berlaku tanpa
    // seseorang menjalankan editor.
    Harness harness;
    harness.CreateEntity("Asli");
    const std::string before = scene::SaveLevelToString(harness.World());

    const std::string id =
        harness.CallOk("history.checkpoint", json{{"label", "sebelum"}}).at("checkpoint");

    harness.CallOk("entity.create_many",
                   json{{"entities", json::array({json{{"name", "X"}}, json{{"name", "Y"}}})}});
    CHECK(harness.World().Count() == harness.baseline + 3);

    const json rolled = harness.CallOk("history.rollback", json{{"checkpoint", id}});
    CHECK(rolled.at("exact") == true);
    CHECK(harness.World().Count() == harness.baseline + 1);
    CHECK(scene::SaveLevelToString(harness.World()) == before);
}

TEST_CASE("history.rollback menolak checkpoint yang tidak dikenal") {
    Harness harness;
    const ai::ToolResult result =
        harness.Call("history.rollback", json{{"checkpoint", "tidak-pernah-ada"}});
    CHECK(result.isError);
}

TEST_CASE("scene.query menyaring menurut nama dan komponen") {
    Harness harness;
    harness.CallOk("entity.create_many",
                   json{{"entities", json::array({json{{"name", "Batu Besar"}},
                                                  json{{"name", "Batu Kecil"}},
                                                  json{{"name", "Pohon"}}})}});

    SUBCASE("nama, tanpa peduli besar-kecil huruf") {
        const json found = harness.CallOk("scene.query", json{{"name", "batu"}});
        CHECK(found.at("total") == 2);
    }

    SUBCASE("nama yang tidak cocok apa pun") {
        const json found = harness.CallOk("scene.query", json{{"name", "tidak ada"}});
        CHECK(found.at("total") == 0);
        CHECK(found.at("entities").empty());
    }

    SUBCASE("komponen yang dimiliki semuanya") {
        // Semua entity punya Transform, termasuk yang sudah ada di level baru.
        const json found = harness.CallOk("scene.query", json{{"component", "Transform"}});
        CHECK(found.at("total") == harness.baseline + 3);
    }

    SUBCASE("komponen yang tidak dikenal ditolak, bukan menghasilkan kosong") {
        // Nol hasil dan "tidak ada komponen bernama itu" adalah dua jawaban
        // berbeda, dan agen yang menerima yang pertama akan menyimpulkan
        // scene-nya yang salah.
        const ai::ToolResult result =
            harness.Call("scene.query", json{{"component", "TidakAda"}});
        CHECK(result.isError);
        CHECK(result.text.find("docs/components") != std::string::npos);
    }

    SUBCASE("paginasi menyebut totalnya") {
        const json page = harness.CallOk("scene.query", json{{"limit", 2}});
        CHECK(page.at("total") == harness.baseline + 3);
        // Yang dikembalikan dibatasi, tapi totalnya tetap disebut apa adanya:
        // agen yang menerima dua hasil tanpa tahu ada lebih banyak akan mengira
        // sudah melihat semuanya.
        CHECK(page.at("returned") == 2);
    }
}

TEST_CASE("entity.get menyaring komponen dan menyebut yang tidak dikembalikan") {
    Harness harness;
    const std::string guid = harness.CreateEntity("Kubus");

    const json all = harness.CallOk("entity.get", json{{"entity", guid}});
    CHECK(all.at("componentValues").contains("Transform"));
    CHECK(all.at("componentValues").contains("Name"));
    CHECK_FALSE(all.contains("notReturned"));

    const json only = harness.CallOk(
        "entity.get", json{{"entity", guid}, {"components", json::array({"Transform"})}});
    CHECK(only.at("componentValues").size() == 1);
    // Yang disaring keluar disebut namanya: daftar yang dipotong tanpa jejak
    // membuat agen menyimpulkan entity-nya memang tidak punya komponen itu.
    REQUIRE(only.contains("notReturned"));
    CHECK(only.at("notReturned").size() >= 1);
}

TEST_CASE("GUID yang tidak ada ditolak dengan pesan yang bisa ditindaklanjuti") {
    Harness harness;
    const std::string hantu = "00000000-0000-0000-0000-0000deadbeef";

    for (const char* name : {"entity.get", "entity.delete", "entity.rename"}) {
        CAPTURE(name);
        json arguments{{"entity", hantu}, {"name", "apa pun"}};
        arguments["entities"] = json::array({hantu});
        const ai::ToolResult result = harness.Call(name, arguments);
        CHECK(result.isError);
        CHECK_FALSE(result.text.empty());
    }
}

TEST_CASE("selection.set menolak GUID yang tidak ada alih-alih memilih sebagiannya") {
    Harness harness;
    const std::string ada = harness.CreateEntity("Ada");
    const std::string hantu = "00000000-0000-0000-0000-0000deadbeef";

    const ai::ToolResult result =
        harness.Call("selection.set", json{{"entities", json::array({ada, hantu})}});
    // Seleksi yang berisi dua dari tiga objek terlihat persis seperti seleksi
    // yang berhasil.
    CHECK(result.isError);
    CHECK(result.text.find(hantu) != std::string::npos);

    const json selected = harness.CallOk("selection.set", json{{"entities", json::array({ada})}});
    CHECK(selected.at("selected") == 1);
    CHECK(harness.CallOk("selection.get").at("count") == 1);
}

TEST_CASE("selection.set tidak mengotori history") {
    // Seleksi bukan data project. Menaruhnya di history membuat pengguna yang
    // menekan Ctrl+Z untuk membatalkan perubahan sungguhan menghabiskan
    // beberapa tekan hanya untuk melewati perubahan seleksi.
    Harness harness;
    const std::string guid = harness.CreateEntity("Kubus");
    const int before = harness.History().Cursor();
    harness.CallOk("selection.set", json{{"entities", json::array({guid})}});
    CHECK(harness.History().Cursor() == before);
}

TEST_CASE("Tool tangkapan layar tidak didaftarkan tanpa perangkat yang bisa menangkap") {
    // Tool yang ada tapi selalu gagal membuat agen mencoba lagi dengan argumen
    // yang berbeda, karena dari sisinya kegagalan berulang terlihat seperti
    // pemakaian yang salah.
    Harness harness;
    CHECK(harness.tools.Find("editor.screenshot") == nullptr);
    CHECK(harness.tools.Find("viewport.capture") == nullptr);
    // Sisanya tetap ada.
    CHECK(harness.tools.Find("editor.status") != nullptr);
    CHECK(harness.tools.Find("scene.describe") != nullptr);
}

TEST_CASE("Resource komponen dibangkitkan dari reflection") {
    Harness harness;
    const ai::ResourceDefinition* docs = harness.resources.Find("simengine://docs/components");
    REQUIRE(docs != nullptr);
    const json described = json::parse(docs->read());

    REQUIRE(described.contains("components"));
    CHECK(described.at("components").size() > 5);

    bool sawTransform = false;
    for (const json& component : described.at("components")) {
        if (component.at("name") != "Transform") {
            continue;
        }
        sawTransform = true;
        // Transform adalah komponen inti: ia tidak bisa ditambahkan maupun
        // dibuang, dan keduanya disebut supaya agen tidak mencobanya.
        CHECK(component.at("addable") == false);
        CHECK(component.at("removable") == false);
        bool sawPosition = false;
        for (const json& field : component.at("fields")) {
            if (field.at("name") == "position") {
                sawPosition = true;
                CHECK(field.at("kind") == "vec3");
            }
        }
        CHECK(sawPosition);
    }
    CHECK(sawTransform);
}

TEST_CASE("scene.describe menyebut cabang yang dipotongnya") {
    Harness harness;
    const std::string induk = harness.CreateEntity("Induk");
    const std::string anak = harness.CreateEntity("Anak");
    harness.CallOk("entity.reparent", json{{"entity", anak}, {"parent", induk}});

    const json shallow = harness.CallOk("scene.describe", json{{"depth", 0}});
    bool sawOmitted = false;
    for (const json& root : shallow.at("roots")) {
        if (root.contains("childrenOmitted")) {
            sawOmitted = true;
            CHECK(root.at("childrenOmitted") == 1);
        }
    }
    // Cabang yang hilang tanpa jejak membuat agen menyimpulkan scene-nya lebih
    // kecil daripada yang sebenarnya, dan kesimpulan itu tidak akan pernah ia
    // periksa lagi.
    CHECK(sawOmitted);

    const json deep = harness.CallOk("scene.describe", json{{"depth", 2}});
    for (const json& root : deep.at("roots")) {
        CHECK_FALSE(root.contains("childrenOmitted"));
    }
}
