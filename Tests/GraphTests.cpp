#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Core/TaskPool.h"
#include "Sim/Scene/Components.h"
#include "Sim/Script/Graph.h"
#include "Sim/Script/GraphCache.h"
#include "Sim/Script/GraphCompiler.h"
#include "Sim/Script/NodeCatalog.h"
#include "Sim/Script/ScriptRuntime.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

using namespace sim;
using namespace sim::script;

namespace {

/// GUID yang bisa dibaca di pesan kegagalan test, dan tetap berbeda satu sama
/// lain. Uuid::Generate() akan membuat test yang gagal sulit dibaca ulang.
Uuid Id(uint64_t n) {
    return Uuid{0x1000'0000'0000'0000ULL, n};
}

GraphNode Node(uint64_t id, std::string type) {
    GraphNode node;
    node.guid = Id(id);
    node.type = std::move(type);
    return node;
}

void Link(Graph& graph, uint64_t from, std::string fromPin, uint64_t to, std::string toPin) {
    GraphLink link;
    link.guid = Id(1000 + graph.links.size());
    link.fromNode = Id(from);
    link.fromPin = std::move(fromPin);
    link.toNode = Id(to);
    link.toPin = std::move(toPin);
    graph.links.push_back(std::move(link));
}

/// Graph "putar entity saat OnUpdate", padanan langsung dari spin.lua.
///
/// 1 OnUpdate → 4 Set Transform
/// 2 sim.time × 3 speed → 5 axis_angle(up, ...) → rotation
Graph SpinGraph() {
    Graph graph;
    graph.variables.push_back({"speed", PinKind::Number, "1.5", true});

    graph.nodes.push_back(Node(1, "event.update"));
    graph.nodes.push_back(Node(2, "sim.time"));
    graph.nodes.push_back(Node(3, "variable.get"));
    graph.nodes.back().settings["variable"] = "speed";
    graph.nodes.push_back(Node(4, "math.multiply"));
    graph.nodes.push_back(Node(5, "sim.up"));
    graph.nodes.push_back(Node(6, "sim.axis_angle"));
    graph.nodes.push_back(Node(7, NodeCatalog::ComponentSetKey("Transform")));

    Link(graph, 2, "value", 4, "a");
    Link(graph, 3, "value", 4, "b");
    Link(graph, 5, "value", 6, "axis");
    Link(graph, 4, "result", 6, "radians");
    Link(graph, 6, "value", 7, "rotation");
    Link(graph, 1, "then", 7, "in");
    return graph;
}

struct TempDir {
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("simgraph_" + std::to_string(::getpid()) + "_" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    void Write(std::string_view name, std::string_view text) const {
        std::ofstream stream(path / name, std::ios::binary | std::ios::trunc);
        stream << text;
    }
    std::filesystem::path path;
};

/// Pesan kesalahan pertama, untuk dilampirkan ke kegagalan test.
std::string FirstError(const CompileResult& result) {
    return result.errors.empty() ? std::string{} : result.errors.front().message;
}

bool Mentions(const std::vector<CompileError>& errors, const Uuid& node) {
    return std::any_of(errors.begin(), errors.end(),
                       [&node](const CompileError& error) { return error.node == node; });
}

}  // namespace

TEST_CASE(".simgraph bolak-balik tanpa kehilangan apa pun") {
    const Graph original = SpinGraph();
    const std::string text = SaveGraphToString(original);

    Graph loaded;
    const GraphIoResult result = LoadGraphFromString(loaded, text);
    REQUIRE(result.ok);

    // Byte-per-byte sama, seperti berkas level: itu yang membuat menyimpan graph
    // yang tidak disunting tidak menghasilkan diff palsu di git.
    CHECK(SaveGraphToString(loaded) == text);

    REQUIRE(loaded.nodes.size() == original.nodes.size());
    REQUIRE(loaded.links.size() == original.links.size());
    REQUIRE(loaded.variables.size() == 1);
    CHECK(loaded.variables[0].name == "speed");
    CHECK(loaded.variables[0].exposed);
    CHECK(loaded.FindNode(Id(3))->Setting("variable") == "speed");
}

TEST_CASE("Node grup bertahan lewat berkas dan tidak mempengaruhi kompilasi") {
    scene::RegisterCoreComponents();
    NodeCatalog::Rebuild();

    Graph graph = SpinGraph();
    const std::string withoutGroup = CompileGraph(graph, "spin.simgraph").lua;

    GraphNode group = Node(90, "group");
    group.position = Vec2(-40.0f, -60.0f);
    group.size = Vec2(320.0f, 240.0f);
    group.settings["text"] = "Rotasi";
    graph.nodes.push_back(std::move(group));

    const std::string text = SaveGraphToString(graph);
    Graph loaded;
    REQUIRE(LoadGraphFromString(loaded, text).ok);
    CHECK(SaveGraphToString(loaded) == text);

    const GraphNode* restored = loaded.FindNode(Id(90));
    REQUIRE(restored != nullptr);
    CHECK(restored->size.x == doctest::Approx(320.0f));
    CHECK(restored->size.y == doctest::Approx(240.0f));
    CHECK(restored->Setting("text") == "Rotasi");

    // Grup murni tata letak: Lua yang dihasilkan harus sama persis seperti
    // sebelum grupnya ada. Kalau tidak, memindahkan kotak di kanvas akan
    // mengubah perilaku permainan.
    const CompileResult after = CompileGraph(loaded, "spin.simgraph");
    INFO(FirstError(after));
    REQUIRE(after.ok);
    CHECK(after.lua == withoutGroup);

    // Node tanpa ukuran tidak menuliskan bidang "size" sama sekali, jadi graph
    // yang tidak memakai grup menghasilkan teks yang sama seperti sebelum
    // bidang itu ada.
    CHECK(SaveGraphToString(SpinGraph()).find("\"size\"") == std::string::npos);
}

TEST_CASE("Koneksi yang menunjuk node yang hilang dibuang saat dimuat") {
    Graph graph = SpinGraph();
    // Node dihapus dari berkas, misalnya karena disunting di luar editor.
    graph.nodes.erase(std::remove_if(graph.nodes.begin(), graph.nodes.end(),
                                     [](const GraphNode& node) { return node.guid == Id(5); }),
                      graph.nodes.end());
    const std::string text = SaveGraphToString(graph);

    Graph loaded;
    REQUIRE(LoadGraphFromString(loaded, text).ok);
    // Yang tersisa adalah graph yang masih utuh, bukan berkas yang menolak
    // dibuka karena satu koneksi menggantung.
    CHECK(loaded.FindNode(Id(5)) == nullptr);
    CHECK(loaded.LinkInto(Id(6), "axis") == nullptr);
    CHECK(loaded.links.size() == graph.links.size() - 1);
}

TEST_CASE("Katalog node dibangkitkan dari reflection komponen") {
    scene::RegisterCoreComponents();
    NodeCatalog::Rebuild();
    const NodeCatalog& catalog = NodeCatalog::Get();

    const NodeType* getter = catalog.Find(NodeCatalog::ComponentGetKey("Transform"));
    REQUIRE(getter != nullptr);
    CHECK(getter->pure);
    // Satu pin per field yang tipenya bisa dialirkan — tanpa satu baris pun
    // yang menyebut "position" di dalam katalog.
    CHECK(getter->FindPin("position") != nullptr);
    CHECK(getter->FindPin("rotation") != nullptr);
    CHECK(getter->FindPin("scale") != nullptr);
    CHECK(getter->FindPin("position")->kind == PinKind::Vec3);
    CHECK(getter->FindPin("rotation")->kind == PinKind::Quat);

    const NodeType* setter = catalog.Find(NodeCatalog::ComponentSetKey("Transform"));
    REQUIRE(setter != nullptr);
    CHECK_FALSE(setter->pure);
    CHECK(setter->ExecInput() != nullptr);
    // Pin setter tidak punya nilai bawaan: yang tidak disambung tidak ditulis.
    CHECK(setter->FindPin("position")->defaultValue.empty());

    // Komponen lain ikut, tanpa didaftarkan satu per satu.
    CHECK(catalog.Find(NodeCatalog::ComponentGetKey("Light")) != nullptr);
}

TEST_CASE("Graph putar-entity menghasilkan Lua yang bisa dibaca") {
    scene::RegisterCoreComponents();
    NodeCatalog::Rebuild();

    const CompileResult result = CompileGraph(SpinGraph(), "spin.simgraph");
    INFO(FirstError(result));
    REQUIRE(result.ok);

    // Bentuknya sama persis dengan skrip tulis tangan: sebuah tabel dengan
    // OnUpdate, dan deklarasi properties untuk yang diekspos.
    CHECK(result.lua.find("local Graph = {}") != std::string::npos);
    CHECK(result.lua.find("function Graph:OnUpdate(dt)") != std::string::npos);
    CHECK(result.lua.find("Graph.properties = {") != std::string::npos);
    CHECK(result.lua.find("speed = 1.5,") != std::string::npos);
    CHECK(result.lua.find("sim.set_component(") != std::string::npos);
    CHECK(result.lua.find("return Graph") != std::string::npos);
    // Komentar per node: inilah yang membuat hasilnya bisa dibaca berdampingan
    // dengan graph-nya.
    CHECK(result.lua.find("-- node ") != std::string::npos);

    // Hanya rotation yang ditulis. Kalau position dan scale ikut, menyetel satu
    // field lewat graph akan diam-diam mengembalikan dua lainnya ke nilai netral.
    CHECK(result.lua.find("rotation =") != std::string::npos);
    CHECK(result.lua.find("position =") == std::string::npos);
    CHECK(result.lua.find("scale =") == std::string::npos);

    // Peta sumber menautkan baris ke node, bukan sebaliknya saja.
    const int line = result.LineOfNode(Id(7));
    CHECK(line > 0);
    CHECK(result.NodeAtLine(line) == Id(7));

    // Kriteria terima E6.5 nomor 7 bertumpu pada ini: baris yang memuat
    // set_component juga memuat ekspresi node-node murni yang mengisinya, dan
    // semuanya harus bisa disorot ketika baris itu gagal saat runtime.
    const int statement = result.LineOfNode(Id(6));
    REQUIRE(statement > 0);
    const std::vector<Uuid> suspects = result.NodesAtLine(statement);
    CHECK(std::find(suspects.begin(), suspects.end(), Id(6)) != suspects.end());
    CHECK(std::find(suspects.begin(), suspects.end(), Id(4)) != suspects.end());
    CHECK(std::find(suspects.begin(), suspects.end(), Id(7)) != suspects.end());
}

TEST_CASE("Graph dan skrip tulis tangan menghasilkan transform yang sama tiap frame") {
    scene::RegisterCoreComponents();
    NodeCatalog::Rebuild();

    TempDir dir;
    const CompileResult compiled = CompileGraph(SpinGraph(), "spin.simgraph");
    REQUIRE(compiled.ok);
    dir.Write("from_graph.lua", compiled.lua);

    // Padanan tulis tangan. Nilai `speed` sengaja dituliskan sama dengan bawaan
    // variabel graph-nya.
    dir.Write("by_hand.lua",
              "local S = {}\n"
              "S.properties = { speed = 1.5 }\n"
              "function S:OnUpdate(dt)\n"
              "    sim.set_component(self.entity, \"Transform\",\n"
              "        { rotation = sim.axis_angle(sim.up(), sim.time() * self.props.speed) })\n"
              "end\n"
              "return S\n");

    TaskPool pool(2);
    assets::AssetDatabase db;
    REQUIRE(db.Initialize({dir.path, &pool, 0.05f}));
    const assets::AssetRecord* graphScript = db.FindByRelativePath("from_graph.lua");
    const assets::AssetRecord* handScript = db.FindByRelativePath("by_hand.lua");
    REQUIRE(graphScript != nullptr);
    REQUIRE(handScript != nullptr);

    scene::World world;
    const scene::Entity fromGraph = world.Create("FromGraph");
    const scene::Entity byHand = world.Create("ByHand");
    world.Add<scene::ScriptComponent>(fromGraph).script = AssetRef{graphScript->guid};
    world.Add<scene::ScriptComponent>(byHand).script = AssetRef{handScript->guid};

    ScriptRuntime runtime;
    REQUIRE(runtime.Initialize(world, &db));
    runtime.Start();

    // Kriteria terima E6.5 nomor 6: perilakunya dibandingkan dengan menjalankan
    // keduanya dan mencocokkan transform TIAP FRAME, bukan hanya di akhir —
    // dua rotasi bisa berpapasan di nilai yang sama pada satu titik waktu.
    for (int frame = 0; frame < 30; ++frame) {
        runtime.Update(0.016f);
        const auto* a = world.TryGet<scene::TransformComponent>(fromGraph);
        const auto* b = world.TryGet<scene::TransformComponent>(byHand);
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        CHECK(a->rotation.w == doctest::Approx(b->rotation.w));
        CHECK(a->rotation.x == doctest::Approx(b->rotation.x));
        CHECK(a->rotation.y == doctest::Approx(b->rotation.y));
        CHECK(a->rotation.z == doctest::Approx(b->rotation.z));
        // Yang tidak disetel harus tetap seperti semula, di kedua jalur.
        CHECK(a->scale.x == doctest::Approx(1.0f));
        CHECK(a->position.y == doctest::Approx(0.0f));
    }

    runtime.Stop();
}

TEST_CASE("Level yang memakai graph dimuat tanpa mengompilasi ulang") {
    scene::RegisterCoreComponents();
    NodeCatalog::Rebuild();

    TempDir assetsDir;
    TempDir cacheDir;
    REQUIRE(SaveGraphToFile(SpinGraph(), assetsDir.path / "spin.simgraph").ok);

    GraphCache cache;
    cache.Initialize(cacheDir.path);
    const Uuid guid = Id(42);
    const std::filesystem::path source = assetsDir.path / "spin.simgraph";

    const std::filesystem::path first = cache.EnsureCompiled(guid, source);
    REQUIRE_FALSE(first.empty());
    REQUIRE(std::filesystem::exists(first));
    const auto compiledAt = std::filesystem::last_write_time(first);

    // Kriteria terima E6.5 nomor 9, bagian pertama: memuat level yang memakai
    // graph tidak mengompilasi apa pun. Yang dipakai adalah `.lua` yang sudah
    // ada, persis seperti aset lain yang sudah diimpor.
    const std::filesystem::path again = cache.EnsureCompiled(guid, source);
    CHECK(again == first);
    CHECK(std::filesystem::last_write_time(first) == compiledAt);

    // Bagian kedua: menyunting graph langsung berlaku, tanpa langkah build
    // manual. Waktu ubah disetel maju secara eksplisit — resolusi jam berkas
    // terlalu kasar untuk dipercaya dalam rentang beberapa milidetik.
    Graph edited = SpinGraph();
    edited.variables[0].defaultValue = "9.5";
    REQUIRE(SaveGraphToFile(edited, source).ok);
    std::filesystem::last_write_time(source, compiledAt + std::chrono::seconds(2));

    const std::filesystem::path rebuilt = cache.EnsureCompiled(guid, source);
    REQUIRE_FALSE(rebuilt.empty());
    std::ifstream stream(rebuilt);
    const std::string lua((std::istreambuf_iterator<char>(stream)),
                          std::istreambuf_iterator<char>());
    CHECK(lua.find("speed = 9.5,") != std::string::npos);
}

TEST_CASE("Kompilasi yang gagal tidak menimpa hasil yang masih baik") {
    scene::RegisterCoreComponents();
    NodeCatalog::Rebuild();

    TempDir assetsDir;
    TempDir cacheDir;
    const std::filesystem::path source = assetsDir.path / "spin.simgraph";
    REQUIRE(SaveGraphToFile(SpinGraph(), source).ok);

    GraphCache cache;
    cache.Initialize(cacheDir.path);
    const Uuid guid = Id(43);
    const std::filesystem::path output = cache.EnsureCompiled(guid, source);
    REQUIRE_FALSE(output.empty());

    // Graph disunting menjadi tidak bisa dikompilasi: 4 → 6 → 4, siklus pada
    // pin data.
    Graph broken = SpinGraph();
    broken.links.erase(std::remove_if(broken.links.begin(), broken.links.end(),
                                      [](const GraphLink& link) {
                                          return link.toNode == Id(4) && link.toPin == "a";
                                      }),
                       broken.links.end());
    Link(broken, 6, "value", 4, "a");
    REQUIRE(SaveGraphToFile(broken, source).ok);

    CHECK(cache.Rebuild(guid, source).empty());
    // Berkas lama tetap utuh. Menimpanya dengan hasil setengah jadi akan
    // membuat Play berikutnya gagal dengan kesalahan Lua yang tidak ada
    // hubungannya dengan yang sebenarnya salah.
    std::ifstream stream(output);
    const std::string lua((std::istreambuf_iterator<char>(stream)),
                          std::istreambuf_iterator<char>());
    CHECK(lua.find("function Graph:OnUpdate(dt)") != std::string::npos);

    const CompileResult* last = cache.LastResult(guid);
    REQUIRE(last != nullptr);
    CHECK_FALSE(last->ok);
}

TEST_CASE("GraphComponent berjalan lewat jalur yang sama dengan ScriptComponent") {
    scene::RegisterCoreComponents();
    NodeCatalog::Rebuild();

    TempDir assetsDir;
    TempDir cacheDir;
    REQUIRE(SaveGraphToFile(SpinGraph(), assetsDir.path / "spin.simgraph").ok);

    TaskPool pool(2);
    assets::AssetDatabase db;
    REQUIRE(db.Initialize({assetsDir.path, &pool, 0.05f}));
    const assets::AssetRecord* record = db.FindByRelativePath("spin.simgraph");
    REQUIRE(record != nullptr);
    CHECK(record->type == assets::AssetType::Graph);

    scene::World world;
    const scene::Entity entity = world.Create("Spinner");
    world.Add<scene::GraphComponent>(entity).graph = AssetRef{record->guid};

    ScriptRuntime runtime;
    REQUIRE(runtime.Initialize(world, &db, cacheDir.path));

    // Properti yang diekspos graph terbaca lewat jalur yang sama dengan skrip —
    // itulah yang membuat Inspector menampilkannya tanpa kode khusus.
    const std::vector<scene::ScriptProperty> declared =
        runtime.DeclaredProperties(record->guid);
    REQUIRE(declared.size() == 1);
    CHECK(declared[0].name == "speed");
    CHECK(declared[0].number == doctest::Approx(1.5f));

    runtime.Start();
    CHECK(world.TryGet<scene::GraphComponent>(entity)->loaded);
    for (int i = 0; i < 5; ++i) {
        runtime.Update(0.016f);
    }
    const auto* transform = world.TryGet<scene::TransformComponent>(entity);
    REQUIRE(transform != nullptr);
    // Sudah berputar: rotasinya bukan lagi identitas.
    CHECK(transform->rotation.y != doctest::Approx(0.0f));

    runtime.Stop();
    CHECK_FALSE(world.TryGet<scene::GraphComponent>(entity)->loaded);
}

TEST_CASE("Breakpoint memanggil penangan dan menyebut node-nya") {
    scene::RegisterCoreComponents();
    NodeCatalog::Rebuild();

    Graph graph;
    graph.nodes.push_back(Node(1, "event.start"));
    graph.nodes.push_back(Node(2, "sim.log"));
    graph.nodes.back().pinValues["message"] = "\"halo\"";
    Link(graph, 1, "then", 2, "in");

    CompileOptions options;
    options.breakpoints.push_back(Id(2));
    const CompileResult result = CompileGraph(graph, "break.simgraph", options);
    INFO(FirstError(result));
    REQUIRE(result.ok);
    CHECK(result.lua.find("sim.breakpoint(") != std::string::npos);

    ScriptRuntime runtime;
    scene::World world;
    REQUIRE(runtime.Initialize(world, nullptr));

    // Tanpa penangan, `sim.breakpoint` tidak boleh menggagalkan apa pun: berkas
    // yang sama harus tetap berjalan di runtime tanpa editor.
    const EvalResult silent =
        runtime.Evaluate("local g = (function()\n" + result.lua + "end)()\n" +
                         "g.state = {} g.entity = 0 g:OnStart()\n");
    INFO(silent.error);
    CHECK(silent.ok);

    std::vector<std::string> hits;
    runtime.SetBreakpointHandler([&hits](const std::string& node) { hits.push_back(node); });
    const EvalResult caught =
        runtime.Evaluate("local g = (function()\n" + result.lua + "end)()\n" +
                         "g.state = {} g.entity = 0 g:OnStart()\n");
    INFO(caught.error);
    REQUIRE(caught.ok);
    REQUIRE(hits.size() == 1);
    // GUID penuh, bukan potongan: editor mencocokkannya dengan node di kanvas.
    CHECK(hits[0] == Id(2).ToString());
}

TEST_CASE("Kesalahan runtime di dalam graph menunjuk node penyebabnya") {
    scene::RegisterCoreComponents();
    NodeCatalog::Rebuild();

    Graph graph;
    graph.nodes.push_back(Node(1, "event.start"));
    graph.nodes.push_back(Node(2, "sim.log"));
    // Nilai pin diisi ekspresi Lua yang gagal saat dijalankan, bukan saat dimuat.
    graph.nodes.back().pinValues["message"] = "error(\"boom\")";
    Link(graph, 1, "then", 2, "in");

    const CompileResult result = CompileGraph(graph, "runtime.simgraph");
    REQUIRE(result.ok);

    ScriptRuntime runtime;
    scene::World world;
    REQUIRE(runtime.Initialize(world, nullptr));
    const EvalResult run =
        runtime.Evaluate("local g = (function()\n" + result.lua + "end)()\n" +
                         "g.state = {} g.entity = 0 g:OnStart()\n");
    REQUIRE_FALSE(run.ok);

    // Kriteria terima E6.5 nomor 7: nomor baris dari traceback diterjemahkan
    // kembali menjadi node, sehingga editor menyorot node — bukan menyerahkan
    // nomor baris di berkas yang tidak pernah dilihat pengguna.
    const int line = result.LineOfNode(Id(2));
    REQUIRE(line > 0);
    const std::vector<Uuid> suspects = result.NodesAtLine(line);
    CHECK(std::find(suspects.begin(), suspects.end(), Id(2)) != suspects.end());
    CHECK(result.NodeAtLine(line) == Id(2));
}

TEST_CASE("Kegagalan saat Play tercatat beserta nomor barisnya") {
    scene::RegisterCoreComponents();
    NodeCatalog::Rebuild();

    TempDir assetsDir;
    TempDir cacheDir;

    // Graph yang gagal saat DIJALANKAN, bukan saat dikompilasi.
    Graph graph;
    graph.nodes.push_back(Node(1, "event.start"));
    graph.nodes.push_back(Node(2, "sim.log"));
    graph.nodes.back().pinValues["message"] = "error(\"boom\")";
    Link(graph, 1, "then", 2, "in");
    REQUIRE(SaveGraphToFile(graph, assetsDir.path / "broken.simgraph").ok);

    TaskPool pool(2);
    assets::AssetDatabase db;
    REQUIRE(db.Initialize({assetsDir.path, &pool, 0.05f}));
    const assets::AssetRecord* record = db.FindByRelativePath("broken.simgraph");
    REQUIRE(record != nullptr);

    scene::World world;
    const scene::Entity entity = world.Create("Broken");
    world.Add<scene::GraphComponent>(entity).graph = AssetRef{record->guid};

    ScriptRuntime runtime;
    REQUIRE(runtime.Initialize(world, &db, cacheDir.path));
    runtime.Start();

    const RuntimeFailure* failure = runtime.LastFailure(record->guid);
    REQUIRE(failure != nullptr);
    CHECK(failure->message.find("boom") != std::string::npos);
    REQUIRE(failure->line > 0);

    // Baris itu menunjuk kembali ke node penyebabnya lewat peta sumber yang
    // disimpan cache — jalur persis yang dipakai panel untuk menyorotnya.
    const CompileResult* compiled = runtime.Graphs().LastResult(record->guid);
    REQUIRE(compiled != nullptr);
    const std::vector<Uuid> suspects = compiled->NodesAtLine(failure->line);
    CHECK(std::find(suspects.begin(), suspects.end(), Id(2)) != suspects.end());

    runtime.Stop();
}

TEST_CASE("Siklus pada pin data ditolak dengan pesan yang menunjuk node penyebabnya") {
    scene::RegisterCoreComponents();
    NodeCatalog::Rebuild();

    Graph graph;
    graph.nodes.push_back(Node(1, "event.update"));
    graph.nodes.push_back(Node(2, "math.add"));
    graph.nodes.push_back(Node(3, "math.multiply"));
    graph.nodes.push_back(Node(4, "sim.log"));
    // 2 → 3 → 2: lingkar tertutup pada pin data.
    Link(graph, 2, "result", 3, "a");
    Link(graph, 3, "result", 2, "a");
    Link(graph, 2, "result", 4, "message");
    Link(graph, 1, "then", 4, "in");

    const CompileResult result = CompileGraph(graph, "cycle.simgraph");
    // Yang penting bukan hanya bahwa ia gagal, tapi bahwa ia KEMBALI — kompiler
    // yang menggantung akan membekukan editor bersamanya.
    CHECK_FALSE(result.ok);
    REQUIRE_FALSE(result.errors.empty());
    CHECK(result.errors.front().message.find("cycle") != std::string::npos);
    // Node penyebabnya ikut disebut, supaya editor bisa menyorotnya.
    CHECK((Mentions(result.errors, Id(2)) || Mentions(result.errors, Id(3))));
}

TEST_CASE("Lingkar pada pin exec ditolak, bukan ditelusuri selamanya") {
    scene::RegisterCoreComponents();
    NodeCatalog::Rebuild();

    Graph graph;
    graph.variables.push_back({"n", PinKind::Number, "0", false});
    graph.nodes.push_back(Node(1, "event.update"));
    graph.nodes.push_back(Node(2, "variable.set"));
    graph.nodes.back().settings["variable"] = "n";
    graph.nodes.push_back(Node(3, "variable.set"));
    graph.nodes.back().settings["variable"] = "n";
    Link(graph, 1, "then", 2, "in");
    Link(graph, 2, "then", 3, "in");
    Link(graph, 3, "then", 2, "in");

    const CompileResult result = CompileGraph(graph, "execloop.simgraph");
    CHECK_FALSE(result.ok);
    CHECK(Mentions(result.errors, Id(2)));
}

TEST_CASE("Pin exec bercabang ditolak dengan saran memakai Sequence") {
    scene::RegisterCoreComponents();
    NodeCatalog::Rebuild();

    Graph graph;
    graph.nodes.push_back(Node(1, "event.start"));
    graph.nodes.push_back(Node(2, "sim.log"));
    graph.nodes.push_back(Node(3, "sim.log"));
    Link(graph, 1, "then", 2, "in");
    Link(graph, 1, "then", 3, "in");

    const CompileResult result = CompileGraph(graph, "fork.simgraph");
    CHECK_FALSE(result.ok);
    REQUIRE_FALSE(result.errors.empty());
    CHECK(result.errors.front().message.find("Sequence") != std::string::npos);
}

TEST_CASE("Koneksi yang tipenya tidak cocok ditolak saat kompilasi") {
    scene::RegisterCoreComponents();
    NodeCatalog::Rebuild();

    Graph graph;
    graph.nodes.push_back(Node(1, "event.update"));
    graph.nodes.push_back(Node(2, "sim.up"));
    graph.nodes.push_back(Node(3, "flow.branch"));
    // Vec3 dialirkan ke pin kondisi yang menuntut bool.
    Link(graph, 2, "value", 3, "condition");
    Link(graph, 1, "then", 3, "in");

    const CompileResult result = CompileGraph(graph, "types.simgraph");
    CHECK_FALSE(result.ok);
    CHECK(Mentions(result.errors, Id(3)));
}

TEST_CASE("Lua hasil kompilasi selalu sah sebagai sintaks Lua") {
    scene::RegisterCoreComponents();
    NodeCatalog::Rebuild();

    Graph graph;
    graph.variables.push_back({"counter", PinKind::Number, "0", false});
    graph.variables.push_back({"speed", PinKind::Number, "2", true});
    graph.nodes.push_back(Node(1, "event.start"));
    graph.nodes.push_back(Node(2, "flow.sequence"));
    graph.nodes.back().settings["count"] = "3";
    graph.nodes.push_back(Node(3, "sim.log"));
    graph.nodes.push_back(Node(4, "flow.for"));
    graph.nodes.push_back(Node(5, "variable.set"));
    graph.nodes.back().settings["variable"] = "counter";
    graph.nodes.push_back(Node(6, "flow.branch"));
    graph.nodes.push_back(Node(7, "compare.greater"));
    graph.nodes.push_back(Node(8, "variable.get"));
    graph.nodes.back().settings["variable"] = "speed";
    graph.nodes.push_back(Node(9, "sim.log"));

    Link(graph, 1, "then", 2, "in");
    Link(graph, 2, "then0", 3, "in");
    Link(graph, 2, "then1", 4, "in");
    Link(graph, 4, "body", 5, "in");
    Link(graph, 4, "index", 5, "value");
    Link(graph, 2, "then2", 6, "in");
    Link(graph, 8, "value", 7, "a");
    Link(graph, 7, "result", 6, "condition");
    Link(graph, 6, "true", 9, "in");

    const CompileResult result = CompileGraph(graph, "kitchen.simgraph");
    INFO(FirstError(result));
    REQUIRE(result.ok);

    // Diperiksa oleh Lua sendiri, bukan dengan mencocokkan teks: kompiler yang
    // menghasilkan kode yang "kelihatan benar" tapi tidak bisa dimuat adalah
    // persis kelas bug yang test ini ada untuk menutupnya.
    ScriptRuntime runtime;
    scene::World world;
    REQUIRE(runtime.Initialize(world, nullptr));
    const std::string error = runtime.CheckSyntax(result.lua, "kitchen.lua");
    INFO(error);
    CHECK(error.empty());
}

TEST_CASE("Variabel yang diekspos memakai nilai dari Inspector, termasuk nol") {
    scene::RegisterCoreComponents();
    NodeCatalog::Rebuild();

    Graph graph;
    graph.variables.push_back({"speed", PinKind::Number, "1.5", true});
    graph.nodes.push_back(Node(1, "event.start"));
    graph.nodes.push_back(Node(2, "sim.log"));
    graph.nodes.push_back(Node(3, "variable.get"));
    graph.nodes.back().settings["variable"] = "speed";
    Link(graph, 1, "then", 2, "in");
    Link(graph, 3, "value", 2, "message");

    const CompileResult result = CompileGraph(graph, "props.simgraph");
    REQUIRE(result.ok);

    ScriptRuntime runtime;
    scene::World world;
    REQUIRE(runtime.Initialize(world, nullptr));

    // Nilai 0 dari Inspector harus menang atas bawaan 1.5. Menyiapkannya dengan
    // `or` — bentuk yang paling menggoda di Lua — akan diam-diam mengembalikan
    // 1.5, dan pengguna melihat propertinya "tidak berlaku".
    // Dibungkus fungsi karena hasil kompilasi diakhiri `return Graph`, dan di
    // Lua `return` harus jadi pernyataan terakhir sebuah blok.
    const std::string harness = "local instance = (function()\n" + result.lua +
                                "end)()\n"
                                "instance.state = {}\n"
                                "instance.props = { speed = 0 }\n"
                                "instance:__ensure()\n"
                                "kSpeed = instance.state.speed\n";
    const EvalResult evaluated = runtime.Evaluate(harness);
    INFO(evaluated.error);
    REQUIRE(evaluated.ok);
    CHECK(runtime.Evaluate("kSpeed").values.at(0).value == "0");
}
