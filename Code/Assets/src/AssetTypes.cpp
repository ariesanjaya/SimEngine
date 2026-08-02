#include "Sim/Assets/AssetTypes.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <utility>

namespace sim::assets {

const char* ToString(AssetType type) {
    switch (type) {
        case AssetType::Texture: return "Texture";
        case AssetType::Mesh: return "Mesh";
        case AssetType::Material: return "Material";
        case AssetType::Script: return "Script";
        case AssetType::Graph: return "Graph";
        case AssetType::Level: return "Level";
        case AssetType::Prefab: return "Prefab";
        case AssetType::Text: return "Text";
        case AssetType::Json: return "JSON";
        case AssetType::Unknown: break;
    }
    return "Unknown";
}

AssetType TypeFromExtension(std::string_view extension) {
    std::string lower(extension);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    static const std::array<std::pair<const char*, AssetType>, 18> kTable{{
        {".png", AssetType::Texture},
        {".jpg", AssetType::Texture},
        {".jpeg", AssetType::Texture},
        {".tga", AssetType::Texture},
        {".bmp", AssetType::Texture},
        {".hdr", AssetType::Texture},
        {".psd", AssetType::Texture},
        {".obj", AssetType::Mesh},
        {".fbx", AssetType::Mesh},
        {".gltf", AssetType::Mesh},
        {".glb", AssetType::Mesh},
        {".simmat", AssetType::Material},
        {".lua", AssetType::Script},
        {".simgraph", AssetType::Graph},
        {".simlevel", AssetType::Level},
        {".simprefab", AssetType::Prefab},
        {".json", AssetType::Json},
        {".txt", AssetType::Text},
    }};

    for (const auto& [suffix, type] : kTable) {
        if (lower == suffix) {
            return type;
        }
    }
    return AssetType::Unknown;
}

}  // namespace sim::assets
