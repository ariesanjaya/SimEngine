#include "Sim/Editor/TerrainStore.h"

#include "Sim/Core/Log.h"

#include <utility>

namespace sim::editor {

TerrainStore::Entry* TerrainStore::FindEntry(const Uuid& guid) {
    const auto found = entries_.find(guid);
    return found == entries_.end() ? nullptr : &found->second;
}

const TerrainStore::Entry* TerrainStore::FindEntry(const Uuid& guid) const {
    const auto found = entries_.find(guid);
    return found == entries_.end() ? nullptr : &found->second;
}

terrain::Terrain* TerrainStore::Get(const Uuid& guid, const std::filesystem::path& path) {
    if (Entry* entry = FindEntry(guid)) {
        return entry->failed ? nullptr : &entry->terrain;
    }

    Entry entry;
    const terrain::TerrainIoResult result =
        terrain::LoadTerrain(entry.terrain, entry.document, path);
    if (!result.ok) {
        SIM_WARN("Terrain", "cannot load {}: {}", path.string(), result.error);
        entry.failed = true;
        entries_.emplace(guid, std::move(entry));
        return nullptr;
    }
    auto [it, inserted] = entries_.emplace(guid, std::move(entry));
    return &it->second.terrain;
}

terrain::Terrain* TerrainStore::Find(const Uuid& guid) {
    Entry* entry = FindEntry(guid);
    return entry == nullptr || entry->failed ? nullptr : &entry->terrain;
}

terrain::TerrainDocument* TerrainStore::Document(const Uuid& guid) {
    Entry* entry = FindEntry(guid);
    return entry == nullptr || entry->failed ? nullptr : &entry->document;
}

terrain::Terrain& TerrainStore::Adopt(const Uuid& guid, terrain::Terrain value,
                                      terrain::TerrainDocument document) {
    Entry& entry = entries_[guid];
    entry.terrain = std::move(value);
    entry.document = std::move(document);
    entry.failed = false;
    // Versinya dinaikkan, bukan disetel ulang: yang menggantikan isi sebuah guid
    // harus terlihat oleh viewport yang sudah mengunggah bentuk lamanya.
    ++entry.version;
    // Terrain yang lahir di editor belum ada berkasnya, jadi ia memang belum
    // tersimpan — dan tombol Save yang mati pada terrain yang baru dibuat
    // terlihat seperti editor yang menolak menyimpan.
    entry.dirty = true;
    return entry.terrain;
}

void TerrainStore::MarkDirty(const Uuid& guid) {
    if (Entry* entry = FindEntry(guid)) {
        ++entry->version;
        entry->dirty = true;
    }
}

uint64_t TerrainStore::Version(const Uuid& guid) const {
    const Entry* entry = FindEntry(guid);
    return entry == nullptr ? 0 : entry->version;
}

bool TerrainStore::Dirty(const Uuid& guid) const {
    const Entry* entry = FindEntry(guid);
    return entry != nullptr && entry->dirty;
}

bool TerrainStore::Save(const Uuid& guid, const std::filesystem::path& path, std::string& error) {
    Entry* entry = FindEntry(guid);
    if (entry == nullptr || entry->failed) {
        error = "terrain itu belum dimuat";
        return false;
    }
    const terrain::TerrainIoResult result =
        terrain::SaveTerrain(entry->terrain, entry->document, path);
    if (!result.ok) {
        error = result.error;
        return false;
    }
    // Versinya **tidak** dinaikkan: yang berubah adalah berkasnya, bukan
    // bentuknya, dan menaikkannya akan menyuruh viewport mengunggah ulang
    // geometri yang sama persis setiap kali seseorang menekan Save.
    entry->dirty = false;
    return true;
}

namespace {

/// Jumlah revisi ubin beserta kedelapan tetangganya.
///
/// **Kedelapan, bukan keempat.** Meshnya membaca satu baris dari tetangga sisi,
/// dan normal beda tengah di simpul pojok membaca satu sampel lagi secara
/// diagonal. Menyunting ubin diagonal karena itu memang bisa menggeser satu
/// simpul di pojok ubin ini — satu simpul, tetapi retakan satu simpul tetap
/// retakan.
///
/// Dijumlah, bukan diambil maksimumnya: maksimum tidak berubah ketika sebuah
/// tetangga naik dari 3 ke 4 sementara yang lain sudah 7, dan yang tidak
/// berubah tidak membangun ulang.
uint64_t NeighborhoodRevision(const terrain::Terrain& terrain, int tileX, int tileY) {
    uint64_t sum = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            sum += terrain.TileRevision(tileX + dx, tileY + dy);
        }
    }
    return sum;
}

bool SameLods(const terrain::TileNeighborLods& a, const terrain::TileNeighborLods& b) {
    return a.negativeX == b.negativeX && a.positiveX == b.positiveX &&
           a.negativeY == b.negativeY && a.positiveY == b.positiveY;
}

}  // namespace

const assets::MeshData* TerrainStore::TileMesh(const Uuid& guid, int tileX, int tileY, int lod,
                                               const terrain::TileNeighborLods& neighbors) {
    Entry* entry = FindEntry(guid);
    if (entry == nullptr || entry->failed) {
        return nullptr;
    }
    const terrain::TerrainDesc& desc = entry->terrain.Desc();
    if (tileX < 0 || tileY < 0 || tileX >= desc.tilesX || tileY >= desc.tilesY) {
        return nullptr;
    }

    const int index = tileY * desc.tilesX + tileX;
    CachedTile& cached = entry->tiles[index];
    const uint64_t revision = NeighborhoodRevision(entry->terrain, tileX, tileY);
    if (cached.builtLod == lod && cached.builtRevision == revision &&
        SameLods(cached.builtNeighbors, neighbors)) {
        return &cached.mesh;
    }

    cached.mesh = terrain::BuildTileMesh(entry->terrain, tileX, tileY, lod, neighbors);
    cached.builtLod = lod;
    cached.builtRevision = revision;
    cached.builtNeighbors = neighbors;
    ++cached.upload;
    return &cached.mesh;
}

uint64_t TerrainStore::TileUpload(const Uuid& guid, int tileX, int tileY) const {
    const Entry* entry = FindEntry(guid);
    if (entry == nullptr || entry->failed) {
        return 0;
    }
    const terrain::TerrainDesc& desc = entry->terrain.Desc();
    if (tileX < 0 || tileY < 0 || tileX >= desc.tilesX || tileY >= desc.tilesY) {
        return 0;
    }
    const auto found = entry->tiles.find(tileY * desc.tilesX + tileX);
    return found == entry->tiles.end() ? 0 : found->second.upload;
}

void TerrainStore::Clear() { entries_.clear(); }

}  // namespace sim::editor
