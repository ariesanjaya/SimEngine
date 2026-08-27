#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/AIBridge/ResourceRegistry.h"
#include "Sim/AIBridge/ToolRegistry.h"
#include "Sim/Editor/AiTools.h"
#include "Sim/Editor/Command.h"
#include "Sim/Editor/EditorApp.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/SceneView/Selection.h"
#include "Sim/Scene/Serialization.h"
#include "Sim/Script/ScriptRuntime.h"
#include "Sim/Animation/AnimationIo.h"
#include "Sim/Animation/Clip.h"
#include "Sim/Terrain/Terrain.h"
#include "Sim/Terrain/TerrainIo.h"
#include "Sim/SceneView/TerrainStore.h"
#include "Sim/Scene/World.h"

#include <nlohmann/json.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include "Sim/Render/IViewportRenderer.h"
#include "Sim/Core/MainThreadQueue.h"
#include "Sim/ImageIO/ImageIO.h"
#include "Sim/Assets/AssetDatabase.h"
#include <fstream>
#include <filesystem>
#include <string>
#include "TestProcess.h"

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
                std::to_string(sim::tests::ProcessId()));
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
    // **Runtime Lua sungguhan, bukan tiruan.** Yang diuji di A3 justru
    // perilakunya: `Evaluate` yang mengembalikan traceback alih-alih
    // menjatuhkan proses, dan `CheckSyntax` yang menolak sebelum ada yang
    // ditulis. Tiruan hanya akan mengulang tebakan penulis ujinya.
    script::ScriptRuntime scripts;
    EditorApp app;
    ai::ToolRegistry tools;
    ai::ResourceRegistry resources;

    Harness() {
        EditorApp::Config config;
        config.configDir = scratch.path / "config";
        config.projectsRoot = scratch.path / "projects";
        config.scripts = &scripts;
        REQUIRE(app.Initialize(config));
        REQUIRE(app.CreateProject(config.projectsRoot, "Uji"));
        REQUIRE(app.CreateLevelFile("scene"));

        // Tanpa `ScreenshotFn`: tool yang menangkap layar menuntut swapchain,
        // dan uji ini memang tidak punya satu pun. Yang tidak didaftarkan tidak
        // bisa dipanggil, dan itu perilaku yang juga diuji di bawah.
        RegisterEditorTools(tools, resources, app);
        RegisterSceneTools(tools, resources, app);
        RegisterEntityTools(tools, app);
        RegisterAssetTools(tools, app);
        RegisterAuthoringTools(tools, app);

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
        REQUIRE_MESSAGE(tool != nullptr, "no tool called ", doctest::String(name.c_str()));
        return tool->handler(arguments.dump());
    }

    /// Hasil sebuah tool sebagai JSON. Gagal bila tool-nya melaporkan galat.
    json CallOk(const std::string& name, const json& arguments = json::object()) {
        const ai::ToolResult result = Call(name, arguments);
        // Dibungkus doctest::String: argumen pesan doctest yang berupa
        // `const char*` non-literal diubah jadi bool, jadi tanpa ini setiap tool
        // call yang gagal di berkas ini melapor "1" alih-alih galatnya.
        REQUIRE_MESSAGE(!result.isError, doctest::String(result.text.c_str()));
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
    // `material.preview` ikut, dan karena alasan yang sama: tidak ada jalur
    // readback GPU di engine ini, jadi ia menempuh tangkapan jendela lalu
    // dipotong. Tanpa penangkap layar ia hanya bisa selalu gagal.
    CHECK(harness.tools.Find("material.preview") == nullptr);
    CHECK(harness.tools.Find("particle.preview") == nullptr);
    CHECK(harness.tools.Find("animation.preview") == nullptr);
    // Sisanya tetap ada, termasuk tool authoring yang tidak butuh gambar.
    CHECK(harness.tools.Find("editor.status") != nullptr);
    CHECK(harness.tools.Find("scene.describe") != nullptr);
    CHECK(harness.tools.Find("material.graph_get") != nullptr);
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

// --- A2: pengurungan jalur ------------------------------------------------------
//
// **Aturan yang berdiri di antara agen dan seluruh berkas di mesin ini.**
// Kriteria terima A2 nomor 2 menyebut `../../etc/passwd`, dan itu memang kasus
// pertamanya — tapi bukan satu-satunya bentuk keluar folder.

TEST_CASE("Jalur di luar folder project ditolak") {
    ScratchDir scratch;
    const std::filesystem::path root = scratch.path / "Proyek";
    std::filesystem::create_directories(root / "Assets" / "Scripts");
    {
        std::ofstream file(root / "Assets" / "Scripts" / "player.lua");
        file << "-- halo";
    }

    std::filesystem::path absolute;
    const auto resolve = [&](const std::string& relative) {
        return ResolveProjectPath(root, relative, absolute);
    };

    SUBCASE("jalur yang sah diterima") {
        CHECK(resolve("Assets/Scripts/player.lua").empty());
        CHECK(absolute == std::filesystem::weakly_canonical(root / "Assets/Scripts/player.lua"));
    }

    SUBCASE("berkas yang belum ada tetap diterima selama di dalam") {
        // `file.write` menulis berkas yang belum ada; menolak jalur yang belum
        // menunjuk apa pun akan membuatnya tidak pernah bisa membuat berkas.
        CHECK(resolve("Assets/baru.txt").empty());
    }

    SUBCASE("naik keluar dengan ..") {
        CHECK_FALSE(resolve("../../etc/passwd").empty());
        CHECK_FALSE(resolve("Assets/../../rahasia.txt").empty());
    }

    SUBCASE("jalur absolut ditolak, bahkan yang di dalam project") {
        // Menerimanya berarti agen harus tahu di mana project ini tersimpan di
        // mesin ini, dan pengetahuan itu tidak pernah dibutuhkan untuk apa pun
        // yang sah.
        CHECK_FALSE(resolve("/etc/passwd").empty());
        CHECK_FALSE(resolve((root / "Assets" / "Scripts" / "player.lua").string()).empty());
    }

    SUBCASE("symlink yang menunjuk keluar ditolak") {
        // Membuang `..` secara leksikal menutup kasus di atas tapi tidak yang
        // ini: jalur teksnya tetap tampak berada di dalam project.
        const std::filesystem::path outside = scratch.path / "diluar.txt";
        {
            std::ofstream file(outside);
            file << "rahasia";
        }
        std::error_code code;
        std::filesystem::create_symlink(outside, root / "Assets" / "pintu", code);
        if (code) {
            MESSAGE("sistem berkas ini tidak mengizinkan symlink — dilewati");
            return;
        }
        CHECK_FALSE(resolve("Assets/pintu").empty());
    }

    SUBCASE("folder tetangga yang namanya berawalan sama") {
        // "/…/Proyek" adalah awalan string dari "/…/ProyekLain", dan
        // perbandingan teks akan menerimanya.
        std::filesystem::create_directories(scratch.path / "ProyekLain");
        {
            std::ofstream file(scratch.path / "ProyekLain" / "curian.txt");
            file << "x";
        }
        CHECK_FALSE(resolve("../ProyekLain/curian.txt").empty());
    }

    SUBCASE("jalur kosong ditolak") { CHECK_FALSE(resolve("").empty()); }
}

TEST_CASE("file.read menolak keluar project dan membaca yang di dalam") {
    Harness harness;
    const std::filesystem::path root = harness.app.CurrentProject().root;
    std::filesystem::create_directories(root / "Assets");
    {
        std::ofstream file(root / "Assets" / "catatan.txt");
        file << "isi berkas";
    }

    const json read = harness.CallOk("file.read", json{{"path", "Assets/catatan.txt"}});
    CHECK(read.at("text") == "isi berkas");
    CHECK_FALSE(read.contains("truncated"));

    const ai::ToolResult escaped = harness.Call("file.read", json{{"path", "../../etc/passwd"}});
    CHECK(escaped.isError);
    CHECK(escaped.text.find("outside the project") != std::string::npos);

    const ai::ToolResult absolute = harness.Call("file.read", json{{"path", "/etc/passwd"}});
    CHECK(absolute.isError);

    const ai::ToolResult folder = harness.Call("file.read", json{{"path", "Assets"}});
    CHECK(folder.isError);
    CHECK(folder.text.find("not a file") != std::string::npos);
}

TEST_CASE("file.read memotong berkas besar dan menyebutnya") {
    Harness harness;
    const std::filesystem::path root = harness.app.CurrentProject().root;
    std::filesystem::create_directories(root / "Assets");
    {
        std::ofstream file(root / "Assets" / "besar.txt");
        file << std::string(300u * 1024u, 'x');
    }
    const json read = harness.CallOk("file.read", json{{"path", "Assets/besar.txt"}});
    // Pemotongan yang tidak disebut membuat agen menyimpulkan berkasnya memang
    // sependek itu.
    CHECK(read.at("truncated") == true);
    CHECK(read.at("bytes") == 300u * 1024u);
    CHECK(read.at("text").get<std::string>().size() == 256u * 1024u);
}

TEST_CASE("project.info menyebut tata letak dan level project") {
    Harness harness;
    const json info = harness.CallOk("project.info");
    CHECK(info.at("name") == "Uji");
    CHECK(info.at("folders").at("assets") == "Assets");
    CHECK(info.at("currentLevel") == "scene");
    REQUIRE(info.at("levels").is_array());
    bool sawScene = false;
    for (const json& level : info.at("levels")) {
        sawScene = sawScene || level == "scene";
    }
    CHECK(sawScene);
}

TEST_CASE("asset.search menyaring dan menyebut totalnya terpisah") {
    Harness harness;
    const assets::AssetDatabase* database = harness.app.Context().assets;
    REQUIRE(database != nullptr);

    SUBCASE("nama yang tidak cocok apa pun") {
        const json found =
            harness.CallOk("asset.search", json{{"name", "tidak ada aset bernama ini"}});
        CHECK(found.at("total") == 0);
        CHECK(found.at("assets").empty());
    }

    SUBCASE("tipe yang tidak dikenal tidak cocok apa pun") {
        const json found = harness.CallOk("asset.search", json{{"type", "TidakAda"}});
        CHECK(found.at("total") == 0);
    }

    SUBCASE("paginasi") {
        const json page = harness.CallOk("asset.search", json{{"limit", 1}});
        CHECK(page.at("returned") <= 1);
        // Total selalu jumlah yang cocok, bukan jumlah yang dikembalikan.
        CHECK(page.at("total") >= page.at("returned").get<std::size_t>());
    }
}

TEST_CASE("asset.info menolak yang tidak ada dengan pesan yang menuntun") {
    Harness harness;
    const ai::ToolResult ghost =
        harness.Call("asset.info", json{{"asset", "00000000-0000-0000-0000-0000deadbeef"}});
    CHECK(ghost.isError);
    CHECK(ghost.text.find("asset.search") != std::string::npos);

    const ai::ToolResult byPath =
        harness.Call("asset.info", json{{"asset", "Assets/tidak-ada.png"}});
    CHECK(byPath.isError);
}

// --- A2: sisi tulis ----------------------------------------------------------------

namespace {

/// Menulis PNG kecil ke `path`, supaya ada tekstur sungguhan untuk diimpor.
void WriteTestTexture(const std::filesystem::path& path, uint32_t size) {
    imageio::Image image;
    image.desc.width = size;
    image.desc.height = size;
    image.desc.channels = 4;
    image.desc.type = imageio::PixelType::UInt8;
    image.desc.colorSpace = imageio::ColorSpace::Srgb;
    image.bytes.resize(static_cast<std::size_t>(size) * size * 4u);
    for (std::size_t pixel = 0; pixel < image.bytes.size() / 4u; ++pixel) {
        image.bytes[pixel * 4u + 0u] = static_cast<uint8_t>(pixel % 256u);
        image.bytes[pixel * 4u + 1u] = 128;
        image.bytes[pixel * 4u + 2u] = 32;
        image.bytes[pixel * 4u + 3u] = 255;
    }
    std::vector<uint8_t> png;
    REQUIRE(imageio::Encode(image, ".png", png).ok);
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(png.data()),
               static_cast<std::streamsize>(png.size()));
}

}  // namespace

TEST_CASE("file.write menolak keluar project dan tidak menimpa diam-diam") {
    Harness harness;

    SUBCASE("menulis berkas baru") {
        const json written = harness.CallOk(
            "file.write", json{{"path", "Assets/Scripts/halo.lua"}, {"text", "print('hai')"}});
        CHECK(written.at("replaced") == false);
        const json read = harness.CallOk("file.read", json{{"path", "Assets/Scripts/halo.lua"}});
        CHECK(read.at("text") == "print('hai')");
    }

    SUBCASE("menolak menimpa tanpa diminta") {
        harness.CallOk("file.write", json{{"path", "Assets/a.txt"}, {"text", "pertama"}});
        // Menimpa diam-diam adalah cara paling sunyi kehilangan pekerjaan.
        const ai::ToolResult second =
            harness.Call("file.write", json{{"path", "Assets/a.txt"}, {"text", "kedua"}});
        CHECK(second.isError);
        CHECK(harness.CallOk("file.read", json{{"path", "Assets/a.txt"}}).at("text") == "pertama");

        const json replaced = harness.CallOk(
            "file.write",
            json{{"path", "Assets/a.txt"}, {"text", "kedua"}, {"overwrite", true}});
        CHECK(replaced.at("replaced") == true);
        CHECK(harness.CallOk("file.read", json{{"path", "Assets/a.txt"}}).at("text") == "kedua");
    }

    SUBCASE("jalur di luar project ditolak") {
        const ai::ToolResult escaped = harness.Call(
            "file.write", json{{"path", "../../tmp/dicuri.txt"}, {"text", "x"}});
        CHECK(escaped.isError);
        CHECK(escaped.text.find("outside the project") != std::string::npos);
        // Dan tidak ada yang tertulis.
        CHECK_FALSE(std::filesystem::exists(harness.app.CurrentProject().root /
                                            ".." / ".." / "tmp" / "dicuri.txt"));
    }
}

TEST_CASE("asset.create menghasilkan aset yang sah dan langsung terindeks") {
    Harness harness;

    SUBCASE("material") {
        const json created =
            harness.CallOk("asset.create", json{{"type", "material"}, {"path", "Assets/Batu"}});
        CHECK(created.at("path") == "Assets/Batu.simmat");
        // Kriteria terima A2 nomor 3: muncul di indeks tanpa restart, jadi
        // GUID-nya sudah ada pada panggilan yang sama.
        REQUIRE(created.contains("guid"));

        const json info = harness.CallOk("asset.info", json{{"asset", created.at("guid")}});
        CHECK(info.at("type") == "Material");

        // **Material yang sah, bukan berkas kosong.** Berkas tanpa isi baru
        // menjadi material sesudah disunting, dan yang membukanya sebelum itu
        // akan mengira asetnya rusak.
        const json body = harness.CallOk("file.read", json{{"path", "Assets/Batu.simmat"}});
        CHECK(body.at("text").get<std::string>().find("output.surface") != std::string::npos);
    }

    SUBCASE("ekstensi tidak digandakan") {
        const json created = harness.CallOk(
            "asset.create", json{{"type", "material"}, {"path", "Assets/Kayu.simmat"}});
        // "Batu.simmat" tidak sedang meminta "Batu.simmat.simmat".
        CHECK(created.at("path") == "Assets/Kayu.simmat");
    }

    SUBCASE("script dengan isi") {
        harness.CallOk("asset.create", json{{"type", "script"},
                                            {"path", "Assets/Scripts/gerak"},
                                            {"text", "function update() end"}});
        CHECK(harness.CallOk("file.read", json{{"path", "Assets/Scripts/gerak.lua"}})
                  .at("text") == "function update() end");
    }

    SUBCASE("tipe yang tidak dikenal ditolak dengan menyebut yang dikenal") {
        const ai::ToolResult result =
            harness.Call("asset.create", json{{"type", "hologram"}, {"path", "Assets/X"}});
        CHECK(result.isError);
        CHECK(result.text.find("material") != std::string::npos);
    }

    SUBCASE("tidak menimpa yang sudah ada") {
        harness.CallOk("asset.create", json{{"type", "material"}, {"path", "Assets/Sama"}});
        const ai::ToolResult again =
            harness.Call("asset.create", json{{"type", "material"}, {"path", "Assets/Sama"}});
        CHECK(again.isError);
    }

    SUBCASE("jalur di luar project ditolak") {
        const ai::ToolResult escaped = harness.Call(
            "asset.create", json{{"type", "material"}, {"path", "../../tmp/Curian"}});
        CHECK(escaped.isError);
    }
}

TEST_CASE("asset.import menyalin dari luar dan mengurung tujuannya") {
    Harness harness;
    const std::filesystem::path outside = harness.scratch.path / "sumber" / "batu.png";
    WriteTestTexture(outside, 64);

    SUBCASE("tujuan bawaan adalah folder aset") {
        const json imported = harness.CallOk("asset.import", json{{"source", outside.string()}});
        CHECK(imported.at("path") == "Assets/batu.png");
        REQUIRE(imported.contains("guid"));
        CHECK(imported.at("type") == "Texture");
    }

    SUBCASE("sumber yang tidak ada ditolak") {
        const ai::ToolResult missing = harness.Call(
            "asset.import", json{{"source", (harness.scratch.path / "hantu.png").string()}});
        CHECK(missing.isError);
    }

    SUBCASE("tujuan di luar project ditolak walaupun sumbernya sah") {
        // Sumbernya memang boleh di luar — itu gunanya tool ini. Tujuannya tidak
        // pernah.
        const ai::ToolResult escaped =
            harness.Call("asset.import", json{{"source", outside.string()},
                                              {"destination", "../../tmp/curian.png"}});
        CHECK(escaped.isError);
        CHECK(escaped.text.find("outside the project") != std::string::npos);
    }

    SUBCASE("tidak menimpa tanpa diminta") {
        harness.CallOk("asset.import", json{{"source", outside.string()}});
        const ai::ToolResult again =
            harness.Call("asset.import", json{{"source", outside.string()}});
        CHECK(again.isError);
        CHECK(harness.CallOk("asset.import",
                             json{{"source", outside.string()}, {"overwrite", true}})
                  .at("path") == "Assets/batu.png");
    }
}

TEST_CASE("asset.thumbnail menggambar tekstur dan menolak yang lain") {
    Harness harness;
    const std::filesystem::path outside = harness.scratch.path / "sumber" / "besar.png";
    WriteTestTexture(outside, 512);
    const json imported = harness.CallOk("asset.import", json{{"source", outside.string()}});
    const std::string guid = imported.at("guid");

    SUBCASE("tekstur menghasilkan PNG yang lebih kecil dari sumbernya") {
        const ai::ToolResult result =
            harness.Call("asset.thumbnail", json{{"asset", guid}, {"size", 64}});
        REQUIRE_FALSE(result.isError);
        REQUIRE_FALSE(result.imageBytes.empty());
        CHECK(result.imageMimeType == "image/png");

        // Tanda tangan PNG, lalu lebar dari kepala IHDR-nya.
        REQUIRE(result.imageBytes.size() > 24);
        CHECK(result.imageBytes[0] == 137);
        CHECK(result.imageBytes[1] == 'P');
        const uint32_t width = (uint32_t(result.imageBytes[16]) << 24) |
                               (uint32_t(result.imageBytes[17]) << 16) |
                               (uint32_t(result.imageBytes[18]) << 8) |
                               uint32_t(result.imageBytes[19]);
        // Level terkecil yang masih setidaknya sebesar yang diminta.
        CHECK(width == 64);
        CHECK(result.text.find("512x512") != std::string::npos);
    }

    SUBCASE("aset bukan tekstur ditolak dengan menyebut jenisnya") {
        const json material =
            harness.CallOk("asset.create", json{{"type", "material"}, {"path", "Assets/M"}});
        const ai::ToolResult result =
            harness.Call("asset.thumbnail", json{{"asset", material.at("guid")}});
        // Gambar kosong yang dikirim sebagai thumbnail membuat agen menyimpulkan
        // asetnya yang kosong.
        CHECK(result.isError);
        CHECK(result.text.find("Material") != std::string::npos);
        CHECK(result.imageBytes.empty());
    }

    SUBCASE("aset yang tidak ada ditolak") {
        const ai::ToolResult result =
            harness.Call("asset.thumbnail", json{{"asset", "Assets/hantu.png"}});
        CHECK(result.isError);
    }
}

// --- A3: tool authoring -------------------------------------------------------

TEST_CASE("lua.eval menjawab nilai, dan kesalahannya tidak menjatuhkan editor") {
    Harness harness;

    SUBCASE("ekspresi menghasilkan nilai") {
        // Kode diperlakukan sebagai ekspresi lebih dulu. Tanpa itu "1 + 1"
        // tidak menghasilkan apa pun, dan tool yang tidak menjawab pertanyaan
        // sederhana akan ditinggalkan agen sama seperti ditinggalkan orang.
        const json result = harness.CallOk("lua.eval", json{{"code", "1 + 1"}});
        REQUIRE(result.at("ok") == true);
        REQUIRE(result.at("values").size() == 1);
        CHECK(result.at("values")[0].at("value") == "2");
    }

    SUBCASE("pernyataan tetap dijalankan") {
        harness.CallOk("lua.eval", json{{"code", "AgenMenulis = 41 + 1"}});
        const json read = harness.CallOk("lua.eval", json{{"code", "AgenMenulis"}});
        CHECK(read.at("values")[0].at("value") == "42");
    }

    SUBCASE("tabel dipecah jadi anak, bukan diratakan jadi alamat") {
        const json result =
            harness.CallOk("lua.eval", json{{"code", "{ warna = 'emas', kasar = 0.4 }"}});
        REQUIRE(result.at("values").size() == 1);
        const json& value = result.at("values")[0];
        REQUIRE(value.contains("children"));
        CHECK(value.at("children").size() == 2);
        // Alamat tabel berbeda tiap kali dijalankan meski keadaannya sama, jadi
        // ia adalah selisih palsu bagi agen yang membandingkan dua jawaban.
        const std::string shown = value.at("value").get<std::string>();
        CHECK(shown.find("0x") == std::string::npos);
        CHECK(shown == "table (2 fields)");
    }

    SUBCASE("kesalahan runtime kembali beserta traceback") {
        // **Kriteria terima A3 nomor 3.** Yang diuji bukan hanya "ada pesan",
        // tapi bahwa editor masih hidup sesudahnya — panggilan berikutnya di
        // harness yang sama harus tetap berhasil.
        const ai::ToolResult failed =
            harness.Call("lua.eval", json{{"code", "error('sengaja gagal')"}});
        REQUIRE(failed.isError);
        const json body = json::parse(failed.text, nullptr, false);
        REQUIRE_FALSE(body.is_discarded());
        CHECK(body.at("ok") == false);
        const std::string message = body.at("error").get<std::string>();
        CHECK(message.find("sengaja gagal") != std::string::npos);
        CHECK(message.find("stack traceback") != std::string::npos);

        CHECK(harness.CallOk("lua.eval", json{{"code", "'masih hidup'"}}).at("ok") == true);
    }

    SUBCASE("kesalahan sintaks juga kembali sebagai galat, bukan sebagai nilai") {
        const ai::ToolResult failed = harness.Call("lua.eval", json{{"code", "if then end"}});
        CHECK(failed.isError);
        CHECK(harness.CallOk("lua.eval", json{{"code", "2"}}).at("ok") == true);
    }
}

TEST_CASE("lua.script_write memeriksa sintaks sebelum ada yang ditulis") {
    Harness harness;

    SUBCASE("skrip yang sah tersimpan dan terindeks") {
        const json written =
            harness.CallOk("lua.script_write", json{{"path", "Assets/Scripts/putar.lua"},
                                                    {"text", "return { OnUpdate = function() end }"}});
        CHECK(written.at("ok") == true);
        CHECK(written.at("replaced") == false);
        // Terindeks pada panggilan yang sama, alasan yang sama dengan A2 nomor 3.
        REQUIRE(written.contains("guid"));
        CHECK(written.at("reloaded") == true);

        CHECK(harness.CallOk("file.read", json{{"path", "Assets/Scripts/putar.lua"}})
                  .at("text")
                  .get<std::string>()
                  .find("OnUpdate") != std::string::npos);
    }

    SUBCASE("skrip yang tidak terurai tidak pernah sampai ke disk") {
        const ai::ToolResult failed =
            harness.Call("lua.script_write", json{{"path", "Assets/Scripts/rusak.lua"},
                                                  {"text", "function ("}});
        REQUIRE(failed.isError);
        // Inilah yang membedakan tool ini dari file.write: berkasnya tidak ada.
        CHECK(harness.Call("file.read", json{{"path", "Assets/Scripts/rusak.lua"}}).isError);
    }

    SUBCASE("berkas non-.lua ditolak") {
        const ai::ToolResult failed = harness.Call(
            "lua.script_write", json{{"path", "Assets/catatan.txt"}, {"text", "-- kosong"}});
        CHECK(failed.isError);
    }

    SUBCASE("tidak menimpa tanpa diminta") {
        const json arguments{{"path", "Assets/Scripts/sama.lua"}, {"text", "return {}"}};
        harness.CallOk("lua.script_write", arguments);
        CHECK(harness.Call("lua.script_write", arguments).isError);

        json overwrite = arguments;
        overwrite["overwrite"] = true;
        CHECK(harness.CallOk("lua.script_write", overwrite).at("replaced") == true);
    }

    SUBCASE("jalur di luar project ditolak") {
        CHECK(harness
                  .Call("lua.script_write",
                        json{{"path", "../../tmp/curian.lua"}, {"text", "return {}"}})
                  .isError);
    }
}

TEST_CASE("material.graph_get dan graph_set bolak-balik tanpa merusak") {
    Harness harness;
    const json created =
        harness.CallOk("asset.create", json{{"type", "material"}, {"path", "Assets/Emas"}});
    const std::string guid = created.at("guid").get<std::string>();

    const json read = harness.CallOk("material.graph_get", json{{"asset", guid}});
    CHECK(read.at("path") == "Emas.simmat");
    REQUIRE(read.at("graph").is_object());
    const std::size_t nodes = read.at("summary").at("nodes").get<std::size_t>();
    CHECK(nodes > 0);

    SUBCASE("menulis kembali apa yang dibaca tetap sah") {
        const json written =
            harness.CallOk("material.graph_set", json{{"asset", guid}, {"graph", read.at("graph")}});
        CHECK(written.at("ok") == true);
        CHECK(written.at("summary").at("nodes") == nodes);

        // Dan yang dibaca sesudahnya masih graph yang sama, bukan berkas yang
        // sah tapi kosong.
        CHECK(harness.CallOk("material.graph_get", json{{"asset", guid}})
                  .at("summary")
                  .at("nodes") == nodes);
    }

    SUBCASE("JSON yang tidak terurai ditolak di tahap parse") {
        const ai::ToolResult failed =
            harness.Call("material.graph_set", json{{"asset", guid}, {"text", "{ bukan json"}});
        REQUIRE(failed.isError);
        const json body = json::parse(failed.text, nullptr, false);
        REQUIRE_FALSE(body.is_discarded());
        CHECK(body.at("stage") == "parse");
    }

    SUBCASE("graph tanpa keluaran ditolak di tahap validasi, dan berkasnya utuh") {
        json empty = read.at("graph");
        empty["nodes"] = json::array();
        empty["links"] = json::array();
        const ai::ToolResult failed =
            harness.Call("material.graph_set", json{{"asset", guid}, {"graph", empty}});
        REQUIRE(failed.isError);
        const json body = json::parse(failed.text, nullptr, false);
        REQUIRE_FALSE(body.is_discarded());
        CHECK(body.at("stage") == "validate");
        REQUIRE(body.at("issues").size() > 0);

        // **Yang ditolak tidak boleh setengah tertulis.** Ini kelas bug yang
        // sama dengan yang lolos di A1: tool melapor sesuatu yang tidak sesuai
        // dengan keadaan sebenarnya.
        CHECK(harness.CallOk("material.graph_get", json{{"asset", guid}})
                  .at("summary")
                  .at("nodes") == nodes);
    }

    SUBCASE("aset yang bukan material ditolak") {
        harness.CallOk("asset.create",
                       json{{"type", "script"}, {"path", "Assets/bukan"}, {"text", "return {}"}});
        CHECK(harness.Call("material.graph_get", json{{"asset", "bukan.lua"}}).isError);
    }
}

TEST_CASE("Dua kelompok tool berbagi satu penangkap layar, dan keduanya mendapatkannya") {
    // **Regresi.** Composition root sempat memanggil `std::move` pada
    // ScreenshotFn di pendaftaran pertama, jadi pendaftaran kedua menerima
    // `std::function` kosong dan `material.preview` diam-diam tidak terdaftar.
    // Tidak ada peringatan kompilasi: fungsi kosong itu sah, dan bernilai false
    // persis seperti "perangkat ini tidak bisa menangkap layar".
    Harness harness;
    ai::ToolRegistry withCapture;
    ai::ResourceRegistry resources;

    int calls = 0;
    ScreenshotFn capture = [&calls](const CaptureRect*, std::vector<uint8_t>& png,
                                    std::string&) {
        ++calls;
        png = {0x89, 'P', 'N', 'G'};
        return true;
    };

    RegisterEditorTools(withCapture, resources, harness.app, capture);
    RegisterAuthoringTools(withCapture, harness.app, std::move(capture));

    CHECK(withCapture.Find("editor.screenshot") != nullptr);
    CHECK(withCapture.Find("viewport.capture") != nullptr);
    CHECK(withCapture.Find("material.preview") != nullptr);
    CHECK(withCapture.Find("particle.preview") != nullptr);
    CHECK(withCapture.Find("animation.preview") != nullptr);
    CHECK(calls == 0);
}

TEST_CASE("material.graph_set menolak apa yang akan dibuang pemuatnya diam-diam") {
    // **Ini ketahuan dengan menjalankannya, bukan dengan membacanya.** Percobaan
    // pertama menyetel `pinValues` dengan nama `metalness` dan `roughness` —
    // tebakan yang wajar, dan keduanya salah. Berkasnya terurai tanpa galat,
    // lolos validasi, dan tersimpan persis seperti sebelumnya. Yang dilihat agen
    // adalah `ok: true`; yang dilihat orang adalah bola putih.
    Harness harness;
    const json created =
        harness.CallOk("asset.create", json{{"type", "material"}, {"path", "Assets/Logam"}});
    const std::string guid = created.at("guid").get<std::string>();
    const json read = harness.CallOk("material.graph_get", json{{"asset", guid}});

    SUBCASE("skema pin memberi nama yang sebenarnya, karena graph-nya tidak") {
        // Material bawaan tidak menulis satu pun "pins" — kunci itu hanya muncul
        // saat nilainya menyimpang dari bawaan katalog. Tanpa skema, graph yang
        // dibaca agen tidak memuat contoh nama pin sama sekali.
        REQUIRE(read.contains("pinSchema"));
        REQUIRE(read.at("pinSchema").contains("output.surface"));
        std::vector<std::string> names;
        for (const json& pin : read.at("pinSchema").at("output.surface")) {
            names.push_back(pin.at("name").get<std::string>());
        }
        CHECK(std::find(names.begin(), names.end(), "baseColor") != names.end());
        // Nama OpenPBR, bukan nama yang akan ditebak siapa pun dari luar.
        CHECK(std::find(names.begin(), names.end(), "baseMetalness") != names.end());
        CHECK(std::find(names.begin(), names.end(), "metalness") == names.end());
    }

    SUBCASE("kunci node yang tidak dibaca pemuat ditolak") {
        json guessed = read.at("graph");
        guessed.at("nodes")[0]["pinValues"] = json{{"baseMetalness", "1.0"}};
        const ai::ToolResult failed =
            harness.Call("material.graph_set", json{{"asset", guid}, {"graph", guessed}});
        REQUIRE(failed.isError);
        const json body = json::parse(failed.text, nullptr, false);
        REQUIRE_FALSE(body.is_discarded());
        CHECK(body.at("stage") == "shape");
        CHECK(body.at("dropped").dump().find("pinValues") != std::string::npos);
    }

    SUBCASE("nama pin yang tidak ada pada tipe node itu ditolak") {
        json guessed = read.at("graph");
        guessed.at("nodes")[0]["pins"] = json{{"metalness", "1.0"}, {"roughness", "0.35"}};
        const ai::ToolResult failed =
            harness.Call("material.graph_set", json{{"asset", guid}, {"graph", guessed}});
        REQUIRE(failed.isError);
        const json body = json::parse(failed.text, nullptr, false);
        CHECK(body.at("stage") == "shape");
        CHECK(body.at("dropped").size() == 2);
    }

    SUBCASE("nama pin yang benar diterima dan benar-benar tersimpan") {
        json gold = read.at("graph");
        gold.at("nodes")[0]["pins"] = json{{"baseColor", "float3(1.0, 0.766, 0.336)"},
                                           {"baseMetalness", "1.0"},
                                           {"specularRoughness", "0.35"}};
        CHECK(harness.CallOk("material.graph_set", json{{"asset", guid}, {"graph", gold}})
                  .at("ok") == true);

        // **Dibaca kembali dari disk.** Kalau nilainya hilang di perjalanan,
        // graph_set tetap melapor berhasil — dan hanya pembacaan ulang yang
        // membedakan keduanya.
        const json again = harness.CallOk("material.graph_get", json{{"asset", guid}});
        const json& pins = again.at("graph").at("nodes")[0].at("pins");
        CHECK(pins.at("baseMetalness") == "1.0");
        CHECK(pins.at("specularRoughness") == "0.35");
    }
}

TEST_CASE("particle.get dan particle.set bolak-balik, dan menolak yang akan dibuang") {
    Harness harness;
    const json created =
        harness.CallOk("asset.create", json{{"type", "particle"}, {"path", "Assets/Asap"}});
    CHECK(created.at("path") == "Assets/Asap.simfx");
    const std::string guid = created.at("guid").get<std::string>();

    const json read = harness.CallOk("particle.get", json{{"asset", guid}});
    REQUIRE(read.at("effect").is_object());
    // Efek baru sudah punya satu emitter: berkas tanpa emitter dibuka orang
    // sebagai kanvas kosong dan tidak memberi tahu apa pun tentang bentuknya.
    REQUIRE(read.at("summary").at("emitters").size() == 1);

    SUBCASE("menulis kembali apa yang dibaca tetap sah") {
        const json written =
            harness.CallOk("particle.set", json{{"asset", guid}, {"effect", read.at("effect")}});
        CHECK(written.at("ok") == true);
        CHECK(harness.CallOk("particle.get", json{{"asset", guid}})
                  .at("summary")
                  .at("emitters")
                  .size() == 1);
    }

    SUBCASE("perubahan yang sah benar-benar tersimpan") {
        json edited = read.at("effect");
        edited["emitters"][0]["name"] = "Asap naik";
        edited["emitters"][0]["maxParticles"] = 2048;
        CHECK(harness.CallOk("particle.set", json{{"asset", guid}, {"effect", edited}})
                  .at("ok") == true);

        // Dibaca kembali dari disk: kalau nilainya hilang di perjalanan,
        // particle.set tetap melapor berhasil.
        const json again = harness.CallOk("particle.get", json{{"asset", guid}});
        CHECK(again.at("summary").at("emitters")[0].at("name") == "Asap naik");
        CHECK(again.at("summary").at("emitters")[0].at("maxParticles") == 2048);
    }

    SUBCASE("kunci yang tidak selamat ditulis ulang ditolak dengan menyebutnya") {
        json guessed = read.at("effect");
        // Salah nama, dan salah sarang. Keduanya terurai tanpa galat dan
        // menghasilkan efek yang persis sama dengan sebelumnya.
        guessed["emitters"][0]["maksimalPartikel"] = 999;
        const ai::ToolResult failed =
            harness.Call("particle.set", json{{"asset", guid}, {"effect", guessed}});
        REQUIRE(failed.isError);
        const json body = json::parse(failed.text, nullptr, false);
        REQUIRE_FALSE(body.is_discarded());
        CHECK(body.at("stage") == "shape");
        CHECK(body.at("dropped").dump().find("maksimalPartikel") != std::string::npos);
    }

    SUBCASE("aset yang bukan efek ditolak") {
        harness.CallOk("asset.create", json{{"type", "material"}, {"path", "Assets/Bukan"}});
        CHECK(harness.Call("particle.get", json{{"asset", "Bukan.simmat"}}).isError);
    }
}

TEST_CASE("Tool terrain membaca, menulis, dan memahat dengan satu goresan undo") {
    Harness harness;
    // Terrain lahir di dalam editor, bukan dari berkas: `TerrainStore::Adopt`
    // ada persis untuk itu, dan uji ini memakai jalur yang sama dengan menu
    // "Terrain baru".
    terrain::TerrainDesc desc;
    desc.tileSamples = 64;
    desc.tilesX = 1;
    desc.tilesY = 1;
    desc.sampleSpacing = 1.0f;
    desc.minHeight = 0.0f;
    desc.maxHeight = 100.0f;

    const std::filesystem::path path =
        harness.app.AssetsDirectory() / "Bukit.simterrain";
    terrain::Terrain built(desc);
    terrain::TerrainDocument document;
    document.name = "Bukit";
    document.desc = desc;
    document.heightmapFile = "Bukit.png";
    {
        // Dokumennya saja: yang diuji di sini adalah tool, dan tool memakai
        // terrain yang sudah ada di TerrainStore — berkas pendampingnya tidak
        // pernah dibaca karena `Adopt` mendahuluinya.
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << terrain::SaveDocumentToString(document, {});
    }
    harness.app.Context().assets->ScanNow();
    const assets::AssetRecord* record =
        harness.app.Context().assets->FindByRelativePath("Bukit.simterrain");
    REQUIRE(record != nullptr);
    harness.app.Context().terrains->Adopt(record->guid, std::move(built), document);
    const std::string guid = record->guid.ToString();

    SUBCASE("heightmap_get menolak petak yang terlalu besar dengan menyebut angkanya") {
        const ai::ToolResult failed = harness.Call(
            "terrain.heightmap_get",
            json{{"asset", guid},
                 {"region", json{{"x", 0}, {"y", 0}, {"width", 64}, {"height", 64}}}});
        // 64x64 = 4096, masih di bawah batas — ini harus berhasil.
        CHECK_FALSE(failed.isError);
    }

    SUBCASE("menulis lalu membaca kembali mengembalikan tinggi yang sama") {
        json rows = json::array();
        for (int y = 0; y < 4; ++y) {
            json row = json::array();
            for (int x = 0; x < 4; ++x) {
                row.push_back(10.0f + static_cast<float>(x));
            }
            rows.push_back(std::move(row));
        }
        const json region{{"x", 2}, {"y", 3}, {"width", 4}, {"height", 4}};
        CHECK(harness
                  .CallOk("terrain.heightmap_set",
                          json{{"asset", guid}, {"region", region}, {"heights", rows}})
                  .at("samplesWritten") == 16);

        const json read = harness.CallOk("terrain.heightmap_get",
                                         json{{"asset", guid}, {"region", region}});
        // Kuantisasi ke uint16 atas rentang 0..100 m: satu langkah ~1.5 mm,
        // jadi yang dibandingkan adalah kedekatan, bukan kesamaan bit.
        CHECK(read.at("heights")[0][0].get<float>() == doctest::Approx(10.0f).epsilon(0.001));
        CHECK(read.at("heights")[3][3].get<float>() == doctest::Approx(13.0f).epsilon(0.001));
        CHECK(read.at("maxHeight").get<float>() == doctest::Approx(13.0f).epsilon(0.001));
    }

    SUBCASE("baris yang tidak sepadan ditolak sebelum satu sampel pun ditulis") {
        const json region{{"x", 0}, {"y", 0}, {"width", 4}, {"height", 4}};
        const json shortRows = json::array({json::array({1.0, 2.0})});
        const ai::ToolResult failed = harness.Call(
            "terrain.heightmap_set",
            json{{"asset", guid}, {"region", region}, {"heights", shortRows}});
        REQUIRE(failed.isError);
        // Setengah petak tertulis akan terlihat sebagai tebing yang tidak
        // diminta siapa pun, jadi yang diperiksa adalah terrain-nya masih rata.
        const json read = harness.CallOk("terrain.heightmap_get",
                                         json{{"asset", guid}, {"region", region}});
        CHECK(read.at("minHeight").get<float>() == read.at("maxHeight").get<float>());
    }

    SUBCASE("sculpt menaikkan terrain, dan seluruh panggilan jadi satu goresan") {
        const json region{{"x", 0}, {"y", 0}, {"width", 32}, {"height", 32}};
        const float before = harness.CallOk("terrain.heightmap_get",
                                            json{{"asset", guid}, {"region", region}})
                                 .at("maxHeight")
                                 .get<float>();

        const json sculpted =
            harness.CallOk("terrain.sculpt", json{{"asset", guid},
                                                  {"kind", "raise"},
                                                  {"at", json::array({json::array({8.0, 8.0}),
                                                                      json::array({12.0, 12.0})})},
                                                  {"radius", 6.0},
                                                  {"strength", 4.0}});
        CHECK(sculpted.at("dabs") == 2);
        // Dua sentuhan, satu goresan: satu undo dari sisi orang mengembalikan
        // keduanya sekaligus.
        CHECK(sculpted.at("undoDepth") == 1);

        const float after = harness.CallOk("terrain.heightmap_get",
                                           json{{"asset", guid}, {"region", region}})
                                .at("maxHeight")
                                .get<float>();
        CHECK(after > before);
    }

    SUBCASE("kuas yang tidak dikenal ditolak dengan menyebut yang dikenal") {
        const ai::ToolResult failed = harness.Call(
            "terrain.sculpt",
            json{{"asset", guid}, {"kind", "meledak"}, {"at", json::array({json::array({1.0, 1.0})})}});
        REQUIRE(failed.isError);
        CHECK(failed.text.find("flatten") != std::string::npos);
    }
}

TEST_CASE("Tool animasi membaca klip dan mengganti kunci satu kanal") {
    Harness harness;
    // Klip ditulis lewat penulis kanonis modul animasi, bukan dikarang uji:
    // berkas yang dikarang uji hanya membuktikan uji itu sepakat dengan dirinya
    // sendiri.
    animation::Clip clip;
    clip.name = "Lambai";
    clip.duration = 2.0f;
    clip.frameRate = 30.0f;
    const int track = clip.EnsureTrack("Tangan", animation::Channel::TranslationY);
    clip.TrackAt(track).curve.AddKey(CurveKey{0.0f, 0.0f, 0.0f, 0.0f, Interpolation::Linear});
    clip.TrackAt(track).curve.AddKey(CurveKey{1.0f, 3.0f, 0.0f, 0.0f, Interpolation::Linear});

    animation::ClipDocument document;

    const std::filesystem::path path = harness.app.AssetsDirectory() / "Lambai.simanim";
    REQUIRE(animation::SaveClip(clip, document, path).ok);
    harness.app.Context().assets->ScanNow();

    const json info = harness.CallOk("animation.clip_info", json{{"asset", "Lambai.simanim"}});
    CHECK(info.at("duration").get<float>() == doctest::Approx(2.0f));
    REQUIRE(info.at("tracks").size() == 1);
    CHECK(info.at("tracks")[0].at("bone") == "Tangan");
    CHECK(info.at("tracks")[0].at("keys") == 2);
    CHECK(info.at("tracks")[0].at("maxValue").get<float>() == doctest::Approx(3.0f));

    SUBCASE("kanal yang salah ketik ditolak, bukan mendarat di kanal lain") {
        // `ChannelFromString` memilih kanal pertama untuk teks yang tidak
        // dikenal, jadi tanpa pemeriksaan ini "translationYY" akan menulis ke
        // TranslationX tanpa satu pun tanda.
        const ai::ToolResult failed =
            harness.Call("animation.key_set", json{{"asset", "Lambai.simanim"},
                                                   {"bone", "Tangan"},
                                                   {"channel", "translationYY"},
                                                   {"keys", json::array()}});
        REQUIRE(failed.isError);
        CHECK(failed.text.find("Known:") != std::string::npos);

        // Dan kanal aslinya tidak tersentuh.
        CHECK(harness.CallOk("animation.clip_info", json{{"asset", "Lambai.simanim"}})
                  .at("tracks")[0]
                  .at("keys") == 2);
    }

    SUBCASE("kunci baru menggantikan seluruh kanal dan benar-benar tersimpan") {
        const std::string channel =
            info.at("tracks")[0].at("channel").get<std::string>();
        const json keys = json::array({json{{"time", 0.0}, {"value", 0.0}},
                                       json{{"time", 0.5}, {"value", 5.0}},
                                       json{{"time", 1.0}, {"value", 0.0}}});
        const json written = harness.CallOk("animation.key_set",
                                            json{{"asset", "Lambai.simanim"},
                                                 {"bone", "Tangan"},
                                                 {"channel", channel},
                                                 {"keys", keys}});
        CHECK(written.at("keys") == 3);

        // Dibaca ulang dari disk: kalau kuncinya tidak sampai ke berkas,
        // key_set tetap melapor berhasil.
        const json again =
            harness.CallOk("animation.clip_info", json{{"asset", "Lambai.simanim"}});
        CHECK(again.at("tracks")[0].at("keys") == 3);
        CHECK(again.at("tracks")[0].at("maxValue").get<float>() == doctest::Approx(5.0f));
    }

    SUBCASE("track baru dibuat untuk bone dan kanal yang belum ada") {
        harness.CallOk("animation.key_set",
                       json{{"asset", "Lambai.simanim"},
                            {"bone", "Kaki"},
                            {"channel", "ScaleX"},
                            {"keys", json::array({json{{"time", 0.0}, {"value", 1.0}}})}});
        CHECK(harness.CallOk("animation.clip_info", json{{"asset", "Lambai.simanim"}})
                  .at("tracks")
                  .size() == 2);
    }

    SUBCASE("interpolasi yang tidak dikenal ditolak dengan menyebut yang dikenal") {
        const ai::ToolResult failed = harness.Call(
            "animation.key_set",
            json{{"asset", "Lambai.simanim"},
                 {"bone", "Tangan"},
                 {"channel", info.at("tracks")[0].at("channel")},
                 {"keys", json::array({json{{"time", 0.0}, {"value", 1.0},
                                            {"interpolation", "melengkung"}}})}});
        REQUIRE(failed.isError);
        CHECK(failed.text.find("bezier") != std::string::npos);
    }
}

namespace {

/// Perender yang tidak menggambar apa pun tapi bisa menyerahkan pikselnya.
///
/// Cukup untuk menguji satu hal: apa yang menentukan `viewport.capture`
/// didaftarkan. Sejak ada readback, jawabannya perendernya — bukan jendelanya.
class FakeCapturingRenderer final : public render::IViewportRenderer {
public:
    void Resize(uint32_t, uint32_t) override {}
    render::MeshAsset AcquireMesh(std::string_view) override { return {}; }
    void Render(const render::ViewportDesc&, const render::ViewportScene&) override {}
    render::TextureHandle ColorTarget() const override { return 1; }
    Vec2 ColorTargetUvMax() const override { return Vec2(1.0f, 1.0f); }
    uint32_t Width() const override { return 4; }
    uint32_t Height() const override { return 2; }
    const char* Name() const override { return "fake"; }

    bool CapturePixels(std::vector<uint8_t>& outRgba, uint32_t& outWidth, uint32_t& outHeight,
                       std::string&) override {
        outWidth = 4;
        outHeight = 2;
        outRgba.assign(static_cast<std::size_t>(outWidth) * outHeight * 4, 0x40);
        return true;
    }
};

}  // namespace

TEST_CASE("Yang menentukan viewport.capture didaftarkan adalah perendernya, bukan jendelanya") {
    // **Kontrak SimHeadless.** Di sana tidak ada jendela sama sekali, jadi
    // `editor.screenshot` memang tidak boleh ada — tapi `viewport.capture`
    // membaca target render, dan mengikatnya pada penangkap jendela berarti
    // agen di CI kehilangan satu-satunya cara melihat hasil kerjanya.
    Harness harness;
    FakeCapturingRenderer renderer;
    harness.app.Context().viewportRenderer = &renderer;

    ai::ToolRegistry tools;
    ai::ResourceRegistry resources;
    RegisterEditorTools(tools, resources, harness.app);  // tanpa ScreenshotFn

    CHECK(tools.Find("viewport.capture") != nullptr);
    CHECK(tools.Find("editor.screenshot") == nullptr);

    // Dan ia benar-benar mengembalikan gambar, bukan galat "tidak ada jendela".
    harness.app.Context().viewportRect.size = Vec2(4.0f, 2.0f);
    harness.app.Context().viewportRect.mainSize = Vec2(4.0f, 2.0f);
    const ai::ToolDefinition* capture = tools.Find("viewport.capture");
    REQUIRE(capture != nullptr);
    // Handler-nya mengantre ke main thread; uji ini adalah main thread-nya —
    // dan sejak sekarang ia juga MENGATAKANNYA. `MainThreadQueue` tidak menebak
    // siapa main thread-nya, ia diberi tahu lewat `BindMainThread`, dan tanpa itu
    // `mainThread_` tetap id kosong yang tidak sama dengan thread mana pun.
    //
    // **Akibatnya bukan uji yang merah melainkan binari yang mati.** `Drain()`
    // menegakkannya dengan `SIM_ASSERT`, yang di Debug memanggil `abort()` —
    // jadi uji ini menjatuhkan seluruh proses, dan setiap uji sesudahnya tidak
    // pernah dijalankan. Salah satunya "Stop mengembalikan scene persis ke
    // keadaan sebelum Play": bukti Play-in-Editor yang dicatat sebagai ada,
    // tidak pernah benar-benar berjalan di suite penuh.
    MainThreadQueue::Get().BindMainThread();

    std::atomic<bool> done{false};
    ai::ToolResult result;
    std::thread caller([&] {
        result = capture->handler("{}");
        done.store(true);
    });
    while (!done.load()) {
        MainThreadQueue::Get().Drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    caller.join();

    CHECK_FALSE(result.isError);
    CHECK_FALSE(result.imageBytes.empty());
    CHECK(result.imageMimeType == "image/png");
    // Ukurannya dari perender, bukan dari tata letak panel: byte 16..23 kepala
    // IHDR-nya.
    REQUIRE(result.imageBytes.size() > 24);
    const auto be32 = [&](std::size_t at) {
        return (uint32_t(result.imageBytes[at]) << 24) |
               (uint32_t(result.imageBytes[at + 1]) << 16) |
               (uint32_t(result.imageBytes[at + 2]) << 8) | uint32_t(result.imageBytes[at + 3]);
    };
    CHECK(be32(16) == 4);
    CHECK(be32(20) == 2);
}

// --- E9: Play-in-Editor, pemisahan state --------------------------------------

TEST_CASE("Stop mengembalikan scene persis ke keadaan sebelum Play") {
    // **Kriteria E9.** Play mengambil cuplikan sebelum satu baris skrip pun
    // berjalan, dan Stop membangun ulang dunia dari cuplikan itu. Yang belum ada
    // sampai sekarang adalah sesuatu yang membuktikannya: sebuah Stop yang
    // mengembalikan *hampir* semuanya terlihat benar sampai seseorang kehilangan
    // pekerjaan setengah jam.
    Harness harness;
    const std::string before = scene::SaveLevelToString(harness.World());
    const std::size_t baseline = harness.World().Count();

    SUBCASE("entity yang dibuat saat Play hilang") {
        harness.app.Play();
        CHECK(harness.app.IsPlaying());
        harness.CreateEntity("DibuatSaatPlay");
        CHECK(harness.World().Count() == baseline + 1);

        harness.app.Stop();
        CHECK_FALSE(harness.app.IsPlaying());
        CHECK(harness.World().Count() == baseline);
        // Byte-per-byte, bukan sekadar jumlahnya. Dunia yang jumlah entity-nya
        // sama tapi transform-nya bergeser adalah dunia yang tidak dikembalikan.
        CHECK(scene::SaveLevelToString(harness.World()) == before);
    }

    SUBCASE("entity yang dihapus saat Play kembali") {
        const std::string guid = harness.CreateEntity("AkanDihapus");
        const std::string withExtra = scene::SaveLevelToString(harness.World());

        harness.app.Play();
        harness.CallOk("entity.delete", json{{"entities", json::array({guid})}});
        CHECK(harness.World().Count() == baseline);

        harness.app.Stop();
        CHECK(scene::SaveLevelToString(harness.World()) == withExtra);
    }

    SUBCASE("transform yang digeser saat Play kembali") {
        const std::string guid = harness.CreateEntity("Bergeser");
        const std::string withEntity = scene::SaveLevelToString(harness.World());

        harness.app.Play();
        harness.CallOk("entity.modify",
                       json{{"entity", guid},
                            {"component", "Transform"},
                            {"values", json{{"position", json::array({5.0, 6.0, 7.0})}}}});
        CHECK(scene::SaveLevelToString(harness.World()) != withEntity);

        harness.app.Stop();
        CHECK(scene::SaveLevelToString(harness.World()) == withEntity);
    }

    SUBCASE("seleksi kembali ke tempat orang meninggalkannya") {
        // Pengguna menekan Play untuk melihat sesuatu berjalan, bukan untuk
        // kehilangan tempat ia sedang bekerja.
        const std::string guid = harness.CreateEntity("Terpilih");
        harness.CallOk("selection.set", json{{"entities", json::array({guid})}});
        const json chosen = harness.CallOk("selection.get");
        REQUIRE(chosen.at("entities").size() == 1);

        harness.app.Play();
        harness.CallOk("selection.set", json{{"entities", json::array()}});
        harness.app.Stop();

        const json after = harness.CallOk("selection.get");
        REQUIRE(after.at("entities").size() == 1);
        CHECK(after.at("entities")[0].at("guid") == guid);
    }

    SUBCASE("Play dua kali berturut-turut tetap mengembalikan yang benar") {
        for (int round = 0; round < 2; ++round) {
            harness.app.Play();
            harness.CreateEntity("Sementara" + std::to_string(round));
            harness.app.Stop();
            CHECK(scene::SaveLevelToString(harness.World()) == before);
        }
    }

    SUBCASE("Stop tanpa Play tidak mengubah apa pun") {
        harness.app.Stop();
        CHECK_FALSE(harness.app.IsPlaying());
        CHECK(scene::SaveLevelToString(harness.World()) == before);
    }

    SUBCASE("Play dua kali tanpa Stop di antaranya tidak menimpa cuplikannya") {
        // **Cuplikan yang tertimpa adalah cuplikan dunia yang sudah berubah.**
        // Play kedua yang mengambil cuplikan baru berarti Stop mengembalikan ke
        // tengah permainan, bukan ke keadaan sebelum Play.
        harness.app.Play();
        harness.CreateEntity("Sementara");
        harness.app.Play();  // diabaikan
        harness.app.Stop();
        CHECK(scene::SaveLevelToString(harness.World()) == before);
    }
}
