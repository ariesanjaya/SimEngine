#include "Sim/Script/GraphCompiler.h"

#include "Sim/Script/NodeCatalog.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <stdexcept>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sim::script {
namespace {

/// Enam huruf pertama GUID node, untuk komentar `-- node <id>`.
///
/// Bukan GUID penuh: komentarnya harus cukup untuk mencocokkan baris dengan
/// node saat membaca berkas, dan tiga puluh enam huruf di setiap blok justru
/// menenggelamkan kodenya. Pencocokan yang sesungguhnya memakai peta sumber,
/// bukan teks komentar ini.
std::string ShortId(const Uuid& guid) {
    return guid.ToString().substr(0, 6);
}

std::string Quote(std::string_view text) {
    std::string result = "\"";
    for (const char c : text) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c; break;
        }
    }
    result += '"';
    return result;
}

/// Nama Lua yang sah dari sepotong teks bebas.
std::string Sanitize(std::string_view text) {
    std::string result;
    for (const char c : text) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        result += ok ? static_cast<char>(std::tolower(c)) : '_';
    }
    if (result.empty() || (result[0] >= '0' && result[0] <= '9')) {
        result.insert(result.begin(), '_');
    }
    return result;
}

/// Literal netral untuk sebuah tipe pin, dipakai variabel tanpa nilai bawaan.
const char* NeutralLiteral(PinKind kind) {
    switch (kind) {
        case PinKind::Bool: return "false";
        case PinKind::Number: return "0";
        case PinKind::String: return "\"\"";
        case PinKind::Vec3: return "sim.vec3(0, 0, 0)";
        case PinKind::Quat: return "sim.quat(1, 0, 0, 0)";
        default: return "nil";
    }
}

/// Yang dihasilkan sebuah node murni: pernyataan yang harus mendahuluinya, dan
/// ekspresi per pin keluaran.
struct PureValue {
    std::map<std::string, std::string> outputs;
};

class Compiler {
public:
    Compiler(const Graph& graph, std::string_view chunkName, const CompileOptions& options)
        : graph_(&graph), chunkName_(chunkName), options_(options) {}

    CompileResult Run();

private:
    // --- keluaran ----------------------------------------------------------
    void Emit(const std::string& line) {
        lines_.push_back(Indent() + line);
        FlushPendingPure();
    }
    void EmitRaw(const std::string& line) {
        lines_.push_back(line);
        FlushPendingPure();
    }
    void EmitBlank() { lines_.emplace_back(); }

    /// Komentar penanda node. Sengaja TIDAK ikut menyerap node murni yang
    /// menunggu: ekspresi mereka mendarat di pernyataan sesudahnya, dan
    /// traceback Lua menyebut baris pernyataan itu — bukan baris komentarnya.
    void EmitComment(const std::string& text) { lines_.push_back(Indent() + text); }

    /// Mencatat node murni yang ekspresinya baru saja mendarat di baris
    /// terakhir. Node murni tidak punya barisnya sendiri — itu justru yang
    /// membuat keluarannya terbaca — jadi tanpa ini ia tidak akan pernah bisa
    /// disorot ketika baris tempatnya berada gagal.
    void FlushPendingPure() {
        if (pendingPure_.empty()) {
            return;
        }
        const int line = static_cast<int>(lines_.size());
        for (const Uuid& node : pendingPure_) {
            const bool duplicate =
                std::any_of(result_.sourceMap.begin(), result_.sourceMap.end(),
                            [&node, line](const SourceMapEntry& entry) {
                                return entry.node == node && entry.line == line;
                            });
            if (!duplicate) {
                result_.sourceMap.push_back({node, line});
            }
        }
        pendingPure_.clear();
    }
    std::string Indent() const { return std::string(static_cast<std::size_t>(indent_) * 4, ' '); }

    /// Mencatat bahwa baris berikutnya milik `node`.
    void Mark(const Uuid& node) {
        result_.sourceMap.push_back({node, static_cast<int>(lines_.size()) + 1});
    }

    void Fail(const Uuid& node, std::string message) {
        result_.errors.push_back({node, std::move(message)});
    }

    // --- eksekusi ----------------------------------------------------------
    void EmitEvent(const GraphNode& node, const char* method, const char* parameters);
    void EmitExecFrom(const GraphNode& node, std::string_view pin);
    void EmitNode(const GraphNode& node);
    void EmitEnsure();

    // --- subgraph ----------------------------------------------------------
    /// Nama fungsi Lua untuk sebuah subgraph, mengeluarkannya bila belum ada.
    /// Kosong berarti gagal — alasannya sudah masuk daftar kesalahan.
    std::string EmitSubgraphFunction(const Uuid& guid);
    /// Memeriksa bahwa sebuah graph memang sah dipakai sebagai subgraph.
    bool ValidateSubgraph(const Graph& target, const Uuid& guid, const GraphNode*& entry);

    // --- data --------------------------------------------------------------
    /// Ekspresi Lua untuk sebuah pin input, entah dari koneksi atau literal.
    std::string InputExpr(const GraphNode& node, const GraphPin& pin);
    /// Ekspresi Lua untuk sebuah pin output milik node murni.
    std::string OutputExpr(const GraphNode& node, std::string_view pinName);
    /// Menghitung ulang node murni bila perlu, lalu mengembalikan hasilnya.
    const PureValue* EvaluatePure(const GraphNode& node);
    PureValue BuildPure(const GraphNode& node);

    /// Berapa banyak koneksi yang berangkat dari seluruh pin keluaran node.
    int OutgoingCount(const Uuid& node) const;

    /// Membuang cache node murni. Dipanggil setelah node yang punya efek
    /// samping berjalan: "murni" berarti tanpa efek samping, bukan tetap —
    /// sebuah get_component setelah set_component harus membaca yang baru.
    void InvalidatePure() { pureCache_.clear(); }

    std::string NextLocal(std::string_view hint) {
        return Sanitize(hint) + "_" + std::to_string(++localCounter_);
    }

    const Graph* graph_ = nullptr;
    std::string chunkName_;
    CompileOptions options_;
    CompileResult result_;
    std::vector<std::string> lines_;
    int indent_ = 0;
    int localCounter_ = 0;

    std::unordered_map<Uuid, PureValue> pureCache_;
    /// Node murni yang ekspresinya sudah disusun tapi barisnya belum ditulis.
    std::vector<Uuid> pendingPure_;
    /// Node murni yang sedang dievaluasi, untuk mendeteksi siklus pin data.
    std::vector<Uuid> evaluating_;
    /// Node exec yang sudah masuk rantai berjalan, untuk mendeteksi lingkar
    /// eksekusi yang akan membuat kompilernya sendiri menggantung.
    std::unordered_set<Uuid> execStack_;
    /// Ekspresi pengganti untuk pin keluaran tertentu, mis. variabel loop.
    std::map<std::pair<Uuid, std::string>, std::string> overrides_;

    /// Subgraph yang sudah dikeluarkan sebagai fungsi: GUID → nama fungsinya.
    std::map<Uuid, std::string> subgraphs_;
    /// Graph yang sedang dikeluarkan, untuk menolak lingkar antar-graph.
    std::vector<Uuid> graphStack_;
};

int Compiler::OutgoingCount(const Uuid& node) const {
    int count = 0;
    for (const GraphLink& link : graph_->links) {
        if (link.fromNode == node) {
            ++count;
        }
    }
    return count;
}

std::string Compiler::InputExpr(const GraphNode& node, const GraphPin& pin) {
    if (const GraphLink* link = graph_->LinkInto(node.guid, pin.name)) {
        const GraphNode* source = graph_->FindNode(link->fromNode);
        if (source == nullptr) {
            Fail(node.guid, "input '" + pin.name + "' comes from a node that no longer exists");
            return "nil";
        }
        const std::vector<GraphPin> sourcePins = PinsOf(*graph_, *source, options_.library);
        const auto it = std::find_if(
            sourcePins.begin(), sourcePins.end(),
            [link](const GraphPin& candidate) { return candidate.name == link->fromPin; });
        if (it == sourcePins.end()) {
            Fail(node.guid, "input '" + pin.name + "' comes from a pin that no longer exists");
            return "nil";
        }
        if (!PinAccepts(pin.kind, it->kind)) {
            Fail(node.guid, std::string("cannot connect a ") + ToString(it->kind) + " into '" +
                                pin.name + "', which takes a " + ToString(pin.kind));
            return "nil";
        }
        return OutputExpr(*source, link->fromPin);
    }

    const auto explicitValue = node.pinValues.find(pin.name);
    if (explicitValue != node.pinValues.end() && !explicitValue->second.empty()) {
        return explicitValue->second;
    }
    return pin.defaultValue;
}

std::string Compiler::OutputExpr(const GraphNode& node, std::string_view pinName) {
    const auto override = overrides_.find({node.guid, std::string(pinName)});
    if (override != overrides_.end()) {
        return override->second;
    }
    const PureValue* value = EvaluatePure(node);
    if (value == nullptr) {
        return "nil";
    }
    // Dicatat di setiap pemakaian, bukan hanya saat dihitung: node murni yang
    // hasilnya dipakai di dua baris berbeda ikut bertanggung jawab atas keduanya.
    pendingPure_.push_back(node.guid);
    const auto it = value->outputs.find(std::string(pinName));
    return it == value->outputs.end() ? "nil" : it->second;
}

const PureValue* Compiler::EvaluatePure(const GraphNode& node) {
    if (const auto cached = pureCache_.find(node.guid); cached != pureCache_.end()) {
        return &cached->second;
    }

    // Siklus pada pin data ditolak di sini, bukan dibiarkan menggantung.
    // Pesannya menunjuk node tempat lingkarnya tertutup — satu-satunya tempat
    // yang bisa disorot editor tanpa menebak.
    if (std::find(evaluating_.begin(), evaluating_.end(), node.guid) != evaluating_.end()) {
        const NodeType* type = NodeCatalog::Get().Find(node.type);
        Fail(node.guid, "data pins form a cycle through '" +
                            (type != nullptr ? type->label : node.type) + "'");
        return nullptr;
    }

    evaluating_.push_back(node.guid);
    PureValue value = BuildPure(node);
    evaluating_.pop_back();

    const auto [it, inserted] = pureCache_.emplace(node.guid, std::move(value));
    return &it->second;
}

PureValue Compiler::BuildPure(const GraphNode& node) {
    PureValue value;
    const NodeType* type = NodeCatalog::Get().Find(node.type);
    if (type == nullptr) {
        Fail(node.guid, "unknown node type '" + node.type + "'");
        return value;
    }
    const std::vector<GraphPin> pins = PinsOf(*graph_, node, options_.library);
    const auto input = [&](const char* name) -> std::string {
        const auto it = std::find_if(pins.begin(), pins.end(), [name](const GraphPin& pin) {
            return pin.name == name && pin.direction == PinDirection::Input;
        });
        return it == pins.end() ? "nil" : InputExpr(node, *it);
    };

    const std::string& key = node.type;

    if (key.rfind("component.get.", 0) == 0) {
        const std::string component = key.substr(std::string("component.get.").size());
        const std::string local = NextLocal(component);
        EmitComment("-- node " + ShortId(node.guid) + " (" + type->label + ")");
        Mark(node.guid);
        Emit("local " + local + " = sim.get_component(" + input("entity") + ", " +
             Quote(component) + ")");
        for (const GraphPin& pin : pins) {
            if (pin.direction == PinDirection::Output) {
                value.outputs[pin.name] = local + "." + pin.name;
            }
        }
        return value;
    }

    if (key == "literal.number") {
        // Diurai lalu ditulis ulang: setelan berisi teks bebas, dan menyalinnya
        // apa adanya ke sumber Lua berarti apa pun yang diketik pengguna ikut
        // masuk sebagai kode.
        double number = 0.0;
        try {
            number = std::stod(node.Setting("value", "0"));
        } catch (const std::exception&) {
            number = 0.0;
        }
        std::ostringstream text;
        text << number;
        value.outputs["value"] = text.str();
        return value;
    }
    if (key == "literal.bool") {
        value.outputs["value"] = node.Setting("value", "false") == "true" ? "true" : "false";
        return value;
    }
    if (key == "literal.string") {
        value.outputs["value"] = Quote(node.Setting("value"));
        return value;
    }
    if (key == "literal.vec3") {
        const auto number = [&node](const char* axis) {
            try {
                return std::to_string(std::stod(node.Setting(axis, "0")));
            } catch (const std::exception&) {
                return std::string("0");
            }
        };
        value.outputs["value"] =
            "sim.vec3(" + number("x") + ", " + number("y") + ", " + number("z") + ")";
        return value;
    }

    if (key == "variable.get") {
        const std::string name = node.Setting("variable");
        if (graph_->FindVariable(name) == nullptr) {
            Fail(node.guid, "no graph variable named '" + name + "'");
            return value;
        }
        value.outputs["value"] = "self.state." + Sanitize(name);
        return value;
    }

    static const std::map<std::string, std::string> kBinary{
        {"math.add", " + "},         {"math.subtract", " - "}, {"math.multiply", " * "},
        {"math.divide", " / "},      {"compare.greater", " > "}, {"compare.less", " < "},
        {"compare.equal", " == "},   {"logic.and", " and "},   {"logic.or", " or "},
        {"vec3.add", " + "},
    };
    if (const auto op = kBinary.find(key); op != kBinary.end()) {
        value.outputs["result"] = "(" + input("a") + op->second + input("b") + ")";
        return value;
    }

    if (key == "math.min" || key == "math.max") {
        const std::string fn = key == "math.min" ? "math.min" : "math.max";
        value.outputs["result"] = fn + "(" + input("a") + ", " + input("b") + ")";
        return value;
    }
    if (key == "math.sin") {
        value.outputs["result"] = "math.sin(" + input("value") + ")";
        return value;
    }
    if (key == "math.cos") {
        value.outputs["result"] = "math.cos(" + input("value") + ")";
        return value;
    }
    if (key == "math.abs") {
        value.outputs["result"] = "math.abs(" + input("value") + ")";
        return value;
    }
    if (key == "math.negate") {
        value.outputs["result"] = "(-" + input("value") + ")";
        return value;
    }
    if (key == "logic.not") {
        value.outputs["result"] = "(not " + input("value") + ")";
        return value;
    }
    if (key == "math.clamp") {
        value.outputs["result"] = "math.min(math.max(" + input("value") + ", " + input("min") +
                                 "), " + input("max") + ")";
        return value;
    }
    if (key == "math.lerp") {
        // Dimaterialkan ke local: a dan b masing-masing muncul dua kali di
        // rumusnya, dan menyalin ekspresinya berarti menghitungnya dua kali.
        const std::string a = NextLocal("lerp_a");
        const std::string b = NextLocal("lerp_b");
        EmitComment("-- node " + ShortId(node.guid) + " (" + type->label + ")");
        Mark(node.guid);
        Emit("local " + a + " = " + input("a"));
        Emit("local " + b + " = " + input("b"));
        value.outputs["result"] = "(" + a + " + (" + b + " - " + a + ") * " + input("t") + ")";
        return value;
    }

    if (key == "vec3.make") {
        value.outputs["value"] =
            "sim.vec3(" + input("x") + ", " + input("y") + ", " + input("z") + ")";
        return value;
    }
    if (key == "vec3.break") {
        const std::string local = NextLocal("vec");
        EmitComment("-- node " + ShortId(node.guid) + " (" + type->label + ")");
        Mark(node.guid);
        Emit("local " + local + " = " + input("value"));
        value.outputs["x"] = local + ".x";
        value.outputs["y"] = local + ".y";
        value.outputs["z"] = local + ".z";
        return value;
    }
    if (key == "vec3.scale") {
        value.outputs["result"] = "(" + input("value") + " * " + input("scale") + ")";
        return value;
    }

    if (key == "sim.time") {
        value.outputs["value"] = "sim.time()";
        return value;
    }
    if (key == "sim.self") {
        value.outputs["entity"] = "self.entity";
        return value;
    }
    if (key == "sim.up" || key == "sim.right" || key == "sim.forward") {
        value.outputs["value"] = key + "()";
        return value;
    }
    if (key == "sim.axis_angle") {
        value.outputs["value"] =
            "sim.axis_angle(" + input("axis") + ", " + input("radians") + ")";
        return value;
    }
    // Komentar dan grup tidak menghasilkan apa pun. Keduanya tidak punya pin,
    // jadi penelusuran tidak akan pernah sampai ke sini lewat koneksi — cabang
    // ini menjaga graph yang menyebutnya secara langsung tetap terkompilasi.
    if (key == "comment" || key == "group") {
        return value;
    }
    // Nilai pin keluaran node Input dan Call disediakan lewat override — nama
    // parameter fungsi dan local hasil panggilan. Sampai di sini berarti
    // override-nya tidak ada, dan itu selalu berarti graph-nya belum utuh.
    if (key == "graph.input") {
        Fail(node.guid, "this Input pin is not part of the subgraph interface");
        return value;
    }

    Fail(node.guid, "node type '" + key + "' cannot be used as a value");
    return value;
}

bool Compiler::ValidateSubgraph(const Graph& target, const Uuid& guid,
                                const GraphNode*& entry) {
    const std::string name = options_.library->NameOf(guid);
    entry = nullptr;
    for (const GraphNode& node : target.nodes) {
        if (node.type == "graph.input") {
            if (entry != nullptr) {
                Fail(Uuid{}, "subgraph '" + name + "' has more than one Input node");
                return false;
            }
            entry = &node;
        }
        // Sebuah graph berjalan ketika dipanggil ATAU ketika sebuah event
        // menyala, tidak keduanya. Membiarkan keduanya berarti event-nya
        // diabaikan diam-diam saat graph itu dipanggil — perbedaan perilaku
        // yang tidak terlihat di kanvas mana pun.
        if (node.type == "event.start" || node.type == "event.update") {
            Fail(Uuid{}, "subgraph '" + name +
                             "' also has an event node; a graph either runs when called "
                             "or when an event fires, not both");
            return false;
        }
    }
    if (entry == nullptr) {
        Fail(Uuid{}, "'" + name + "' has no Input node, so there is nowhere to start");
        return false;
    }
    // Variabel graph adalah keadaan yang bertahan antar frame. Di dalam sesuatu
    // yang dipanggil dari beberapa tempat, "bertahan milik siapa" tidak punya
    // jawaban yang bisa dipertahankan — jadi ditolak, bukan ditebak.
    if (!target.variables.empty()) {
        Fail(Uuid{}, "subgraph '" + name +
                         "' declares variables; pass values through its Input and Output "
                         "pins instead");
        return false;
    }
    return true;
}

std::string Compiler::EmitSubgraphFunction(const Uuid& guid) {
    if (const auto known = subgraphs_.find(guid); known != subgraphs_.end()) {
        return known->second;
    }
    if (options_.library == nullptr) {
        Fail(Uuid{}, "this graph calls another graph, but no graph library was provided");
        return {};
    }
    if (std::find(graphStack_.begin(), graphStack_.end(), guid) != graphStack_.end()) {
        // Lingkar antar-graph: A memanggil B yang memanggil A. Ditolak dengan
        // menyebut graph-nya, bukan node-nya — yang salah adalah rujukannya.
        Fail(Uuid{}, "graph '" + options_.library->NameOf(guid) +
                         "' calls itself, directly or through another graph");
        return {};
    }
    const Graph* target = options_.library->Find(guid);
    if (target == nullptr) {
        Fail(Uuid{}, "cannot open the graph this node refers to");
        return {};
    }
    const GraphNode* entry = nullptr;
    if (!ValidateSubgraph(*target, guid, entry)) {
        return {};
    }

    const std::string name = "sub_" + Sanitize(options_.library->NameOf(guid)) + "_" +
                             std::to_string(subgraphs_.size() + 1);

    // Keadaan penelusuran milik graph yang sedang dikerjakan. Disimpan dan
    // dikembalikan supaya menyisipkan subgraph di tengah tidak mengacaukan
    // cache, override, dan deteksi lingkar milik pemanggilnya.
    const Graph* outerGraph = graph_;
    auto outerPure = std::move(pureCache_);
    auto outerEvaluating = std::move(evaluating_);
    auto outerExec = std::move(execStack_);
    auto outerOverrides = std::move(overrides_);
    const int outerIndent = indent_;

    graph_ = target;
    pureCache_.clear();
    evaluating_.clear();
    execStack_.clear();
    overrides_.clear();
    indent_ = 0;
    graphStack_.push_back(guid);

    // Fungsi bersarang milik subgraph ini dikeluarkan lebih dulu: Lua menuntut
    // sebuah local sudah ada sebelum dipakai.
    for (const GraphNode& node : target->nodes) {
        if (node.type == "graph.call") {
            EmitSubgraphFunction(Uuid::Parse(node.Setting("graph")));
        }
    }

    std::string parameters = "self";
    for (const GraphPort& port : target->inputs) {
        const std::string local = Sanitize(port.name);
        parameters += ", " + local;
        overrides_[{entry->guid, port.name}] = local;
    }

    EmitBlank();
    EmitRaw("-- Subgraph: " + options_.library->NameOf(guid));
    EmitRaw("local function " + name + "(" + parameters + ")");
    ++indent_;
    EmitExecFrom(*entry, "then");
    --indent_;
    EmitRaw("end");

    graphStack_.pop_back();
    graph_ = outerGraph;
    pureCache_ = std::move(outerPure);
    evaluating_ = std::move(outerEvaluating);
    execStack_ = std::move(outerExec);
    overrides_ = std::move(outerOverrides);
    indent_ = outerIndent;

    subgraphs_.emplace(guid, name);
    return name;
}

void Compiler::EmitNode(const GraphNode& node) {
    const NodeType* type = NodeCatalog::Get().Find(node.type);
    if (type == nullptr) {
        Fail(node.guid, "unknown node type '" + node.type + "'");
        return;
    }
    // Lingkar pada pin exec akan membuat penelusuran ini tidak pernah berhenti.
    // Ditolak dengan pesan, bukan dibiarkan menggantungkan editor.
    if (!execStack_.insert(node.guid).second) {
        Fail(node.guid, "execution pins form a loop through '" + type->label +
                            "'; use a For Loop or While Loop instead");
        return;
    }

    // Breakpoint disisipkan sebagai pemanggilan biasa, bukan mekanisme runtime
    // tersendiri. Konsekuensinya menyenangkan: berkas yang memuatnya tetap
    // berjalan apa adanya di runtime yang tidak memasang penangan apa pun.
    if (std::find(options_.breakpoints.begin(), options_.breakpoints.end(), node.guid) !=
        options_.breakpoints.end()) {
        Emit("sim.breakpoint(" + Quote(node.guid.ToString()) + ")");
    }

    const std::vector<GraphPin> pins = PinsOf(*graph_, node, options_.library);
    const auto input = [&](const char* name) -> std::string {
        const auto it = std::find_if(pins.begin(), pins.end(), [name](const GraphPin& pin) {
            return pin.name == name && pin.direction == PinDirection::Input;
        });
        return it == pins.end() ? "nil" : InputExpr(node, *it);
    };

    const std::string& key = node.type;

    if (key == "flow.branch") {
        const std::string condition = input("condition");
        EmitComment("-- node " + ShortId(node.guid) + " (Branch)");
        Mark(node.guid);
        Emit("if " + condition + " then");
        ++indent_;
        EmitExecFrom(node, "true");
        --indent_;
        if (graph_->LinkInto(node.guid, "false") != nullptr ||
            !graph_->LinksFrom(node.guid, "false").empty()) {
            Emit("else");
            ++indent_;
            EmitExecFrom(node, "false");
            --indent_;
        }
        Emit("end");
    } else if (key == "flow.sequence") {
        EmitComment("-- node " + ShortId(node.guid) + " (Sequence)");
        Mark(node.guid);
        for (const GraphPin& pin : pins) {
            if (pin.direction == PinDirection::Output && pin.kind == PinKind::Exec) {
                EmitExecFrom(node, pin.name);
            }
        }
    } else if (key == "flow.while") {
        EmitComment("-- node " + ShortId(node.guid) + " (While Loop)");
        Mark(node.guid);
        // `while true` dengan break di dalam, bukan `while <cond>`: menarik
        // nilai sebuah pin bisa menghasilkan pernyataan (get_component, misalnya),
        // dan pernyataan tidak muat di dalam kepala `while`. Bentuk ini juga
        // yang membuat kondisinya benar-benar dihitung ulang tiap putaran.
        Emit("while true do");
        ++indent_;
        InvalidatePure();
        const std::string condition = input("condition");
        Emit("if not (" + condition + ") then break end");
        EmitExecFrom(node, "body");
        --indent_;
        Emit("end");
        InvalidatePure();
        EmitExecFrom(node, "completed");
    } else if (key == "flow.for") {
        const std::string first = input("first");
        const std::string last = input("last");
        const std::string step = input("step");
        const std::string counter = NextLocal("i");
        EmitComment("-- node " + ShortId(node.guid) + " (For Loop)");
        Mark(node.guid);
        Emit("for " + counter + " = " + first + ", " + last + ", " + step + " do");
        ++indent_;
        InvalidatePure();
        overrides_[{node.guid, "index"}] = counter;
        EmitExecFrom(node, "body");
        overrides_.erase({node.guid, "index"});
        --indent_;
        Emit("end");
        InvalidatePure();
        EmitExecFrom(node, "completed");
    } else if (key == "variable.set") {
        const std::string name = node.Setting("variable");
        if (graph_->FindVariable(name) == nullptr) {
            Fail(node.guid, "no graph variable named '" + name + "'");
        } else {
            const std::string expr = input("value");
            EmitComment("-- node " + ShortId(node.guid) + " (Set " + name + ")");
            Mark(node.guid);
            Emit("self.state." + Sanitize(name) + " = " + expr);
            InvalidatePure();
        }
        EmitExecFrom(node, "then");
    } else if (key == "graph.output") {
        std::string values;
        for (const GraphPort& port : graph_->outputs) {
            const auto pin = std::find_if(pins.begin(), pins.end(), [&port](const GraphPin& p) {
                return p.name == port.name && p.direction == PinDirection::Input;
            });
            values += (values.empty() ? "" : ", ");
            values += pin == pins.end() ? "nil" : InputExpr(node, *pin);
        }
        EmitComment("-- node " + ShortId(node.guid) + " (Output)");
        Mark(node.guid);
        Emit(values.empty() ? "return" : "return " + values);
        // Sengaja tidak menelusuri lebih jauh: `return` mengakhiri fungsinya,
        // dan Lua menolak pernyataan apa pun sesudahnya di blok yang sama.
    } else if (key == "graph.call") {
        const Uuid target = Uuid::Parse(node.Setting("graph"));
        const std::string function = EmitSubgraphFunction(target);
        if (function.empty()) {
            Fail(node.guid, "this Call Graph node does not refer to a usable graph");
        } else {
            std::string arguments = "self";
            std::vector<std::string> results;
            for (const GraphPin& pin : pins) {
                if (pin.kind == PinKind::Exec) {
                    continue;
                }
                if (pin.direction == PinDirection::Input) {
                    arguments += ", " + InputExpr(node, pin);
                } else {
                    const std::string local = NextLocal(pin.name);
                    overrides_[{node.guid, pin.name}] = local;
                    results.push_back(local);
                }
            }
            EmitComment("-- node " + ShortId(node.guid) + " (Call " +
                        (options_.library != nullptr ? options_.library->NameOf(target)
                                                     : std::string("graph")) +
                        ")");
            Mark(node.guid);
            std::string call = function + "(" + arguments + ")";
            if (!results.empty()) {
                std::string names;
                for (std::size_t i = 0; i < results.size(); ++i) {
                    names += (i == 0 ? "" : ", ") + results[i];
                }
                call = "local " + names + " = " + call;
            }
            Emit(call);
            InvalidatePure();
        }
        EmitExecFrom(node, "then");
    } else if (key == "sim.log") {
        const std::string message = input("message");
        EmitComment("-- node " + ShortId(node.guid) + " (Log)");
        Mark(node.guid);
        Emit("sim.log(tostring(" + message + "))");
        EmitExecFrom(node, "then");
    } else if (key.rfind("component.set.", 0) == 0) {
        const std::string component = key.substr(std::string("component.set.").size());
        const std::string entity = input("entity");
        // Hanya field yang benar-benar diberi nilai yang masuk tabel. Itu bukan
        // penghematan melainkan syarat: `sim.set_component` hanya menulis field
        // yang ada di tabelnya, jadi menyertakan yang kosong akan mengembalikan
        // rotasi dan skala entity ke nilai netral setiap kali posisinya disetel.
        std::vector<std::string> assignments;
        for (const GraphPin& pin : pins) {
            if (pin.direction != PinDirection::Input || pin.kind == PinKind::Exec ||
                pin.name == "entity") {
                continue;
            }
            const bool connected = graph_->LinkInto(node.guid, pin.name) != nullptr;
            const auto explicitValue = node.pinValues.find(pin.name);
            const bool hasLiteral =
                explicitValue != node.pinValues.end() && !explicitValue->second.empty();
            if (!connected && !hasLiteral) {
                continue;
            }
            assignments.push_back(pin.name + " = " + InputExpr(node, pin));
        }
        EmitComment("-- node " + ShortId(node.guid) + " (Set " + component + ")");
        Mark(node.guid);
        if (assignments.empty()) {
            Emit("-- tidak ada field yang disetel");
        } else {
            std::string table;
            for (std::size_t i = 0; i < assignments.size(); ++i) {
                table += (i == 0 ? "" : ", ") + assignments[i];
            }
            Emit("sim.set_component(" + entity + ", " + Quote(component) + ", { " + table +
                 " })");
        }
        InvalidatePure();
        EmitExecFrom(node, "then");
    } else {
        Fail(node.guid, "node type '" + key + "' cannot be placed in an execution chain");
    }

    execStack_.erase(node.guid);
}

void Compiler::EmitExecFrom(const GraphNode& node, std::string_view pin) {
    const std::vector<const GraphLink*> links = graph_->LinksFrom(node.guid, pin);
    if (links.empty()) {
        return;
    }
    // Satu pin exec keluaran hanya boleh punya satu tujuan; kalau tidak, urutan
    // jalannya ditentukan urutan penyimpanan link — sesuatu yang tidak terlihat
    // di kanvas. Sequence adalah cara yang benar untuk bercabang.
    if (links.size() > 1) {
        Fail(node.guid, "execution pin '" + std::string(pin) +
                            "' has more than one connection; use a Sequence node");
    }
    const GraphNode* target = graph_->FindNode(links.front()->toNode);
    if (target != nullptr) {
        EmitNode(*target);
    }
}

void Compiler::EmitEvent(const GraphNode& node, const char* method, const char* parameters) {
    EmitBlank();
    Mark(node.guid);
    EmitRaw(std::string("function Graph:") + method + "(" + parameters + ")");
    ++indent_;
    if (!graph_->variables.empty()) {
        Emit("self:__ensure()");
    }
    InvalidatePure();
    if (node.type == "event.update") {
        overrides_[{node.guid, "dt"}] = "dt";
    }
    EmitExecFrom(node, "then");
    overrides_.clear();
    --indent_;
    EmitRaw("end");
}

void Compiler::EmitEnsure() {
    if (graph_->variables.empty()) {
        return;
    }
    EmitBlank();
    EmitRaw("-- Menyiapkan variabel graph sekali per instance. Yang terekspos");
    EmitRaw("-- mengambil nilainya dari Inspector; sisanya memakai bawaan graph.");
    EmitRaw("function Graph:__ensure()");
    ++indent_;
    Emit("if self.state.__ready then");
    ++indent_;
    Emit("return");
    --indent_;
    Emit("end");
    Emit("self.state.__ready = true");
    Emit("local props = self.props or {}");
    for (const GraphVariable& variable : graph_->variables) {
        const std::string name = Sanitize(variable.name);
        const std::string fallback =
            variable.defaultValue.empty() ? NeutralLiteral(variable.kind) : variable.defaultValue;
        if (variable.exposed) {
            Emit("self.state." + name + " = props." + name);
            // Perbandingan dengan nil, bukan `or`: sebuah properti bernilai
            // false atau 0 juga sah, dan `or` akan diam-diam menggantinya
            // dengan bawaan.
            Emit("if self.state." + name + " == nil then self.state." + name + " = " + fallback +
                 " end");
        } else {
            Emit("self.state." + name + " = " + fallback);
        }
    }
    --indent_;
    EmitRaw("end");
}

CompileResult Compiler::Run() {
    EmitRaw("-- Dihasilkan dari " +
            (chunkName_.empty() ? std::string("sebuah graph") : chunkName_) + ".");
    EmitRaw("--");
    EmitRaw("-- Jangan disunting tangan: berkas ini ditimpa setiap graph-nya");
    EmitRaw("-- dikompilasi ulang. Sunting graph-nya, lalu simpan.");
    EmitBlank();
    EmitRaw("local Graph = {}");

    // Properti yang diekspos ditulis sebagai deklarasi `properties` biasa —
    // bentuk yang sama persis dengan skrip tulis tangan. Karena itu Inspector,
    // serialisasi level, dan undo berlaku untuk graph tanpa satu baris pun kode
    // khusus di sisi editor.
    std::vector<const GraphVariable*> exposed;
    for (const GraphVariable& variable : graph_->variables) {
        if (variable.exposed) {
            exposed.push_back(&variable);
        }
    }
    if (!exposed.empty()) {
        EmitBlank();
        EmitRaw("Graph.properties = {");
        ++indent_;
        for (const GraphVariable* variable : exposed) {
            const std::string fallback = variable->defaultValue.empty()
                                             ? NeutralLiteral(variable->kind)
                                             : variable->defaultValue;
            Emit(Sanitize(variable->name) + " = " + fallback + ",");
        }
        --indent_;
        EmitRaw("}");
    }

    // Subgraph dikeluarkan sebagai fungsi lokal SEBELUM handler mana pun:
    // Lua menuntut sebuah local sudah ada sebelum dipakai, dan handler-lah yang
    // memakainya.
    for (const GraphNode& node : graph_->nodes) {
        if (node.type == "graph.call") {
            EmitSubgraphFunction(Uuid::Parse(node.Setting("graph")));
        }
    }

    EmitEnsure();

    // Urutan tetap: OnStart lebih dulu, lalu OnUpdate. Dua graph dengan isi sama
    // tapi urutan node berbeda harus menghasilkan berkas yang sama.
    struct EventKind {
        const char* type;
        const char* method;
        const char* parameters;
    };
    constexpr std::array<EventKind, 2> kEvents{{
        {"event.start", "OnStart", ""},
        {"event.update", "OnUpdate", "dt"},
    }};

    for (const EventKind& event : kEvents) {
        int seen = 0;
        for (const GraphNode& node : graph_->nodes) {
            if (node.type != event.type) {
                continue;
            }
            if (++seen > 1) {
                Fail(node.guid, std::string("more than one ") + event.method +
                                    " event; a graph can only have one");
                continue;
            }
            EmitEvent(node, event.method, event.parameters);
        }
    }

    // Sebuah subgraph yang dikompilasi sendirian sah dan memang tidak punya
    // handler: ia berjalan ketika dipanggil. Dikatakan di berkasnya supaya
    // tidak terbaca sebagai hasil kompilasi yang gagal separuh.
    if (graph_->IsSubgraph()) {
        EmitBlank();
        EmitRaw("-- Graph ini sebuah subgraph: ia berjalan ketika dipanggil graph lain,");
        EmitRaw("-- jadi ia sengaja tidak punya OnStart maupun OnUpdate.");
    }

    for (const auto& [guid, function] : subgraphs_) {
        result_.referencedGraphs.push_back(guid);
    }

    EmitBlank();
    EmitRaw("return Graph");

    std::string text;
    for (const std::string& line : lines_) {
        text += line;
        text += '\n';
    }
    result_.lua = std::move(text);
    result_.ok = result_.errors.empty();
    return std::move(result_);
}

}  // namespace

Uuid CompileResult::NodeAtLine(int line) const {
    // Entri terdekat yang tidak melewati `line`: sebuah node menandai baris
    // pertamanya, dan baris-baris sesudahnya milik node yang sama sampai node
    // berikutnya menandai miliknya.
    //
    // Ketika beberapa node berbagi baris — pernyataan beserta node murni yang
    // disisipkan ke dalamnya — yang menang adalah yang tercatat lebih dulu,
    // yaitu pernyataannya. Node murni yang ikut di baris itu tetap terjangkau
    // lewat NodesAtLine().
    Uuid best;
    int bestLine = 0;
    for (const SourceMapEntry& entry : sourceMap) {
        if (entry.line <= line && entry.line > bestLine) {
            bestLine = entry.line;
            best = entry.node;
        }
    }
    return best;
}

std::vector<Uuid> CompileResult::NodesAtLine(int line) const {
    std::vector<Uuid> nodes;
    for (const SourceMapEntry& entry : sourceMap) {
        if (entry.line == line &&
            std::find(nodes.begin(), nodes.end(), entry.node) == nodes.end()) {
            nodes.push_back(entry.node);
        }
    }
    // Pernyataan yang mencakup baris ini ikut disertakan bila belum ada —
    // sebuah baris di tengah blok `if` tetap milik node Branch yang membukanya.
    const Uuid covering = NodeAtLine(line);
    if (covering.IsValid() && std::find(nodes.begin(), nodes.end(), covering) == nodes.end()) {
        nodes.push_back(covering);
    }
    return nodes;
}

int CompileResult::LineOfNode(const Uuid& node) const {
    for (const SourceMapEntry& entry : sourceMap) {
        if (entry.node == node) {
            return entry.line;
        }
    }
    return 0;
}

CompileResult CompileGraph(const Graph& graph, std::string_view chunkName,
                           const CompileOptions& options) {
    Compiler compiler(graph, chunkName, options);
    return compiler.Run();
}

}  // namespace sim::script
