#include "Sim/Material/MaterialCompiler.h"

#include "Sim/Material/MaterialNodeCatalog.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

namespace sim::material {
namespace {

/// Nama tipe Slang untuk sebuah ValueKind.
const char* SlangType(ValueKind kind) {
    switch (kind) {
        case ValueKind::Float: return "float";
        case ValueKind::Float2: return "float2";
        case ValueKind::Float3: return "float3";
        case ValueKind::Float4: return "float4";
        case ValueKind::Bool: return "bool";
        // Numeric tidak pernah sampai sini: penyimpulan sudah menyelesaikannya
        // menjadi tipe konkret sebelum ada satu baris pun ditulis.
        case ValueKind::Numeric: return "float";
        case ValueKind::Texture: break;
    }
    return "Texture2D<float4>";
}

/// Menaruh tanda kurung hanya jika perlu — ekspresi yang sudah berupa satu
/// nama atau satu pemanggilan tidak dibungkus lagi, supaya kode yang keluar
/// tidak dipenuhi kurung yang tidak menjelaskan apa-apa.
bool NeedsParentheses(const std::string& text) {
    int depth = 0;
    for (const char c : text) {
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            --depth;
        } else if (depth == 0 && (c == ' ' || c == '+' || c == '-' || c == '*' || c == '/')) {
            return true;
        }
    }
    return false;
}

std::string Wrap(const std::string& text) {
    return NeedsParentheses(text) ? "(" + text + ")" : text;
}

/// Pengenal Slang yang sah dan mudah dilacak balik ke node-nya.
std::string SanitizeIdentifier(std::string_view text, std::string_view fallback) {
    std::string result;
    for (const char c : text) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        result.push_back(ok ? c : '_');
    }
    if (result.empty() || (result.front() >= '0' && result.front() <= '9')) {
        result = std::string(fallback) + result;
    }
    return result;
}

/// Node yang hanya ada untuk tata letak dan tidak menghasilkan kode.
bool IsLayoutOnly(const MaterialNode& node) {
    return node.type == "comment" || node.type == "group";
}

class Emitter {
public:
    Emitter(const MaterialGraph& graph, const MaterialCompileOptions& options)
        : graph_(graph), options_(options), types_(MaterialTypes::Infer(graph)) {}

    MaterialCompileResult Run();

private:
    /// Urutan emisi: node yang dibutuhkan keluaran, masukan lebih dulu.
    ///
    /// Hanya yang benar-benar terjangkau dari node keluaran. Node yang
    /// menganggur di kanvas — sisa percobaan, potongan yang belum disambung —
    /// tidak menghasilkan satu baris pun, sehingga membiarkannya di sana tidak
    /// membebani shader.
    void CollectReachable(const Uuid& node);

    /// Ekspresi Slang untuk sebuah pin masukan: variabel sumbernya, nilai yang
    /// diketik pengguna, atau nilai bawaan katalog — menurut urutan itu.
    std::string InputExpression(const MaterialNode& node, const MaterialPin& pin);

    /// True bila pin masukan itu punya sumber atau nilai yang ditulis pengguna.
    bool IsDriven(const MaterialNode& node, const MaterialPin& pin) const;

    void EmitNode(const MaterialNode& node);
    void Line(std::string text, const Uuid& node = {});

    const MaterialGraph& graph_;
    const MaterialCompileOptions& options_;

    MaterialTypes types_;

    MaterialCompileResult result_;
    std::ostringstream body_;
    int line_ = 0;

    std::vector<const MaterialNode*> order_;
    std::set<Uuid> seen_;
    /// Nama variabel per (node, pin keluaran).
    std::map<std::pair<Uuid, std::string>, std::string> values_;
    /// Nama binding tekstur per node `input.texture`.
    std::map<Uuid, std::string> textureNames_;
    int nextIndex_ = 0;
};

void Emitter::Line(std::string text, const Uuid& node) {
    body_ << text << '\n';
    ++line_;
    if (node.IsValid()) {
        result_.sourceMap.push_back({node, line_});
    }
}

void Emitter::CollectReachable(const Uuid& guid) {
    if (!seen_.insert(guid).second) {
        return;
    }
    const MaterialNode* node = graph_.FindNode(guid);
    if (node == nullptr) {
        return;
    }
    // Masukan dikumpulkan lebih dulu, sehingga node muncul di `order_` setelah
    // semua yang ia butuhkan. Validasi sudah menolak siklus, jadi penelusuran
    // ini tidak bisa berputar.
    for (const MaterialLink& link : graph_.links) {
        if (link.toNode == guid) {
            CollectReachable(link.fromNode);
        }
    }
    if (!IsLayoutOnly(*node)) {
        order_.push_back(node);
    }
}

bool Emitter::IsDriven(const MaterialNode& node, const MaterialPin& pin) const {
    return graph_.LinkInto(node.guid, pin.name) != nullptr ||
           node.pinValues.count(pin.name) != 0;
}

std::string Emitter::InputExpression(const MaterialNode& node, const MaterialPin& pin) {
    if (const MaterialLink* link = graph_.LinkInto(node.guid, pin.name)) {
        const auto it = values_.find(std::make_pair(link->fromNode, link->fromPin));
        if (it != values_.end()) {
            return it->second;
        }
    }
    const auto typed = node.pinValues.find(pin.name);
    if (typed != node.pinValues.end() && !typed->second.empty()) {
        return typed->second;
    }
    return pin.defaultValue;
}

void Emitter::EmitNode(const MaterialNode& node) {
    const std::vector<MaterialPin> pins = PinsOf(graph_, node);
    const std::string index = std::to_string(nextIndex_++);

    const auto input = [&](std::string_view name) -> std::string {
        const auto it = std::find_if(pins.begin(), pins.end(), [name](const MaterialPin& pin) {
            return pin.name == name && pin.direction == PinDirection::Input;
        });
        return it == pins.end() ? std::string{} : InputExpression(node, *it);
    };
    const auto inputType = [&](std::string_view name) -> ValueKind {
        return types_.Into(graph_, node, name);
    };
    /// Tipe yang sudah disimpulkan untuk sebuah pin keluaran node ini.
    const auto outputType = [&](std::string_view name) -> ValueKind {
        return types_.Of(graph_, node, name);
    };
    /// Mendaftarkan satu pin keluaran beserta ekspresi dan tipenya, lalu
    /// menuliskannya sebagai satu variabel.
    const auto define = [&](std::string_view pinName, ValueKind kind,
                            const std::string& expression) {
        const std::string name = "n" + index + "_" + SanitizeIdentifier(pinName, "p");
        values_[std::make_pair(node.guid, std::string(pinName))] = name;
        Line(std::string("    ") + SlangType(kind) + " " + name + " = " + expression + ";",
             node.guid);
    };
    /// Untuk node yang cukup diberi nama tanpa baris sendiri — tekstur, yang
    /// bukan nilai melainkan binding.
    const auto alias = [&](std::string_view pinName, std::string expression) {
        values_[std::make_pair(node.guid, std::string(pinName))] = std::move(expression);
    };

    const std::string& type = node.type;

    if (type == "input.texture") {
        // Tekstur tidak punya baris di dalam fungsi: ia dideklarasikan di
        // lingkup modul, dan yang mengalir lewat kabel hanyalah namanya.
        const std::string name =
            "t" + SanitizeIdentifier(node.Setting("name", "Texture" + index), "t");
        textureNames_[node.guid] = name;
        result_.textures.push_back({name, Uuid::Parse(node.Setting("texture")), node.guid});
        alias("texture", name);
        return;
    }
    if (type == "input.sample") {
        std::string texture = input("texture");
        if (texture.empty()) {
            texture = "tMissing";
        }
        const std::string sampler = "s" + texture.substr(texture.front() == 't' ? 1 : 0);
        define("rgba", outputType("rgba"),
               texture + ".Sample(" + sampler + ", " + input("uv") + ")");
        const std::string rgba = values_.at(std::make_pair(node.guid, std::string("rgba")));
        alias("rgb", rgba + ".rgb");
        alias("r", rgba + ".r");
        alias("g", rgba + ".g");
        alias("b", rgba + ".b");
        alias("a", rgba + ".a");
        return;
    }
    if (type == "input.uv") {
        alias("uv", "inputs.uv0");
        return;
    }
    if (type == "input.vertexColor") {
        alias("color", "inputs.vertexColor");
        return;
    }
    if (type == "input.time") {
        alias("time", "inputs.time");
        return;
    }
    if (type == "input.normal") {
        alias("normal", "inputs.worldNormal");
        return;
    }
    if (type == "input.viewDirection") {
        alias("direction", "inputs.viewDirection");
        return;
    }
    if (type == "input.constant") {
        const ValueKind kind = ValueKindFromString(node.Setting("kind", "float"));
        std::string value = node.Setting("value");
        if (value.empty()) {
            value = kind == ValueKind::Float ? "0.0" : std::string(SlangType(kind)) + "(0.0)";
        }
        alias("value", value);
        return;
    }
    if (type == "param.get") {
        const MaterialParameter* parameter = graph_.FindParameter(node.Setting("parameter"));
        if (parameter == nullptr) {
            result_.errors.push_back(
                {node.guid, {}, "Parameter '" + node.Setting("parameter") + "' does not exist"});
            alias("value", "0.0");
            return;
        }
        const std::string name = SanitizeIdentifier(parameter->name, "p");
        if (std::find(result_.usedParameters.begin(), result_.usedParameters.end(),
                      parameter->name) == result_.usedParameters.end()) {
            result_.usedParameters.push_back(parameter->name);
        }
        alias("value", name);
        return;
    }

    // --- matematika ---------------------------------------------------------
    static const std::map<std::string, const char*> kBinaryOperators{
        {"math.add", "+"}, {"math.subtract", "-"}, {"math.multiply", "*"}, {"math.divide", "/"}};
    if (const auto op = kBinaryOperators.find(type); op != kBinaryOperators.end()) {
        define("result", outputType("result"),
               Wrap(input("a")) + " " + op->second + " " + Wrap(input("b")));
        return;
    }
    static const std::map<std::string, const char*> kBinaryFunctions{
        {"math.min", "min"}, {"math.max", "max"}, {"math.power", "pow"}, {"math.step", "step"}};
    if (const auto fn = kBinaryFunctions.find(type); fn != kBinaryFunctions.end()) {
        define("result", outputType("result"),
               std::string(fn->second) + "(" + input("a") + ", " + input("b") + ")");
        return;
    }
    if (type == "math.dot") {
        // Satu-satunya node biner yang hasilnya selalu skalar, apa pun
        // masukannya.
        define("result", outputType("result"), "dot(" + input("a") + ", " + input("b") + ")");
        return;
    }
    static const std::map<std::string, const char*> kUnaryFunctions{
        {"math.abs", "abs"},   {"math.saturate", "saturate"}, {"math.normalize", "normalize"},
        {"math.sin", "sin"},   {"math.cos", "cos"},           {"math.fract", "frac"}};
    if (const auto fn = kUnaryFunctions.find(type); fn != kUnaryFunctions.end()) {
        define("result", outputType("result"), std::string(fn->second) + "(" + input("x") + ")");
        return;
    }
    if (type == "math.oneMinus") {
        define("result", outputType("result"), "1.0 - " + Wrap(input("x")));
        return;
    }

    // --- utilitas -----------------------------------------------------------
    if (type == "util.lerp") {
        define("result", outputType("result"),
               "lerp(" + input("a") + ", " + input("b") + ", " + input("t") + ")");
        return;
    }
    if (type == "util.clamp") {
        define("result", outputType("result"),
               "clamp(" + input("x") + ", " + input("min") + ", " + input("max") + ")");
        return;
    }
    if (type == "util.fresnel") {
        define("result", outputType("result"),
               "pow(1.0 - saturate(dot(" + input("normal") + ", inputs.viewDirection)), " +
                   input("power") + ")");
        return;
    }
    if (type == "util.normalBlend") {
        // Reoriented normal mapping (Barré-Brisebois & Hill 2012). Bukan
        // penjumlahan biasa: menjumlahkan dua normal peta lalu menormalkannya
        // meratakan detailnya justru di tempat yang paling terlihat.
        const std::string base = "float3(" + input("base") + ".xy, " + input("base") + ".z + 1.0)";
        const std::string detail =
            "float3(-" + input("detail") + ".xy, " + input("detail") + ".z)";
        define("result", outputType("result"),
               "normalize(" + base + " * dot(" + base + ", " + detail + ") / " + base +
                   ".z - " + detail + ")");
        return;
    }
    if (type == "util.combine") {
        define("xyzw", outputType("xyzw"),
               "float4(" + input("x") + ", " + input("y") + ", " + input("z") + ", " +
                   input("w") + ")");
        const std::string xyzw = values_.at(std::make_pair(node.guid, std::string("xyzw")));
        alias("xy", xyzw + ".xy");
        alias("xyz", xyzw + ".xyz");
        return;
    }
    if (type == "util.split") {
        // Dipadatkan ke float4 lebih dulu. Pin-nya menerima lebar berapa pun,
        // dan `.z` pada sebuah float2 bukan kode yang sah — Slang menyambung
        // konstruktor, jadi float4(float3, 0.0) adalah bentuk yang benar.
        static const char* const kPad[] = {"", ", 0.0, 0.0, 0.0", ", 0.0, 0.0", ", 0.0", ""};
        const int components = ComponentCount(inputType("value"));
        define("value", ValueKind::Float4,
               std::string("float4(") + input("value") + kPad[components] + ")");
        const std::string value = values_.at(std::make_pair(node.guid, std::string("value")));
        alias("x", value + ".x");
        alias("y", value + ".y");
        alias("z", value + ".z");
        alias("w", value + ".w");
        return;
    }
    if (type == kSurfaceOutputType) {
        return;  // ditulis terpisah di Run(), setelah seluruh masukannya siap
    }

    result_.errors.push_back({node.guid, {}, "No shader translation for node '" + type + "'"});
}

MaterialCompileResult Emitter::Run() {
    const ValidationResult validated = ValidateMaterial(graph_);
    if (!validated.ok) {
        result_.errors = validated.errors;
        return result_;
    }

    const auto outputIt =
        std::find_if(graph_.nodes.begin(), graph_.nodes.end(), [](const MaterialNode& node) {
            return node.type == kSurfaceOutputType;
        });
    const MaterialNode& output = *outputIt;  // validasi menjamin tepat satu

    CollectReachable(output.guid);
    for (const MaterialNode* node : order_) {
        EmitNode(*node);
    }

    // Bagian keluaran ditulis terakhir supaya seluruh variabelnya sudah ada.
    const std::vector<MaterialPin> outputPins = PinsOf(graph_, output);
    std::ostringstream tail;
    tail << "\n    MaterialSurface result;\n";
    tail << "    result.surface = OpenPBRSurface::defaults();\n";
    for (const MaterialPin& pin : outputPins) {
        if (pin.direction != PinDirection::Input) {
            continue;
        }
        const bool extra =
            pin.name == "normal" || pin.name == "emissive" || pin.name == "opacity";
        // Yang tidak dikemudikan dibiarkan pada nilai defaults() — kecuali tiga
        // pin di luar OpenPBRSurface, yang tidak punya defaults() untuk
        // bersandar.
        if (!extra && !IsDriven(output, pin)) {
            continue;
        }
        const std::string target = extra ? "result." + pin.name : "result.surface." + pin.name;
        tail << "    " << target << " = " << InputExpression(output, pin) << ";\n";
    }
    tail << "    return result;\n";

    // --- kepala modul -------------------------------------------------------
    std::ostringstream head;
    head << "// Dihasilkan dari " << options_.moduleName
         << ".simmat — jangan disunting tangan.\n";
    head << "// Nilai permukaan mengikuti OpenPBR Surface v1.1 (openpbr.slang).\n";
    head << "import openpbr;\n\n";

    if (!graph_.parameters.empty()) {
        head << "cbuffer MaterialParams\n{\n";
        for (const MaterialParameter& parameter : graph_.parameters) {
            head << "    " << SlangType(parameter.kind) << " "
                 << SanitizeIdentifier(parameter.name, "p") << ";\n";
        }
        head << "}\n\n";
    }
    for (const TextureBinding& binding : result_.textures) {
        head << "Texture2D<float4> " << binding.name << ";\n";
        head << "SamplerState s" << binding.name.substr(1) << ";\n";
    }
    if (!result_.textures.empty()) {
        head << '\n';
    }

    head << "struct MaterialInputs\n{\n";
    head << "    float2 uv0;\n";
    head << "    float4 vertexColor;\n";
    head << "    float3 worldNormal;\n";
    head << "    float3 viewDirection;\n";
    head << "    float  time;\n";
    head << "};\n\n";

    head << "struct MaterialSurface\n{\n";
    head << "    OpenPBRSurface surface;\n";
    head << "    float3 normal;\n";
    head << "    float3 emissive;\n";
    head << "    float  opacity;\n";
    head << "};\n\n";

    head << "MaterialSurface evalMaterial(MaterialInputs inputs)\n{\n";

    // Peta sumber dihitung ulang terhadap berkas utuh: `line_` selama emisi
    // hanya menghitung baris di dalam badan fungsi.
    const std::string headText = head.str();
    const int headLines = static_cast<int>(std::count(headText.begin(), headText.end(), '\n'));
    for (SourceMapEntry& entry : result_.sourceMap) {
        entry.line += headLines;
    }

    result_.slang = headText + body_.str() + tail.str() + "}\n";
    result_.ok = result_.errors.empty();
    return result_;
}

}  // namespace

Uuid MaterialCompileResult::NodeAtLine(int line) const {
    const auto it = std::find_if(sourceMap.begin(), sourceMap.end(),
                                 [line](const SourceMapEntry& entry) { return entry.line == line; });
    return it == sourceMap.end() ? Uuid{} : it->node;
}

int MaterialCompileResult::LineOfNode(const Uuid& node) const {
    const auto it = std::find_if(sourceMap.begin(), sourceMap.end(),
                                 [&node](const SourceMapEntry& entry) { return entry.node == node; });
    return it == sourceMap.end() ? 0 : it->line;
}

MaterialCompileResult CompileMaterial(const MaterialGraph& graph,
                                      const MaterialCompileOptions& options) {
    Emitter emitter(graph, options);
    return emitter.Run();
}

}  // namespace sim::material
