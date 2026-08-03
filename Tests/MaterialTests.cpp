#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Material/MaterialGraph.h"
#include "Sim/Material/MaterialNodeCatalog.h"
#include "Sim/Material/MaterialValidation.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>

using namespace sim;
using namespace sim::material;

namespace {

/// GUID yang bisa dibaca di pesan kegagalan test, dan tetap berbeda satu sama
/// lain. Uuid::Generate() akan membuat test yang gagal sulit dibaca ulang.
Uuid Id(uint64_t n) {
    return Uuid{0x2000'0000'0000'0000ULL, n};
}

MaterialNode Node(uint64_t id, std::string type) {
    MaterialNode node;
    node.guid = Id(id);
    node.type = std::move(type);
    return node;
}

void Link(MaterialGraph& graph, uint64_t from, std::string fromPin, uint64_t to,
          std::string toPin) {
    MaterialLink link;
    link.guid = Id(5000 + graph.links.size());
    link.fromNode = Id(from);
    link.fromPin = std::move(fromPin);
    link.toNode = Id(to);
    link.toPin = std::move(toPin);
    graph.links.push_back(std::move(link));
}

/// Material paling sederhana yang tetap sah: satu keluaran, satu warna.
MaterialGraph MinimalGraph() {
    MaterialGraph graph;
    graph.nodes.push_back(Node(1, std::string(kSurfaceOutputType)));
    graph.nodes.push_back(Node(2, "input.constant"));
    graph.nodes.back().settings["kind"] = "float3";
    graph.nodes.back().settings["value"] = "float3(0.5, 0.2, 0.1)";
    Link(graph, 2, "value", 1, "baseColor");
    return graph;
}

std::string FirstError(const ValidationResult& result) {
    return result.errors.empty() ? std::string{} : result.errors.front().message;
}

}  // namespace

TEST_CASE("Katalog memakai parameter OpenPBR Surface pada node keluaran") {
    const MaterialNodeType* output = MaterialNodeCatalog::Get().Find(kSurfaceOutputType);
    REQUIRE(output != nullptr);

    // Nama pin-nya sengaja sama persis dengan field OpenPBRSurface di
    // openpbr.slang. Kalau salah satunya berganti nama tanpa yang lain ikut,
    // penyambungnya akan menulis field yang tidak ada — dan itu baru ketahuan
    // saat shader-nya dikompilasi, jauh dari sini.
    for (const char* name : {"baseWeight", "baseColor", "baseMetalness", "baseDiffuseRoughness",
                             "specularWeight", "specularColor", "specularRoughness",
                             "specularRoughnessAnisotropy", "specularIor", "coatWeight",
                             "coatColor", "coatRoughness", "coatRoughnessAnisotropy", "coatIor",
                             "coatDarkening", "fuzzWeight", "fuzzColor", "fuzzRoughness"}) {
        INFO("pin ", name);
        CHECK(output->FindPin(name) != nullptr);
    }

    // Nilai bawaannya juga mengikuti OpenPBRSurface::defaults(). Material yang
    // dibiarkan apa adanya harus terlihat sama di editor dan di shader.
    CHECK(output->FindPin("specularRoughness")->defaultValue == "0.3");
    CHECK(output->FindPin("specularIor")->defaultValue == "1.5");
    CHECK(output->FindPin("coatIor")->defaultValue == "1.6");
    CHECK(output->FindPin("baseColor")->defaultValue == "float3(0.8)");

    // Node keluaran tidak punya pin keluar — ia ujung graph.
    CHECK(output->Outputs().empty());
}

TEST_CASE(".simmat bolak-balik tanpa kehilangan apa pun") {
    MaterialGraph original = MinimalGraph();
    original.parameters.push_back({"tint", ValueKind::Float3, "float3(1.0)", "Warna dasar", 0.0f,
                                   0.0f});
    original.parameters.push_back({"roughness", ValueKind::Float, "0.3", {}, 0.0f, 1.0f});

    const std::string text = SaveMaterialToString(original);

    MaterialGraph loaded;
    const MaterialIoResult result = LoadMaterialFromString(loaded, text);
    REQUIRE(result.ok);

    // Byte-per-byte sama, seperti berkas level dan `.simgraph`: itu yang membuat
    // menyimpan material yang tidak disunting tidak menghasilkan diff palsu.
    CHECK(SaveMaterialToString(loaded) == text);

    REQUIRE(loaded.parameters.size() == 2);
    CHECK(loaded.parameters[0].name == "tint");
    CHECK(loaded.parameters[0].kind == ValueKind::Float3);
    CHECK(loaded.parameters[0].tooltip == "Warna dasar");
    CHECK(loaded.parameters[1].maxValue == doctest::Approx(1.0f));
    CHECK(loaded.FindNode(Id(2))->Setting("kind") == "float3");
}

TEST_CASE("Graph 30+ node disimpan dan dimuat identik") {
    MaterialGraph graph;
    graph.nodes.push_back(Node(1, std::string(kSurfaceOutputType)));

    // Rantai panjang add/multiply yang berakhir di baseColor. Bukan 30 node
    // yang berdiri sendiri: kriteria terimanya tentang graph yang sungguh
    // tersambung, dan urutan link ikut menentukan byte yang ditulis.
    uint64_t previous = 0;
    for (uint64_t i = 0; i < 32; ++i) {
        const uint64_t id = 100 + i;
        graph.nodes.push_back(Node(id, i % 2 == 0 ? "math.add" : "math.multiply"));
        graph.nodes.back().position = Vec2(static_cast<float>(i) * 180.0f,
                                           static_cast<float>(i % 5) * 120.0f);
        graph.nodes.back().pinValues["b"] = std::to_string(i);
        if (previous != 0) {
            Link(graph, previous, "result", id, "a");
        }
        previous = id;
    }
    Link(graph, previous, "result", 1, "baseColor");
    REQUIRE(graph.nodes.size() == 33);

    const std::string text = SaveMaterialToString(graph);
    MaterialGraph loaded;
    REQUIRE(LoadMaterialFromString(loaded, text).ok);

    CHECK(loaded.nodes.size() == graph.nodes.size());
    CHECK(loaded.links.size() == graph.links.size());
    CHECK(SaveMaterialToString(loaded) == text);

    const ValidationResult validated = ValidateMaterial(loaded);
    INFO(FirstError(validated));
    CHECK(validated.ok);
}

TEST_CASE("Skalar melebar ke vektor, arah sebaliknya ditolak") {
    // Aturan yang sama dengan Slang: `0.5` sah untuk sebuah float3, tapi
    // memilihkan komponen mana dari float3 yang menjadi float adalah keputusan
    // yang harus ditulis pengguna.
    CHECK(Accepts(ValueKind::Float3, ValueKind::Float));
    CHECK(Accepts(ValueKind::Float4, ValueKind::Float));
    CHECK_FALSE(Accepts(ValueKind::Float, ValueKind::Float3));
    CHECK_FALSE(Accepts(ValueKind::Float2, ValueKind::Float3));

    // Tekstur berdiri sendiri sampai ada yang men-sampling-nya.
    CHECK_FALSE(Accepts(ValueKind::Float3, ValueKind::Texture));
    CHECK_FALSE(Accepts(ValueKind::Texture, ValueKind::Float));
    CHECK(Accepts(ValueKind::Texture, ValueKind::Texture));

    // Bool tidak diam-diam menjadi 0/1.
    CHECK_FALSE(Accepts(ValueKind::Float, ValueKind::Bool));
}

TEST_CASE("Koneksi bertipe salah ditolak dengan pesan yang menyebut kedua tipe") {
    MaterialGraph graph = MinimalGraph();
    graph.nodes.push_back(Node(3, "input.texture"));
    // Tekstur langsung ke warna: tidak ada arti yang masuk akal untuk ini.
    Link(graph, 3, "texture", 1, "specularColor");

    const ValidationResult result = ValidateMaterial(graph);
    REQUIRE_FALSE(result.ok);
    CHECK(result.Mentions(Id(1)));

    const auto issue = std::find_if(result.errors.begin(), result.errors.end(),
                                    [](const MaterialIssue& candidate) {
                                        return candidate.pin == "specularColor";
                                    });
    REQUIRE(issue != result.errors.end());
    // Pesannya menyebut apa yang disambungkan ke apa. "Tipe tidak cocok" saja
    // memaksa pengguna menebak yang mana di antara keduanya yang salah.
    CHECK(issue->message.find("texture") != std::string::npos);
    CHECK(issue->message.find("float3") != std::string::npos);
}

TEST_CASE("Siklus ditolak, bukan ditelusuri selamanya") {
    MaterialGraph graph = MinimalGraph();
    graph.nodes.push_back(Node(10, "math.add"));
    graph.nodes.push_back(Node(11, "math.multiply"));
    Link(graph, 10, "result", 11, "a");
    Link(graph, 11, "result", 10, "a");
    Link(graph, 11, "result", 1, "baseMetalness");

    const ValidationResult result = ValidateMaterial(graph);
    REQUIRE_FALSE(result.ok);
    CHECK(result.Mentions(Id(10)));
    CHECK(result.Mentions(Id(11)));
}

TEST_CASE("Pin wajib yang kosong dilaporkan, yang punya bawaan tidak") {
    MaterialGraph graph = MinimalGraph();
    // Sample tanpa tekstur: pin `texture` sengaja tidak punya nilai bawaan,
    // karena tidak ada tekstur netral yang masuk akal.
    graph.nodes.push_back(Node(20, "input.sample"));
    Link(graph, 20, "rgb", 1, "emissive");

    const ValidationResult result = ValidateMaterial(graph);
    REQUIRE_FALSE(result.ok);
    const auto issue =
        std::find_if(result.errors.begin(), result.errors.end(),
                     [](const MaterialIssue& candidate) { return candidate.pin == "texture"; });
    REQUIRE(issue != result.errors.end());

    // `uv` punya bawaan, jadi membiarkannya kosong bukan kesalahan.
    CHECK(std::none_of(result.errors.begin(), result.errors.end(),
                       [](const MaterialIssue& candidate) { return candidate.pin == "uv"; }));

    // Mengisi nilainya di pin — bukan menyambungkan kabel — sama sahnya.
    graph.nodes.back().pinValues["texture"] = "gAlbedo";
    CHECK(ValidateMaterial(graph).ok);
}

TEST_CASE("Menghapus node membersihkan seluruh kabel yang menyentuhnya") {
    MaterialGraph graph = MinimalGraph();
    graph.nodes.push_back(Node(30, "math.multiply"));
    Link(graph, 2, "value", 30, "a");
    Link(graph, 30, "result", 1, "specularColor");
    REQUIRE(graph.links.size() == 3);

    graph.RemoveNode(Id(30));

    CHECK(graph.FindNode(Id(30)) == nullptr);
    // Yang tersisa hanya kabel yang tidak menyentuhnya sama sekali.
    REQUIRE(graph.links.size() == 1);
    CHECK(graph.links.front().toNode == Id(1));
    CHECK(ValidateMaterial(graph).ok);
}

TEST_CASE("Kabel yang menunjuk node hilang dibuang saat dimuat") {
    MaterialGraph graph = MinimalGraph();
    // Menghapus node-nya langsung dari daftar, meniru berkas hasil suntingan
    // tangan atau merge yang salah — RemoveNode() justru yang mencegah ini.
    graph.nodes.erase(graph.nodes.begin() + 1);
    const std::string text = SaveMaterialToString(graph);

    MaterialGraph loaded;
    REQUIRE(LoadMaterialFromString(loaded, text).ok);
    CHECK(loaded.links.empty());
    CHECK(loaded.nodes.size() == 1);
}

TEST_CASE("Dua kabel ke satu pin input: yang kedua dibuang saat dimuat") {
    MaterialGraph graph = MinimalGraph();
    graph.nodes.push_back(Node(40, "input.constant"));
    Link(graph, 40, "value", 1, "baseColor");  // pin yang sama dengan node 2
    REQUIRE(graph.links.size() == 2);

    MaterialGraph loaded;
    REQUIRE(LoadMaterialFromString(loaded, SaveMaterialToString(graph)).ok);
    REQUIRE(loaded.links.size() == 1);
    CHECK(loaded.links.front().fromNode == Id(2));
}

TEST_CASE("Node parameter mengambil tipenya dari deklarasi parameter") {
    MaterialGraph graph = MinimalGraph();
    graph.parameters.push_back({"tint", ValueKind::Float3, "float3(1.0)", {}, 0.0f, 0.0f});
    graph.nodes.push_back(Node(50, "param.get"));
    graph.nodes.back().settings["parameter"] = "tint";

    const std::vector<MaterialPin> pins = PinsOf(graph, graph.nodes.back());
    REQUIRE(pins.size() == 1);
    CHECK(pins.front().kind == ValueKind::Float3);

    // Menyambungkannya ke sebuah float3 karena itu sah, dan ke float tidak.
    Link(graph, 50, "value", 1, "specularColor");
    CHECK(ValidateMaterial(graph).ok);

    Link(graph, 50, "value", 1, "specularRoughness");
    CHECK_FALSE(ValidateMaterial(graph).ok);
}

TEST_CASE("Graph tanpa node keluaran, dan dengan dua, sama-sama ditolak") {
    MaterialGraph empty;
    empty.nodes.push_back(Node(1, "math.add"));
    CHECK_FALSE(ValidateMaterial(empty).ok);

    MaterialGraph twice = MinimalGraph();
    twice.nodes.push_back(Node(60, std::string(kSurfaceOutputType)));
    const ValidationResult result = ValidateMaterial(twice);
    REQUIRE_FALSE(result.ok);
    CHECK(FirstError(result).find("more than one") != std::string::npos);
}

TEST_CASE("Node komentar dan grup tidak ikut dinilai") {
    MaterialGraph graph = MinimalGraph();
    graph.nodes.push_back(Node(70, "comment"));
    graph.nodes.back().settings["text"] = "kanal warna";
    graph.nodes.push_back(Node(71, "group"));
    graph.nodes.back().size = Vec2(320.0f, 240.0f);

    const ValidationResult result = ValidateMaterial(graph);
    INFO(FirstError(result));
    CHECK(result.ok);

    // Ukurannya bertahan lewat berkas — itu satu-satunya alasan node grup ada.
    MaterialGraph loaded;
    REQUIRE(LoadMaterialFromString(loaded, SaveMaterialToString(graph)).ok);
    CHECK(loaded.FindNode(Id(71))->size.x == doctest::Approx(320.0f));
}
