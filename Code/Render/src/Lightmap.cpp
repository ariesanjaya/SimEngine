#include "Sim/Render/Lightmap.h"

#include "Sim/Core/AtomicWrite.h"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace sim::render {
namespace {

/// Dinaikkan setiap kali arti isi berkasnya berubah. Ia bagian dari kunci, jadi
/// menaikkannya membuat artefak lama tidak pernah terbaca lagi alih-alih
/// terbaca salah.
constexpr uint32_t kLightmapVersion = 1;
constexpr char kMagic[4] = {'S', 'L', 'M', 'P'};

struct Header {
    char magic[4];
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t placementCount;
    uint64_t texelCount;
};

uint64_t HashInto(uint64_t hash, const void* data, std::size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (std::size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

}  // namespace

const LightmapPlacement* Lightmap::Find(uint64_t owner) const {
    for (const LightmapPlacement& placement : placements) {
        if (placement.owner == owner) {
            return &placement;
        }
    }
    return nullptr;
}

uint64_t LightmapCacheKey(uint64_t inputKey) {
    uint64_t hash = HashInto(1469598103934665603ull, &inputKey, sizeof(inputKey));
    return HashInto(hash, &kLightmapVersion, sizeof(kLightmapVersion));
}

std::filesystem::path LightmapCachePath(const std::filesystem::path& cacheDir, uint64_t key) {
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.simlmap", static_cast<unsigned long long>(key));
    return cacheDir / name;
}

bool WriteLightmap(const std::filesystem::path& file, const Lightmap& lightmap,
                   std::string& error) {
    if (!lightmap.IsValid()) {
        error = "nothing baked to write";
        return false;
    }
    std::error_code code;
    if (!file.parent_path().empty()) {
        std::filesystem::create_directories(file.parent_path(), code);
    }

    const std::filesystem::path temporary = UniqueTemporaryPath(file);
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "cannot open " + temporary.string() + " for writing";
        return false;
    }

    Header header{};
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.version = kLightmapVersion;
    header.width = lightmap.width;
    header.height = lightmap.height;
    header.placementCount = static_cast<uint32_t>(lightmap.placements.size());
    header.texelCount = lightmap.texels.size();

    stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!lightmap.placements.empty()) {
        stream.write(reinterpret_cast<const char*>(lightmap.placements.data()),
                     static_cast<std::streamsize>(sizeof(LightmapPlacement) *
                                                  lightmap.placements.size()));
    }
    stream.write(reinterpret_cast<const char*>(lightmap.texels.data()),
                 static_cast<std::streamsize>(sizeof(Vec3) * lightmap.texels.size()));
    stream.close();
    if (!stream) {
        error = "write failed for " + temporary.string();
        std::filesystem::remove(temporary, code);
        return false;
    }

    std::filesystem::rename(temporary, file, code);
    if (code) {
        error = "cannot move " + temporary.string() + " into place: " + code.message();
        std::filesystem::remove(temporary, code);
        return false;
    }
    return true;
}

bool ReadLightmap(const std::filesystem::path& file, Lightmap& out, std::string& error) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        error = "no cached lightmap at " + file.string();
        return false;
    }

    Header header{};
    stream.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!stream || std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0) {
        error = file.string() + " is not a baked lightmap";
        return false;
    }
    if (header.version != kLightmapVersion) {
        error = "cached lightmap was written by another baker version";
        return false;
    }
    // Batas atas yang masuk akal: jumlahnya datang dari berkas, dan
    // mengalokasikan sebanyak yang disebutnya berarti sebuah berkas rusak bisa
    // meminta puluhan gigabyte sebelum satu pun byte isinya dibaca.
    constexpr uint64_t kMaxTexels = 64ull * 1024 * 1024;
    if (header.width == 0 || header.height == 0 || header.texelCount > kMaxTexels ||
        header.placementCount > 1024u * 1024u) {
        error = "cached lightmap has an implausible size";
        return false;
    }
    // **Yang disebut header harus cocok dengan bentuk yang disebutnya.** Ditolak
    // di sini berarti pesannya menyebut artefak yang rusak, bukan galat dari
    // lapisan yang tidak tahu berkas mana yang menyebabkannya.
    if (header.texelCount != static_cast<uint64_t>(header.width) * header.height) {
        error = "cached lightmap says " + std::to_string(header.texelCount) +
                " texels but its size needs " +
                std::to_string(static_cast<uint64_t>(header.width) * header.height);
        return false;
    }

    Lightmap lightmap;
    lightmap.width = header.width;
    lightmap.height = header.height;
    lightmap.placements.resize(header.placementCount);
    lightmap.texels.resize(static_cast<std::size_t>(header.texelCount));

    if (!lightmap.placements.empty()) {
        stream.read(reinterpret_cast<char*>(lightmap.placements.data()),
                    static_cast<std::streamsize>(sizeof(LightmapPlacement) *
                                                 lightmap.placements.size()));
    }
    stream.read(reinterpret_cast<char*>(lightmap.texels.data()),
                static_cast<std::streamsize>(sizeof(Vec3) * lightmap.texels.size()));
    if (!stream) {
        error = "cached lightmap is shorter than its header promises";
        return false;
    }

    out = std::move(lightmap);
    return true;
}

}  // namespace sim::render
