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
#include "Sim/ImageIO/ImageIO.h"
#include "Sim/Assets/AssetDatabase.h"
#include <fstream>
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
        RegisterAssetTools(tools, app);

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
        return ResolveProjectPathForTest(root, relative, absolute);
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
