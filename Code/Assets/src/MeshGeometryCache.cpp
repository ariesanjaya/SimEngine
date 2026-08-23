#include "Sim/Assets/MeshGeometryCache.h"

#include "Sim/Core/Log.h"
#include "Sim/Core/TaskPool.h"

#include <utility>

namespace sim::assets {

MeshGeometryCache::MeshGeometryCache(TaskPool* tasks) : tasks_(tasks) {}
MeshGeometryCache::~MeshGeometryCache() = default;

MeshGeometryRef MeshGeometryCache::Request(const std::filesystem::path& source) {
    if (source.empty()) {
        return MeshGeometryRef{MeshGeometryState::Failed, nullptr};
    }
    const std::string key = source.string();

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(key);
        if (found != entries_.end()) {
            return MeshGeometryRef{found->second.state, found->second.data};
        }
        // Dicatat `Pending` **sebelum** tugasnya diantre, alasan yang sama
        // dengan `MeshSdfBakery`: permintaan kedua pada frame yang sama tidak
        // boleh mengantre pemuatan kedua atas berkas yang sama. Untuk mesh
        // harganya tajam — mengurai satu FBX memakan ratusan milidetik, dan
        // sebuah adegan berisi seratus prop meminta setiap mesh tiap frame.
        entries_.emplace(key, Entry{});
    }

    const auto load = [this, key]() {
        std::string error;
        MeshData mesh = LoadMesh(std::filesystem::path(key), error);

        Entry entry;
        if (mesh.IsValid()) {
            entry.state = MeshGeometryState::Ready;
            entry.data = std::make_shared<const MeshData>(std::move(mesh));
            SIM_INFO("Assets", "mesh geometry ready: {} ({} vertices, {} triangles, {} KB)",
                     std::filesystem::path(key).filename().string(), entry.data->vertices.size(),
                     entry.data->indices.size() / 3,
                     (entry.data->vertices.size() * sizeof(MeshVertex) +
                      entry.data->indices.size() * sizeof(uint32_t)) /
                         1024);
        } else {
            entry.state = MeshGeometryState::Failed;
            SIM_WARN("Assets", "cannot load mesh geometry for {}: {}", key,
                     error.empty() ? "berkas tidak menghasilkan segitiga" : error);
        }

        const std::lock_guard<std::mutex> lock(mutex_);
        entries_[key] = std::move(entry);
    };

    if (tasks_ != nullptr) {
        tasks_->Submit(load);
        return MeshGeometryRef{MeshGeometryState::Pending, nullptr};
    }

    load();
    const std::lock_guard<std::mutex> lock(mutex_);
    const Entry& entry = entries_[key];
    return MeshGeometryRef{entry.state, entry.data};
}

MeshGeometryRef MeshGeometryCache::Adopt(const std::string& key, MeshData data) {
    Entry entry;
    if (data.IsValid()) {
        entry.state = MeshGeometryState::Ready;
        entry.data = std::make_shared<const MeshData>(std::move(data));
    } else {
        entry.state = MeshGeometryState::Failed;
    }

    const std::lock_guard<std::mutex> lock(mutex_);
    // Ditimpa, bukan dilewati bila sudah ada: yang mengadopsi adalah bentuk yang
    // baru saja disunting di editor, dan entri lama justru bentuk sebelum
    // suntingan itu.
    entries_[key] = entry;
    return MeshGeometryRef{entry.state, entry.data};
}

MeshGeometryRef MeshGeometryCache::Find(const std::string& key) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return MeshGeometryRef{MeshGeometryState::Pending, nullptr};
    }
    return MeshGeometryRef{found->second.state, found->second.data};
}

void MeshGeometryCache::Invalidate(const std::string& key) {
    const std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(key);
}

std::size_t MeshGeometryCache::PendingCount() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::size_t total = 0;
    for (const auto& [key, entry] : entries_) {
        (void)key;
        if (entry.state == MeshGeometryState::Pending) {
            ++total;
        }
    }
    return total;
}

std::size_t MeshGeometryCache::ReadyCount() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::size_t total = 0;
    for (const auto& [key, entry] : entries_) {
        (void)key;
        if (entry.state == MeshGeometryState::Ready) {
            ++total;
        }
    }
    return total;
}

std::size_t MeshGeometryCache::BytesHeld() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::size_t total = 0;
    for (const auto& [key, entry] : entries_) {
        (void)key;
        if (entry.data == nullptr) {
            continue;
        }
        total += entry.data->vertices.size() * sizeof(MeshVertex);
        total += entry.data->indices.size() * sizeof(uint32_t);
    }
    return total;
}

}  // namespace sim::assets
