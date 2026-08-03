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
    //
    // Angkanya dicocokkan dengan berkas NORMATIF OpenPBR, bukan halaman prosa
    // spesifikasinya: `reference/open_pbr_surface.mtlx` di repo
    // AcademySoftwareFoundation/OpenPBR (nodedef isdefaultversion, v1.1.1).
    // Halaman prosanya sempat membuat `coat_roughness` terbaca 0.03 dan
    // `coat_ior` terbaca 1.5 — keduanya angka revisi lama, dari bagian yang
    // masih menyebut coat_affect_color/coat_affect_roughness. Berkas nodedef
    // itu yang menang, dan seluruh coat dikunci di sini supaya salah baca yang
    // sama tidak bisa terulang menjadi perubahan kode.
    CHECK(output->FindPin("baseWeight")->defaultValue == "1.0");
    CHECK(output->FindPin("baseColor")->defaultValue == "float3(0.8)");
    CHECK(output->FindPin("baseMetalness")->defaultValue == "0.0");
    CHECK(output->FindPin("baseDiffuseRoughness")->defaultValue == "0.0");
    CHECK(output->FindPin("specularWeight")->defaultValue == "1.0");
    CHECK(output->FindPin("specularColor")->defaultValue == "float3(1.0)");
    CHECK(output->FindPin("specularRoughness")->defaultValue == "0.3");
    CHECK(output->FindPin("specularRoughnessAnisotropy")->defaultValue == "0.0");
    CHECK(output->FindPin("specularIor")->defaultValue == "1.5");
    CHECK(output->FindPin("coatWeight")->defaultValue == "0.0");
    CHECK(output->FindPin("coatColor")->defaultValue == "float3(1.0)");
    CHECK(output->FindPin("coatRoughness")->defaultValue == "0.0");
    CHECK(output->FindPin("coatRoughnessAnisotropy")->defaultValue == "0.0");
    CHECK(output->FindPin("coatIor")->defaultValue == "1.6");
    CHECK(output->FindPin("coatDarkening")->defaultValue == "1.0");
    CHECK(output->FindPin("fuzzWeight")->defaultValue == "0.0");
    CHECK(output->FindPin("fuzzColor")->defaultValue == "float3(1.0)");
    CHECK(output->FindPin("fuzzRoughness")->defaultValue == "0.5");

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

// =============================================================================
// Kompiler graph → Slang
// =============================================================================

#include "Sim/Material/MaterialCompiler.h"

namespace {

/// Graph yang menyentuh sebagian besar jalur kompiler: tekstur, parameter,
/// matematika bertipe campur, dan tiga pin di luar OpenPBRSurface.
MaterialGraph TexturedGraph() {
    MaterialGraph graph;
    graph.parameters.push_back({"tint", ValueKind::Float3, "float3(1.0)", {}, 0.0f, 0.0f});

    graph.nodes.push_back(Node(1, std::string(kSurfaceOutputType)));

    graph.nodes.push_back(Node(2, "input.texture"));
    graph.nodes.back().settings["name"] = "Albedo";
    graph.nodes.back().settings["texture"] = "11112222-3333-4444-5555-666677778888";

    graph.nodes.push_back(Node(3, "input.sample"));
    graph.nodes.push_back(Node(4, "param.get"));
    graph.nodes.back().settings["parameter"] = "tint";
    graph.nodes.push_back(Node(5, "math.multiply"));

    Link(graph, 2, "texture", 3, "texture");
    Link(graph, 3, "rgb", 5, "a");
    Link(graph, 4, "value", 5, "b");
    Link(graph, 5, "result", 1, "baseColor");
    Link(graph, 3, "a", 1, "opacity");
    return graph;
}

}  // namespace

TEST_CASE("Kompiler mengisi OpenPBRSurface, bukan menghitung cahaya") {
    const MaterialCompileResult result = CompileMaterial(MinimalGraph());
    INFO(result.slang);
    REQUIRE(result.ok);

    CHECK(result.slang.find("import openpbr;") != std::string::npos);
    CHECK(result.slang.find("MaterialSurface evalMaterial(MaterialInputs inputs)") !=
          std::string::npos);
    CHECK(result.slang.find("result.surface = OpenPBRSurface::defaults();") != std::string::npos);
    CHECK(result.slang.find("result.surface.baseColor =") != std::string::npos);

    // Tidak ada jejak model shading di sini: itu milik openpbr.slang.
    CHECK(result.slang.find("evalOpenPBR") == std::string::npos);
    CHECK(result.slang.find("D_GGX") == std::string::npos);
}

TEST_CASE("Pin yang tidak dikemudikan tidak ditulis sama sekali") {
    const MaterialCompileResult result = CompileMaterial(MinimalGraph());
    REQUIRE(result.ok);

    // Hanya baseColor yang tersambung. Sisanya bersandar pada defaults() di
    // shader — satu tempat untuk nilai bawaan runtime, bukan dua.
    CHECK(result.slang.find("result.surface.baseColor") != std::string::npos);
    CHECK(result.slang.find("result.surface.specularRoughness") == std::string::npos);
    CHECK(result.slang.find("result.surface.coatWeight") == std::string::npos);

    // Tiga pin di luar OpenPBRSurface tidak punya defaults() untuk bersandar,
    // jadi ketiganya selalu ditulis.
    CHECK(result.slang.find("result.normal =") != std::string::npos);
    CHECK(result.slang.find("result.emissive =") != std::string::npos);
    CHECK(result.slang.find("result.opacity =") != std::string::npos);
}

TEST_CASE("Nilai yang diketik di pin ikut ditulis, seperti kabel") {
    MaterialGraph graph = MinimalGraph();
    graph.nodes.front().pinValues["specularRoughness"] = "0.85";

    const MaterialCompileResult result = CompileMaterial(graph);
    REQUIRE(result.ok);
    CHECK(result.slang.find("result.surface.specularRoughness = 0.85;") != std::string::npos);
}

TEST_CASE("Tipe hasil disimpulkan dari yang paling lebar di antara masukannya") {
    const MaterialCompileResult result = CompileMaterial(TexturedGraph());
    INFO(result.slang);
    REQUIRE(result.ok);

    // float3 (rgb tekstur) dikalikan float3 (parameter) tetap float3. Kalau
    // penyimpulannya jatuh ke float, Slang akan menolak penugasannya ke
    // baseColor — kegagalan yang muncul jauh dari sini.
    CHECK(result.slang.find("float3 n") != std::string::npos);
    CHECK(result.slang.find("float n") == std::string::npos);
}

TEST_CASE("Skalar dikali vektor menghasilkan vektor") {
    MaterialGraph graph = MinimalGraph();
    graph.nodes.push_back(Node(10, "input.constant"));
    graph.nodes.back().settings["kind"] = "float3";
    graph.nodes.back().settings["value"] = "float3(1.0, 0.5, 0.25)";
    graph.nodes.push_back(Node(11, "math.multiply"));
    graph.nodes.back().pinValues["b"] = "2.0";
    Link(graph, 10, "value", 11, "a");
    Link(graph, 11, "result", 1, "emissive");

    const MaterialCompileResult result = CompileMaterial(graph);
    INFO(result.slang);
    REQUIRE(result.ok);
    CHECK(result.slang.find("float3 n") != std::string::npos);
}

TEST_CASE("Dot selalu menghasilkan skalar, apa pun masukannya") {
    MaterialGraph graph = MinimalGraph();
    graph.nodes.push_back(Node(20, "input.normal"));
    graph.nodes.push_back(Node(21, "input.viewDirection"));
    graph.nodes.push_back(Node(22, "math.dot"));
    Link(graph, 20, "normal", 22, "a");
    Link(graph, 21, "direction", 22, "b");
    Link(graph, 22, "result", 1, "specularRoughness");

    const MaterialCompileResult result = CompileMaterial(graph);
    INFO(result.slang);
    REQUIRE(result.ok);
    // Barisnya dideklarasikan float, bukan float3. Nomor variabelnya tidak
    // diikat di sini: ia mengikuti urutan emisi, bukan janji apa pun ke
    // pengguna.
    const std::size_t at = result.slang.find("dot(inputs.worldNormal, inputs.viewDirection)");
    REQUIRE(at != std::string::npos);
    const std::size_t begin = result.slang.rfind('\n', at) + 1;
    CHECK(result.slang.compare(begin, 10, "    float ") == 0);
}

TEST_CASE("Tekstur menjadi binding di lingkup modul, bukan baris di dalam fungsi") {
    const MaterialCompileResult result = CompileMaterial(TexturedGraph());
    REQUIRE(result.ok);

    CHECK(result.slang.find("Texture2D<float4> tAlbedo;") != std::string::npos);
    CHECK(result.slang.find("SamplerState sAlbedo;") != std::string::npos);
    CHECK(result.slang.find("tAlbedo.Sample(sAlbedo, inputs.uv0)") != std::string::npos);

    REQUIRE(result.textures.size() == 1);
    CHECK(result.textures.front().name == "tAlbedo");
    CHECK(result.textures.front().texture ==
          Uuid::Parse("11112222-3333-4444-5555-666677778888"));
}

TEST_CASE("Parameter menjadi cbuffer, dan yang dipakai dicatat") {
    MaterialGraph graph = TexturedGraph();
    // Parameter yang tidak pernah dibaca tetap ikut ditulis: tata letak cbuffer
    // harus sama dengan yang diharapkan material instance, apa pun isi graph.
    graph.parameters.push_back({"unused", ValueKind::Float, "1.0", {}, 0.0f, 0.0f});

    const MaterialCompileResult result = CompileMaterial(graph);
    REQUIRE(result.ok);

    CHECK(result.slang.find("cbuffer MaterialParams") != std::string::npos);
    CHECK(result.slang.find("float3 tint;") != std::string::npos);
    CHECK(result.slang.find("float unused;") != std::string::npos);

    REQUIRE(result.usedParameters.size() == 1);
    CHECK(result.usedParameters.front() == "tint");
}

TEST_CASE("Node yang tidak terjangkau keluaran tidak menghasilkan satu baris pun") {
    MaterialGraph graph = MinimalGraph();
    // Potongan yang menganggur di kanvas — sisa percobaan yang belum disambung.
    graph.nodes.push_back(Node(30, "input.time"));
    graph.nodes.push_back(Node(31, "math.sin"));
    Link(graph, 30, "time", 31, "x");

    const MaterialCompileResult result = CompileMaterial(graph);
    INFO(result.slang);
    REQUIRE(result.ok);
    CHECK(result.slang.find("sin(") == std::string::npos);
}

TEST_CASE("Peta sumber menerjemahkan baris kembali menjadi node") {
    const MaterialCompileResult result = CompileMaterial(TexturedGraph());
    REQUIRE(result.ok);
    REQUIRE_FALSE(result.sourceMap.empty());

    // Node Multiply menghasilkan sebuah baris, dan baris itu menunjuk balik
    // kepadanya — itulah yang mengubah kesalahan shader menjadi node yang
    // menyala merah di kanvas.
    const int line = result.LineOfNode(Id(5));
    REQUIRE(line > 0);
    CHECK(result.NodeAtLine(line) == Id(5));
}

TEST_CASE("Graph yang tidak lolos validasi tidak dikompilasi") {
    MaterialGraph graph = MinimalGraph();
    graph.nodes.push_back(Node(40, "input.texture"));
    Link(graph, 40, "texture", 1, "baseMetalness");

    const MaterialCompileResult result = CompileMaterial(graph);
    CHECK_FALSE(result.ok);
    REQUIRE_FALSE(result.errors.empty());
    CHECK(result.slang.empty());
}

TEST_CASE("Kompilasi bersifat deterministik") {
    const MaterialGraph graph = TexturedGraph();
    CHECK(CompileMaterial(graph).slang == CompileMaterial(graph).slang);
}

// =============================================================================
// Material instance (.simmatinst)
// =============================================================================

#include "Sim/Material/MaterialInstance.h"

namespace {

/// Induk dengan dua parameter: satu warna, satu angka bersalur.
MaterialGraph ParentGraph() {
    MaterialGraph graph = MinimalGraph();
    graph.parameters.push_back({"tint", ValueKind::Float3, "float3(0.8, 0.4, 0.2)", "Warna",
                                0.0f, 0.0f});
    graph.parameters.push_back({"roughness", ValueKind::Float, "0.3", {}, 0.0f, 1.0f});
    return graph;
}

}  // namespace

TEST_CASE("Literal Slang terurai menjadi angka, termasuk bentuk satu-nilai") {
    // Satu angka mengisi seluruh komponen, sama seperti di Slang.
    const MaterialValue broadcast = ParseValue(ValueKind::Float3, "float3(0.8)");
    CHECK(broadcast.components[0] == doctest::Approx(0.8f));
    CHECK(broadcast.components[1] == doctest::Approx(0.8f));
    CHECK(broadcast.components[2] == doctest::Approx(0.8f));

    const MaterialValue listed = ParseValue(ValueKind::Float3, "float3(1.0, 0.5, 0.25)");
    CHECK(listed.components[0] == doctest::Approx(1.0f));
    CHECK(listed.components[1] == doctest::Approx(0.5f));
    CHECK(listed.components[2] == doctest::Approx(0.25f));

    CHECK(ParseValue(ValueKind::Float, "0.3").components[0] == doctest::Approx(0.3f));
    CHECK(ParseValue(ValueKind::Float4, "float4(1,1,1,1)").components[3] == doctest::Approx(1.0f));

    // Literal yang salah ketik memberi nilai netral, bukan kegagalan: satu
    // parameter rusak tidak boleh membuat seluruh material gagal dibuka.
    CHECK(ParseValue(ValueKind::Float, "bukan angka").components[0] == doctest::Approx(0.0f));
}

TEST_CASE("Nilai bolak-balik lewat teks tanpa berubah") {
    for (const char* literal : {"0.3", "float3(0.8)", "float3(1.0, 0.5, 0.25)",
                                "float4(0.1, 0.2, 0.3, 0.4)"}) {
        const ValueKind kind = ValueKindFromString(
            std::string(literal).find("float4") == 0
                ? "float4"
                : (std::string(literal).find("float3") == 0 ? "float3" : "float"));
        const MaterialValue value = ParseValue(kind, literal);
        INFO(literal);
        CHECK(ParseValue(kind, FormatValue(value)) == value);
    }
}

TEST_CASE("Instance mengubah satu parameter tanpa menyentuh induknya") {
    const MaterialGraph parent = ParentGraph();
    const std::string parentBefore = SaveMaterialToString(parent);

    MaterialInstance instance;
    instance.parent = Id(900);
    instance.Set("roughness", ParseValue(ValueKind::Float, "0.9"));

    // Kriteria terima E7.1 nomor 4. Induknya tidak boleh tersentuh sama sekali
    // — bukan "nilainya sama", melainkan berkasnya byte-per-byte sama.
    CHECK(SaveMaterialToString(parent) == parentBefore);

    const std::vector<ResolvedParameter> resolved = ResolveParameters(parent, instance);
    REQUIRE(resolved.size() == 2);

    // Yang ditimpa memakai nilai instance...
    const auto rough = std::find_if(resolved.begin(), resolved.end(),
                                    [](const ResolvedParameter& parameter) {
                                        return parameter.name == "roughness";
                                    });
    REQUIRE(rough != resolved.end());
    CHECK(rough->value.components[0] == doctest::Approx(0.9f));
    CHECK(rough->overridden);

    // ...dan yang tidak ditimpa tetap memakai nilai induk.
    const auto tint = std::find_if(resolved.begin(), resolved.end(),
                                   [](const ResolvedParameter& parameter) {
                                       return parameter.name == "tint";
                                   });
    REQUIRE(tint != resolved.end());
    CHECK(tint->value.components[0] == doctest::Approx(0.8f));
    CHECK(tint->value.components[1] == doctest::Approx(0.4f));
    CHECK_FALSE(tint->overridden);
}

TEST_CASE("Mengubah bawaan induk mengalir ke instance yang tidak menimpanya") {
    MaterialGraph parent = ParentGraph();
    MaterialInstance instance;
    instance.parent = Id(900);
    instance.Set("roughness", ParseValue(ValueKind::Float, "0.9"));

    // Inilah gunanya instance tidak menyalin graph: memperbaiki induk
    // memperbaiki seluruh instance-nya sekaligus.
    parent.parameters[0].defaultValue = "float3(0.1, 0.2, 0.3)";

    const std::vector<ResolvedParameter> resolved = ResolveParameters(parent, instance);
    CHECK(resolved[0].value.components[0] == doctest::Approx(0.1f));
    // Yang ditimpa tetap milik instance — bawaan induk tidak menariknya kembali.
    CHECK(resolved[1].value.components[0] == doctest::Approx(0.9f));
}

TEST_CASE("Timpaan dibuang saat parameternya hilang atau berganti tipe") {
    MaterialGraph parent = ParentGraph();
    MaterialInstance instance;
    instance.parent = Id(900);
    instance.Set("roughness", ParseValue(ValueKind::Float, "0.9"));
    instance.Set("sudahTidakAda", ParseValue(ValueKind::Float, "1.0"));

    // Parameter yang tidak ada di induk tidak muncul di hasil resolusi.
    CHECK(ResolveParameters(parent, instance).size() == 2);

    // Tipe yang berganti mengalahkan nilai tersimpan: sebuah angka yang menjadi
    // warna tidak punya nilai lama yang masuk akal.
    parent.parameters[1].kind = ValueKind::Float3;
    parent.parameters[1].defaultValue = "float3(0.25)";
    const std::vector<ResolvedParameter> resolved = ResolveParameters(parent, instance);
    CHECK_FALSE(resolved[1].overridden);
    CHECK(resolved[1].value.components[0] == doctest::Approx(0.25f));
}

TEST_CASE(".simmatinst bolak-balik tanpa kehilangan apa pun") {
    MaterialInstance original;
    original.parent = Id(900);
    original.Set("tint", ParseValue(ValueKind::Float3, "float3(0.2, 0.4, 0.6)"));
    original.Set("roughness", ParseValue(ValueKind::Float, "0.75"));

    const std::string text = SaveInstanceToString(original);
    MaterialInstance loaded;
    REQUIRE(LoadInstanceFromString(loaded, text).ok);

    CHECK(SaveInstanceToString(loaded) == text);
    CHECK(loaded.parent == original.parent);
    REQUIRE(loaded.overrides.size() == 2);
    CHECK(loaded.Find("tint")->value.components[2] == doctest::Approx(0.6f));
    CHECK(loaded.Find("roughness")->value.components[0] == doctest::Approx(0.75f));

    // Berkasnya kecil berapa pun besar graph induknya: yang tersimpan hanya
    // yang benar-benar diubah.
    CHECK(text.find("nodes") == std::string::npos);
}

TEST_CASE("Instance tanpa induk ditolak saat dimuat") {
    MaterialInstance loaded;
    const MaterialIoResult result =
        LoadInstanceFromString(loaded, "{\n  \"version\": 1,\n  \"overrides\": []\n}\n");
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("parent") != std::string::npos);
}

TEST_CASE("Membersihkan timpaan mengembalikan parameter ke nilai induk") {
    const MaterialGraph parent = ParentGraph();
    MaterialInstance instance;
    instance.parent = Id(900);
    instance.Set("roughness", ParseValue(ValueKind::Float, "0.9"));
    REQUIRE(ResolveParameters(parent, instance)[1].overridden);

    instance.Clear("roughness");
    const std::vector<ResolvedParameter> resolved = ResolveParameters(parent, instance);
    CHECK_FALSE(resolved[1].overridden);
    CHECK(resolved[1].value.components[0] == doctest::Approx(0.3f));
    CHECK(instance.overrides.empty());
}
