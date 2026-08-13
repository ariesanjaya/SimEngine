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

void TerrainStore::Clear() { entries_.clear(); }

}  // namespace sim::editor
