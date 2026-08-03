#include "Sim/Material/MaterialValidation.h"

#include "Sim/Material/MaterialNodeCatalog.h"

#include <algorithm>
#include <map>
#include <set>

namespace sim::material {
namespace {

/// Pin sebuah node, dicari menurut nama. Null bila tipenya tidak dikenal atau
/// pin-nya memang tidak ada.
const MaterialPin* FindPin(const std::vector<MaterialPin>& pins, std::string_view name) {
    const auto it = std::find_if(pins.begin(), pins.end(),
                                 [name](const MaterialPin& pin) { return pin.name == name; });
    return it == pins.end() ? nullptr : &*it;
}

/// Node yang hanya ada untuk tata letak. Tidak punya pin, tidak ikut
/// dikompilasi, dan karena itu tidak boleh ikut dinilai.
bool IsLayoutOnly(const MaterialNode& node) {
    return node.type == "comment" || node.type == "group";
}

/// Menelusuri graph mundur dari sebuah node untuk mencari lingkar.
///
/// Iteratif, bukan rekursif. Graph material yang dikarang pengguna bisa dalam,
/// dan tumpukan yang meledak adalah cara paling buruk untuk melaporkan bahwa
/// sebuah graph terlalu besar.
bool ReachesItself(const MaterialGraph& graph, const Uuid& start) {
    std::set<Uuid> visited;
    std::vector<Uuid> stack{start};
    bool first = true;

    while (!stack.empty()) {
        const Uuid current = stack.back();
        stack.pop_back();

        if (!first && current == start) {
            return true;
        }
        first = false;
        if (!visited.insert(current).second) {
            continue;
        }
        for (const MaterialLink& link : graph.links) {
            if (link.toNode == current) {
                stack.push_back(link.fromNode);
            }
        }
    }
    return false;
}

}  // namespace

bool ValidationResult::Mentions(const Uuid& node) const {
    return std::any_of(errors.begin(), errors.end(),
                       [&node](const MaterialIssue& issue) { return issue.node == node; });
}

ValidationResult ValidateMaterial(const MaterialGraph& graph) {
    ValidationResult result;

    // Pin setiap node dihitung sekali. PinsOf() membaca daftar parameter untuk
    // node param.get, dan mengulanginya per-link akan membuat pemeriksaan ini
    // kuadratik pada graph yang justru paling perlu diperiksa.
    std::map<Uuid, std::vector<MaterialPin>> pinsOf;
    int outputCount = 0;
    for (const MaterialNode& node : graph.nodes) {
        if (IsLayoutOnly(node)) {
            continue;
        }
        if (MaterialNodeCatalog::Get().Find(node.type) == nullptr) {
            result.errors.push_back({node.guid, {}, "Unknown node type '" + node.type + "'"});
            continue;
        }
        if (node.type == kSurfaceOutputType) {
            ++outputCount;
        }
        pinsOf[node.guid] = PinsOf(graph, node);
    }

    if (outputCount == 0) {
        result.errors.push_back({Uuid{}, {}, "Material has no Surface Output node"});
    } else if (outputCount > 1) {
        result.errors.push_back(
            {Uuid{}, {}, "Material has more than one Surface Output node; only one can apply"});
    }

    // --- tipe koneksi -------------------------------------------------------
    for (const MaterialLink& link : graph.links) {
        const auto from = pinsOf.find(link.fromNode);
        const auto to = pinsOf.find(link.toNode);
        if (from == pinsOf.end() || to == pinsOf.end()) {
            continue;  // node-nya sendiri sudah dilaporkan di atas
        }
        const MaterialPin* source = FindPin(from->second, link.fromPin);
        const MaterialPin* target = FindPin(to->second, link.toPin);
        if (source == nullptr) {
            result.errors.push_back(
                {link.fromNode, link.fromPin, "Pin '" + link.fromPin + "' no longer exists"});
            continue;
        }
        if (target == nullptr) {
            result.errors.push_back(
                {link.toNode, link.toPin, "Pin '" + link.toPin + "' no longer exists"});
            continue;
        }
        if (!Accepts(target->kind, source->kind)) {
            result.errors.push_back(
                {link.toNode, link.toPin,
                 std::string("Cannot connect ") + ToString(source->kind) + " to " +
                     ToString(target->kind) + " on pin '" + link.toPin + "'"});
        }
    }

    // --- siklus -------------------------------------------------------------
    for (const auto& [guid, pins] : pinsOf) {
        if (ReachesItself(graph, guid)) {
            result.errors.push_back({guid, {}, "This node feeds itself through a cycle"});
        }
    }

    // --- pin wajib yang kosong ---------------------------------------------
    for (const MaterialNode& node : graph.nodes) {
        const auto pins = pinsOf.find(node.guid);
        if (pins == pinsOf.end()) {
            continue;
        }
        for (const MaterialPin& pin : pins->second) {
            if (pin.direction != PinDirection::Input || !pin.defaultValue.empty()) {
                continue;
            }
            // Nilai yang diketik pengguna pada pin itu sama sahnya dengan
            // kabel: keduanya menjawab pertanyaan "dari mana nilainya".
            if (graph.LinkInto(node.guid, pin.name) != nullptr ||
                node.pinValues.count(pin.name) != 0) {
                continue;
            }
            result.errors.push_back(
                {node.guid, pin.name, "Pin '" + pin.name + "' must be connected"});
        }
    }

    result.ok = result.errors.empty();
    return result;
}

}  // namespace sim::material
