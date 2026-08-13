#pragma once

#include "Sim/Core/Uuid.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sim::assets {

/// Golongan aset, diturunkan dari ekstensi berkas.
///
/// Dipakai filter pencarian, pemilihan ikon, dan pemilihan importer. Sengaja
/// kasar: yang membedakan `.png` dari `.tga` adalah importer-nya, bukan tipe
/// yang dilihat pengguna di panel.
enum class AssetType : uint8_t {
    Unknown,
    Texture,
    Mesh,
    Material,
    Script,
    /// Efek partikel (`.simfx`).
    Particle,
    /// Graph visual scripting (`.simgraph`), dikompilasi menjadi Lua.
    Graph,
    /// Terrain (`.simterrain`) — deskriptor; heightmapnya berkas pendamping.
    Terrain,
    /// Vegetasi (`.simveg`) — aturan sebaran; instance-nya dihitung ulang.
    Vegetation,
    /// Volume (`.vdb`) — asap, api, awan. Grid jarang dari Houdini/EmberGen.
    Volume,
    /// Rangka animasi (`.simskel`).
    Skeleton,
    /// Klip animasi (`.simanim`).
    AnimationClip,
    /// Graph state machine animasi (`.simanimgraph`).
    AnimationGraph,
    Level,
    Prefab,
    Text,
    Json,
};

const char* ToString(AssetType type);

/// Menebak tipe dari ekstensi berkas (termasuk titiknya, huruf besar-kecil
/// diabaikan).
AssetType TypeFromExtension(std::string_view extension);

enum class ImportState : uint8_t {
    /// Berkas terlihat tapi belum pernah diimpor.
    Pending,
    Importing,
    Ready,
    Failed,
};

/// Satu berkas aset beserta yang sudah diketahui tentangnya.
struct AssetRecord {
    Uuid guid;
    /// Relatif terhadap folder akar aset, memakai '/' sebagai pemisah di semua
    /// platform. Disimpan relatif supaya memindahkan seluruh folder proyek
    /// tidak membatalkan apa pun.
    std::string relativePath;
    std::string name;
    AssetType type = AssetType::Unknown;
    ImportState state = ImportState::Pending;
    std::string error;

    std::uintmax_t fileSize = 0;
    std::int64_t modifiedSeconds = 0;

    /// Terisi untuk tekstur. Nol bila belum diimpor atau bukan tekstur.
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t channels = 0;

    /// GUID aset lain yang dirujuk berkas ini.
    std::vector<Uuid> dependencies;
};

}  // namespace sim::assets
