#include "Sim/Material/MaterialGraph.h"

#include "Sim/Core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>

namespace sim::material {
namespace {

/// ordered_json, alasan yang sama dengan berkas level dan `.simgraph`: yang
/// biasa mengurutkan kunci menurut abjad, sehingga "nodes" muncul setelah
/// "links" dan berkasnya jadi lebih sulit dibaca daripada perlunya.
using Json = nlohmann::ordered_json;

struct ValueKindName {
    ValueKind kind;
    const char* name;
};

constexpr std::array<ValueKindName, 7> kValueKindNames{{
    {ValueKind::Float, "float"},
    {ValueKind::Float2, "float2"},
    {ValueKind::Float3, "float3"},
    {ValueKind::Float4, "float4"},
    {ValueKind::Texture, "texture"},
    {ValueKind::Bool, "bool"},
    {ValueKind::Numeric, "numeric"},
}};

Json WriteStrings(const std::map<std::string, std::string>& values) {
    Json object = Json::object();
    for (const auto& [key, value] : values) {
        object[key] = value;
    }
    return object;
}

void ReadStrings(const Json& object, std::map<std::string, std::string>& values) {
    if (!object.is_object()) {
        return;
    }
    for (const auto& [key, value] : object.items()) {
        if (value.is_string()) {
            values[key] = value.get<std::string>();
        }
    }
}

Vec2 ReadVec2(const Json& entry) {
    if (!entry.is_array() || entry.size() != 2) {
        return Vec2(0.0f, 0.0f);
    }
    return Vec2(entry[0].get<float>(), entry[1].get<float>());
}

}  // namespace

const char* ToString(ValueKind kind) {
    for (const ValueKindName& entry : kValueKindNames) {
        if (entry.kind == kind) {
            return entry.name;
        }
    }
    return "float";
}

ValueKind ValueKindFromString(std::string_view text) {
    for (const ValueKindName& entry : kValueKindNames) {
        if (text == entry.name) {
            return entry.kind;
        }
    }
    return ValueKind::Float;
}

int ComponentCount(ValueKind kind) {
    switch (kind) {
        case ValueKind::Float2: return 2;
        case ValueKind::Float3: return 3;
        case ValueKind::Float4: return 4;
        case ValueKind::Numeric: return 0;
        case ValueKind::Float:
        case ValueKind::Texture:
        case ValueKind::Bool: break;
    }
    return 1;
}

bool Accepts(ValueKind to, ValueKind from) {
    if (to == from) {
        return true;
    }
    // Numeric menerima lebar berapa pun, dan hanya itu — tekstur dan bool
    // ditangani cabang di bawah. Di sisi `from` ia berarti tipe yang belum
    // diselesaikan penyimpulan; membiarkannya lewat di sini menghindari
    // kesalahan palsu pada graph yang belum utuh.
    if (to == ValueKind::Numeric || from == ValueKind::Numeric) {
        return to != ValueKind::Texture && from != ValueKind::Texture &&
               to != ValueKind::Bool && from != ValueKind::Bool;
    }
    // Tekstur dan bool berdiri sendiri. Sebuah tekstur bukan angka sampai ada
    // yang men-sampling-nya, dan bool yang diam-diam jadi 0/1 membuat node
    // switch bisa disambungkan ke apa saja tanpa peringatan.
    if (to == ValueKind::Texture || from == ValueKind::Texture || to == ValueKind::Bool ||
        from == ValueKind::Bool) {
        return false;
    }
    // Skalar melebar ke vektor apa pun, seperti di Slang. Arah sebaliknya
    // tidak: memilihkan komponen mana yang dipakai adalah keputusan yang harus
    // ditulis pengguna lewat node Split, bukan ditebak kompiler.
    return from == ValueKind::Float;
}

ValueKind Widest(ValueKind a, ValueKind b) {
    return ComponentCount(a) >= ComponentCount(b) ? a : b;
}

std::string MaterialNode::Setting(std::string_view key, std::string_view fallback) const {
    const auto it = settings.find(std::string(key));
    return it == settings.end() ? std::string(fallback) : it->second;
}

const MaterialNode* MaterialGraph::FindNode(const Uuid& guid) const {
    const auto it = std::find_if(nodes.begin(), nodes.end(),
                                 [&guid](const MaterialNode& node) { return node.guid == guid; });
    return it == nodes.end() ? nullptr : &*it;
}

const MaterialParameter* MaterialGraph::FindParameter(std::string_view name) const {
    const auto it = std::find_if(
        parameters.begin(), parameters.end(),
        [name](const MaterialParameter& parameter) { return parameter.name == name; });
    return it == parameters.end() ? nullptr : &*it;
}

const MaterialLink* MaterialGraph::LinkInto(const Uuid& node, std::string_view pin) const {
    const auto it =
        std::find_if(links.begin(), links.end(), [&node, pin](const MaterialLink& link) {
            return link.toNode == node && link.toPin == pin;
        });
    return it == links.end() ? nullptr : &*it;
}

std::vector<const MaterialLink*> MaterialGraph::LinksFrom(const Uuid& node,
                                                          std::string_view pin) const {
    std::vector<const MaterialLink*> result;
    for (const MaterialLink& link : links) {
        if (link.fromNode == node && link.fromPin == pin) {
            result.push_back(&link);
        }
    }
    return result;
}

void MaterialGraph::RemoveNode(const Uuid& guid) {
    links.erase(std::remove_if(links.begin(), links.end(),
                               [&guid](const MaterialLink& link) {
                                   return link.fromNode == guid || link.toNode == guid;
                               }),
                links.end());
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                               [&guid](const MaterialNode& node) { return node.guid == guid; }),
                nodes.end());
}

std::string SaveMaterialToString(const MaterialGraph& graph) {
    Json root;
    root["version"] = kMaterialSchemaVersion;

    Json parameters = Json::array();
    for (const MaterialParameter& parameter : graph.parameters) {
        Json entry;
        entry["name"] = parameter.name;
        entry["kind"] = ToString(parameter.kind);
        entry["default"] = parameter.defaultValue;
        if (!parameter.tooltip.empty()) {
            entry["tooltip"] = parameter.tooltip;
        }
        // Rentang hanya ditulis bila benar-benar dibatasi. Menulis 0..0 untuk
        // setiap parameter tanpa slider hanya menambah baris yang tidak
        // mengatakan apa-apa.
        if (parameter.minValue != parameter.maxValue) {
            entry["range"] = Json::array({parameter.minValue, parameter.maxValue});
        }
        parameters.push_back(std::move(entry));
    }
    root["parameters"] = std::move(parameters);

    Json nodes = Json::array();
    for (const MaterialNode& node : graph.nodes) {
        Json entry;
        entry["guid"] = node.guid.ToString();
        entry["type"] = node.type;
        entry["position"] = Json::array({node.position.x, node.position.y});
        if (node.size.x > 0.0f || node.size.y > 0.0f) {
            entry["size"] = Json::array({node.size.x, node.size.y});
        }
        if (!node.settings.empty()) {
            entry["settings"] = WriteStrings(node.settings);
        }
        if (!node.pinValues.empty()) {
            entry["pins"] = WriteStrings(node.pinValues);
        }
        nodes.push_back(std::move(entry));
    }
    root["nodes"] = std::move(nodes);

    Json links = Json::array();
    for (const MaterialLink& link : graph.links) {
        Json entry;
        entry["guid"] = link.guid.ToString();
        entry["from"] = Json::array({link.fromNode.ToString(), link.fromPin});
        entry["to"] = Json::array({link.toNode.ToString(), link.toPin});
        links.push_back(std::move(entry));
    }
    root["links"] = std::move(links);

    return root.dump(2) + "\n";
}

MaterialIoResult SaveMaterialToFile(const MaterialGraph& graph,
                                    const std::filesystem::path& path) {
    MaterialIoResult result;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        result.error = "cannot open " + path.string();
        return result;
    }
    stream << SaveMaterialToString(graph);
    result.ok = static_cast<bool>(stream);
    if (!result.ok) {
        result.error = "write failed: " + path.string();
    }
    return result;
}

MaterialIoResult LoadMaterialFromString(MaterialGraph& graph, const std::string& text) {
    MaterialIoResult result;
    Json root;
    try {
        root = Json::parse(text);
    } catch (const nlohmann::json::exception& error) {
        result.error = error.what();
        return result;
    }
    if (!root.is_object()) {
        result.error = "root is not an object";
        return result;
    }

    graph = MaterialGraph{};
    result.sourceVersion = root.value("version", kMaterialSchemaVersion);

    if (const auto parameters = root.find("parameters");
        parameters != root.end() && parameters->is_array()) {
        for (const Json& entry : *parameters) {
            MaterialParameter parameter;
            parameter.name = entry.value("name", std::string{});
            parameter.kind = ValueKindFromString(entry.value("kind", std::string("float")));
            parameter.defaultValue = entry.value("default", std::string{});
            parameter.tooltip = entry.value("tooltip", std::string{});
            if (const auto range = entry.find("range");
                range != entry.end() && range->is_array() && range->size() == 2) {
                parameter.minValue = (*range)[0].get<float>();
                parameter.maxValue = (*range)[1].get<float>();
            }
            if (!parameter.name.empty()) {
                graph.parameters.push_back(std::move(parameter));
            }
        }
    }

    if (const auto nodes = root.find("nodes"); nodes != root.end() && nodes->is_array()) {
        for (const Json& entry : *nodes) {
            MaterialNode node;
            node.guid = Uuid::Parse(entry.value("guid", std::string{}));
            node.type = entry.value("type", std::string{});
            if (const auto position = entry.find("position"); position != entry.end()) {
                node.position = ReadVec2(*position);
            }
            if (const auto size = entry.find("size"); size != entry.end()) {
                node.size = ReadVec2(*size);
            }
            if (const auto settings = entry.find("settings"); settings != entry.end()) {
                ReadStrings(*settings, node.settings);
            }
            if (const auto pins = entry.find("pins"); pins != entry.end()) {
                ReadStrings(*pins, node.pinValues);
            }
            if (node.guid.IsValid() && !node.type.empty()) {
                graph.nodes.push_back(std::move(node));
            }
        }
    }

    if (const auto links = root.find("links"); links != root.end() && links->is_array()) {
        for (const Json& entry : *links) {
            const auto from = entry.find("from");
            const auto to = entry.find("to");
            if (from == entry.end() || to == entry.end() || !from->is_array() ||
                !to->is_array() || from->size() != 2 || to->size() != 2) {
                continue;
            }
            MaterialLink link;
            link.guid = Uuid::Parse(entry.value("guid", std::string{}));
            link.fromNode = Uuid::Parse((*from)[0].get<std::string>());
            link.fromPin = (*from)[1].get<std::string>();
            link.toNode = Uuid::Parse((*to)[0].get<std::string>());
            link.toPin = (*to)[1].get<std::string>();

            // Koneksi yang menunjuk node yang tidak ada dibuang saat dimuat,
            // bukan dibiarkan lalu ditolak kompiler. Berkas hasil suntingan
            // tangan atau merge yang salah tidak boleh membuat seluruh material
            // gagal dibuka — yang hilang cukup kabelnya, dan itu terlihat.
            if (graph.FindNode(link.fromNode) == nullptr ||
                graph.FindNode(link.toNode) == nullptr) {
                SIM_WARN("Material", "Dropping link to a node that no longer exists");
                continue;
            }
            // Satu pin input hanya boleh punya satu sumber. Yang kedua di
            // berkas akan diam-diam dikalahkan yang pertama saat dikompilasi —
            // perilaku yang ditentukan urutan penyimpanan dan tidak terlihat
            // sama sekali di kanvas.
            if (graph.LinkInto(link.toNode, link.toPin) != nullptr) {
                SIM_WARN("Material", "Dropping a second link into pin '{}'", link.toPin);
                continue;
            }
            graph.links.push_back(std::move(link));
        }
    }

    result.ok = true;
    return result;
}

MaterialIoResult LoadMaterialFromFile(MaterialGraph& graph, const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        MaterialIoResult result;
        result.error = "cannot open " + path.string();
        return result;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return LoadMaterialFromString(graph, buffer.str());
}

}  // namespace sim::material
