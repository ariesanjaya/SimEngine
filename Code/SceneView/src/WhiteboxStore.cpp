#include "Sim/SceneView/WhiteboxStore.h"

#include <algorithm>

#include "Sim/Core/Log.h"
#include "Sim/Whitebox/WhiteboxIo.h"

#include <utility>

namespace sim::view {

whitebox::WhiteboxMesh* WhiteboxStore::Get(const Uuid& guid,
                                           const std::filesystem::path& path) {
    const auto found = entries_.find(guid);
    if (found != entries_.end()) {
        // Yang pernah gagal tidak dicoba lagi: berkas rusak yang diurai ulang
        // tiap frame membanjiri log dengan pesan yang sama sampai tidak ada
        // pesan lain yang terbaca.
        return found->second.failed ? nullptr : &found->second.mesh;
    }

    Entry entry;
    const whitebox::WhiteboxIoResult result = whitebox::LoadFromFile(entry.mesh, path);
    if (!result.ok) {
        SIM_WARN("Whitebox", "cannot load {}: {}", path.string(), result.error);
        entry.failed = true;
        entries_.emplace(guid, std::move(entry));
        return nullptr;
    }
    auto [it, inserted] = entries_.emplace(guid, std::move(entry));
    return &it->second.mesh;
}

whitebox::WhiteboxMesh* WhiteboxStore::Find(const Uuid& guid) {
    const auto found = entries_.find(guid);
    if (found == entries_.end() || found->second.failed) {
        return nullptr;
    }
    return &found->second.mesh;
}

whitebox::WhiteboxMesh& WhiteboxStore::Adopt(const Uuid& guid, whitebox::WhiteboxMesh mesh) {
    Entry& entry = entries_[guid];
    entry.mesh = std::move(mesh);
    entry.failed = false;
    // Versinya dinaikkan, bukan disetel ulang: yang menggantikan isi sebuah
    // guid harus terlihat oleh viewport yang sudah mengunggah bentuk lamanya.
    ++entry.version;
    return entry.mesh;
}

void WhiteboxStore::MarkDirty(const Uuid& guid) {
    const auto found = entries_.find(guid);
    if (found != entries_.end()) {
        ++found->second.version;
    }
}

uint64_t WhiteboxStore::Version(const Uuid& guid) const {
    const auto found = entries_.find(guid);
    return found == entries_.end() ? 0 : found->second.version;
}

bool WhiteboxStore::Save(const Uuid& guid, const std::filesystem::path& path,
                         std::string& error) {
    const auto found = entries_.find(guid);
    if (found == entries_.end() || found->second.failed) {
        error = "whitebox itu belum dimuat";
        return false;
    }
    const whitebox::WhiteboxIoResult result = whitebox::SaveToFile(found->second.mesh, path);
    if (!result.ok) {
        error = result.error;
        return false;
    }
    return true;
}

const assets::MeshData* WhiteboxStore::BuiltMesh(const Uuid& guid) {
    const auto found = entries_.find(guid);
    if (found == entries_.end() || found->second.failed) {
        return nullptr;
    }
    Entry& entry = found->second;
    if (entry.builtVersion != entry.version) {
        entry.built = entry.mesh.BuildMeshData();
        entry.builtVersion = entry.version;
    }
    return &entry.built;
}

namespace {

/// Membalik keanggotaan sebuah handle di dalam daftar.
///
/// Yang baru masuk di **belakang**, dan itu yang membuat "yang terakhir dipilih"
/// punya arti — gizmo sisi berdiri di poligon itu, sama seperti seleksi entity
/// memakai yang terakhir sebagai acuan.
template <typename Handle>
void ToggleIn(std::vector<Handle>& list, Handle handle) {
    const auto found = std::find(list.begin(), list.end(), handle);
    if (found != list.end()) {
        list.erase(found);
        return;
    }
    list.push_back(handle);
}

}  // namespace

void WhiteboxStore::Select(const Uuid& guid, whitebox::PolygonHandle polygon) {
    // Mengganti seluruh seleksi, bukan menambah: inilah jalur klik biasa, dan
    // klik biasa selalu berarti "hanya ini".
    selected_.asset = guid;
    selected_.polygons.clear();
    selected_.edges.clear();
    selected_.vertices.clear();
    if (whitebox::IsValid(polygon)) {
        selected_.polygons.push_back(polygon);
    }
}

void WhiteboxStore::Toggle(const Uuid& guid, whitebox::PolygonHandle polygon) {
    if (selected_.asset != guid) {
        Select(guid, polygon);
        return;
    }
    if (whitebox::IsValid(polygon)) {
        ToggleIn(selected_.polygons, polygon);
    }
}

void WhiteboxStore::Toggle(const Uuid& guid, whitebox::EdgeHandle edge) {
    if (selected_.asset != guid) {
        Select(guid, whitebox::PolygonHandle::Invalid);
        selected_.asset = guid;
    }
    if (whitebox::IsValid(edge)) {
        ToggleIn(selected_.edges, edge);
    }
}

void WhiteboxStore::Toggle(const Uuid& guid, whitebox::VertexHandle vertex) {
    if (selected_.asset != guid) {
        Select(guid, whitebox::PolygonHandle::Invalid);
        selected_.asset = guid;
    }
    if (whitebox::IsValid(vertex)) {
        ToggleIn(selected_.vertices, vertex);
    }
}

void WhiteboxStore::Add(const Uuid& guid, whitebox::EdgeHandle edge) {
    if (selected_.asset != guid) {
        Select(guid, whitebox::PolygonHandle::Invalid);
        selected_.asset = guid;
    }
    if (whitebox::IsValid(edge) && !selected_.Contains(edge)) {
        selected_.edges.push_back(edge);
    }
}

void WhiteboxStore::Add(const Uuid& guid, whitebox::VertexHandle vertex) {
    if (selected_.asset != guid) {
        Select(guid, whitebox::PolygonHandle::Invalid);
        selected_.asset = guid;
    }
    if (whitebox::IsValid(vertex) && !selected_.Contains(vertex)) {
        selected_.vertices.push_back(vertex);
    }
}

void WhiteboxStore::Clear() {
    entries_.clear();
    selected_ = {};
}

}  // namespace sim::view
