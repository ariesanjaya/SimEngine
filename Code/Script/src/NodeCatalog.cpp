#include "Sim/Script/NodeCatalog.h"

#include "Sim/Scene/ComponentRegistry.h"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>

namespace sim::script {
namespace {

GraphPin Exec(std::string name, PinDirection direction, std::string label = {}) {
    GraphPin pin;
    pin.name = std::move(name);
    pin.label = std::move(label);
    pin.kind = PinKind::Exec;
    pin.direction = direction;
    return pin;
}

GraphPin In(std::string name, PinKind kind, std::string defaultValue, std::string label = {}) {
    GraphPin pin;
    pin.name = std::move(name);
    pin.label = std::move(label);
    pin.kind = kind;
    pin.direction = PinDirection::Input;
    pin.defaultValue = std::move(defaultValue);
    return pin;
}

GraphPin Out(std::string name, PinKind kind, std::string label = {}) {
    GraphPin pin;
    pin.name = std::move(name);
    pin.label = std::move(label);
    pin.kind = kind;
    pin.direction = PinDirection::Output;
    return pin;
}

/// Pin data untuk sebuah field komponen, atau nullopt bila tipenya belum bisa
/// dialirkan lewat graph.
///
/// Yang dilewati — Vec2, Vec4, enum, struct bersarang, vektor, rujukan aset —
/// sengaja tidak muncul sebagai pin sama sekali. Menampilkannya sebagai pin
/// bertipe Any yang diam-diam gagal saat dikompilasi jauh lebih membingungkan
/// daripada tidak menawarkannya.
bool PinKindForField(reflect::FieldKind kind, PinKind& out) {
    using reflect::FieldKind;
    switch (kind) {
        case FieldKind::Bool: out = PinKind::Bool; return true;
        case FieldKind::Int:
        case FieldKind::UInt:
        case FieldKind::Float: out = PinKind::Number; return true;
        case FieldKind::String: out = PinKind::String; return true;
        case FieldKind::Vec3: out = PinKind::Vec3; return true;
        case FieldKind::Quat: out = PinKind::Quat; return true;
        default: return false;
    }
}

std::unique_ptr<NodeCatalog>& Storage() {
    static std::unique_ptr<NodeCatalog> catalog;
    return catalog;
}

}  // namespace

const GraphPin* NodeType::FindPin(std::string_view name) const {
    const auto it = std::find_if(pins.begin(), pins.end(),
                                 [name](const GraphPin& pin) { return pin.name == name; });
    return it == pins.end() ? nullptr : &*it;
}

std::vector<const GraphPin*> NodeType::Inputs() const {
    std::vector<const GraphPin*> result;
    for (const GraphPin& pin : pins) {
        if (pin.direction == PinDirection::Input) {
            result.push_back(&pin);
        }
    }
    return result;
}

std::vector<const GraphPin*> NodeType::Outputs() const {
    std::vector<const GraphPin*> result;
    for (const GraphPin& pin : pins) {
        if (pin.direction == PinDirection::Output) {
            result.push_back(&pin);
        }
    }
    return result;
}

const GraphPin* NodeType::ExecInput() const {
    for (const GraphPin& pin : pins) {
        if (pin.kind == PinKind::Exec && pin.direction == PinDirection::Input) {
            return &pin;
        }
    }
    return nullptr;
}

std::string NodeCatalog::ComponentGetKey(std::string_view component) {
    return "component.get." + std::string(component);
}

std::string NodeCatalog::ComponentSetKey(std::string_view component) {
    return "component.set." + std::string(component);
}

const NodeCatalog& NodeCatalog::Get() {
    std::unique_ptr<NodeCatalog>& catalog = Storage();
    if (catalog == nullptr) {
        catalog.reset(new NodeCatalog());
    }
    return *catalog;
}

void NodeCatalog::Rebuild() {
    Storage().reset(new NodeCatalog());
}

NodeCatalog::NodeCatalog() {
    AddCoreTypes();
    AddComponentTypes();
}

const NodeType* NodeCatalog::Find(std::string_view key) const {
    const auto it = std::find_if(types_.begin(), types_.end(),
                                 [key](const NodeType& type) { return type.key == key; });
    return it == types_.end() ? nullptr : &*it;
}

void NodeCatalog::AddCoreTypes() {
    const auto add = [this](NodeType type) { types_.push_back(std::move(type)); };

    // --- Event -------------------------------------------------------------
    //
    // Node event tidak punya pin exec masuk: ia adalah tempat sebuah rantai
    // eksekusi dimulai. OnCollision yang direncanakan belum ada di sini —
    // fisika baru datang di E9, dan node yang tidak pernah bisa menyala adalah
    // janji yang tidak bisa ditepati katalog ini.
    add({"event.start",
         "On Start",
         "Event",
         false,
         "Berjalan sekali saat Play ditekan",
         {Exec("then", PinDirection::Output)}});
    add({"event.update",
         "On Update",
         "Event",
         false,
         "Berjalan setiap frame",
         {Exec("then", PinDirection::Output), Out("dt", PinKind::Number, "dt")}});

    // --- Alur --------------------------------------------------------------
    add({"flow.branch",
         "Branch",
         "Flow",
         false,
         "Memilih cabang menurut sebuah kondisi",
         {Exec("in", PinDirection::Input),
          In("condition", PinKind::Bool, "false"),
          Exec("true", PinDirection::Output),
          Exec("false", PinDirection::Output)}});
    // Jumlah keluarannya ditentukan setelan "count"; pin-nya dibangkitkan
    // kompiler dan panel dari angka itu, bukan didaftarkan di sini.
    add({"flow.sequence",
         "Sequence",
         "Flow",
         false,
         "Menjalankan beberapa cabang berurutan",
         {Exec("in", PinDirection::Input), Exec("then0", PinDirection::Output),
          Exec("then1", PinDirection::Output)}});
    add({"flow.while",
         "While Loop",
         "Flow",
         false,
         "Mengulang selama kondisinya benar",
         {Exec("in", PinDirection::Input),
          In("condition", PinKind::Bool, "false"),
          Exec("body", PinDirection::Output),
          Exec("completed", PinDirection::Output)}});
    add({"flow.for",
         "For Loop",
         "Flow",
         false,
         "Mengulang dari `first` sampai `last`",
         {Exec("in", PinDirection::Input),
          In("first", PinKind::Number, "1"),
          In("last", PinKind::Number, "10"),
          In("step", PinKind::Number, "1"),
          Exec("body", PinDirection::Output),
          Out("index", PinKind::Number, "index"),
          Exec("completed", PinDirection::Output)}});

    // --- Variabel ----------------------------------------------------------
    //
    // Tipe pin-nya mengikuti deklarasi variabel di graph, jadi yang tertulis di
    // sini hanya bentuk umumnya; kompiler dan panel membacanya dari setelan
    // "variable".
    add({"variable.get",
         "Get Variable",
         "Variable",
         true,
         "Membaca variabel graph",
         {Out("value", PinKind::Any, "value")}});
    add({"variable.set",
         "Set Variable",
         "Variable",
         false,
         "Menulis variabel graph",
         {Exec("in", PinDirection::Input), In("value", PinKind::Any, ""),
          Exec("then", PinDirection::Output)}});

    // --- Literal -----------------------------------------------------------
    add({"literal.number", "Number", "Literal", true, "Angka tetap",
         {Out("value", PinKind::Number, "value")}});
    add({"literal.bool", "Boolean", "Literal", true, "true atau false",
         {Out("value", PinKind::Bool, "value")}});
    add({"literal.string", "String", "Literal", true, "Teks tetap",
         {Out("value", PinKind::String, "value")}});
    add({"literal.vec3", "Vector 3", "Literal", true, "Vektor tetap",
         {Out("value", PinKind::Vec3, "value")}});

    // --- Matematika --------------------------------------------------------
    const auto binaryNumber = [&add](std::string key, std::string label, std::string tooltip) {
        add({std::move(key),
             std::move(label),
             "Math",
             true,
             std::move(tooltip),
             {In("a", PinKind::Number, "0"), In("b", PinKind::Number, "0"),
              Out("result", PinKind::Number, "result")}});
    };
    binaryNumber("math.add", "Add", "a + b");
    binaryNumber("math.subtract", "Subtract", "a - b");
    binaryNumber("math.multiply", "Multiply", "a * b");
    binaryNumber("math.divide", "Divide", "a / b");
    binaryNumber("math.min", "Min", "Yang terkecil di antara a dan b");
    binaryNumber("math.max", "Max", "Yang terbesar di antara a dan b");

    const auto unaryNumber = [&add](std::string key, std::string label, std::string tooltip) {
        add({std::move(key),
             std::move(label),
             "Math",
             true,
             std::move(tooltip),
             {In("value", PinKind::Number, "0"), Out("result", PinKind::Number, "result")}});
    };
    unaryNumber("math.sin", "Sin", "Sinus, dalam radian");
    unaryNumber("math.cos", "Cos", "Kosinus, dalam radian");
    unaryNumber("math.abs", "Abs", "Nilai mutlak");
    unaryNumber("math.negate", "Negate", "-value");

    add({"math.clamp",
         "Clamp",
         "Math",
         true,
         "Menjepit nilai ke rentang",
         {In("value", PinKind::Number, "0"), In("min", PinKind::Number, "0"),
          In("max", PinKind::Number, "1"), Out("result", PinKind::Number, "result")}});
    add({"math.lerp",
         "Lerp",
         "Math",
         true,
         "Interpolasi linear a→b",
         {In("a", PinKind::Number, "0"), In("b", PinKind::Number, "1"),
          In("t", PinKind::Number, "0"), Out("result", PinKind::Number, "result")}});

    const auto compare = [&add](std::string key, std::string label, std::string tooltip) {
        add({std::move(key),
             std::move(label),
             "Math",
             true,
             std::move(tooltip),
             {In("a", PinKind::Number, "0"), In("b", PinKind::Number, "0"),
              Out("result", PinKind::Bool, "result")}});
    };
    compare("compare.greater", "Greater", "a > b");
    compare("compare.less", "Less", "a < b");
    compare("compare.equal", "Equal", "a == b");

    add({"logic.and",
         "And",
         "Logic",
         true,
         "a dan b",
         {In("a", PinKind::Bool, "false"), In("b", PinKind::Bool, "false"),
          Out("result", PinKind::Bool, "result")}});
    add({"logic.or",
         "Or",
         "Logic",
         true,
         "a atau b",
         {In("a", PinKind::Bool, "false"), In("b", PinKind::Bool, "false"),
          Out("result", PinKind::Bool, "result")}});
    add({"logic.not",
         "Not",
         "Logic",
         true,
         "Kebalikan dari value",
         {In("value", PinKind::Bool, "false"), Out("result", PinKind::Bool, "result")}});

    // --- Vektor ------------------------------------------------------------
    add({"vec3.make",
         "Make Vector 3",
         "Vector",
         true,
         "Menyusun vektor dari tiga angka",
         {In("x", PinKind::Number, "0"), In("y", PinKind::Number, "0"),
          In("z", PinKind::Number, "0"), Out("value", PinKind::Vec3, "value")}});
    add({"vec3.break",
         "Break Vector 3",
         "Vector",
         true,
         "Memecah vektor jadi tiga angka",
         {In("value", PinKind::Vec3, "sim.vec3(0, 0, 0)"), Out("x", PinKind::Number, "x"),
          Out("y", PinKind::Number, "y"), Out("z", PinKind::Number, "z")}});
    add({"vec3.add",
         "Add Vectors",
         "Vector",
         true,
         "a + b",
         {In("a", PinKind::Vec3, "sim.vec3(0, 0, 0)"),
          In("b", PinKind::Vec3, "sim.vec3(0, 0, 0)"), Out("result", PinKind::Vec3, "result")}});
    add({"vec3.scale",
         "Scale Vector",
         "Vector",
         true,
         "Mengalikan vektor dengan skalar",
         {In("value", PinKind::Vec3, "sim.vec3(0, 0, 0)"), In("scale", PinKind::Number, "1"),
          Out("result", PinKind::Vec3, "result")}});

    // --- Panggilan engine --------------------------------------------------
    add({"sim.log",
         "Log",
         "Engine",
         false,
         "Menulis pesan ke Console",
         {Exec("in", PinDirection::Input), In("message", PinKind::Any, "\"\""),
          Exec("then", PinDirection::Output)}});
    add({"sim.time",
         "Time",
         "Engine",
         true,
         "Detik sejak Play ditekan",
         {Out("value", PinKind::Number, "value")}});
    add({"sim.self",
         "Self",
         "Engine",
         true,
         "Entity tempat graph ini menempel",
         {Out("entity", PinKind::Entity, "entity")}});
    add({"sim.axis_angle",
         "Axis Angle",
         "Engine",
         true,
         "Rotasi mengelilingi sebuah sumbu",
         {In("axis", PinKind::Vec3, "sim.up()"), In("radians", PinKind::Number, "0"),
          Out("value", PinKind::Quat, "value")}});
    add({"sim.up", "Up", "Engine", true, "Sumbu atas dunia",
         {Out("value", PinKind::Vec3, "value")}});
    add({"sim.right", "Right", "Engine", true, "Sumbu kanan dunia",
         {Out("value", PinKind::Vec3, "value")}});
    add({"sim.forward", "Forward", "Engine", true, "Sumbu depan dunia",
         {Out("value", PinKind::Vec3, "value")}});

    // --- Komentar dan grup -------------------------------------------------
    //
    // Tanpa pin sama sekali: keduanya tidak ikut dikompilasi, hanya ikut
    // tersimpan. Grup memindahkan node yang berada di dalamnya ketika digeser;
    // keanggotaannya ditentukan letak, bukan daftar, jadi tidak ada yang perlu
    // diperbarui ketika node ditambah atau dipindahkan.
    add({"comment", "Comment", "Misc", true, "Catatan di kanvas", {}});
    add({"group", "Group", "Misc", true,
         "Kotak yang memindahkan seluruh node di dalamnya", {}});
}

void NodeCatalog::AddComponentTypes() {
    for (const scene::ComponentOps& ops : scene::ComponentRegistry::Get().All()) {
        if (ops.type == nullptr) {
            continue;
        }
        const std::string& name = ops.type->name;

        NodeType getter;
        getter.key = ComponentGetKey(name);
        getter.label = "Get " + name;
        getter.category = "Component";
        getter.pure = true;
        getter.tooltip = "Membaca komponen " + name;
        getter.pins.push_back(In("entity", PinKind::Entity, "self.entity"));

        NodeType setter;
        setter.key = ComponentSetKey(name);
        setter.label = "Set " + name;
        setter.category = "Component";
        setter.pure = false;
        setter.tooltip = "Menulis komponen " + name;
        setter.pins.push_back(Exec("in", PinDirection::Input));
        setter.pins.push_back(In("entity", PinKind::Entity, "self.entity"));

        for (const reflect::FieldDesc& field : ops.type->fields) {
            PinKind kind = PinKind::Any;
            if (!PinKindForField(field.kind, kind)) {
                continue;
            }
            const std::string label = field.label.empty() ? field.name : field.label;
            getter.pins.push_back(Out(field.name, kind, label));
            // Bawaan kosong, dan itu bukan kelalaian: pin yang tidak tersambung
            // membuat field-nya TIDAK ikut ditulis. Kalau ia punya bawaan,
            // menyetel posisi lewat graph akan diam-diam mengembalikan rotasi
            // dan skala entity ke nilai netral.
            setter.pins.push_back(In(field.name, kind, "", label));
        }

        setter.pins.push_back(Exec("then", PinDirection::Output));

        types_.push_back(std::move(getter));
        types_.push_back(std::move(setter));
    }
}

std::vector<GraphPin> PinsOf(const Graph& graph, const GraphNode& node) {
    const NodeType* type = NodeCatalog::Get().Find(node.type);
    if (type == nullptr) {
        return {};
    }
    std::vector<GraphPin> pins = type->pins;

    if (node.type == "flow.sequence") {
        // Dua keluaran bawaan sudah ada di katalog; sisanya ditambahkan di sini.
        int count = 2;
        try {
            count = std::stoi(node.Setting("count", "2"));
        } catch (const std::exception&) {
            count = 2;
        }
        count = std::clamp(count, 1, 16);
        pins.erase(std::remove_if(pins.begin(), pins.end(),
                                  [](const GraphPin& pin) {
                                      return pin.direction == PinDirection::Output;
                                  }),
                   pins.end());
        for (int i = 0; i < count; ++i) {
            pins.push_back(Exec("then" + std::to_string(i), PinDirection::Output,
                                "then " + std::to_string(i)));
        }
        return pins;
    }

    if (node.type == "variable.get" || node.type == "variable.set") {
        const GraphVariable* variable = graph.FindVariable(node.Setting("variable"));
        if (variable != nullptr) {
            for (GraphPin& pin : pins) {
                if (pin.kind == PinKind::Any) {
                    pin.kind = variable->kind;
                }
            }
        }
    }
    return pins;
}

}  // namespace sim::script
