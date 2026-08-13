#include "Sim/Whitebox/WhiteboxIo.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace sim::whitebox {
namespace {

using Json = nlohmann::ordered_json;

/// Menulis angka pecahan apa adanya.
///
/// **Pembulatan ditolak di sini**, bukan diterima diam-diam: simpul yang
/// bergeser sepersejuta setiap kali disimpan membuat berkas yang tidak pernah
/// berhenti berubah di kontrol versi, dan sesudah dua puluh kali simpan blockout
/// yang tampak sama sudah bukan bentuk yang sama.
Json WriteVec3(const Vec3& v) { return Json::array({v.x, v.y, v.z}); }

bool ReadVec3(const Json& node, Vec3& out) {
    if (!node.is_array() || node.size() != 3) {
        return false;
    }
    for (const Json& component : node) {
        if (!component.is_number()) {
            return false;
        }
    }
    out = Vec3(node[0].get<float>(), node[1].get<float>(), node[2].get<float>());
    return true;
}

}  // namespace

std::string SaveToString(const WhiteboxMesh& box) {
    const WhiteboxData data = box.ToData();

    Json root;
    root["schemaVersion"] = kWhiteboxSchemaVersion;

    Json positions = Json::array();
    for (const Vec3& position : data.positions) {
        positions.push_back(WriteVec3(position));
    }
    root["vertices"] = std::move(positions);

    Json faces = Json::array();
    for (const std::vector<uint32_t>& loop : data.faces) {
        faces.push_back(loop);
    }
    root["faces"] = std::move(faces);

    Json hidden = Json::array();
    for (const auto& [a, b] : data.hiddenEdges) {
        hidden.push_back(Json::array({a, b}));
    }
    root["hiddenEdges"] = std::move(hidden);

    root["faceMaterials"] = data.faceMaterials;

    return root.dump(2) + "\n";
}

WhiteboxIoResult LoadFromString(WhiteboxMesh& box, const std::string& text) {
    WhiteboxIoResult result;

    Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        result.error = "berkas whitebox bukan JSON yang sah";
        return result;
    }
    result.sourceVersion = root.value("schemaVersion", 1);
    if (result.sourceVersion > kWhiteboxSchemaVersion) {
        result.error = "berkas whitebox ditulis versi yang lebih baru (" +
                       std::to_string(result.sourceVersion) + ")";
        return result;
    }

    WhiteboxData data;
    for (const Json& node : root.value("vertices", Json::array())) {
        Vec3 position(0.0f);
        if (!ReadVec3(node, position)) {
            result.error = "sebuah simpul bukan tiga angka";
            return result;
        }
        data.positions.push_back(position);
    }
    for (const Json& node : root.value("faces", Json::array())) {
        if (!node.is_array()) {
            result.error = "sebuah face bukan daftar";
            return result;
        }
        std::vector<uint32_t> loop;
        for (const Json& index : node) {
            if (!index.is_number_unsigned()) {
                result.error = "sebuah indeks simpul bukan bilangan bulat tak bertanda";
                return result;
            }
            loop.push_back(index.get<uint32_t>());
        }
        data.faces.push_back(std::move(loop));
    }
    for (const Json& node : root.value("hiddenEdges", Json::array())) {
        if (!node.is_array() || node.size() != 2) {
            result.error = "sebuah rusuk tersembunyi bukan sepasang simpul";
            return result;
        }
        data.hiddenEdges.emplace_back(node[0].get<uint32_t>(), node[1].get<uint32_t>());
    }
    data.faceMaterials = root.value("faceMaterials", std::vector<int>{});
    // Daftar yang lebih pendek diperlakukan sebagai sisa tanpa material, dengan
    // alasan yang sama seperti daftar material mesh: menambah sisi di berkas
    // tidak boleh membuat berkas lama gagal dimuat.
    data.faceMaterials.resize(data.faces.size(), kNoMaterial);

    std::string error;
    if (!WhiteboxMesh::Build(box, data, error)) {
        result.error = error;
        return result;
    }

    result.ok = true;
    return result;
}

WhiteboxIoResult SaveToFile(const WhiteboxMesh& box, const std::filesystem::path& path) {
    WhiteboxIoResult result;
    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        result.error = "tidak bisa menulis " + path.string();
        return result;
    }
    stream << SaveToString(box);
    result.ok = stream.good();
    if (!result.ok) {
        result.error = "penulisan " + path.string() + " gagal di tengah";
    }
    return result;
}

WhiteboxIoResult LoadFromFile(WhiteboxMesh& box, const std::filesystem::path& path) {
    WhiteboxIoResult result;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        result.error = "tidak bisa membuka " + path.string();
        return result;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return LoadFromString(box, buffer.str());
}

}  // namespace sim::whitebox
