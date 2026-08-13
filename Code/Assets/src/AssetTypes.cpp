#include "Sim/Assets/AssetTypes.h"

#include "Sim/ImageIO/ImageIO.h"

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
        case AssetType::Particle: return "Particle";
        case AssetType::Graph: return "Graph";
        case AssetType::Terrain: return "Terrain";
        case AssetType::Vegetation: return "Vegetation";
        case AssetType::Skeleton: return "Skeleton";
        case AssetType::AnimationClip: return "Animation Clip";
        case AssetType::AnimationGraph: return "Animation Graph";
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

    // **Tekstur ditanyakan, bukan dipatok.** Daftar ekstensi gambar yang
    // konstan di sini adalah akar cacat `.psd`: `.psd` terdaftar sebagai
    // tekstur sementara yang benar-benar bisa membacanya bergantung pada
    // backend yang aktif — dan janji yang tidak bisa ditepati lebih buruk
    // daripada format yang tidak ada. `Sim::ImageIO` membangkitkan daftarnya
    // dari kemampuan yang sungguh terkompilasi, jadi ia yang ditanya.
    //
    // Arah ketergantungannya sekali jalan: AssetTypes membaca kapabilitas
    // ImageIO, bukan sebaliknya. Membalikkannya berarti ImageIO harus tahu apa
    // itu aset, dan modul yang menghasilkan piksel tidak perlu tahu itu.
    if (imageio::CanRead(lower)) {
        return AssetType::Texture;
    }

    static const std::array<std::pair<const char*, AssetType>, 22> kTable{{
        {".obj", AssetType::Mesh},
        {".fbx", AssetType::Mesh},
        {".gltf", AssetType::Mesh},
        {".glb", AssetType::Mesh},
        // Keempatnya panggung USD yang sama, berbeda pengemasan: .usda teks,
        // .usdc biner, .usdz arsip, dan .usd yang bisa jadi teks maupun biner.
        // Yang membedakannya urusan pembacanya, bukan urusan indeks aset.
        {".usd", AssetType::Mesh},
        {".usda", AssetType::Mesh},
        {".usdc", AssetType::Mesh},
        {".usdz", AssetType::Mesh},
        {".simmat", AssetType::Material},
        {".simmatinst", AssetType::Material},
        {".simfx", AssetType::Particle},
        {".lua", AssetType::Script},
        {".simgraph", AssetType::Graph},
        {".simterrain", AssetType::Terrain},
        {".simveg", AssetType::Vegetation},
        {".simskel", AssetType::Skeleton},
        {".simanim", AssetType::AnimationClip},
        {".simanimgraph", AssetType::AnimationGraph},
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
