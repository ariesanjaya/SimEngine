#include "Sim/Assets/MeshSettings.h"

#include "Sim/Core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace sim::assets {
namespace {

using Json = nlohmann::ordered_json;

constexpr const char* kExtension = ".simmeshcfg";
constexpr int kVersion = 1;

}  // namespace

bool MeshSettings::ClothFor(std::string_view material) const {
    for (const MeshPartSettings& part : parts) {
        if (part.material == material) {
            return part.cloth;
        }
    }
    return false;
}

void MeshSettings::SetCloth(std::string_view material, bool cloth) {
    for (MeshPartSettings& part : parts) {
        if (part.material == material) {
            part.cloth = cloth;
            return;
        }
    }
    MeshPartSettings part;
    part.material = std::string(material);
    part.cloth = cloth;
    parts.push_back(std::move(part));
}

Uuid MeshSettings::MaterialFor(std::string_view material) const {
    for (const MeshPartSettings& part : parts) {
        if (part.material == material) {
            return part.materialAsset;
        }
    }
    return Uuid{};
}

void MeshSettings::SetMaterial(std::string_view material, const Uuid& asset) {
    for (MeshPartSettings& part : parts) {
        if (part.material == material) {
            part.materialAsset = asset;
            return;
        }
    }
    MeshPartSettings part;
    part.material = std::string(material);
    part.materialAsset = asset;
    parts.push_back(std::move(part));
}

void MeshSettings::Prune() {
    // Entri yang tidak menandai kain DAN tidak menunjuk material tidak
    // menyatakan apa pun — nilai bawaannya sudah sama dengan tidak adanya entri.
    const auto tail = std::remove_if(parts.begin(), parts.end(), [](const MeshPartSettings& part) {
        return part.material.empty() || (!part.cloth && !part.materialAsset.IsValid());
    });
    parts.erase(tail, parts.end());
}

bool MeshSettings::Empty() const {
    return parts.empty() && lodBias == 0.0f && castShadows && receiveShadows;
}

std::filesystem::path MeshSettingsPath(const std::filesystem::path& meshPath) {
    // Ditempel di belakang nama lengkapnya, bukan mengganti ekstensinya — sama
    // seperti `.meta`. `rig.fbx` dan `rig.obj` di satu folder adalah dua aset
    // yang berbeda, dan mengganti ekstensi membuat keduanya berbagi satu berkas
    // pengaturan.
    return meshPath.string() + kExtension;
}

bool LoadMeshSettings(MeshSettings& settings, const std::filesystem::path& meshPath) {
    settings = MeshSettings{};
    std::ifstream stream(MeshSettingsPath(meshPath));
    if (!stream) {
        return false;
    }
    Json root;
    try {
        stream >> root;
    } catch (const nlohmann::json::exception& error) {
        SIM_WARN("Assets", "Damaged mesh settings for {}: {}", meshPath.string(), error.what());
        return false;
    }
    settings.lodBias = root.value("lodBias", 0.0f);
    settings.castShadows = root.value("castShadows", true);
    settings.receiveShadows = root.value("receiveShadows", true);

    const auto parts = root.find("parts");
    if (parts == root.end() || !parts->is_array()) {
        return false;
    }
    for (const Json& entry : *parts) {
        if (!entry.is_object()) {
            continue;
        }
        MeshPartSettings part;
        part.material = entry.value("material", std::string{});
        part.cloth = entry.value("cloth", false);
        part.materialAsset = Uuid::Parse(entry.value("materialAsset", std::string{}));
        if (!part.material.empty()) {
            settings.parts.push_back(std::move(part));
        }
    }
    return true;
}

bool SaveMeshSettings(const MeshSettings& settings, const std::filesystem::path& meshPath) {
    MeshSettings trimmed = settings;
    trimmed.Prune();

    const std::filesystem::path path = MeshSettingsPath(meshPath);
    std::error_code error;
    if (trimmed.Empty()) {
        std::filesystem::remove(path, error);
        return true;
    }

    // Urutannya dipatok menurut nama material supaya menyimpan dua kali
    // menghasilkan byte yang sama — berkas yang urutannya mengikuti urutan
    // klik menghasilkan diff yang tidak membawa informasi.
    std::sort(trimmed.parts.begin(), trimmed.parts.end(),
              [](const MeshPartSettings& a, const MeshPartSettings& b) {
                  return a.material < b.material;
              });

    Json root;
    root["version"] = kVersion;
    root["lodBias"] = trimmed.lodBias;
    root["castShadows"] = trimmed.castShadows;
    root["receiveShadows"] = trimmed.receiveShadows;
    Json parts = Json::array();
    for (const MeshPartSettings& part : trimmed.parts) {
        parts.push_back(Json{{"material", part.material},
                             {"materialAsset", part.materialAsset.ToString()},
                             {"cloth", part.cloth}});
    }
    root["parts"] = std::move(parts);

    std::ofstream stream(path);
    if (!stream) {
        SIM_WARN("Assets", "Cannot write {}", path.string());
        return false;
    }
    stream << root.dump(2) << '\n';
    return stream.good();
}

}  // namespace sim::assets
