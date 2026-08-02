#include "Sim/Script/Graph.h"

#include "Sim/Core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>

namespace sim::script {
namespace {

/// ordered_json, alasan yang sama dengan berkas level: yang biasa mengurutkan
/// kunci menurut abjad, sehingga "nodes" muncul setelah "links" dan berkasnya
/// jadi lebih sulit dibaca daripada perlunya.
using Json = nlohmann::ordered_json;

struct PinKindName {
    PinKind kind;
    const char* name;
};

constexpr std::array<PinKindName, 8> kPinKindNames{{
    {PinKind::Exec, "exec"},
    {PinKind::Bool, "bool"},
    {PinKind::Number, "number"},
    {PinKind::String, "string"},
    {PinKind::Vec3, "vec3"},
    {PinKind::Quat, "quat"},
    {PinKind::Entity, "entity"},
    {PinKind::Any, "any"},
}};

Json WritePorts(const std::vector<GraphPort>& ports) {
    Json array = Json::array();
    for (const GraphPort& port : ports) {
        Json entry;
        entry["name"] = port.name;
        entry["kind"] = ToString(port.kind);
        entry["default"] = port.defaultValue;
        array.push_back(std::move(entry));
    }
    return array;
}

void ReadPorts(const Json& array, std::vector<GraphPort>& ports) {
    if (!array.is_array()) {
        return;
    }
    for (const Json& entry : array) {
        GraphPort port;
        port.name = entry.value("name", std::string{});
        port.kind = PinKindFromString(entry.value("kind", std::string("number")));
        port.defaultValue = entry.value("default", std::string{});
        if (!port.name.empty()) {
            ports.push_back(std::move(port));
        }
    }
}

Json WriteSettings(const std::map<std::string, std::string>& values) {
    Json object = Json::object();
    for (const auto& [key, value] : values) {
        object[key] = value;
    }
    return object;
}

void ReadSettings(const Json& object, std::map<std::string, std::string>& values) {
    if (!object.is_object()) {
        return;
    }
    for (const auto& [key, value] : object.items()) {
        if (value.is_string()) {
            values[key] = value.get<std::string>();
        }
    }
}

}  // namespace

const char* ToString(PinKind kind) {
    for (const PinKindName& entry : kPinKindNames) {
        if (entry.kind == kind) {
            return entry.name;
        }
    }
    return "any";
}

PinKind PinKindFromString(std::string_view text) {
    for (const PinKindName& entry : kPinKindNames) {
        if (text == entry.name) {
            return entry.kind;
        }
    }
    return PinKind::Any;
}

bool PinAccepts(PinKind to, PinKind from) {
    // Exec hanya menyambung ke exec, dan tidak pernah lewat Any: sebuah pin
    // "apa saja" yang ikut menerima urutan eksekusi akan membuat graph yang
    // menyambungkan alur ke sebuah nilai lolos kompilasi.
    if (to == PinKind::Exec || from == PinKind::Exec) {
        return to == from;
    }
    if (to == PinKind::Any || from == PinKind::Any) {
        return true;
    }
    return to == from;
}

std::string GraphNode::Setting(std::string_view key, std::string_view fallback) const {
    const auto it = settings.find(std::string(key));
    return it == settings.end() ? std::string(fallback) : it->second;
}

const GraphNode* Graph::FindNode(const Uuid& guid) const {
    const auto it = std::find_if(nodes.begin(), nodes.end(),
                                 [&guid](const GraphNode& node) { return node.guid == guid; });
    return it == nodes.end() ? nullptr : &*it;
}

const GraphVariable* Graph::FindVariable(std::string_view name) const {
    const auto it =
        std::find_if(variables.begin(), variables.end(),
                     [name](const GraphVariable& variable) { return variable.name == name; });
    return it == variables.end() ? nullptr : &*it;
}

const GraphLink* Graph::LinkInto(const Uuid& node, std::string_view pin) const {
    const auto it = std::find_if(links.begin(), links.end(), [&node, pin](const GraphLink& link) {
        return link.toNode == node && link.toPin == pin;
    });
    return it == links.end() ? nullptr : &*it;
}

std::vector<const GraphLink*> Graph::LinksFrom(const Uuid& node, std::string_view pin) const {
    std::vector<const GraphLink*> result;
    for (const GraphLink& link : links) {
        if (link.fromNode == node && link.fromPin == pin) {
            result.push_back(&link);
        }
    }
    return result;
}

std::string SaveGraphToString(const Graph& graph) {
    Json root;
    root["version"] = kGraphSchemaVersion;

    // Antarmuka ditulis lebih dulu dan hanya bila terisi: graph yang bukan
    // subgraph menghasilkan teks yang sama persis seperti sebelum konsep ini
    // ada, jadi menaikkan versi skema tidak membuat setiap berkas ikut berubah.
    if (!graph.inputs.empty()) {
        root["inputs"] = WritePorts(graph.inputs);
    }
    if (!graph.outputs.empty()) {
        root["outputs"] = WritePorts(graph.outputs);
    }

    Json variables = Json::array();
    for (const GraphVariable& variable : graph.variables) {
        Json entry;
        entry["name"] = variable.name;
        entry["kind"] = ToString(variable.kind);
        entry["default"] = variable.defaultValue;
        entry["exposed"] = variable.exposed;
        variables.push_back(std::move(entry));
    }
    root["variables"] = std::move(variables);

    Json nodes = Json::array();
    for (const GraphNode& node : graph.nodes) {
        Json entry;
        entry["guid"] = node.guid.ToString();
        entry["type"] = node.type;
        entry["position"] = Json::array({node.position.x, node.position.y});
        if (node.size.x > 0.0f || node.size.y > 0.0f) {
            entry["size"] = Json::array({node.size.x, node.size.y});
        }
        if (!node.settings.empty()) {
            entry["settings"] = WriteSettings(node.settings);
        }
        if (!node.pinValues.empty()) {
            entry["pins"] = WriteSettings(node.pinValues);
        }
        nodes.push_back(std::move(entry));
    }
    root["nodes"] = std::move(nodes);

    Json links = Json::array();
    for (const GraphLink& link : graph.links) {
        Json entry;
        entry["guid"] = link.guid.ToString();
        entry["from"] = Json::array({link.fromNode.ToString(), link.fromPin});
        entry["to"] = Json::array({link.toNode.ToString(), link.toPin});
        links.push_back(std::move(entry));
    }
    root["links"] = std::move(links);

    return root.dump(2) + "\n";
}

GraphIoResult SaveGraphToFile(const Graph& graph, const std::filesystem::path& path) {
    GraphIoResult result;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        result.error = "cannot open " + path.string();
        return result;
    }
    file << SaveGraphToString(graph);
    result.ok = true;
    return result;
}

GraphIoResult LoadGraphFromString(Graph& graph, const std::string& text) {
    GraphIoResult result;
    Json root;
    try {
        root = Json::parse(text);
    } catch (const nlohmann::json::exception& error) {
        result.error = std::string("not valid JSON: ") + error.what();
        return result;
    }
    if (!root.is_object()) {
        result.error = "root is not an object";
        return result;
    }

    result.sourceVersion = root.value("version", kGraphSchemaVersion);
    if (result.sourceVersion > kGraphSchemaVersion) {
        result.error = "graph was written by a newer editor (version " +
                       std::to_string(result.sourceVersion) + ")";
        return result;
    }

    graph = Graph{};

    if (const auto inputs = root.find("inputs"); inputs != root.end()) {
        ReadPorts(*inputs, graph.inputs);
    }
    if (const auto outputs = root.find("outputs"); outputs != root.end()) {
        ReadPorts(*outputs, graph.outputs);
    }

    if (const auto variables = root.find("variables");
        variables != root.end() && variables->is_array()) {
        for (const Json& entry : *variables) {
            GraphVariable variable;
            variable.name = entry.value("name", std::string{});
            variable.kind = PinKindFromString(entry.value("kind", std::string("number")));
            variable.defaultValue = entry.value("default", std::string{});
            variable.exposed = entry.value("exposed", false);
            if (!variable.name.empty()) {
                graph.variables.push_back(std::move(variable));
            }
        }
    }

    if (const auto nodes = root.find("nodes"); nodes != root.end() && nodes->is_array()) {
        for (const Json& entry : *nodes) {
            GraphNode node;
            node.guid = Uuid::Parse(entry.value("guid", std::string{}));
            node.type = entry.value("type", std::string{});
            if (const auto position = entry.find("position");
                position != entry.end() && position->is_array() && position->size() >= 2) {
                node.position.x = (*position)[0].get<float>();
                node.position.y = (*position)[1].get<float>();
            }
            if (const auto size = entry.find("size");
                size != entry.end() && size->is_array() && size->size() >= 2) {
                node.size.x = (*size)[0].get<float>();
                node.size.y = (*size)[1].get<float>();
            }
            if (const auto settings = entry.find("settings"); settings != entry.end()) {
                ReadSettings(*settings, node.settings);
            }
            if (const auto pins = entry.find("pins"); pins != entry.end()) {
                ReadSettings(*pins, node.pinValues);
            }
            // Node tanpa GUID tidak bisa dirujuk koneksi mana pun, jadi
            // menyimpannya hanya menyisakan node yang tidak bisa disambung.
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
                !to->is_array() || from->size() < 2 || to->size() < 2) {
                continue;
            }
            GraphLink link;
            link.guid = Uuid::Parse(entry.value("guid", std::string{}));
            link.fromNode = Uuid::Parse((*from)[0].get<std::string>());
            link.fromPin = (*from)[1].get<std::string>();
            link.toNode = Uuid::Parse((*to)[0].get<std::string>());
            link.toPin = (*to)[1].get<std::string>();
            // Koneksi yang menunjuk node yang tidak ada dibuang saat dimuat,
            // bukan dibiarkan untuk digagalkan kompiler. Yang tersisa setelah
            // sebuah node dihapus di luar editor adalah berkas yang masih utuh.
            if (graph.FindNode(link.fromNode) == nullptr ||
                graph.FindNode(link.toNode) == nullptr) {
                continue;
            }
            // Sebuah pin input hanya boleh punya satu sumber. Yang kedua dibuang
            // alih-alih diam-diam dikalahkan yang pertama: berkas hasil suntingan
            // tangan yang memuat keduanya akan berperilaku menurut urutan
            // penyimpanannya — sesuatu yang tidak terlihat sama sekali di kanvas.
            if (graph.LinkInto(link.toNode, link.toPin) != nullptr) {
                continue;
            }
            if (!link.guid.IsValid()) {
                link.guid = Uuid::Generate();
            }
            graph.links.push_back(std::move(link));
        }
    }

    result.ok = true;
    return result;
}

GraphIoResult LoadGraphFromFile(Graph& graph, const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        GraphIoResult result;
        result.error = "cannot open " + path.string();
        return result;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return LoadGraphFromString(graph, buffer.str());
}

}  // namespace sim::script
