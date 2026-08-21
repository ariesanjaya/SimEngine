#include "Sim/Assets/MeshSdfBakery.h"

#include "Sim/Core/Log.h"
#include "Sim/Core/TaskPool.h"

namespace sim::assets {

MeshSdfBakery::MeshSdfBakery(std::filesystem::path cacheDir, TaskPool* tasks,
                             const MeshSdfSettings& settings)
    : cacheDir_(std::move(cacheDir)), tasks_(tasks), settings_(settings) {}

MeshSdfBakery::~MeshSdfBakery() = default;

MeshSdfRef MeshSdfBakery::Request(const std::filesystem::path& source) {
    if (source.empty()) {
        return MeshSdfRef{MeshSdfState::Failed, nullptr};
    }
    const std::string key = source.string();

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(key);
        if (found != entries_.end()) {
            return MeshSdfRef{found->second.state, found->second.grid};
        }
        // Dicatat `Pending` **sebelum** tugasnya diantre, alasan yang sama
        // dengan `TextureBakery`: permintaan kedua pada frame yang sama tidak
        // boleh mengantre bake kedua atas berkas yang sama. Untuk mesh harganya
        // lebih tajam lagi — dua OpenVDB atas Sponza sekaligus adalah dua kali
        // 69 MB dan dua kali menitnya.
        entries_.emplace(key, Entry{});
    }

    if (tasks_ != nullptr) {
        tasks_->Submit([this, key]() { Bake(key); });
        return MeshSdfRef{MeshSdfState::Pending, nullptr};
    }

    Bake(key);
    const std::lock_guard<std::mutex> lock(mutex_);
    const Entry& entry = entries_[key];
    return MeshSdfRef{entry.state, entry.grid};
}

void MeshSdfBakery::Bake(const std::string& key) {
    const std::filesystem::path source(key);

    const MeshSdfBakeResult baked = BakeMeshSdfFile(source, settings_, cacheDir_);
    Entry entry;
    if (baked) {
        auto grid = std::make_shared<SdfGrid>();
        std::string error;
        if (ReadMeshSdf(baked.path, *grid, error)) {
            entry.state = MeshSdfState::Ready;
            entry.grid = std::move(grid);
            SIM_INFO("Assets", "mesh SDF ready: {} ({}x{}x{} at {:.3f} m, {} KB{})",
                     source.filename().string(), entry.grid->sizeX, entry.grid->sizeY,
                     entry.grid->sizeZ, entry.grid->voxelSize,
                     entry.grid->distances.size() * sizeof(float) / 1024,
                     baked.fromCache ? ", from cache" : "");
        } else {
            entry.state = MeshSdfState::Failed;
            SIM_WARN("Assets", "cannot read the baked SDF for {}: {}", key, error);
        }
    } else {
        entry.state = MeshSdfState::Failed;
        SIM_WARN("Assets", "cannot bake an SDF for {}: {}", key, baked.error);
    }

    const std::lock_guard<std::mutex> lock(mutex_);
    entries_[key] = std::move(entry);
}

void MeshSdfBakery::Invalidate(const std::filesystem::path& source) {
    const std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(source.string());
}

std::size_t MeshSdfBakery::PendingCount() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::size_t pending = 0;
    for (const auto& [key, entry] : entries_) {
        if (entry.state == MeshSdfState::Pending) {
            ++pending;
        }
    }
    return pending;
}

}  // namespace sim::assets
