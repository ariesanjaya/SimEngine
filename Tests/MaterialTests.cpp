#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Material/MaterialGraph.h"
#include "Sim/Material/MaterialCompiler.h"
#include "Sim/Material/MaterialNodeCatalog.h"
#include "Sim/Material/MaterialParameterBlock.h"
#include "Sim/Material/MaterialShaderModule.h"
#include "Sim/Material/MaterialValidation.h"
#include "Sim/Material/ShaderCache.h"

#include <doctest/doctest.h>
#include <set>

#include <cstring>

#include <filesystem>
#include <fstream>
#include <memory>

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
    // Nama sumbernya dipakai apa adanya. Kompiler yang menambahkan ".simmat"
    // sendiri menghasilkan "Batu.simmat.simmat" untuk pemanggil yang sudah
    // menyertakannya — komentar kepala yang menyebut berkas yang tidak ada.
    MaterialCompileOptions named;
    named.moduleName = "Batu.simmat";
    CHECK(CompileMaterial(MinimalGraph(), named).slang.find("dari Batu.simmat —") !=
          std::string::npos);
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
#include "Sim/Material/MaterialParameterBlock.h"

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

// =============================================================================
// E8.2 — tata letak blok uniform
// =============================================================================

namespace {

MaterialParameter Param(const std::string& name, ValueKind kind, const std::string& value = {}) {
    MaterialParameter parameter;
    parameter.name = name;
    parameter.kind = kind;
    parameter.defaultValue = value;
    return parameter;
}

float ReadFloat(const std::vector<uint8_t>& block, uint32_t offset) {
    float value = 0.0f;
    std::memcpy(&value, block.data() + offset, sizeof(value));
    return value;
}

}  // namespace

TEST_CASE("Float3 berjajar 16 byte, dan float sesudahnya tidak mengisi celahnya") {
    // Inilah aturan std140 yang paling sering dilupakan, dan satu-satunya yang
    // salahnya tidak menghasilkan galat apa pun — hanya nilai yang bergeser.
    // Offset di bawah dihitung tangan, bukan disalin dari keluaran kode.
    MaterialParameterBlock block;
    block.Build({
        Param("roughness", ValueKind::Float),   // 0  .. 4
        Param("tint", ValueKind::Float3),       // 16 .. 28   (didorong dari 4)
        Param("metalness", ValueKind::Float),   // 28 .. 32   (celah 12..16 TIDAK dipakai)
        Param("uvScale", ValueKind::Float2),    // 32 .. 40
        Param("emissive", ValueKind::Float4),   // 48 .. 64   (didorong dari 40)
    });

    REQUIRE(block.SlotCount() == 5);
    CHECK(block.Slot(0).offset == 0u);
    CHECK(block.Slot(1).offset == 16u);
    CHECK(block.Slot(2).offset == 28u);
    CHECK(block.Slot(3).offset == 32u);
    CHECK(block.Slot(4).offset == 48u);
    CHECK(block.Bytes() == 64u);
}

TEST_CASE("Tekstur tidak menempati blok uniform dan tidak menggeser yang sesudahnya") {
    MaterialParameterBlock block;
    block.Build({
        Param("roughness", ValueKind::Float),
        Param("albedoMap", ValueKind::Texture),
        Param("metalness", ValueKind::Float),
    });
    REQUIRE(block.SlotCount() == 2);
    CHECK(block.Slot(0).offset == 0u);
    // 4, bukan 8: tekstur punya tabel slotnya sendiri dan tidak memakan tempat
    // di sini sama sekali.
    CHECK(block.Slot(1).offset == 4u);
    CHECK(block.Find("albedoMap") == -1);
}

TEST_CASE("Blok dibulatkan ke kelipatan 16") {
    MaterialParameterBlock block;
    block.Build({Param("a", ValueKind::Float)});
    CHECK(block.Bytes() == 16u);
    block.Build({Param("a", ValueKind::Float4), Param("b", ValueKind::Float)});
    CHECK(block.Bytes() == 32u);
}

TEST_CASE("Nilai bawaan terisi, lalu ditimpa override instance") {
    const std::vector<MaterialParameter> parameters{
        Param("roughness", ValueKind::Float, "0.4"),
        Param("tint", ValueKind::Float3, "float3(0.2, 0.4, 0.6)"),
    };
    MaterialParameterBlock block;
    block.Build(parameters);

    std::vector<uint8_t> bytes;
    block.Fill(parameters, {}, bytes);
    REQUIRE(bytes.size() == block.Bytes());
    CHECK(ReadFloat(bytes, 0) == doctest::Approx(0.4f));
    CHECK(ReadFloat(bytes, 16) == doctest::Approx(0.2f));
    CHECK(ReadFloat(bytes, 24) == doctest::Approx(0.6f));

    MaterialValue override;
    override.kind = ValueKind::Float;
    override.components = {0.9f, 0.0f, 0.0f, 0.0f};
    block.Fill(parameters, {ParameterOverride{"roughness", override}}, bytes);
    CHECK(ReadFloat(bytes, 0) == doctest::Approx(0.9f));
    // Yang tidak ditimpa tetap dari induknya.
    CHECK(ReadFloat(bytes, 16) == doctest::Approx(0.2f));
}

TEST_CASE("Override yang parameternya sudah tidak ada diabaikan, bukan menggagalkan") {
    // Menghapus sebuah parameter dari induk tidak boleh membuat instance-nya
    // gagal dimuat.
    const std::vector<MaterialParameter> parameters{Param("roughness", ValueKind::Float, "0.5")};
    MaterialParameterBlock block;
    block.Build(parameters);

    MaterialValue ghost;
    ghost.kind = ValueKind::Float;
    ghost.components = {1.0f, 0.0f, 0.0f, 0.0f};
    std::vector<uint8_t> bytes;
    block.Fill(parameters, {ParameterOverride{"sudahDihapus", ghost}}, bytes);
    CHECK(bytes.size() == block.Bytes());
    CHECK(ReadFloat(bytes, 0) == doctest::Approx(0.5f));
}

TEST_CASE("Sisipan selalu nol, jadi dua blok yang isinya sama berbanding sama") {
    // Renderer memakai perbandingan byte untuk memutuskan apakah sebuah blok
    // perlu diunggah ulang; sisipan yang tidak diinisialisasi membuat dua blok
    // yang sebenarnya sama terlihat berbeda.
    // `cutoff` lebih dulu, lalu `tint`: penjajaran 16 milik float3 mendorongnya
    // ke offset 16 dan meninggalkan celah nyata di 4..16. Urutan sebaliknya
    // tidak menyisakan celah sama sekali — float justru mengisi offset +12,
    // karena ukuran float3 memang 12.
    const std::vector<MaterialParameter> parameters{
        Param("cutoff", ValueKind::Float, "0.5"),
        Param("tint", ValueKind::Float3, "float3(1.0)"),
    };
    MaterialParameterBlock block;
    block.Build(parameters);
    REQUIRE(block.Slot(1).offset == 16u);
    std::vector<uint8_t> a;
    std::vector<uint8_t> b;
    block.Fill(parameters, {}, a);
    block.Fill(parameters, {}, b);
    CHECK(a == b);
    // Celah 4..16 benar-benar nol.
    CHECK(ReadFloat(a, 4) == doctest::Approx(0.0f));
    CHECK(ReadFloat(a, 8) == doctest::Approx(0.0f));
    CHECK(ReadFloat(a, 12) == doctest::Approx(0.0f));
}

TEST_CASE("Menulis satu nilai tidak melewati batas slotnya") {
    const std::vector<MaterialParameter> parameters{
        Param("tint", ValueKind::Float3, "float3(0.0)"),
        Param("cutoff", ValueKind::Float, "0.25"),
    };
    MaterialParameterBlock block;
    block.Build(parameters);
    std::vector<uint8_t> bytes;
    block.Fill(parameters, {}, bytes);

    // Instance lama menyimpan float4 untuk parameter yang kini float3. `cutoff`
    // duduk tepat di offset 12 — mengisi celah ukuran float3 — jadi komponen
    // keempat yang ditulis melewati slotnya akan menimpanya persis.
    MaterialValue wide;
    wide.kind = ValueKind::Float4;
    wide.components = {1.0f, 1.0f, 1.0f, 99.0f};
    CHECK(block.Write(bytes, "tint", wide));
    CHECK(ReadFloat(bytes, 12) == doctest::Approx(0.25f));
}

TEST_CASE("Menulis parameter yang tidak ada mengembalikan false") {
    MaterialParameterBlock block;
    block.Build({Param("a", ValueKind::Float)});
    std::vector<uint8_t> bytes(block.Bytes(), 0u);
    CHECK(!block.Write(bytes, "b", MaterialValue{}));
}

// --- Cache SPIR-V ------------------------------------------------------------

namespace {

/// Direktori cache yang bersih untuk satu test, dan terhapus sesudahnya.
///
/// Nama diambil dari nama test-nya, bukan dari waktu atau angka acak: test yang
/// gagal harus bisa dijalankan ulang dan menemui keadaan awal yang sama, dan
/// direktori bernama acak yang tertinggal karena test-nya crash tidak bisa
/// dikenali lagi sebagai milik siapa.
struct TempCacheDir {
    std::filesystem::path path;

    explicit TempCacheDir(std::string_view name) {
        std::error_code ec;
        path = std::filesystem::temp_directory_path(ec) / ("sim-shader-cache-" + std::string(name));
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }
    ~TempCacheDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempCacheDir(const TempCacheDir&) = delete;
    TempCacheDir& operator=(const TempCacheDir&) = delete;
};

/// SPIR-V palsu yang lolos pemeriksaan bentuk: magic + header + satu kata
/// penanda supaya isinya bisa dibedakan antar-kompilasi.
std::vector<uint32_t> FakeSpirv(uint32_t marker) {
    return {0x07230203u, 0x00010500u, 0u, 1u, 0u, marker};
}

/// Kompilator yang menghitung berapa kali ia benar-benar dipanggil.
struct CountingCompiler {
    std::shared_ptr<int> calls = std::make_shared<int>(0);
    uint32_t marker = 1u;

    ShaderCache::Compiler Bind() {
        auto counter = calls;
        const uint32_t value = marker;
        return [counter, value](const CompileRequest&) {
            ++*counter;
            CompileOutput out;
            out.ok = true;
            out.spirv = FakeSpirv(value);
            return out;
        };
    }
};

CompileRequest Request(std::string source, ShaderStage stage = ShaderStage::Fragment,
                       std::string entry = "fragmentMain") {
    CompileRequest request;
    request.source = std::move(source);
    request.stage = stage;
    request.entryPoint = std::move(entry);
    return request;
}

}  // namespace

TEST_CASE("Kompilasi kedua dilayani cache, bukan kompilator") {
    TempCacheDir dir("hit");
    CountingCompiler compiler;
    ShaderCache cache;
    cache.Configure(dir.path, "slangc-2026.8");
    cache.SetCompiler(compiler.Bind());

    const CompileOutput first = cache.Get(Request("float4 f() { return 0; }"));
    REQUIRE(first.ok);
    const CompileOutput second = cache.Get(Request("float4 f() { return 0; }"));
    REQUIRE(second.ok);

    CHECK(*compiler.calls == 1);
    CHECK(cache.Statistics().hits == 1);
    CHECK(cache.Statistics().misses == 1);
    CHECK(second.spirv == first.spirv);
}

TEST_CASE("Cache bertahan melewati instance ShaderCache") {
    TempCacheDir dir("persist");
    CountingCompiler compiler;
    {
        ShaderCache cache;
        cache.Configure(dir.path, "slangc-2026.8");
        cache.SetCompiler(compiler.Bind());
        REQUIRE(cache.Get(Request("a")).ok);
    }
    ShaderCache fresh;
    fresh.Configure(dir.path, "slangc-2026.8");
    fresh.SetCompiler(compiler.Bind());
    REQUIRE(fresh.Get(Request("a")).ok);

    // Inti cache disk: proses berikutnya tidak mengompilasi ulang.
    CHECK(*compiler.calls == 1);
    CHECK(fresh.Statistics().hits == 1);
}

TEST_CASE("Sumber, tahap, dan entry point masing-masing memisahkan kunci") {
    ShaderCache cache;
    cache.Configure({}, "slangc-2026.8");

    const std::string base = cache.KeyOf(Request("a", ShaderStage::Fragment, "main"));
    CHECK(cache.KeyOf(Request("b", ShaderStage::Fragment, "main")) != base);
    CHECK(cache.KeyOf(Request("a", ShaderStage::Vertex, "main")) != base);
    CHECK(cache.KeyOf(Request("a", ShaderStage::Fragment, "other")) != base);

    // Medan yang bersebelahan tidak boleh bisa saling meminjam karakter: kalau
    // kuncinya sekadar penggabungan, "ab"+"c" dan "a"+"bc" akan bertemu.
    CHECK(cache.KeyOf(Request("ab", ShaderStage::Fragment, "c")) !=
          cache.KeyOf(Request("a", ShaderStage::Fragment, "bc")));
}

TEST_CASE("Kompilator yang berganti versi membatalkan cache") {
    TempCacheDir dir("compiler");
    CountingCompiler compiler;
    ShaderCache cache;
    cache.Configure(dir.path, "slangc-2026.8");
    cache.SetCompiler(compiler.Bind());
    REQUIRE(cache.Get(Request("a")).ok);

    // Ini yang mencegah SPIR-V dari kompilator yang sudah tidak ada dipakai
    // ulang — bug yang muncul sebagai shader yang jalan di satu mesin saja.
    cache.Configure(dir.path, "slangc-2027.1");
    REQUIRE(cache.Get(Request("a")).ok);
    CHECK(*compiler.calls == 2);
    CHECK(cache.Statistics().hits == 0);
}

TEST_CASE("Varian tidak memisahkan kunci — ia konstanta spesialisasi") {
    ShaderVariant plain;
    ShaderVariant skinned;
    skinned.skinned = true;

    CHECK(plain.Mask() == 0u);
    CHECK(skinned.Mask() == 1u);
    CHECK(skinned.Constants() == std::array<uint32_t, 3>{1u, 0u, 0u});

    ShaderVariant all;
    all.skinned = true;
    all.instanced = true;
    all.alphaTest = true;
    CHECK(all.Mask() == 7u);
    CHECK(all.Constants() == std::array<uint32_t, 3>{1u, 1u, 1u});

    // Satu modul untuk kedelapan kombinasinya: ShaderVariant tidak punya jalan
    // masuk ke CompileRequest sama sekali, jadi tidak ada cara ia ikut kunci.
    ShaderCache cache;
    cache.Configure({}, "slangc-2026.8");
    CHECK(cache.KeyOf(Request("a")) == cache.KeyOf(Request("a")));
}

TEST_CASE("Entri yang terpotong dibaca sebagai miss, bukan sebagai SPIR-V") {
    TempCacheDir dir("truncated");
    CountingCompiler compiler;
    ShaderCache cache;
    cache.Configure(dir.path, "slangc-2026.8");
    cache.SetCompiler(compiler.Bind());

    const CompileRequest request = Request("a");
    REQUIRE(cache.Get(request).ok);

    // Simulasi proses yang mati di tengah tulis pada versi lama, atau salinan
    // proyek yang rusak: berkasnya ada, namanya benar, isinya tidak utuh.
    const std::filesystem::path entry =
        dir.path / (cache.KeyOf(request) + ".spv");
    {
        std::ofstream file(entry, std::ios::binary | std::ios::trunc);
        const uint32_t magicOnly = 0x07230203u;
        file.write(reinterpret_cast<const char*>(&magicOnly), sizeof(magicOnly));
    }

    const CompileOutput out = cache.Get(request);
    CHECK(out.ok);
    CHECK(LooksLikeSpirv(out.spirv));
    CHECK(*compiler.calls == 2);
    CHECK(cache.Statistics().rejected == 1);
}

TEST_CASE("Berkas yang bukan kelipatan empat byte ditolak") {
    TempCacheDir dir("ragged");
    CountingCompiler compiler;
    ShaderCache cache;
    cache.Configure(dir.path, "slangc-2026.8");
    cache.SetCompiler(compiler.Bind());

    const CompileRequest request = Request("a");
    REQUIRE(cache.Get(request).ok);
    {
        std::ofstream file(dir.path / (cache.KeyOf(request) + ".spv"),
                           std::ios::binary | std::ios::trunc);
        file << "not spirv";
    }
    CHECK(cache.Get(request).ok);
    CHECK(cache.Statistics().rejected == 1);
}

TEST_CASE("Kompilator yang gagal tidak meninggalkan entri") {
    TempCacheDir dir("failure");
    ShaderCache cache;
    cache.Configure(dir.path, "slangc-2026.8");
    cache.SetCompiler([](const CompileRequest&) {
        CompileOutput out;
        out.error = "expected ';'";
        return out;
    });

    const CompileRequest request = Request("bad");
    const CompileOutput out = cache.Get(request);
    CHECK(!out.ok);
    CHECK(out.error == "expected ';'");

    // Kegagalan yang tersimpan akan membuat shader yang sudah diperbaiki tetap
    // gagal sampai cache-nya dibersihkan tangan.
    CHECK(!std::filesystem::exists(dir.path / (cache.KeyOf(request) + ".spv")));
}

TEST_CASE("Sukses tanpa SPIR-V yang sah dilaporkan gagal") {
    TempCacheDir dir("empty");
    ShaderCache cache;
    cache.Configure(dir.path, "slangc-2026.8");
    cache.SetCompiler([](const CompileRequest&) {
        CompileOutput out;
        out.ok = true;  // mengaku berhasil
        return out;     // tapi tidak menyerahkan apa pun
    });

    const CompileOutput out = cache.Get(Request("a"));
    CHECK(!out.ok);
    CHECK(!out.error.empty());
    CHECK(out.spirv.empty());
}

TEST_CASE("Cache tanpa direktori tetap mengompilasi") {
    CountingCompiler compiler;
    ShaderCache cache;
    cache.Configure({}, "slangc-2026.8");
    cache.SetCompiler(compiler.Bind());

    CHECK(cache.Get(Request("a")).ok);
    CHECK(cache.Get(Request("a")).ok);
    // Tanpa lapisan disk setiap permintaan meleset — yang benar, dan tidak
    // sama dengan gagal.
    CHECK(*compiler.calls == 2);
    CHECK(cache.Statistics().hits == 0);
}

TEST_CASE("Tanpa kompilator, cache melaporkan alasannya") {
    ShaderCache cache;
    cache.Configure({}, "slangc-2026.8");
    const CompileOutput out = cache.Get(Request("a"));
    CHECK(!out.ok);
    CHECK(!out.error.empty());
}

TEST_CASE("Purge hanya menyentuh berkas milik cache") {
    TempCacheDir dir("purge");
    CountingCompiler compiler;
    ShaderCache cache;
    cache.Configure(dir.path, "slangc-2026.8");
    cache.SetCompiler(compiler.Bind());
    REQUIRE(cache.Get(Request("a")).ok);

    const std::filesystem::path bystander = dir.path / "catatan.txt";
    { std::ofstream(bystander) << "bukan milik cache"; }

    cache.Purge();
    CHECK(!std::filesystem::exists(dir.path / (cache.KeyOf(Request("a")) + ".spv")));
    // Direktori cache yang ternyata ditunjuk ke tempat lain tidak boleh menjadi
    // penghapusan menyeluruh.
    CHECK(std::filesystem::exists(bystander));
}

TEST_CASE("slangc sungguhan menghasilkan SPIR-V yang bisa di-cache") {
    const std::string identity = SlangCompilerIdentity();
    if (identity.empty()) {
        MESSAGE("slangc tidak ditemukan — bagian integrasi dilewati");
        return;
    }

    TempCacheDir dir("slangc");
    ShaderCache cache;
    cache.Configure(dir.path, identity);
    cache.SetCompiler(MakeSlangCompiler());

    const std::string source = R"(
[shader("fragment")]
float4 fragmentMain(float2 uv : TEXCOORD0) : SV_Target {
    return float4(uv, 0.0, 1.0);
}
)";
    const CompileOutput out = cache.Get(Request(source));
    INFO("slangc: ", out.error);
    REQUIRE(out.ok);
    CHECK(LooksLikeSpirv(out.spirv));
    CHECK(out.spirv.size() > 16);

    const CompileOutput again = cache.Get(Request(source));
    REQUIRE(again.ok);
    CHECK(again.spirv == out.spirv);
    CHECK(cache.Statistics().hits == 1);
}

TEST_CASE("Sumber Slang yang salah dilaporkan dengan pesan slangc") {
    if (SlangCompilerIdentity().empty()) {
        MESSAGE("slangc tidak ditemukan — bagian integrasi dilewati");
        return;
    }
    ShaderCache cache;
    cache.Configure({}, "x");
    cache.SetCompiler(MakeSlangCompiler());

    const CompileOutput out = cache.Get(Request("this is not slang at all"));
    CHECK(!out.ok);
    // Pesannya diteruskan apa adanya; panel material yang menaruhnya di samping
    // node yang salah, dan itu hanya bisa kalau teksnya tidak dibuang di sini.
    CHECK(!out.error.empty());
}

TEST_CASE("slangc yang ditunjuk ke berkas yang tidak ada gagal dengan jelas") {
    ShaderCache cache;
    cache.Configure({}, "x");
    cache.SetCompiler(MakeSlangCompiler("/tidak/ada/slangc"));

    const CompileOutput out = cache.Get(Request("a"));
    CHECK(!out.ok);
    CHECK(out.error.find("/tidak/ada/slangc") != std::string::npos);
}

// --- Graph → Slang → SPIR-V --------------------------------------------------

TEST_CASE("Modul yang dirakit menanam prelude, bukan meng-import-nya") {
    const MaterialCompileResult compiled = CompileMaterial(MinimalGraph());
    REQUIRE(compiled.ok);
    // Kode yang dihasilkan sengaja tetap memakai import — ia ditulis untuk
    // dibaca manusia.
    CHECK(compiled.slang.find("import openpbr;") != std::string::npos);

    MaterialModuleOptions options;
    options.prelude = "struct OpenPBRSurface { int marker; };\n";
    const std::string module = AssembleMaterialModule(compiled.slang, options);

    CHECK(module.find("import openpbr;") == std::string::npos);
    CHECK(module.find("int marker;") != std::string::npos);
    CHECK(module.find("evalMaterial") != std::string::npos);
    CHECK(module.find("[shader(\"fragment\")]") != std::string::npos);
    CHECK(module.find("vk::constant_id(2)") != std::string::npos);
}

/// Modul pass forward untuk sebuah graph minimal, apa adanya.
///
/// **Yang diuji dua kasus di bawah adalah teksnya, bukan gambarnya.** Mode clay
/// dan detail lighting tergambar identik pada setiap material yang tidak punya
/// peta normal — yaitu hampir seluruh material uji — jadi perbandingan gambar
/// tidak bisa membedakan "keduanya memang sama di sini" dari "cabangnya tidak
/// pernah jalan".
std::string ForwardModuleForMinimalGraph() {
    MaterialGraph graph;
    MaterialNode output;
    output.guid = Uuid::Generate();
    output.type = std::string(kSurfaceOutputType);
    graph.nodes.push_back(output);

    MaterialCompileOptions options;
    options.moduleName = "uji.simmat";

    ForwardMaterialOptions moduleOptions;
    moduleOptions.prelude = LoadOpenPbrPrelude(SIM_SHADER_DIR);
    moduleOptions.frameDeclarations = InlineShaderIncludes(
        SIM_SHADER_DIR, {"box_varyings.slang", "cluster_common.slang", "gi_resolve.slang"});

    const MaterialCompileResult compiled = CompileMaterial(graph, options);
    if (!compiled.ok) {
        return {};
    }
    moduleOptions.lobes = compiled.lobes;
    return AssembleForwardMaterialModule(compiled.slang, moduleOptions);
}

TEST_CASE("Detail lighting mempertahankan peta normal, clay tidak") {
    const std::string module = ForwardModuleForMinimalGraph();
    REQUIRE(!module.empty());

    const std::size_t clay = module.find("if (clayView()) {");
    REQUIRE(clay != std::string::npos);
    const std::size_t guard = module.find("if (!detailLightingView()) {", clay);
    REQUIRE(guard != std::string::npos);
    const std::size_t reset = module.find("m.normal = float3(0.0);", clay);
    REQUIRE(reset != std::string::npos);
    // Pembuangan normal berada **di dalam** syaratnya, bukan sesudahnya.
    CHECK(guard < reset);
}

TEST_CASE("Kanal permukaan keluar sebelum penyinaran dijalankan") {
    // Mode kanal mengembalikan satu angka permukaan apa adanya. Kalau cabangnya
    // berada sesudah penyinaran, yang keluar bukan angka itu melainkan angka itu
    // setelah dikalikan cahaya — gambar yang meyakinkan dan salah.
    const std::string module = ForwardModuleForMinimalGraph();
    REQUIRE(!module.empty());

    const std::size_t channels = module.find("case kViewBaseColor:");
    const std::size_t unlit = module.find("if (unlitView() || !anyLightInScene()) {");
    const std::size_t lit = module.find("evaluateOpenPBR(m.surface");
    REQUIRE(channels != std::string::npos);
    REQUIRE(unlit != std::string::npos);
    REQUIRE(lit != std::string::npos);
    CHECK(channels < unlit);
    CHECK(channels < lit);

    // Pantulan menyetel permukaannya lalu **membiarkan penyinaran berjalan** —
    // ia mode yang menampilkan lingkungan, jadi ia justru menuntut cahaya.
    const std::size_t reflections = module.find("if (reflectionsView()) {");
    REQUIRE(reflections != std::string::npos);
    CHECK(reflections < lit);
}

TEST_CASE("Prelude yang berubah membatalkan cache material") {
    TempCacheDir dir("prelude");
    CountingCompiler compiler;
    ShaderCache cache;
    cache.Configure(dir.path, "slangc-2026.8");
    cache.SetCompiler(compiler.Bind());

    const MaterialCompileResult compiled = CompileMaterial(MinimalGraph());
    REQUIRE(compiled.ok);

    MaterialModuleOptions before;
    before.prelude = "// model shading versi lama\n";
    REQUIRE(cache.Get(MakeMaterialRequest(compiled.slang, ShaderStage::Fragment, before)).ok);
    REQUIRE(cache.Get(MakeMaterialRequest(compiled.slang, ShaderStage::Fragment, before)).ok);
    CHECK(*compiler.calls == 1);

    // Inilah alasan prelude ditanam: kalau ia hanya di-import, sumber yang
    // di-hash tidak berubah sedikit pun dan cache akan menyerahkan SPIR-V yang
    // dibangun terhadap model shading yang sudah tidak ada.
    MaterialModuleOptions after;
    after.prelude = "// model shading versi baru\n";
    REQUIRE(cache.Get(MakeMaterialRequest(compiled.slang, ShaderStage::Fragment, after)).ok);
    CHECK(*compiler.calls == 2);
}

TEST_CASE("Graph sungguhan berjalan sampai SPIR-V") {
    const std::string identity = SlangCompilerIdentity();
    if (identity.empty()) {
        MESSAGE("slangc tidak ditemukan — bagian integrasi dilewati");
        return;
    }

    MaterialModuleOptions options;
    options.prelude = LoadOpenPbrPrelude(SIM_SHADER_DIR);
    REQUIRE_MESSAGE(!options.prelude.empty(), "openpbr.slang tidak terbaca dari " SIM_SHADER_DIR);

    // Material yang menyentuh lebih dari satu kanal, supaya yang diuji bukan
    // hanya jalur baseColor.
    MaterialGraph graph = MinimalGraph();
    graph.nodes.push_back(Node(3, "input.constant"));
    graph.nodes.back().settings["kind"] = "float";
    graph.nodes.back().settings["value"] = "0.65";
    Link(graph, 3, "value", 1, "specularRoughness");

    graph.nodes.push_back(Node(4, "input.constant"));
    graph.nodes.back().settings["kind"] = "float";
    graph.nodes.back().settings["value"] = "1.0";
    Link(graph, 4, "value", 1, "baseMetalness");

    const MaterialCompileResult compiled = CompileMaterial(graph);
    REQUIRE(compiled.ok);

    TempCacheDir dir("endtoend");
    ShaderCache cache;
    cache.Configure(dir.path, identity);
    cache.SetCompiler(MakeSlangCompiler());

    const CompileOutput out = cache.Get(MakeMaterialRequest(compiled.slang, ShaderStage::Fragment, options));
    INFO("slangc: ", out.error);
    REQUIRE(out.ok);
    CHECK(LooksLikeSpirv(out.spirv));
    CHECK(cache.Statistics().compiles == 1);

    // Dan sekali lagi lewat cache — jalur yang benar-benar dipakai editor saat
    // material yang sama diminta ulang.
    const CompileOutput again = cache.Get(MakeMaterialRequest(compiled.slang, ShaderStage::Fragment, options));
    REQUIRE(again.ok);
    CHECK(again.spirv == out.spirv);
    CHECK(cache.Statistics().hits == 1);
}

TEST_CASE("Material bertekstur dan berparameter juga sampai SPIR-V") {
    const std::string identity = SlangCompilerIdentity();
    if (identity.empty()) {
        MESSAGE("slangc tidak ditemukan — bagian integrasi dilewati");
        return;
    }
    MaterialModuleOptions options;
    options.prelude = LoadOpenPbrPrelude(SIM_SHADER_DIR);
    REQUIRE(!options.prelude.empty());

    MaterialGraph graph;
    graph.nodes.push_back(Node(1, std::string(kSurfaceOutputType)));
    graph.nodes.push_back(Node(2, "input.texture"));
    graph.nodes.back().settings["texture"] = Id(900).ToString();
    graph.nodes.push_back(Node(3, "input.sample"));
    Link(graph, 2, "texture", 3, "texture");
    Link(graph, 3, "rgb", 1, "baseColor");

    graph.nodes.push_back(Node(4, "param.get"));
    graph.nodes.back().settings["parameter"] = "roughness";
    MaterialParameter roughness;
    roughness.name = "roughness";
    roughness.kind = ValueKind::Float;
    roughness.defaultValue = "0.4";
    graph.parameters.push_back(roughness);
    Link(graph, 4, "value", 1, "specularRoughness");

    const MaterialCompileResult compiled = CompileMaterial(graph);
    REQUIRE_MESSAGE(compiled.ok, FirstError({false, compiled.errors}));
    REQUIRE(compiled.textures.size() == 1);

    ShaderCache cache;
    cache.Configure({}, identity);
    cache.SetCompiler(MakeSlangCompiler());
    const CompileOutput out = cache.Get(MakeMaterialRequest(compiled.slang, ShaderStage::Fragment, options));
    INFO("slangc: ", out.error);
    REQUIRE(out.ok);

    // Tata letak cbuffer yang dihitung C++ harus cocok dengan yang ditulis ke
    // Slang. Keduanya berangkat dari daftar parameter yang sama, dan test ini
    // yang menjaga keduanya tidak berpisah diam-diam.
    MaterialParameterBlock block;
    block.Build(graph.parameters);
    CHECK(block.SlotCount() == 1);
    CHECK(block.Slot(0).offset == 0u);
    CHECK(block.Bytes() == 16u);
}

TEST_CASE("openpbr.slang sendiri bisa dikompilasi") {
    if (SlangCompilerIdentity().empty()) {
        MESSAGE("slangc tidak ditemukan — bagian integrasi dilewati");
        return;
    }
    const std::string prelude = LoadOpenPbrPrelude(SIM_SHADER_DIR);
    REQUIRE(!prelude.empty());

    // Model shading diuji lepas dari material mana pun. Kalau ia hanya pernah
    // dikompilasi sebagai bagian modul yang dirakit, kesalahan di dalamnya akan
    // muncul sebagai "material anu gagal" — menunjuk ke tempat yang salah.
    ShaderCache cache;
    cache.Configure({}, "x");
    cache.SetCompiler(MakeSlangCompiler());

    CompileRequest request;
    request.stage = ShaderStage::Fragment;
    request.entryPoint = "probeMain";
    request.source = prelude + R"(
[shader("fragment")]
float4 probeMain(float3 normal : NORMAL, float3 view : TEXCOORD0) : SV_Target {
    OpenPBRSurface surface = OpenPBRSurface::defaults();
    surface.baseMetalness = 0.5;
    surface.coatWeight = 1.0;
    surface.fuzzWeight = 0.25;
    surface.specularRoughnessAnisotropy = -0.5;
    ShadingFrame frame;
    frame.normal = normalize(normal);
    frame.view = normalize(view);
    frame.tangent = float3(1, 0, 0);
    frame.bitangent = float3(0, 1, 0);
    float3 direct = evaluateOpenPBR(surface, frame, float3(0, 1, 0), float3(1));
    // Jalur IBL diuji di sini juga: kesalahan di dalamnya harus muncul sebagai
    // "openpbr.slang gagal", bukan sebagai "material anu gagal".
    float3 reflectDir = prefilterDirection(frame);
    float mip = prefilterMipForRoughness(surface.specularRoughness, 6.0);
    float3 ambient = evaluateOpenPBR_IBL(surface, frame, float3(0.4), float3(0.6),
                                         float3(0.7), float2(0.9, 0.02));
    return float4(direct + ambient + reflectDir * mip * 0.0, 1);
}
)";
    const CompileOutput out = cache.Get(request);
    INFO("slangc: ", out.error);
    CHECK(out.ok);
}

TEST_CASE("Pin geometri OpenPBR ada, dan berada di luar OpenPBRSurface") {
    const MaterialNodeType* output = MaterialNodeCatalog::Get().Find(kSurfaceOutputType);
    REQUIRE(output != nullptr);

    // `geometry_tangent`, `geometry_coat_normal`, dan `geometry_coat_tangent`
    // di spesifikasi. Tanpa yang pertama, `specularRoughnessAnisotropy` tidak
    // bisa diarahkan sama sekali — sorotnya selalu meregang menurut tangent
    // mesh, bukan menurut goresan yang digambar pengarangnya.
    for (const char* name : {"tangent", "coatNormal", "coatTangent"}) {
        INFO("pin ", name);
        const MaterialPin* pin = output->FindPin(name);
        REQUIRE(pin != nullptr);
        CHECK(pin->kind == ValueKind::Float3);
        // Bawaannya sumbu identitas, bukan nol: nol berarti "tidak menunjuk ke
        // mana pun", dan itu bukan hal yang sama.
        CHECK(SurfacePinIsExtra(name));
    }
    CHECK(output->FindPin("tangent")->defaultValue == "float3(1.0, 0.0, 0.0)");
    CHECK(output->FindPin("coatNormal")->defaultValue == "float3(0.0, 0.0, 1.0)");

    // Decal tidak punya coat, dan tanpa anisotropi tidak punya sumbu untuk
    // diarahkan.
    CHECK_FALSE(SurfacePinApplies("tangent", MaterialDomain::Decal));
    CHECK_FALSE(SurfacePinApplies("coatNormal", MaterialDomain::Decal));
    CHECK(SurfacePinApplies("coatNormal", MaterialDomain::Opaque));
}

TEST_CASE("Bingkai geometri dibangun hanya untuk material yang memintanya") {
    MaterialGraph graph;
    MaterialNode output;
    output.guid = Uuid::Generate();
    output.type = std::string(kSurfaceOutputType);
    graph.nodes.push_back(output);

    MaterialCompileOptions options;
    options.moduleName = "uji.simmat";

    // **Ditanya "dikemudikan atau tidak", bukan "mungkin bukan nol".** Nilai
    // bawaan pin ini sumbu identitas, jadi membandingkannya dengan nol tidak
    // menjawab apa pun — dan lobe yang dinyalakan karena salah tanya akan
    // membuat coat berhenti mengikuti peta normal dasarnya tanpa satu pun galat.
    const MaterialCompileResult plain = CompileMaterial(graph, options);
    REQUIRE(plain.ok);
    CHECK_FALSE(plain.lobes.tangent);
    CHECK_FALSE(plain.lobes.coatFrame);

    graph.nodes[0].pinValues["tangent"] = "float3(0.0, 1.0, 0.0)";
    const MaterialCompileResult rotated = CompileMaterial(graph, options);
    REQUIRE(rotated.ok);
    CHECK(rotated.lobes.tangent);
    CHECK_FALSE(rotated.lobes.coatFrame);

    // Salah satu dari keduanya sudah cukup: bingkai coat dibangun utuh atau
    // tidak sama sekali.
    graph.nodes[0].pinValues["coatTangent"] = "float3(0.0, 1.0, 0.0)";
    CHECK(CompileMaterial(graph, options).lobes.coatFrame);
}

TEST_CASE("Modul forward memasang bingkai coat hanya ketika materialnya memintanya") {
    if (SlangCompilerIdentity().empty()) {
        MESSAGE("slangc tidak ditemukan — bagian integrasi dilewati");
        return;
    }
    MaterialGraph graph;
    MaterialNode output;
    output.guid = Uuid::Generate();
    output.type = std::string(kSurfaceOutputType);
    graph.nodes.push_back(output);

    MaterialCompileOptions options;
    options.moduleName = "uji.simmat";

    ForwardMaterialOptions moduleOptions;
    moduleOptions.prelude = LoadOpenPbrPrelude(SIM_SHADER_DIR);
    moduleOptions.frameDeclarations = InlineShaderIncludes(
        SIM_SHADER_DIR, {"box_varyings.slang", "cluster_common.slang", "gi_resolve.slang"});
    REQUIRE(!moduleOptions.prelude.empty());
    REQUIRE(!moduleOptions.frameDeclarations.empty());

    const MaterialCompileResult plain = CompileMaterial(graph, options);
    REQUIRE(plain.ok);
    moduleOptions.lobes = plain.lobes;
    const std::string plainModule =
        AssembleForwardMaterialModule(plain.slang, moduleOptions);
    // **Dicari pemanggilannya, bukan namanya.** Prelude ikut ditanam ke dalam
    // modul, jadi definisi kedua fungsi itu selalu ada di sana — yang menandakan
    // materialnya benar-benar membayarnya adalah baris yang memanggilnya.
    CHECK(plainModule.find("ShadingFrame coatFrame = makeCoatFrame") == std::string::npos);
    CHECK(plainModule.find("frame.tangent = rotatedTangent") == std::string::npos);
    CHECK(plainModule.find("evaluateOpenPBR(m.surface, frame, light.direction") !=
          std::string::npos);

    graph.nodes[0].pinValues["coatNormal"] = "float3(0.0, 0.2, 1.0)";
    graph.nodes[0].pinValues["tangent"] = "float3(0.0, 1.0, 0.0)";
    const MaterialCompileResult driven = CompileMaterial(graph, options);
    REQUIRE(driven.ok);
    moduleOptions.lobes = driven.lobes;
    const std::string drivenModule =
        AssembleForwardMaterialModule(driven.slang, moduleOptions);
    CHECK(drivenModule.find("ShadingFrame coatFrame = makeCoatFrame") != std::string::npos);
    CHECK(drivenModule.find("frame.tangent = rotatedTangent") != std::string::npos);
    CHECK(drivenModule.find("evaluateOpenPBR(m.surface, frame, coatFrame") != std::string::npos);

    // Dan yang dihasilkan benar-benar bisa dikompilasi, bukan sekadar
    // mengandung teks yang benar.
    ShaderCache cache;
    cache.Configure({}, "x");
    cache.SetCompiler(MakeSlangCompiler());
    CompileRequest request;
    request.stage = ShaderStage::Fragment;
    // Entry point modul forward bernama `main`, bukan `fragmentMain`.
    request.entryPoint = "main";
    request.source = drivenModule;
    const CompileOutput out = cache.Get(request);
    INFO("slangc: ", out.error);
    CHECK(out.ok);
}

TEST_CASE("openpbr.slang bisa dikompilasi pada keenam belas kombinasi lapisan") {
    if (SlangCompilerIdentity().empty()) {
        MESSAGE("slangc tidak ditemukan — bagian integrasi dilewati");
        return;
    }
    const std::string prelude = LoadOpenPbrPrelude(SIM_SHADER_DIR);
    REQUIRE(!prelude.empty());

    // **Lapisan yang dimatikan adalah kode yang tidak ada, bukan cabang yang
    // tidak diambil** — dan itulah yang membuat kombinasinya perlu diuji satu
    // per satu. Sebuah nama yang hanya dideklarasikan di dalam `#if` yang menyala
    // akan tetap lolos pada material yang memakai seluruh lapisan, dan baru
    // gagal pada material pertama yang mematikan salah satunya. Itu bentuk
    // kegagalan yang menunjuk ke material, padahal salahnya di sini.
    ShaderCache cache;
    cache.Configure({}, "x");
    cache.SetCompiler(MakeSlangCompiler());

    for (int mask = 0; mask < 16; ++mask) {
        const int coat = (mask >> 0) & 1;
        const int fuzz = (mask >> 1) & 1;
        const int aniso = (mask >> 2) & 1;
        const int diffuseRoughness = (mask >> 3) & 1;

        std::string defines;
        defines += "#define OPENPBR_HAS_COAT " + std::to_string(coat) + "\n";
        defines += "#define OPENPBR_HAS_FUZZ " + std::to_string(fuzz) + "\n";
        defines += "#define OPENPBR_HAS_ANISOTROPY " + std::to_string(aniso) + "\n";
        defines += "#define OPENPBR_HAS_DIFFUSE_ROUGHNESS " +
                   std::to_string(diffuseRoughness) + "\n";

        CompileRequest request;
        request.stage = ShaderStage::Fragment;
        request.entryPoint = "probeMain";
        request.source = defines + prelude + R"(
[shader("fragment")]
float4 probeMain(float3 normal : NORMAL, float3 view : TEXCOORD0) : SV_Target {
    OpenPBRSurface surface = OpenPBRSurface::defaults();
    surface.baseMetalness = 1.0;
    surface.baseWeight = 0.75;
    surface.specularColor = float3(0.6, 0.7, 0.9);
    surface.specularRoughnessAnisotropy = 0.8;
    surface.baseDiffuseRoughness = 0.5;
    surface.coatWeight = 0.5;
    surface.fuzzWeight = 0.25;
    ShadingFrame frame;
    frame.normal = normalize(normal);
    frame.view = normalize(view);
    frame.tangent = float3(1, 0, 0);
    frame.bitangent = float3(0, 1, 0);
    float3 direct = evaluateOpenPBR(surface, frame, float3(0, 1, 0), float3(1));
    float3 ambient = evaluateOpenPBR_IBL(surface, frame, float3(0.4), float3(0.6),
                                         float3(0.7), float2(0.9, 0.02));
    // Ketiganya dipanggil langsung supaya tanda tangannya ikut terkunci: yang
    // hanya dipakai di dalam `#if` yang mati tidak akan diperiksa slangc.
    float3 edge = fresnelF82(float3(0.9), surface.specularColor, 0.25);
    float ratio = specularIorRatio(surface);
    float ax, ay;
    anisotropicAlpha(surface.specularRoughness, surface.specularRoughnessAnisotropy, ax, ay);
    float ca, cb;
    orenNayarCoefficients(surface.baseDiffuseRoughness, ca, cb);
    ShadingFrame coatFrame = makeCoatFrame(frame, float3(0.1, 0.0, 1.0), float3(0, 1, 0));
    float3 probe = float3(rotatedTangent(frame, float3(0, 1, 0)).x) +
                   evaluateOpenPBR(surface, frame, coatFrame, float3(0, 1, 0), float3(1)) +
                   coatUnderlayer(surface, 0.05) +
                   diffuseAlbedo(surface, 0.6) * orenNayarAlbedo(ca, cb, 0.6) +
                   float3(fresnelAverageAlbedo(surface.coatIor) *
                          zeltnerSheenAlbedo(0.6, 0.4) *
                          zeltnerSheenLobe(float3(0, 1, 0), frame, 0.6, 0.4));
    return float4(direct + ambient + edge * ratio * (ax + ay) + probe, 1);
}
)";
        INFO("coat=", coat, " fuzz=", fuzz, " aniso=", aniso, " diffuseRoughness=",
             diffuseRoughness);
        const CompileOutput out = cache.Get(request);
        INFO("slangc: ", out.error);
        CHECK(out.ok);
    }
}

TEST_CASE("Nilai bawaan OpenPBRSurface sama dengan nilai bawaan pin") {
    // Satu keputusan yang tertulis di dua tempat: katalog node dan
    // openpbr.slang. Yang membuatnya bertahan bukan kedisiplinan melainkan test
    // ini — pin yang bawaannya bergeser tanpa shader-nya ikut menghasilkan
    // material yang berubah rupa hanya ketika pin-nya dilepas.
    const std::string prelude = LoadOpenPbrPrelude(SIM_SHADER_DIR);
    REQUIRE(!prelude.empty());
    const MaterialNodeType* output = MaterialNodeCatalog::Get().Find(kSurfaceOutputType);
    REQUIRE(output != nullptr);

    for (const MaterialPin& pin : output->pins) {
        if (pin.direction != PinDirection::Input || pin.defaultValue.empty()) {
            continue;
        }
        // Pin di luar OpenPBRSurface tidak punya padanan di struct-nya.
        // Daftarnya dibaca dari katalog, bukan disalin ke sini — menyalinnya
        // berarti pin geometri berikutnya lolos dari uji ini tanpa suara.
        if (SurfacePinIsExtra(pin.name)) {
            continue;
        }
        const std::string assignment = "s." + pin.name + " = " + pin.defaultValue + ";";
        INFO("pin ", pin.name);
        CHECK(prelude.find(assignment) != std::string::npos);
    }
}

// --- Tahap vertex ------------------------------------------------------------

TEST_CASE("Modul memuat kedua entry point dari satu sumber") {
    const MaterialCompileResult compiled = CompileMaterial(MinimalGraph());
    REQUIRE(compiled.ok);
    MaterialModuleOptions options;
    options.prelude = "// prelude\n";

    const CompileRequest vertex =
        MakeMaterialRequest(compiled.slang, ShaderStage::Vertex, options);
    const CompileRequest fragment =
        MakeMaterialRequest(compiled.slang, ShaderStage::Fragment, options);

    // Sumbernya sama persis — struct varying-nya tertulis sekali, jadi tidak ada
    // cara kedua tahap memakai bentuk yang berbeda.
    CHECK(vertex.source == fragment.source);
    CHECK(vertex.entryPoint == "vertexMain");
    CHECK(fragment.entryPoint == "fragmentMain");
    CHECK(vertex.source.find("[shader(\"vertex\")]") != std::string::npos);
    CHECK(vertex.source.find("[shader(\"fragment\")]") != std::string::npos);

    // Tapi kuncinya berbeda: SPIR-V yang dihasilkan memang tidak sama.
    ShaderCache cache;
    cache.Configure({}, "x");
    CHECK(cache.KeyOf(vertex) != cache.KeyOf(fragment));
}

TEST_CASE("Binding material ditulis eksplisit, bukan dinomori otomatis") {
    MaterialGraph graph;
    graph.nodes.push_back(Node(1, std::string(kSurfaceOutputType)));
    graph.nodes.push_back(Node(2, "input.texture"));
    graph.nodes.back().settings["texture"] = Id(900).ToString();
    graph.nodes.push_back(Node(3, "input.sample"));
    Link(graph, 2, "texture", 3, "texture");
    Link(graph, 3, "rgb", 1, "baseColor");

    graph.nodes.push_back(Node(4, "input.texture"));
    graph.nodes.back().settings["texture"] = Id(901).ToString();
    graph.nodes.push_back(Node(5, "input.sample"));
    Link(graph, 4, "texture", 5, "texture");
    Link(graph, 5, "rgb", 1, "normal");

    MaterialParameter tint;
    tint.name = "tint";
    tint.kind = ValueKind::Float3;
    tint.defaultValue = "float3(1.0)";
    graph.parameters.push_back(tint);

    const MaterialCompileResult compiled = CompileMaterial(graph);
    REQUIRE(compiled.ok);
    REQUIRE(compiled.textures.size() == 2);

    CHECK(compiled.slang.find("[[vk::binding(0, 2)]]\ncbuffer MaterialParams") !=
          std::string::npos);
    // Tekstur dan sampler berselang-seling: tekstur ke-i selalu di 1 + 2i,
    // berapa pun banyaknya seluruhnya.
    for (int i = 0; i < 2; ++i) {
        const std::string texture = "[[vk::binding(" +
                                    std::to_string(MaterialBindings::TextureBinding(
                                        static_cast<uint32_t>(i))) +
                                    ", 2)]]\nTexture2D<float4> " + compiled.textures[i].name;
        const std::string sampler = "[[vk::binding(" +
                                    std::to_string(MaterialBindings::SamplerBinding(
                                        static_cast<uint32_t>(i))) +
                                    ", 2)]]\nSamplerState s" +
                                    compiled.textures[i].name.substr(1);
        INFO("tekstur ke-", i);
        CHECK(compiled.slang.find(texture) != std::string::npos);
        CHECK(compiled.slang.find(sampler) != std::string::npos);
    }

    // Parameter yang tidak dibaca graph tetap ditulis ke cbuffer. Menyaringnya
    // akan membuat offset sisi C++ dan sisi Slang berbeda tepat pada material
    // yang sedang disunting.
    CHECK(compiled.slang.find("float3 tint;") != std::string::npos);
}

TEST_CASE("Argumen slangc ikut identitas kompilator") {
    const std::string identity = SlangCompilerIdentity();
    if (identity.empty()) {
        MESSAGE("slangc tidak ditemukan — bagian integrasi dilewati");
        return;
    }
    // Mengubah satu flag mengubah SPIR-V dari sumber yang sama persis. Kalau
    // flag tidak ikut kunci, seluruh entri lama tetap tampak sah — dan
    // `-matrix-layout-column-major` yang hilang berarti setiap matriks
    // tertranspose, yaitu mesh yang terpelintir tanpa satu pun pesan galat.
    CHECK(identity.find(std::string(SlangArguments())) != std::string::npos);
    CHECK(SlangArguments().find("-matrix-layout-column-major") != std::string_view::npos);
}

TEST_CASE("Tahap vertex material sungguhan berjalan sampai SPIR-V") {
    const std::string identity = SlangCompilerIdentity();
    if (identity.empty()) {
        MESSAGE("slangc tidak ditemukan — bagian integrasi dilewati");
        return;
    }
    MaterialModuleOptions options;
    options.prelude = LoadOpenPbrPrelude(SIM_SHADER_DIR);
    REQUIRE(!options.prelude.empty());

    const MaterialCompileResult compiled = CompileMaterial(MinimalGraph());
    REQUIRE(compiled.ok);

    TempCacheDir dir("vertex");
    ShaderCache cache;
    cache.Configure(dir.path, identity);
    cache.SetCompiler(MakeSlangCompiler());

    const CompileOutput vertex =
        cache.Get(MakeMaterialRequest(compiled.slang, ShaderStage::Vertex, options));
    INFO("slangc vertex: ", vertex.error);
    REQUIRE(vertex.ok);
    CHECK(LooksLikeSpirv(vertex.spirv));

    const CompileOutput fragment =
        cache.Get(MakeMaterialRequest(compiled.slang, ShaderStage::Fragment, options));
    INFO("slangc fragment: ", fragment.error);
    REQUIRE(fragment.ok);

    // Sumber yang sama, dua modul yang berbeda — dan dua entri cache.
    CHECK(vertex.spirv != fragment.spirv);
    CHECK(cache.Statistics().compiles == 2);
    CHECK(cache.Statistics().hits == 0);
}

TEST_CASE("Material bertekstur dan berparameter berjalan di kedua tahap") {
    const std::string identity = SlangCompilerIdentity();
    if (identity.empty()) {
        MESSAGE("slangc tidak ditemukan — bagian integrasi dilewati");
        return;
    }
    MaterialModuleOptions options;
    options.prelude = LoadOpenPbrPrelude(SIM_SHADER_DIR);
    REQUIRE(!options.prelude.empty());

    MaterialGraph graph;
    graph.nodes.push_back(Node(1, std::string(kSurfaceOutputType)));
    graph.nodes.push_back(Node(2, "input.texture"));
    graph.nodes.back().settings["texture"] = Id(900).ToString();
    graph.nodes.push_back(Node(3, "input.sample"));
    Link(graph, 2, "texture", 3, "texture");
    Link(graph, 3, "rgb", 1, "baseColor");
    graph.nodes.push_back(Node(4, "param.get"));
    graph.nodes.back().settings["parameter"] = "roughness";
    MaterialParameter roughness;
    roughness.name = "roughness";
    roughness.kind = ValueKind::Float;
    roughness.defaultValue = "0.4";
    graph.parameters.push_back(roughness);
    Link(graph, 4, "value", 1, "specularRoughness");

    const MaterialCompileResult compiled = CompileMaterial(graph);
    REQUIRE(compiled.ok);

    ShaderCache cache;
    cache.Configure({}, identity);
    cache.SetCompiler(MakeSlangCompiler());
    for (const ShaderStage stage : {ShaderStage::Vertex, ShaderStage::Fragment}) {
        const CompileOutput out =
            cache.Get(MakeMaterialRequest(compiled.slang, stage, options));
        INFO("tahap ", ToString(stage), ": ", out.error);
        CHECK(out.ok);
    }
}

TEST_CASE("Semua atribut vertex punya lokasi eksplisit") {
    const MaterialCompileResult compiled = CompileMaterial(MinimalGraph());
    REQUIRE(compiled.ok);
    const std::string module = AssembleMaterialModule(compiled.slang, {});

    // Tanpa lokasi eksplisit, atribut dinomori menurut urutan yang *bertahan* —
    // dan dengan kSkinned mati kedua atribut tulang bisa hilang, menggeser
    // seluruh atribut sesudahnya sementara sisi C++ tidak ikut bergeser.
    const std::pair<uint32_t, const char*> expected[] = {
        {MaterialVertexLocation::kPosition, "position"},
        {MaterialVertexLocation::kNormal, "normal"},
        {MaterialVertexLocation::kTangent, "tangent"},
        {MaterialVertexLocation::kUv0, "uv0"},
        {MaterialVertexLocation::kColor, "color"},
        {MaterialVertexLocation::kBoneIndices, "boneIndices"},
        {MaterialVertexLocation::kBoneWeights, "boneWeights"},
    };
    for (const auto& [location, name] : expected) {
        const std::string marker = "[[vk::location(" + std::to_string(location) + ")]]";
        const size_t at = module.find(marker);
        INFO("atribut ", name);
        REQUIRE(at != std::string::npos);
        CHECK(module.find(name, at) < module.find('\n', at));
    }
}

TEST_CASE("Skinning menormalisasi bobot dan mengabaikan translasi pada arah") {
    const MaterialCompileResult compiled = CompileMaterial(MinimalGraph());
    REQUIRE(compiled.ok);
    const std::string module = AssembleMaterialModule(compiled.slang, {});

    // Bobot yang jumlahnya 0.98 karena dikuantisasi menyusutkan mesh 2% secara
    // merata — cacat yang terbaca sebagai "model ini agak kecil", bukan sebagai
    // bug skinning.
    CHECK(module.find("v.boneWeights / total") != std::string::npos);
    // Posisi memakai matriks penuh, arah hanya bagian 3x3-nya.
    CHECK(module.find("mul(bone, float4(position, 1.0))") != std::string::npos);
    CHECK(module.find("mul((float3x3)bone, normal)") != std::string::npos);
    CHECK(module.find("mul((float3x3)bone, tangent)") != std::string::npos);
}

// --- material bawaan editor ----------------------------------------------------

TEST_CASE("Setiap material bawaan editor terbaca, sah, dan menghasilkan berkas yang sama") {
    // **Isi bawaan yang tidak bisa dimuat lebih buruk daripada tidak ada isi
    // bawaan sama sekali**: ia muncul di Asset Browser, disalin orang sebagai
    // titik awal, lalu gagal justru sesudah dipakai. Ujinya membaca berkas yang
    // benar-benar dikirim editor, bukan salinan di folder build.
    const std::filesystem::path folder = std::filesystem::path(SIM_BUILTIN_DIR) / "Materials";
    REQUIRE_MESSAGE(std::filesystem::is_directory(folder),
                    "folder material bawaan tidak ada di " SIM_BUILTIN_DIR);

    // **Aturan "tepat satu node" hanya berlaku di folder teratas**, dan
    // subfoldernya memang dikecualikan: yang di bawah bukan titik awal untuk
    // disalin melainkan material yang punya pekerjaan sendiri.
    // `Materials/Sistem/` berisi induk bersama — `Material Impor.simmat`,
    // tempat material hasil impor mengikat diri, yang memang bercabang satu
    // `param.get` per parameter. `Materials/Prefab/` berisi material milik
    // prefab bawaan, yang menyampel teksturnya sendiri.
    //
    // Yang **tetap** berlaku untuk semuanya: terbaca, sah, dan bolak-balik tanpa
    // perubahan. Itu diperiksa lintasan kedua di bawah.
    int checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".simmat") {
            continue;
        }
        INFO("material bawaan " << entry.path().filename().string());
        MaterialGraph graph;
        const MaterialIoResult loaded = LoadMaterialFromFile(graph, entry.path());
        CHECK(loaded.ok);
        CHECK(loaded.error.empty());

        // Tepat satu output surface: material bawaan adalah titik awal, dan
        // titik awal yang sudah bercabang bukan titik awal lagi.
        REQUIRE(graph.nodes.size() == 1);
        CHECK(graph.nodes[0].type == kSurfaceOutputType);
        CHECK_FALSE(graph.nodes[0].pinValues.empty());

        const ValidationResult validation = ValidateMaterial(graph);
        for (const MaterialIssue& issue : validation.errors) {
            INFO("galat validasi: " << issue.message);
            CHECK(false);
        }
        CHECK(validation.ok);

        // Bolak-balik tanpa perubahan menghasilkan teks yang sama persis. Tanpa
        // sifat ini, membuka lalu menyimpan sebuah material bawaan menghasilkan
        // diff yang tidak mengubah apa pun — dan diff semacam itu yang membuat
        // orang berhenti membaca diff.
        MaterialGraph again;
        REQUIRE(LoadMaterialFromString(again, SaveMaterialToString(graph)).ok);
        CHECK(SaveMaterialToString(again) == SaveMaterialToString(graph));
        ++checked;
    }
    CHECK(checked >= 4);

    // Lintasan kedua: seluruh material bawaan, termasuk yang di subfolder.
    // Aturan percabangannya tidak berlaku, sisanya berlaku — material bawaan
    // yang tidak bisa dimuat tetap lebih buruk daripada tidak ada, di folder
    // mana pun ia tinggal.
    int nested = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(folder)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".simmat") {
            continue;
        }
        INFO("material bawaan " << entry.path().string());
        MaterialGraph graph;
        REQUIRE(LoadMaterialFromFile(graph, entry.path()).ok);
        const ValidationResult validation = ValidateMaterial(graph);
        for (const MaterialIssue& issue : validation.errors) {
            INFO("galat validasi: " << issue.message);
            CHECK(false);
        }
        CHECK(validation.ok);
        MaterialGraph again;
        REQUIRE(LoadMaterialFromString(again, SaveMaterialToString(graph)).ok);
        CHECK(SaveMaterialToString(again) == SaveMaterialToString(graph));
        ++nested;
    }
    // Lebih banyak daripada yang di folder teratas: subfoldernya memang berisi.
    CHECK(nested > checked);
}

TEST_CASE("Jalur A: instance menyimpan tekstur parameternya, dan itu bertahan lewat berkas") {
    // **Terpisah dari `ParameterOverride`, dan bukan karena kerapian.**
    // `MaterialValue` adalah empat float; sebuah GUID juga enam belas byte, jadi
    // menyelundupkannya lewat medan yang sama akan kompilasi dengan mulus dan
    // menghasilkan tekstur yang berganti setiap kali ada yang menormalkan
    // nilainya atau menuliskannya sebagai `float4(...)`.
    MaterialInstance instance;
    instance.parent = Uuid::Generate();
    instance.Set("baseColor", ParseValue(ValueKind::Float3, "float3(0.8)"));

    const Uuid albedo = Uuid::Generate();
    instance.SetTexture("Albedo", albedo);
    CHECK(instance.Texture("Albedo") == albedo);
    CHECK_FALSE(instance.Texture("Normal").IsValid());

    const std::string text = SaveInstanceToString(instance);
    MaterialInstance loaded;
    INFO(text);
    REQUIRE(LoadInstanceFromString(loaded, text).ok);
    CHECK(loaded.Texture("Albedo") == albedo);
    CHECK(loaded.overrides.size() == 1);
    // Bolak-balik menghasilkan berkas yang sama: yang berubah urutannya tiap
    // simpan membuat setiap `.simmatinst` tampak termodifikasi di git.
    CHECK(SaveInstanceToString(loaded) == text);

    // Memasang ulang mengganti, bukan menumpuk.
    const Uuid other = Uuid::Generate();
    instance.SetTexture("Albedo", other);
    CHECK(instance.textures.size() == 1);
    CHECK(instance.Texture("Albedo") == other);

    // GUID tak sah membuang pemasangannya: satu jalan untuk "tidak ada
    // tekstur", bukan dua yang harus dibedakan setiap pembaca.
    instance.SetTexture("Albedo", Uuid{});
    CHECK(instance.textures.empty());
    CHECK_FALSE(instance.Texture("Albedo").IsValid());

    // Instance tanpa tekstur tidak menulis kunci "textures" sama sekali —
    // berkas dari sebelum tekstur ada tetap terbaca, dan yang tidak memakainya
    // tetap sekecil sebelumnya.
    CHECK(SaveInstanceToString(instance).find("textures") == std::string::npos);
}

TEST_CASE("Jalur A: instance hanya bisa mengisi tekstur yang dinyatakan induknya") {
    // **Inilah yang membedakan material instance dari menyunting induknya.**
    // Versi pertama mekanisme ini menyebut node `input.texture` lewat GUID, dan
    // itu membuat sebuah instance bisa menjangkau bagian mana pun dari graph
    // induk — termasuk tekstur yang penulis induk maksudkan tetap. Yang boleh
    // diisi harus dinyatakan, sama seperti parameter skalar.
    MaterialGraph parent;
    MaterialParameter albedo;
    albedo.name = "Albedo";
    albedo.kind = ValueKind::Texture;
    albedo.tooltip = "Warna dasar";
    const Uuid parentDefault = Uuid::Generate();
    albedo.defaultValue = parentDefault.ToString();
    parent.parameters.push_back(albedo);

    MaterialParameter tint;
    tint.name = "Tint";
    tint.kind = ValueKind::Float3;
    tint.defaultValue = "float3(1.0)";
    parent.parameters.push_back(tint);

    MaterialInstance instance;
    instance.parent = Uuid::Generate();

    // Tanpa timpaan: yang berlaku bawaan induknya.
    std::vector<ResolvedTexture> resolved = ResolveTextures(parent, instance);
    REQUIRE(resolved.size() == 1);  // hanya yang bertipe Texture
    CHECK(resolved[0].name == "Albedo");
    CHECK(resolved[0].texture == parentDefault);
    CHECK_FALSE(resolved[0].overridden);

    // Dengan timpaan: instance menang, tanpa satu byte pun berubah di induk.
    const Uuid mine = Uuid::Generate();
    instance.SetTexture("Albedo", mine);
    resolved = ResolveTextures(parent, instance);
    REQUIRE(resolved.size() == 1);
    CHECK(resolved[0].texture == mine);
    CHECK(resolved[0].overridden);
    CHECK(parent.parameters[0].defaultValue == parentDefault.ToString());

    // **Timpaan untuk yang tidak dinyatakan tidak berpengaruh.** Ia tersimpan
    // — sama seperti timpaan parameter skalar yang namanya salah ketik — tetapi
    // tidak pernah muncul di hasil, karena yang ditelusuri deklarasi induknya.
    instance.SetTexture("TidakAda", Uuid::Generate());
    resolved = ResolveTextures(parent, instance);
    CHECK(resolved.size() == 1);
    CHECK(resolved[0].name == "Albedo");

    // Satu induk, dua instance, dua tekstur berbeda — dan itu justru gunanya:
    // induk menjadi template fungsi material, instance mengisi gambarnya.
    MaterialInstance second;
    second.parent = instance.parent;
    const Uuid other = Uuid::Generate();
    second.SetTexture("Albedo", other);
    CHECK(ResolveTextures(parent, second)[0].texture == other);
    CHECK(ResolveTextures(parent, instance)[0].texture == mine);
}

TEST_CASE("Material Impor punya slot tekstur, dan yang kosong tidak mengubah apa pun") {
    // **Induk ini sudah dirujuk setiap `.simmatinst` yang pernah dibangkitkan**,
    // jadi menambah rantai teksturnya harus menjaga material yang ada tergambar
    // sama persis. Bentuknya: sampel dikalikan ke `baseColor`, dan slot yang
    // kosong diikat tekstur putih 1x1 oleh renderer — nilai satuan perkalian.
    const std::filesystem::path path = std::filesystem::path(SIM_BUILTIN_DIR) / "Materials" /
                                       "Sistem" / "Material Impor.simmat";
    REQUIRE_MESSAGE(std::filesystem::exists(path), path.string());

    MaterialGraph graph;
    const MaterialIoResult loaded = LoadMaterialFromFile(graph, path);
    INFO(loaded.error);
    REQUIRE(loaded.ok);

    const ValidationResult validation = ValidateMaterial(graph);
    for (const MaterialIssue& issue : validation.errors) {
        INFO("galat validasi: " << issue.message);
        CHECK(false);
    }
    REQUIRE(validation.ok);

    // Tepat satu parameter bertipe Texture, dan namanya yang dipakai importir.
    MaterialInstance empty;
    const std::vector<ResolvedTexture> textures = ResolveTextures(graph, empty);
    REQUIRE(textures.size() == 1);
    CHECK(textures[0].name == "baseColorTexture");
    CHECK_FALSE(textures[0].texture.IsValid());  // bawaan induk: kosong

    // Kompilasinya menghasilkan tepat satu binding tekstur, dan binding itu
    // menyebut parameternya — tanpa itu, tidak ada yang bisa memasangkan gambar
    // yang dipilih instance ke slot yang benar.
    MaterialCompileOptions options;
    options.moduleName = "Material Impor.simmat";
    const MaterialCompileResult compiled = CompileMaterial(graph, options);
    for (const MaterialIssue& issue : compiled.errors) {
        INFO("galat kompilasi: " << issue.message);
        CHECK(false);
    }
    REQUIRE(compiled.ok);
    REQUIRE(compiled.textures.size() == 1);
    CHECK(compiled.textures[0].parameter == "baseColorTexture");

    // Dan sampelnya benar-benar masuk ke warna dasarnya, bukan menggantung
    // tanpa pemakai: yang menggantung akan dibuang kompiler, dan teksturnya
    // tidak pernah terlihat.
    INFO(compiled.slang);
    CHECK(compiled.slang.find(compiled.textures[0].name + ".Sample") != std::string::npos);

    // Instance mengisinya tanpa menyentuh induknya.
    MaterialInstance instance;
    const Uuid image = Uuid::Generate();
    instance.SetTexture("baseColorTexture", image);
    CHECK(ResolveTextures(graph, instance)[0].texture == image);
    CHECK(ResolveTextures(graph, empty)[0].texture.IsValid() == false);
}

TEST_CASE("Rekomendasi 1: lapisan yang tidak mungkin dipakai dimatikan saat kompilasi") {
    // **Di GPU, cabang yang diambil sebagian lane dalam satu warp membayar kedua
    // sisinya.** Jadi satu material bercoat di layar membuat tetangganya ikut
    // membayar, dan material yang coat-nya nol tetap membawa kodenya di dalam
    // SPIR-V. Yang dimatikan saat kompilasi bukan cabang yang tidak diambil
    // melainkan kode yang tidak pernah ada.
    MaterialGraph graph;
    MaterialNode output;
    output.guid = Uuid::Generate();
    output.type = std::string(kSurfaceOutputType);
    graph.nodes.push_back(output);

    MaterialCompileOptions options;
    options.moduleName = "uji.simmat";

    // Material polos: keempat lapisan tambahan bawaannya nol.
    const MaterialCompileResult plain = CompileMaterial(graph, options);
    REQUIRE(plain.ok);
    CHECK_FALSE(plain.lobes.coat);
    CHECK_FALSE(plain.lobes.fuzz);
    CHECK_FALSE(plain.lobes.anisotropy);
    CHECK_FALSE(plain.lobes.diffuseRoughness);

    // Literal nol yang ditulis eksplisit tetap dibaca nol.
    graph.nodes[0].pinValues["coatWeight"] = "0.0";
    CHECK_FALSE(CompileMaterial(graph, options).lobes.coat);

    // Literal bukan nol menyalakannya.
    graph.nodes[0].pinValues["coatWeight"] = "1.0";
    CHECK(CompileMaterial(graph, options).lobes.coat);
    graph.nodes[0].pinValues["fuzzWeight"] = "0.25";
    CHECK(CompileMaterial(graph, options).lobes.fuzz);

    // **Satu sakelar untuk kedua anisotropi**: `anisotropicAlpha` dipakai lobe
    // spekular maupun coat, jadi mematikannya menuntut kedua-duanya isotropik.
    graph.nodes[0].pinValues["coatRoughnessAnisotropy"] = "0.5";
    CHECK(CompileMaterial(graph, options).lobes.anisotropy);
    graph.nodes[0].pinValues["coatRoughnessAnisotropy"] = "0.0";
    CHECK_FALSE(CompileMaterial(graph, options).lobes.anisotropy);
    graph.nodes[0].pinValues["specularRoughnessAnisotropy"] = "0.3";
    CHECK(CompileMaterial(graph, options).lobes.anisotropy);

    // Literal yang tidak terbaca sebagai angka dijawab "mungkin": tebakan salah
    // ke arah ini hanya membuat material membayar lobe yang tidak dipakainya,
    // sedangkan ke arah sebaliknya ia menghilangkan lapisan tanpa satu pun
    // galat.
    MaterialGraph odd = graph;
    odd.nodes[0].pinValues.clear();
    odd.nodes[0].pinValues["coatWeight"] = "gWeirdGlobal";
    CHECK(CompileMaterial(odd, options).lobes.coat);
}

TEST_CASE("Rekomendasi 1: modul yang dirakit menyebut lapisannya sebagai konstanta") {
    // Yang menentukan biaya bukan bendera di dalam `MaterialCompileResult`
    // melainkan `#define` yang benar-benar sampai ke `slangc`.
    MaterialGraph graph;
    MaterialNode output;
    output.guid = Uuid::Generate();
    output.type = std::string(kSurfaceOutputType);
    graph.nodes.push_back(output);

    MaterialCompileOptions compileOptions;
    compileOptions.moduleName = "uji.simmat";
    const MaterialCompileResult compiled = CompileMaterial(graph, compileOptions);
    REQUIRE(compiled.ok);

    MaterialModuleOptions options;
    options.prelude = "// prelude tiruan\n";
    options.lobes = compiled.lobes;
    const std::string module = AssembleMaterialModule(compiled.slang, options);

    INFO(module.substr(0, 800));
    CHECK(module.find("#define OPENPBR_HAS_COAT 0") != std::string::npos);
    CHECK(module.find("#define OPENPBR_HAS_FUZZ 0") != std::string::npos);
    CHECK(module.find("#define OPENPBR_HAS_ANISOTROPY 0") != std::string::npos);
    CHECK(module.find("#define OPENPBR_HAS_DIFFUSE_ROUGHNESS 0") != std::string::npos);

    // **Sebelum prelude-nya**, kalau tidak `#ifndef` di dalam openpbr.slang
    // sudah terlanjur memasang nilai bawaannya dan yang di sini tidak berlaku.
    CHECK(module.find("#define OPENPBR_HAS_COAT") < module.find("// prelude tiruan"));

    // Dan material bercoat menyalakannya.
    graph.nodes[0].pinValues["coatWeight"] = "1.0";
    const MaterialCompileResult coated = CompileMaterial(graph, compileOptions);
    options.lobes = coated.lobes;
    CHECK(AssembleMaterialModule(coated.slang, options).find("#define OPENPBR_HAS_COAT 1") !=
          std::string::npos);
}

TEST_CASE("Rekomendasi 1: material polos menghasilkan SPIR-V yang lebih kecil") {
    // **Inilah pembuktiannya, dan bukan yang lain.** Bendera di dalam
    // `MaterialCompileResult` dan `#define` di dalam teks modul keduanya bisa
    // benar sementara `slangc` tetap menghasilkan instruksi yang sama. Yang
    // menentukan biaya adalah SPIR-V-nya.
    const std::string identity = SlangCompilerIdentity();
    if (identity.empty()) {
        MESSAGE("slangc tidak ditemukan — bagian integrasi dilewati");
        return;
    }

    MaterialModuleOptions moduleOptions;
    moduleOptions.prelude = LoadOpenPbrPrelude(std::filesystem::path(SIM_SHADER_DIR));
    REQUIRE_FALSE(moduleOptions.prelude.empty());

    MaterialGraph graph;
    MaterialNode output;
    output.guid = Uuid::Generate();
    output.type = std::string(kSurfaceOutputType);
    graph.nodes.push_back(output);

    MaterialCompileOptions compileOptions;
    compileOptions.moduleName = "uji.simmat";
    const MaterialCompileResult compiled = CompileMaterial(graph, compileOptions);
    REQUIRE(compiled.ok);
    REQUIRE_FALSE(compiled.lobes.coat);

    TempCacheDir dir("lobes");
    ShaderCache cache;
    cache.Configure(dir.path, identity);
    cache.SetCompiler(MakeSlangCompiler());

    const auto compileWith = [&](const SurfaceLobes& lobes) {
        MaterialModuleOptions options = moduleOptions;
        options.lobes = lobes;
        const CompileRequest request =
            MakeMaterialRequest(compiled.slang, ShaderStage::Fragment, options);
        const CompileOutput out = cache.Get(request);
        INFO("slangc: " << out.error);
        REQUIRE(out.ok);
        return out.spirv.size();
    };

    // Material yang sama, dua kali: sekali dengan lapisan yang benar-benar
    // dipakainya, sekali dengan seluruh lapisan dinyalakan.
    const std::size_t lean = compileWith(compiled.lobes);
    SurfaceLobes everything;  // bawaannya menyalakan semuanya
    const std::size_t full = compileWith(everything);

    MESSAGE("SPIR-V: " << lean << " byte tanpa lapisan tambahan, " << full
                       << " byte dengan semuanya");
    CHECK(lean < full);
}

// ---------------------------------------------------------------------------
// E8.4 #4 — shader material untuk pass forward renderer
// ---------------------------------------------------------------------------

TEST_CASE("E8.4: deklarasi renderer ditanam beserta include-nya, sekali masing-masing") {
    // **Ditanam, bukan diserahkan ke `-I`-nya slangc.** Kunci cache adalah hash
    // teks sumber; sumber yang cuma menulis `#include "cluster_common.slang"`
    // menghasilkan kunci yang tidak berubah ketika berkas itu berubah, dan cache
    // akan menyerahkan SPIR-V yang dibangun terhadap deklarasi yang sudah tidak
    // ada lagi.
    const std::string text = InlineShaderIncludes(
        SIM_SHADER_DIR, {"box_varyings.slang", "cluster_common.slang", "gi_resolve.slang"});
    REQUIRE_MESSAGE(!text.empty(), "berkas shader tidak terbaca dari " SIM_SHADER_DIR);

    // Tidak ada satu pun `#include` yang tersisa: yang tersisa adalah yang tidak
    // ikut ditanam, dan slangc akan mencarinya di direktori yang tidak disebut
    // siapa pun.
    CHECK(text.find("#include \"") == std::string::npos);

    // Isinya benar-benar sampai — ketiga akar dan yang mereka bawa.
    CHECK(text.find("struct BoxVarying") != std::string::npos);
    CHECK(text.find("ClusterLightSample") != std::string::npos);
    CHECK(text.find("giIrradianceAt") != std::string::npos);
    // `shadow_common.slang` ikut lewat `cluster_common.slang`, tanpa disebut.
    CHECK(text.find("shadowParams") != std::string::npos);

    // Dan **sekali saja masing-masing**. Yang dihitung `shadow_common.slang`,
    // bukan sembarang berkas: ia satu-satunya yang ditarik **dua akar yang
    // berbeda** — `cluster_common.slang` dan `gi_resolve.slang` — jadi hanya ia
    // yang bisa memperlihatkan dedupnya bekerja. Menghitung berkas yang cuma
    // punya satu penarik akan lulus juga tanpa dedup sama sekali, dan uji itu
    // tidak menjaga apa pun. (Saya menghitung yang salah lebih dulu; mutasinya
    // yang memberitahu.)
    std::size_t count = 0;
    for (std::size_t at = text.find("struct ShadowParams"); at != std::string::npos;
         at = text.find("struct ShadowParams", at + 1)) {
        ++count;
    }
    CHECK(count == 1);
}

TEST_CASE("E8.4: shader material pass forward dikompilasi slangc sungguhan") {
    const std::string identity = SlangCompilerIdentity();
    if (identity.empty()) {
        MESSAGE("slangc tidak ditemukan — bagian integrasi dilewati");
        return;
    }

    ForwardMaterialOptions options;
    options.prelude = LoadOpenPbrPrelude(SIM_SHADER_DIR);
    options.frameDeclarations = InlineShaderIncludes(
        SIM_SHADER_DIR, {"box_varyings.slang", "cluster_common.slang", "gi_resolve.slang"});
    REQUIRE(!options.prelude.empty());
    REQUIRE(!options.frameDeclarations.empty());

    // Material yang menyentuh normal map dan logam sekaligus: keduanya justru
    // yang tidak bisa diperlihatkan `box.frag`, dan keduanya yang menuntut
    // tangent serta model shading yang sungguhan.
    MaterialGraph graph = MinimalGraph();
    graph.nodes.push_back(Node(3, "input.constant"));
    graph.nodes.back().settings["kind"] = "float";
    graph.nodes.back().settings["value"] = "0.35";
    Link(graph, 3, "value", 1, "specularRoughness");
    graph.nodes.push_back(Node(4, "input.constant"));
    graph.nodes.back().settings["kind"] = "float";
    graph.nodes.back().settings["value"] = "1.0";
    Link(graph, 4, "value", 1, "baseMetalness");

    const MaterialCompileResult compiled = CompileMaterial(graph);
    REQUIRE(compiled.ok);
    options.lobes = compiled.lobes;

    TempCacheDir dir("forward");
    ShaderCache cache;
    cache.Configure(dir.path, identity);
    cache.SetCompiler(MakeSlangCompiler());

    const CompileOutput out = cache.Get(MakeForwardMaterialRequest(compiled.slang, options));
    INFO("slangc: ", out.error);
    REQUIRE(out.ok);
    CHECK(LooksLikeSpirv(out.spirv));

    // **Inilah yang membuktikan set 0-nya set 0 renderer.** Modul yang
    // menuliskan `FrameParams` sendiri tetap dikompilasi dengan sukses; yang
    // membedakan adalah ia tidak akan pernah bisa membaca bayangan maupun lampu
    // cluster. Sumbernya diperiksa langsung karena itu.
    const std::string module = AssembleForwardMaterialModule(compiled.slang, options);
    CHECK(module.find("sampleShadow(") != std::string::npos);
    CHECK(module.find("clusterLightAt(") != std::string::npos);
    CHECK(module.find("giIrradianceAt(") != std::string::npos);
    CHECK(module.find("evaluateOpenPBR(") != std::string::npos);
    // Dan ia memakai varying kotak, bukan varying miliknya sendiri — itu yang
    // membuat `box.vert` tetap menjadi tahap vertexnya.
    CHECK(module.find("float4 main(BoxVarying input)") != std::string::npos);
    CHECK(module.find("struct MaterialVarying") == std::string::npos);
}

TEST_CASE("Editor: tekstur yang dijatuhkan menghasilkan pasangan yang bisa mencapai Base Color") {
    // **Bentuk yang dihasilkan Material Editor saat sebuah tekstur dijatuhkan
    // ke kanvasnya**, disusun di sini supaya aturannya dijaga uji alih-alih
    // diingat.
    //
    // Yang dijaga: keluaran `input.texture` bertipe `Texture` — sebuah binding,
    // bukan warna — jadi ia **tidak bisa** disambungkan ke `baseColor`. Yang
    // bisa adalah keluaran `rgb` milik `Sample Texture` di antaranya. Menjatuhkan
    // node tekstur sendirian karena itu menghasilkan sesuatu yang tampak seperti
    // kabelnya menolak, padahal yang kurang adalah node di tengah.
    MaterialGraph graph;
    graph.nodes.push_back(Node(1, std::string(kSurfaceOutputType)));
    graph.nodes.push_back(Node(2, "input.texture"));
    graph.nodes.back().settings["name"] = "Albedo";
    graph.nodes.back().settings["texture"] = Id(99).ToString();
    graph.nodes.push_back(Node(3, "input.sample"));
    Link(graph, 2, "texture", 3, "texture");
    Link(graph, 3, "rgb", 1, "baseColor");

    const ValidationResult validation = ValidateMaterial(graph);
    INFO(FirstError(validation));
    CHECK(validation.errors.empty());

    const MaterialCompileResult compiled = CompileMaterial(graph);
    REQUIRE(compiled.ok);
    // Teksturnya sampai ke daftar binding, beserta GUID-nya — itu yang dibaca
    // renderer saat mengikat set materialnya.
    REQUIRE(compiled.textures.size() == 1);
    CHECK(compiled.textures.front().name == "tAlbedo");
    CHECK(compiled.textures.front().texture == Id(99));
    // Dan ia benar-benar disampel di kode yang dihasilkan, bukan sekadar
    // dideklarasikan.
    CHECK(compiled.slang.find("tAlbedo.Sample") != std::string::npos);

    // Sisi sebaliknya: menyambungkan node teksturnya **langsung** ke `baseColor`
    // memang ditolak. Kalau suatu hari ia diterima, pasangan yang dibuat saat
    // menjatuhkan tekstur berhenti punya alasan — dan uji ini yang akan
    // memberitahu.
    MaterialGraph direct;
    direct.nodes.push_back(Node(1, std::string(kSurfaceOutputType)));
    direct.nodes.push_back(Node(2, "input.texture"));
    direct.nodes.back().settings["name"] = "Albedo";
    Link(direct, 2, "texture", 1, "baseColor");
    CHECK_FALSE(ValidateMaterial(direct).errors.empty());
}

TEST_CASE("E8.4: setiap shader yang ditanam modul material ikut disalin ke sebelah executable") {
    // **Uji yang menangkap kegagalan yang hanya muncul di editor terpasang.**
    // Modul material dirakit saat editor berjalan dengan menanam berkas shader
    // renderer dari direktori di sebelah executable — bukan dari folder sumber.
    // Berkas yang tidak ikut disalin menjadi `#error` di dalam modul, dan yang
    // terlihat adalah "preprocessor error" dari slangc, jauh dari sebabnya.
    //
    // Uji lain di berkas ini membaca folder sumber, tempat semuanya selalu ada,
    // jadi tidak satu pun dari mereka bisa melihatnya. Yang ini melihatnya.
    const std::filesystem::path shaders(SIM_SHADER_DIR);

    // Telusuri `#include` dari akar yang sama dengan yang ditanam
    // `MaterialPrograms`.
    std::set<std::string> needed{"openpbr.slang"};
    std::vector<std::string> queue{"box_varyings.slang", "cluster_common.slang",
                                   "gi_resolve.slang"};
    while (!queue.empty()) {
        const std::string name = queue.back();
        queue.pop_back();
        if (!needed.insert(name).second) {
            continue;
        }
        std::ifstream file(shaders / name);
        REQUIRE_MESSAGE(file, "tidak bisa membaca " << name);
        std::string line;
        while (std::getline(file, line)) {
            const std::size_t at = line.find("#include \"");
            if (at == std::string::npos) {
                continue;
            }
            const std::size_t start = at + 10;
            const std::size_t end = line.find('"', start);
            if (end != std::string::npos) {
                queue.push_back(line.substr(start, end - start));
            }
        }
    }

    std::ifstream cmake(std::filesystem::path(SIM_CODE_DIR) / "Render" / "CMakeLists.txt");
    REQUIRE(cmake);
    const std::string text((std::istreambuf_iterator<char>(cmake)),
                           std::istreambuf_iterator<char>());
    const std::size_t listStart = text.find("SIM_SHADER_SOURCES_COPIED");
    REQUIRE_MESSAGE(listStart != std::string::npos,
                    "daftar shader yang disalin tidak ditemukan di Code/Render/CMakeLists.txt");
    const std::size_t listEnd = text.find(')', listStart);
    REQUIRE(listEnd != std::string::npos);
    const std::string list = text.substr(listStart, listEnd - listStart);

    for (const std::string& name : needed) {
        INFO("shader yang ditanam tapi tidak disalin: " << name);
        CHECK(list.find(name) != std::string::npos);
    }
}

TEST_CASE("T-mask: mode alfa dibaca dari node keluaran, dan uji buangnya masuk modul forward") {
    MaterialGraph graph = MinimalGraph();

    SUBCASE("bawaannya tanpa uji apa pun") {
        const MaterialCompileResult compiled = CompileMaterial(graph);
        REQUIRE(compiled.ok);
        CHECK_FALSE(compiled.alphaTest);

        ForwardMaterialOptions options;
        options.alphaTest = compiled.alphaTest;
        options.alphaCutoff = compiled.alphaCutoff;
        const std::string module = AssembleForwardMaterialModule(compiled.slang, options);
        // **Yang buram tidak boleh membayar satu instruksi pun.** `discard`
        // mematikan uji depth awal pada sebagian perangkat keras, dan
        // membayarnya untuk material yang opasitasnya tetap satu adalah
        // membayarnya untuk hampir seluruh adegan.
        CHECK(module.find("discard") == std::string::npos);
    }

    SUBCASE("domain masked menyalakannya, beserta ambangnya") {
        auto& output = graph.nodes[0];
        REQUIRE(output.type == "output.surface");
        output.settings["domain"] = "masked";
        output.settings["alphaCutoff"] = "0.25";

        const MaterialCompileResult compiled = CompileMaterial(graph);
        REQUIRE(compiled.ok);
        CHECK(compiled.alphaTest);
        CHECK(compiled.alphaCutoff == doctest::Approx(0.25f));

        ForwardMaterialOptions options;
        options.alphaTest = compiled.alphaTest;
        options.alphaCutoff = compiled.alphaCutoff;
        const std::string module = AssembleForwardMaterialModule(compiled.slang, options);
        const std::size_t at = module.find("discard");
        REQUIRE(at != std::string::npos);
        CHECK(module.find("m.opacity < 0.25") != std::string::npos);

        // **Buangnya mendahului bingkai shading.** Menaruhnya di akhir tetap
        // benar hasilnya dan tetap membayar seluruh evaluasi lobe untuk fragmen
        // yang toh dibuang.
        const std::size_t frame = module.find("ShadingFrame frame;");
        REQUIRE(frame != std::string::npos);
        CHECK(at < frame);
    }

    SUBCASE("domain transparent menyalakan padu, dan justru tidak boleh menyalakan topeng") {
        auto& output = graph.nodes[0];
        output.settings["domain"] = "transparent";
        const MaterialCompileResult compiled = CompileMaterial(graph);
        REQUIRE(compiled.ok);
        CHECK(compiled.alphaBlend);
        // **Dan `alphaTest` tetap mati.** Keduanya berangkat dari opasitas yang
        // sama dan berakhir sangat berbeda: yang dibuang kehilangan seluruh
        // gradasinya menjadi tepi biner. Material yang diam-diam berpindah ke
        // jalur topeng terlihat sebagai kotoran yang bertepi keras alih-alih
        // membaur — cacat yang mudah disangka salah aset.
        CHECK_FALSE(compiled.alphaTest);

        ForwardMaterialOptions options;
        options.alphaTest = compiled.alphaTest;
        const std::string module = AssembleForwardMaterialModule(compiled.slang, options);
        CHECK(module.find("discard") == std::string::npos);
        // Opasitasnya harus sampai ke alfa keluaran; padu tanpa alfa adalah
        // permukaan pejal yang pipeline-nya kebetulan menyalakan blending.
        CHECK(module.find("m.opacity") != std::string::npos);
    }
}

TEST_CASE("G5: jalur bindless mengubah dari mana datanya datang, bukan bentuk materialnya") {
    // **Yang diuji di sini ABI-nya, bukan sintaksnya.** Kedua jalur menghasilkan
    // modul yang sama sahnya, jadi tidak ada satu pun galat yang muncul ketika
    // salah satunya salah — yang muncul hanya material yang parameternya
    // bergeser atau teksturnya milik benda lain.
    const std::filesystem::path path = std::filesystem::path(SIM_BUILTIN_DIR) / "Materials" /
                                       "Sistem" / "Material Impor.simmat";
    REQUIRE_MESSAGE(std::filesystem::exists(path), path.string());

    MaterialGraph graph;
    REQUIRE(LoadMaterialFromFile(graph, path).ok);

    MaterialCompileOptions classicOptions;
    classicOptions.moduleName = "Material Impor.simmat";
    MaterialCompileOptions bindlessOptions = classicOptions;
    bindlessOptions.bindless = true;

    const MaterialCompileResult classic = CompileMaterial(graph, classicOptions);
    const MaterialCompileResult bindless = CompileMaterial(graph, bindlessOptions);
    REQUIRE(classic.ok);
    REQUIRE(bindless.ok);
    REQUIRE(classic.textures.size() == 1);
    REQUIRE(bindless.textures.size() == classic.textures.size());

    // Jalur mundur tidak berubah satu huruf pun: satu set per material, blok
    // parameter di binding 0, tekstur dan sampler berselang mulai binding 1.
    INFO(classic.slang);
    CHECK(classic.slang.find("[[vk::binding(0, 2)]]\ncbuffer MaterialParams") !=
          std::string::npos);
    CHECK(classic.slang.find("[[vk::binding(1, 2)]]\nTexture2D<float4> " +
                             classic.textures[0].name) != std::string::npos);
    CHECK(classic.slang.find("gBindlessTextures") == std::string::npos);

    // Bindless: larik bersama, nomor binding yang sama persis. Nomor yang sama
    // itulah yang membuat `box.frag` dan `box_bindless.frag` berbeda satu baris.
    INFO(bindless.slang);
    CHECK(bindless.slang.find("[[vk::binding(0, 2)]]\nConstantBuffer<MaterialParams> "
                              "gMaterialParams[];") != std::string::npos);
    CHECK(bindless.slang.find("[[vk::binding(1, 2)]]\nTexture2D<float4> gBindlessTextures[];") !=
          std::string::npos);
    CHECK(bindless.slang.find("[[vk::binding(2, 2)]]\nSamplerState gBindlessSamplers[];") !=
          std::string::npos);
    CHECK(bindless.slang.find("cbuffer MaterialParams") == std::string::npos);

    // Tabel slot tekstur: `uint4`, empat slot per baris, dan satu baris untuk
    // satu tekstur. Lihat catatannya di `MaterialCompiler` — `uint` satu-satu
    // akan mengisi celah sisipan di dalam blok parameter alih-alih jatuh di
    // ujungnya.
    CHECK(bindless.slang.find("uint4 gTextureSlots[1];") != std::string::npos);

    // Prolognya menghidupkan kembali nama yang dipakai badan: itu yang membuat
    // emisi badan **sama persis** di kedua jalur, dan yang membuat jalur mundur
    // tidak bisa diam-diam berhenti diuji karena hanya salah satunya punya kode.
    CHECK(bindless.slang.find(
              "const MaterialParams gMat = gMaterialParams[inputs.materialSlot];") !=
          std::string::npos);
    CHECK(bindless.slang.find("Texture2D<float4> " + bindless.textures[0].name +
                              " = gBindlessTextures[slot0];") != std::string::npos);
    CHECK(bindless.slang.find("NonUniformResourceIndex(gMat.gTextureSlots[0][0])") !=
          std::string::npos);

    // Dan badannya memang tidak berubah: kedua jalur menyampel lewat nama yang
    // sama, dari baris yang sama.
    const std::string sample = bindless.textures[0].name + ".Sample";
    CHECK(classic.slang.find(sample) != std::string::npos);
    CHECK(bindless.slang.find(sample) != std::string::npos);
}

TEST_CASE("G5: tabel slot tekstur jatuh tepat di ujung blok parameter") {
    // **Ini kesepakatan antara dua berkas yang tidak saling melihat.**
    // `AcquireMaterial` menempelkan tabel slot sesudah blok parameter yang
    // sudah dibulatkan ke 16, sementara yang menentukan letaknya di sisi shader
    // adalah aturan penjajaran std140 atas `uint4`. Keduanya bertemu hanya
    // kalau ukuran blok itu memang kelipatan 16 — dan kalau tidak, yang terjadi
    // bukan galat melainkan tekstur yang terbaca dari angka sembarang.
    MaterialParameterBlock block;

    // Kasus yang paling mudah salah: `float3` di akhir. Ia berukuran 12 dan
    // berjajar 16, jadi bloknya berakhir di offset 12 sementara ukurannya 16 —
    // dan sebuah `uint` yang menyusulnya akan mengisi celah di +12, bukan
    // memulai baris berikutnya.
    std::vector<MaterialParameter> parameters;
    MaterialParameter colour;
    colour.name = "warna";
    colour.kind = ValueKind::Float3;
    colour.defaultValue = "1 1 1";
    parameters.push_back(colour);
    block.Build(parameters);
    CHECK(block.Slots().size() == 1);
    CHECK(block.Slot(0).offset == 0);
    CHECK(block.Slot(0).size == 12);
    CHECK(block.Bytes() == 16);
    CHECK(block.Bytes() % 16 == 0);

    // Beberapa bentuk lain, semuanya harus berakhir di kelipatan 16.
    for (const ValueKind kind :
         {ValueKind::Float, ValueKind::Float2, ValueKind::Float3, ValueKind::Float4}) {
        MaterialParameter extra;
        extra.name = "tambahan";
        extra.kind = kind;
        extra.defaultValue = "0";
        std::vector<MaterialParameter> mixed = parameters;
        mixed.push_back(extra);
        block.Build(mixed);
        INFO("kind = " << static_cast<int>(kind));
        CHECK(block.Bytes() % 16 == 0);
        CHECK(block.Bytes() >= block.Slot(block.SlotCount() - 1).offset +
                                   block.Slot(block.SlotCount() - 1).size);
    }
}

TEST_CASE("G5: modul material bindless dikompilasi slangc sungguhan") {
    const std::string identity = SlangCompilerIdentity();
    if (identity.empty()) {
        MESSAGE("slangc tidak ditemukan — bagian integrasi dilewati");
        return;
    }

    ForwardMaterialOptions options;
    options.prelude = LoadOpenPbrPrelude(SIM_SHADER_DIR);
    // **`box_push.slang` ikut, dan ia yang membawa `push.materialSlot`.**
    // Tanpanya modul bindless tidak punya nomor slotnya, dan yang muncul bukan
    // gambar yang salah melainkan galat preprocessor — yaitu bentuk kegagalan
    // yang benar.
    options.frameDeclarations =
        InlineShaderIncludes(SIM_SHADER_DIR, {"box_push.slang", "box_varyings.slang",
                                              "cluster_common.slang", "gi_resolve.slang"});
    REQUIRE(!options.prelude.empty());
    REQUIRE(!options.frameDeclarations.empty());

    const std::filesystem::path path = std::filesystem::path(SIM_BUILTIN_DIR) / "Materials" /
                                       "Sistem" / "Material Impor.simmat";
    REQUIRE_MESSAGE(std::filesystem::exists(path), path.string());
    MaterialGraph graph;
    REQUIRE(LoadMaterialFromFile(graph, path).ok);

    MaterialCompileOptions compileOptions;
    compileOptions.bindless = true;
    const MaterialCompileResult compiled = CompileMaterial(graph, compileOptions);
    REQUIRE(compiled.ok);
    options.lobes = compiled.lobes;

    TempCacheDir dir("bindless");
    ShaderCache cache;
    cache.Configure(dir.path, identity);
    cache.SetCompiler(MakeSlangCompiler());

    const CompileOutput out = cache.Get(MakeForwardMaterialRequest(compiled.slang, options));
    INFO("slangc: ", out.error);
    REQUIRE(out.ok);
    CHECK(LooksLikeSpirv(out.spirv));

    // **Nomor slotnya datang dari varying, dan itu kesepakatan dengan
    // `box.vert`.** Sejak G6 ia data per instance, bukan push constant — itulah
    // yang membuat dua entity bermaterial berbeda bisa berbagi satu panggilan
    // gambar. Yang menuliskannya `box.vert` ke `slots.x`; yang membacanya baris
    // ini. Keduanya tidak saling melihat, jadi yang menjaganya adalah uji ini.
    const std::string module = AssembleForwardMaterialModule(compiled.slang, options);
    CHECK(module.find("inputs.materialSlot = input.slots.x;") != std::string::npos);
}

TEST_CASE("Pratinjau pin: yang digambar nilainya, bukan nilainya di bawah lampu") {
    MaterialGraph graph;
    graph.nodes.push_back(Node(1, std::string(kSurfaceOutputType)));
    graph.nodes.back().settings["domain"] = "masked";
    graph.nodes.push_back(Node(2, "input.texture"));
    graph.nodes.back().settings["texture"] = Id(900).ToString();
    graph.nodes.back().settings["name"] = "Albedo";
    graph.nodes.push_back(Node(3, "input.sample"));
    Link(graph, 2, "texture", 3, "texture");
    Link(graph, 3, "rgb", 1, "baseColor");

    // Cabang yang **belum** tersambung ke keluaran: UV yang digeser terhadap x.
    // Justru cabang seperti inilah yang paling sering ingin dilihat orang, karena
    // ia sedang dirakit dan belum menghasilkan apa pun di permukaan.
    graph.nodes.push_back(Node(4, "input.uv"));
    graph.nodes.push_back(Node(5, "math.add"));
    graph.nodes.back().pinValues["b"] = "float2(0.25, 0.0)";
    Link(graph, 4, "uv", 5, "a");

    const MaterialCompileResult plain = CompileMaterial(graph);
    REQUIRE_MESSAGE(plain.ok, FirstError({false, plain.errors}));
    CHECK(plain.slang.find("float2(0.25, 0.0)") == std::string::npos);
    CHECK(plain.alphaTest);

    MaterialCompileOptions options;
    options.previewNode = Id(5);
    options.previewPin = "result";
    const MaterialCompileResult preview = CompileMaterial(graph, options);
    REQUIRE_MESSAGE(preview.ok, FirstError({false, preview.errors}));

    // Cabangnya ikut ditulis sekarang, meski tidak menyentuh node keluaran.
    CHECK(preview.slang.find("inputs.uv0 + float2(0.25, 0.0)") != std::string::npos);

    // Permukaannya dibuang: albedo nol dan spekular mati, jadi tidak ada satu
    // pun jalur yang mengalikan nilai itu dengan cahaya sebelum ia terlihat.
    CHECK(preview.slang.find("result.surface.baseColor = float3(0.0, 0.0, 0.0);") !=
          std::string::npos);
    CHECK(preview.slang.find("result.surface.specularWeight = 0.0;") != std::string::npos);
    CHECK(preview.slang.find("result.normal = float3(0.0, 0.0, 1.0);") != std::string::npos);
    CHECK(preview.slang.find("result.opacity = 1.0;") != std::string::npos);

    // Float2 dilebarkan dengan nol, sehingga UV terbaca sebagai gradien
    // merah-hijau — dan pergeseran terhadap x terlihat sebagai gradien yang
    // bergeser, bukan sebagai warna yang berganti.
    CHECK(preview.slang.find("result.emissive = float3(n4_result, 0.0);") != std::string::npos);

    // Mode alfa material tidak ikut: sebuah pratinjau yang dibuang uji topeng
    // adalah kotak kosong, dan kotak kosong tidak menjawab apa pun.
    CHECK_FALSE(preview.alphaTest);
    CHECK_FALSE(preview.alphaBlend);
}

TEST_CASE("Pratinjau pin: node tekstur ditampilkan gambarnya, bukan binding-nya") {
    MaterialGraph graph;
    graph.nodes.push_back(Node(1, std::string(kSurfaceOutputType)));
    graph.nodes.push_back(Node(2, "input.texture"));
    graph.nodes.back().settings["texture"] = Id(900).ToString();
    graph.nodes.back().settings["name"] = "Albedo";
    graph.nodes.push_back(Node(3, "input.sample"));
    Link(graph, 2, "texture", 3, "texture");
    Link(graph, 3, "rgb", 1, "baseColor");

    MaterialCompileOptions options;
    options.previewNode = Id(2);
    options.previewPin = "texture";
    const MaterialCompileResult preview = CompileMaterial(graph, options);
    REQUIRE_MESSAGE(preview.ok, FirstError({false, preview.errors}));

    // Yang mengalir dari node tekstur cuma namanya. Menampilkannya apa adanya
    // tidak mungkin — sebuah `Texture2D` bukan warna — jadi ia disampel di sini
    // dengan pasangan sampler yang sama dengan yang dipakai node Sample.
    CHECK(preview.slang.find("result.emissive = tAlbedo.Sample(sAlbedo, inputs.uv0).rgb;") !=
          std::string::npos);
    REQUIRE(preview.textures.size() == 1);
    CHECK(preview.textures.front().name == "tAlbedo");
}

TEST_CASE("Pratinjau pin: pin yang tidak ada ditolak dengan pesan, bukan digambar hitam") {
    MaterialGraph graph = MinimalGraph();

    MaterialCompileOptions options;
    options.previewNode = Id(2);
    options.previewPin = "tidakAda";
    const MaterialCompileResult preview = CompileMaterial(graph, options);
    CHECK_FALSE(preview.ok);
    REQUIRE_FALSE(preview.errors.empty());
    CHECK(preview.errors.front().message.find("no value to preview") != std::string::npos);
}

// --- Domain material ---------------------------------------------------------

TEST_CASE("domain dibaca dari node keluaran") {
    MaterialGraph graph = MinimalGraph();

    SUBCASE("tanpa setting apa pun, buram") {
        // Bukan sekadar nilai awal yang nyaman: buram adalah satu-satunya
        // domain yang menggambar benar tanpa alfa sama sekali, jadi ia jawaban
        // yang paling tidak merusak untuk berkas yang tidak menyatakan apa pun.
        CHECK(graph.Domain() == MaterialDomain::Opaque);
    }

    SUBCASE("keempat nama terbaca") {
        const std::pair<const char*, MaterialDomain> cases[] = {
            {"opaque", MaterialDomain::Opaque},
            {"masked", MaterialDomain::Masked},
            {"transparent", MaterialDomain::Transparent},
            {"decal", MaterialDomain::Decal},
        };
        for (const auto& [text, expected] : cases) {
            for (MaterialNode& node : graph.nodes) {
                if (node.type == kSurfaceOutputType) {
                    node.settings["domain"] = text;
                }
            }
            CAPTURE(text);
            CHECK(graph.Domain() == expected);
        }
    }

    SUBCASE("nama yang tidak dikenali jatuh ke buram, bukan menggagalkan") {
        // **Satu setting salah ketik tidak boleh membuat material menolak
        // terbuka.** Yang keliru menggambar buram, dan itu terbaca di viewport
        // sebagai sesuatu yang salah — jauh lebih berguna daripada aset yang
        // hilang.
        for (MaterialNode& node : graph.nodes) {
            if (node.type == kSurfaceOutputType) {
                node.settings["domain"] = "mask";  // ejaan yang bukan nama domain
            }
        }
        CHECK(graph.Domain() == MaterialDomain::Opaque);
    }

    SUBCASE("SetDomain menulis satu setting, dan hanya itu") {
        graph.SetDomain(MaterialDomain::Masked);
        const MaterialNode* output = nullptr;
        for (const MaterialNode& node : graph.nodes) {
            if (node.type == kSurfaceOutputType) {
                output = &node;
            }
        }
        REQUIRE(output != nullptr);
        CHECK(output->Setting("domain") == "masked");

        graph.SetDomain(MaterialDomain::Decal);
        CHECK(output->Setting("domain") == "decal");

        // Bolak-balik kembali ke asal, bukan menumpuk sisa.
        graph.SetDomain(MaterialDomain::Opaque);
        CHECK(output->Setting("domain") == "opaque");
    }
}

TEST_CASE("domain menentukan pin mana yang boleh dikemudikan") {
    // Opacity satu-satunya yang benar-benar tidak dibaca siapa pun saat buram.
    CHECK_FALSE(SurfacePinApplies("opacity", MaterialDomain::Opaque));
    CHECK(SurfacePinApplies("opacity", MaterialDomain::Masked));
    CHECK(SurfacePinApplies("opacity", MaterialDomain::Transparent));
    CHECK(SurfacePinApplies("opacity", MaterialDomain::Decal));

    // Base color berlaku di mana-mana.
    for (const MaterialDomain domain :
         {MaterialDomain::Opaque, MaterialDomain::Masked, MaterialDomain::Transparent,
          MaterialDomain::Decal}) {
        CHECK(SurfacePinApplies("baseColor", domain));
    }

    // Decal: selembar kulit, bukan bahan berlapis.
    CHECK_FALSE(SurfacePinApplies("coatWeight", MaterialDomain::Decal));
    CHECK_FALSE(SurfacePinApplies("fuzzWeight", MaterialDomain::Decal));
    CHECK_FALSE(SurfacePinApplies("specularRoughnessAnisotropy", MaterialDomain::Decal));
    CHECK(SurfacePinApplies("coatWeight", MaterialDomain::Opaque));
}

TEST_CASE("penyaringan domain tidak menyentuh kode yang dihasilkan") {
    // **Regresi, dan pelajarannya mahal.** Percobaan pertama menyaring pin di
    // dalam `PinsOf`, dan akibatnya `result.opacity` tidak pernah ditulis —
    // sebuah float tak berisi di setiap material buram — sementara berkas lama
    // yang menyambungkan opacity berhenti bisa dikompilasi sama sekali.
    MaterialGraph graph = MinimalGraph();
    graph.SetDomain(MaterialDomain::Opaque);

    const MaterialCompileResult result = CompileMaterial(graph);
    REQUIRE(result.ok);
    CHECK(result.domain == MaterialDomain::Opaque);
    // Ketiga medan di luar OpenPBRSurface tetap ditulis, berapa pun yang
    // ditawarkan kanvas.
    CHECK(result.slang.find("result.opacity =") != std::string::npos);
    CHECK(result.slang.find("result.normal =") != std::string::npos);
    CHECK(result.slang.find("result.emissive =") != std::string::npos);
    CHECK_FALSE(result.alphaTest);
    CHECK_FALSE(result.alphaBlend);
}

TEST_CASE("uji alfa dan pencampuran diturunkan dari domain") {
    MaterialGraph graph = MinimalGraph();

    graph.SetDomain(MaterialDomain::Masked);
    const MaterialCompileResult masked = CompileMaterial(graph);
    REQUIRE(masked.ok);
    CHECK(masked.alphaTest);
    CHECK_FALSE(masked.alphaBlend);

    graph.SetDomain(MaterialDomain::Transparent);
    const MaterialCompileResult blended = CompileMaterial(graph);
    REQUIRE(blended.ok);
    CHECK_FALSE(blended.alphaTest);
    CHECK(blended.alphaBlend);

    // Decal ikut dicampur: tepinya memudar, bukan berhenti mendadak.
    graph.SetDomain(MaterialDomain::Decal);
    const MaterialCompileResult decal = CompileMaterial(graph);
    REQUIRE(decal.ok);
    CHECK_FALSE(decal.alphaTest);
    CHECK(decal.alphaBlend);
}
