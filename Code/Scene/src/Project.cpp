#include "Sim/Scene/Project.h"

#include "Sim/Core/Log.h"
#include "Sim/Scene/Serialization.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <unordered_map>

namespace sim::scene {
namespace {

constexpr const char* kProjectFileName = "project.simproj";

}  // namespace

bool LoadProject(Project& project, const std::filesystem::path& path, std::string& error) {
    std::filesystem::path file = path;
    if (std::filesystem::is_directory(file)) {
        file /= kProjectFileName;
    }

    std::ifstream stream(file);
    if (!stream) {
        error = "cannot open " + file.string();
        return false;
    }

    nlohmann::json root;
    try {
        stream >> root;
    } catch (const nlohmann::json::exception& exception) {
        error = std::string("project file is not valid JSON: ") + exception.what();
        return false;
    }

    project.name = root.value("name", std::string("Untitled"));
    project.assetsPath = root.value("assetsPath", std::string("Assets"));
    project.startupLevel = root.value("startupLevel", std::string{});
    project.root = file.parent_path();
    return true;
}

bool SaveProject(const Project& project, const std::filesystem::path& path) {
    std::filesystem::path file = path;
    if (file.extension() != ".simproj") {
        file /= kProjectFileName;
    }

    nlohmann::ordered_json root;
    root["name"] = project.name;
    root["assetsPath"] = project.assetsPath;
    root["startupLevel"] = project.startupLevel;

    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream stream(file);
    if (!stream) {
        return false;
    }
    stream << root.dump(2) << '\n';
    return true;
}

bool SavePrefab(const World& world, Entity root, const std::filesystem::path& path) {
    if (!world.IsAlive(root)) {
        return false;
    }
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream stream(path);
    if (!stream) {
        return false;
    }
    stream << SaveSubtreeToString(world, root);
    return true;
}

Entity InstantiatePrefab(World& world, const std::filesystem::path& path, Entity parent) {
    std::ifstream stream(path);
    if (!stream) {
        SIM_WARN("Scene", "Cannot open prefab {}", path.string());
        return kNullEntity;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();

    // GUID di berkas prefab ditukar dengan yang baru sebelum dibangun, supaya
    // dua salinan prefab yang sama tidak saling bertabrakan. Penukarannya
    // konsisten dalam satu berkas, jadi hubungan induk-anak tetap utuh.
    nlohmann::json document;
    try {
        document = nlohmann::json::parse(buffer.str());
    } catch (const nlohmann::json::exception& exception) {
        SIM_WARN("Scene", "Prefab {} is not valid JSON: {}", path.string(), exception.what());
        return kNullEntity;
    }
    if (!document.contains("entities") || !document["entities"].is_array()) {
        return kNullEntity;
    }

    std::unordered_map<std::string, std::string> remap;
    for (auto& record : document["entities"]) {
        const std::string oldGuid = record.value("guid", std::string{});
        if (oldGuid.empty()) {
            continue;
        }
        const std::string newGuid = Uuid::Generate().ToString();
        remap.emplace(oldGuid, newGuid);
        record["guid"] = newGuid;
    }
    for (auto& record : document["entities"]) {
        const auto it = record.find("parent");
        if (it == record.end() || !it->is_string()) {
            continue;
        }
        const auto mapped = remap.find(it->get<std::string>());
        if (mapped != remap.end()) {
            *it = mapped->second;
        } else {
            // Induk di luar prefab: entity ini adalah akarnya, dan induknya
            // ditentukan pemanggil.
            record.erase("parent");
        }
    }

    const std::size_t before = world.Count();
    if (!RestoreSubtree(world, document.dump(), world.GuidOf(parent))) {
        return kNullEntity;
    }
    if (world.Count() == before) {
        return kNullEntity;
    }

    const std::string rootGuid = document["entities"].front().value("guid", std::string{});
    return world.FindByGuid(Uuid::Parse(rootGuid));
}

}  // namespace sim::scene
