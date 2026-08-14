#include "Sim/Material/MaterialNodeCatalog.h"

#include <algorithm>

namespace sim::material {
namespace {

MaterialPin In(std::string name, ValueKind kind, std::string defaultValue,
               std::string label = {}) {
    MaterialPin pin;
    pin.name = std::move(name);
    pin.label = std::move(label);
    pin.kind = kind;
    pin.direction = PinDirection::Input;
    pin.defaultValue = std::move(defaultValue);
    return pin;
}

MaterialPin Out(std::string name, ValueKind kind, std::string label = {}) {
    MaterialPin pin;
    pin.name = std::move(name);
    pin.label = std::move(label);
    pin.kind = kind;
    pin.direction = PinDirection::Output;
    return pin;
}

}  // namespace

const MaterialPin* MaterialNodeType::FindPin(std::string_view name) const {
    const auto it = std::find_if(pins.begin(), pins.end(),
                                 [name](const MaterialPin& pin) { return pin.name == name; });
    return it == pins.end() ? nullptr : &*it;
}

std::vector<const MaterialPin*> MaterialNodeType::Inputs() const {
    std::vector<const MaterialPin*> result;
    for (const MaterialPin& pin : pins) {
        if (pin.direction == PinDirection::Input) {
            result.push_back(&pin);
        }
    }
    return result;
}

std::vector<const MaterialPin*> MaterialNodeType::Outputs() const {
    std::vector<const MaterialPin*> result;
    for (const MaterialPin& pin : pins) {
        if (pin.direction == PinDirection::Output) {
            result.push_back(&pin);
        }
    }
    return result;
}

MaterialNodeCatalog::MaterialNodeCatalog() {
    const auto add = [this](MaterialNodeType type) { types_.push_back(std::move(type)); };

    // --- Keluaran -----------------------------------------------------------
    //
    // Urutan dan nilai bawaannya mengikuti `OpenPBRSurface::defaults()` di
    // openpbr.slang. Menyalin angkanya ke sini memang menduplikasi, tapi
    // alternatifnya lebih buruk: material yang dibiarkan apa adanya di editor
    // akan terlihat berbeda dari material yang dibiarkan apa adanya di shader,
    // dan selisih itu tidak akan pernah dicari orang karena tidak ada yang
    // mengaku salah.
    add({std::string(kSurfaceOutputType),
         "Surface Output",
         "Output",
         "Permukaan akhir, mengikuti OpenPBR Surface v1.1",
         {
             In("baseWeight", ValueKind::Float, "1.0", "Base Weight"),
             In("baseColor", ValueKind::Float3, "float3(0.8)", "Base Color"),
             In("baseMetalness", ValueKind::Float, "0.0", "Base Metalness"),
             In("baseDiffuseRoughness", ValueKind::Float, "0.0", "Diffuse Roughness"),

             In("specularWeight", ValueKind::Float, "1.0", "Specular Weight"),
             In("specularColor", ValueKind::Float3, "float3(1.0)", "Specular Color"),
             In("specularRoughness", ValueKind::Float, "0.3", "Specular Roughness"),
             In("specularRoughnessAnisotropy", ValueKind::Float, "0.0", "Specular Anisotropy"),
             In("specularIor", ValueKind::Float, "1.5", "Specular IOR"),

             In("coatWeight", ValueKind::Float, "0.0", "Coat Weight"),
             In("coatColor", ValueKind::Float3, "float3(1.0)", "Coat Color"),
             In("coatRoughness", ValueKind::Float, "0.0", "Coat Roughness"),
             In("coatRoughnessAnisotropy", ValueKind::Float, "0.0", "Coat Anisotropy"),
             In("coatIor", ValueKind::Float, "1.6", "Coat IOR"),
             In("coatDarkening", ValueKind::Float, "1.0", "Coat Darkening"),

             In("fuzzWeight", ValueKind::Float, "0.0", "Fuzz Weight"),
             In("fuzzColor", ValueKind::Float3, "float3(1.0)", "Fuzz Color"),
             In("fuzzRoughness", ValueKind::Float, "0.5", "Fuzz Roughness"),

             // Di luar OpenPBRSurface: ketiganya tidak ikut dalam campuran lobe.
             // Normal membelokkan bingkai shading sebelum BRDF dievaluasi,
             // emissive ditambahkan setelahnya, dan opacity dipakai saat
             // menggabungkan ke target.
             In("normal", ValueKind::Float3, "float3(0.0, 0.0, 1.0)", "Normal (tangent space)"),
             In("emissive", ValueKind::Float3, "float3(0.0)", "Emissive"),
             In("opacity", ValueKind::Float, "1.0", "Opacity"),
         }});

    // --- Input --------------------------------------------------------------
    add({"input.texture",
         "Texture",
         "Input",
         "Tekstur yang dirujuk material ini",
         {Out("texture", ValueKind::Texture)}});

    add({"input.sample",
         "Sample Texture",
         "Input",
         "Membaca tekstur pada sebuah koordinat",
         {
             In("texture", ValueKind::Texture, {}),
             In("uv", ValueKind::Float2, "inputs.uv0"),
             Out("rgba", ValueKind::Float4),
             Out("rgb", ValueKind::Float3),
             Out("r", ValueKind::Float),
             Out("g", ValueKind::Float),
             Out("b", ValueKind::Float),
             Out("a", ValueKind::Float),
         }});

    add({"input.uv", "UV", "Input", "Koordinat tekstur", {Out("uv", ValueKind::Float2)}});
    add({"input.vertexColor",
         "Vertex Color",
         "Input",
         "Warna per-vertex",
         {Out("color", ValueKind::Float4)}});
    add({"input.time", "Time", "Input", "Waktu berjalan, detik", {Out("time", ValueKind::Float)}});
    add({"input.normal",
         "World Normal",
         "Input",
         "Normal permukaan di ruang dunia",
         {Out("normal", ValueKind::Float3)}});
    add({"input.viewDirection",
         "View Direction",
         "Input",
         "Arah dari permukaan ke kamera, ternormalisasi",
         {Out("direction", ValueKind::Float3)}});

    add({"input.constant",
         "Constant",
         "Input",
         "Nilai tetap; tipenya ditentukan setelan 'kind'",
         {Out("value", ValueKind::Float)}});

    add({"param.get",
         "Parameter",
         "Input",
         "Nilai parameter yang bisa ditimpa material instance",
         {Out("value", ValueKind::Float)}});

    // Tekstur yang **dinyatakan sebagai parameter**, berbeda dari
    // `input.texture` yang menjadi bagian tetap definisi induknya.
    //
    // Pemisahan yang sama dengan skalar: `input.constant` tidak bisa ditimpa,
    // `param.get` bisa. Tanpa pemisahan ini, sebuah instance bisa menjangkau
    // tekstur mana pun di dalam graph induk — termasuk yang penulisnya
    // maksudkan tetap — dan "material instance" berhenti berarti apa pun.
    add({"param.texture",
         "Texture Parameter",
         "Input",
         "Tekstur yang bisa diisi material instance",
         {Out("texture", ValueKind::Texture)}});

    // --- Matematika ---------------------------------------------------------
    //
    // Pin bertipe Float dengan pelebaran skalar: menyambungkan float3 ke `a`
    // dan float ke `b` adalah bentuk paling umum di graph material, dan
    // aturan pelebaran Slang sudah menanganinya. Tipe hasil disimpulkan
    // kompiler dari yang paling lebar di antara masukannya.
    const auto binary = [&add](const char* key, const char* label, const char* tooltip) {
        add({key,
             label,
             "Math",
             tooltip,
             {In("a", ValueKind::Numeric, "0.0"), In("b", ValueKind::Numeric, "0.0"),
              Out("result", ValueKind::Numeric)}});
    };
    binary("math.add", "Add", "a + b");
    binary("math.subtract", "Subtract", "a - b");
    binary("math.multiply", "Multiply", "a * b");
    binary("math.divide", "Divide", "a / b");
    binary("math.min", "Min", "min(a, b)");
    binary("math.max", "Max", "max(a, b)");
    binary("math.power", "Power", "pow(a, b)");
    binary("math.step", "Step", "step(a, b)");

    // Dot didaftarkan tersendiri: satu-satunya node biner yang hasilnya selalu
    // skalar, apa pun lebar masukannya. Ikut lewat `binary` akan membuatnya
    // mengembalikan float3 untuk masukan float3, dan graph yang menyambungkan
    // hasilnya ke roughness baru gagal saat shader-nya dikompilasi.
    add({"math.dot",
         "Dot",
         "Math",
         "dot(a, b)",
         {In("a", ValueKind::Numeric, "0.0"), In("b", ValueKind::Numeric, "0.0"),
          Out("result", ValueKind::Float)}});

    const auto unary = [&add](const char* key, const char* label, const char* tooltip) {
        add({key,
             label,
             "Math",
             tooltip,
             {In("x", ValueKind::Numeric, "0.0"), Out("result", ValueKind::Numeric)}});
    };
    unary("math.abs", "Abs", "abs(x)");
    unary("math.saturate", "Saturate", "clamp(x, 0, 1)");
    unary("math.oneMinus", "One Minus", "1 - x");
    unary("math.normalize", "Normalize", "normalize(x)");
    unary("math.sin", "Sin", "sin(x)");
    unary("math.cos", "Cos", "cos(x)");
    unary("math.fract", "Fract", "frac(x)");

    // --- Utilitas -----------------------------------------------------------
    add({"util.lerp",
         "Lerp",
         "Utility",
         "lerp(a, b, t)",
         {In("a", ValueKind::Numeric, "0.0"), In("b", ValueKind::Numeric, "1.0"),
          In("t", ValueKind::Numeric, "0.5"), Out("result", ValueKind::Numeric)}});

    add({"util.clamp",
         "Clamp",
         "Utility",
         "clamp(x, min, max)",
         {In("x", ValueKind::Numeric, "0.0"), In("min", ValueKind::Numeric, "0.0"),
          In("max", ValueKind::Numeric, "1.0"), Out("result", ValueKind::Numeric)}});

    add({"util.fresnel",
         "Fresnel",
         "Utility",
         "Schlick pada normal dan arah pandang saat ini",
         {In("normal", ValueKind::Float3, "inputs.worldNormal"),
          In("power", ValueKind::Float, "5.0"), Out("result", ValueKind::Float)}});

    add({"util.normalBlend",
         "Normal Blend",
         "Utility",
         "Menggabungkan dua normal peta (reoriented normal mapping)",
         {In("base", ValueKind::Float3, {}), In("detail", ValueKind::Float3, {}),
          Out("result", ValueKind::Float3)}});

    add({"util.combine",
         "Combine",
         "Utility",
         "Menyusun skalar menjadi vektor",
         {In("x", ValueKind::Float, "0.0"), In("y", ValueKind::Float, "0.0"),
          In("z", ValueKind::Float, "0.0"), In("w", ValueKind::Float, "1.0"),
          Out("xy", ValueKind::Float2), Out("xyz", ValueKind::Float3),
          Out("xyzw", ValueKind::Float4)}});

    add({"util.split",
         "Split",
         "Utility",
         "Memecah vektor menjadi komponennya",
         {In("value", ValueKind::Numeric, "float4(0.0)"), Out("x", ValueKind::Float),
          Out("y", ValueKind::Float), Out("z", ValueKind::Float), Out("w", ValueKind::Float)}});

    // --- Tata letak ---------------------------------------------------------
    //
    // Tidak menghasilkan satu baris pun shader. Ada di katalog supaya panel
    // tidak perlu memperlakukan node "bukan sungguhan" sebagai pengecualian.
    add({"comment", "Comment", "Layout", "Catatan bebas di kanvas", {}});
    add({"group", "Group", "Layout", "Kotak yang menggeser isinya bersama", {}});
}

const MaterialNodeCatalog& MaterialNodeCatalog::Get() {
    static const MaterialNodeCatalog catalog;
    return catalog;
}

const MaterialNodeType* MaterialNodeCatalog::Find(std::string_view key) const {
    const auto it = std::find_if(types_.begin(), types_.end(),
                                 [key](const MaterialNodeType& type) { return type.key == key; });
    return it == types_.end() ? nullptr : &*it;
}

std::vector<MaterialPin> PinsOf(const MaterialGraph& graph, const MaterialNode& node) {
    const MaterialNodeType* type = MaterialNodeCatalog::Get().Find(node.type);
    if (type == nullptr) {
        return {};
    }
    std::vector<MaterialPin> pins = type->pins;

    // Node parameter dan konstanta mengambil tipenya dari tempat lain: yang
    // pertama dari deklarasi parameter, yang kedua dari setelannya sendiri.
    // Keduanya dilayani di sini, bukan di kompiler, supaya kanvas menolak
    // sambungan yang salah tipe pada saat diseret — bukan setelah dikompilasi.
    if (node.type == "param.get") {
        const MaterialParameter* parameter = graph.FindParameter(node.Setting("parameter"));
        if (parameter != nullptr && !pins.empty()) {
            pins.front().kind = parameter->kind;
        }
    } else if (node.type == "input.constant") {
        if (!pins.empty()) {
            pins.front().kind = ValueKindFromString(node.Setting("kind", "float"));
        }
    }
    return pins;
}


// =============================================================================
// Penyimpulan tipe
// =============================================================================

MaterialTypes MaterialTypes::Infer(const MaterialGraph& graph) {
    MaterialTypes types;
    for (const MaterialNode& node : graph.nodes) {
        types.Resolve(graph, node.guid);
    }
    return types;
}

void MaterialTypes::Resolve(const MaterialGraph& graph, const Uuid& guid) {
    const MaterialNode* node = graph.FindNode(guid);
    if (node == nullptr) {
        return;
    }
    // Penjaga siklus. Validasi memang menolak lingkar, tapi penyimpulan ini
    // juga dipakai panel selagi pengguna menyeret kabel — yaitu justru saat
    // graph-nya boleh sementara tidak sah.
    if (!resolving_.insert(guid).second) {
        return;
    }
    for (const MaterialLink& link : graph.links) {
        if (link.toNode == guid) {
            Resolve(graph, link.fromNode);
        }
    }

    const std::vector<MaterialPin> pins = PinsOf(graph, *node);
    // Lebar terlebar di antara seluruh masukan bertipe Numeric. Itulah yang
    // menentukan tipe hasil node matematika.
    ValueKind widest = ValueKind::Numeric;
    for (const MaterialPin& pin : pins) {
        if (pin.direction == PinDirection::Input && pin.kind == ValueKind::Numeric) {
            widest = Widest(widest, Into(graph, *node, pin.name));
        }
    }
    if (widest == ValueKind::Numeric) {
        widest = ValueKind::Float;  // seluruh masukannya literal skalar
    }

    for (const MaterialPin& pin : pins) {
        if (pin.direction != PinDirection::Output) {
            continue;
        }
        outputs_[std::make_pair(guid, pin.name)] =
            pin.kind == ValueKind::Numeric ? widest : pin.kind;
    }
}

ValueKind MaterialTypes::Of(const MaterialGraph& graph, const MaterialNode& node,
                            std::string_view pin) const {
    const auto it = outputs_.find(std::make_pair(node.guid, std::string(pin)));
    if (it != outputs_.end()) {
        return it->second;
    }
    const std::vector<MaterialPin> pins = PinsOf(graph, node);
    const auto declared = std::find_if(
        pins.begin(), pins.end(), [pin](const MaterialPin& candidate) {
            return candidate.name == pin;
        });
    return declared == pins.end() ? ValueKind::Float : declared->kind;
}

ValueKind MaterialTypes::Into(const MaterialGraph& graph, const MaterialNode& node,
                              std::string_view pin) const {
    if (const MaterialLink* link = graph.LinkInto(node.guid, pin)) {
        if (const MaterialNode* source = graph.FindNode(link->fromNode)) {
            return Of(graph, *source, link->fromPin);
        }
    }
    return Of(graph, node, pin);
}

}  // namespace sim::material
