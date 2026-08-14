#include "Sim/Assets/TextureBakery.h"

#include "Sim/Assets/TextureBake.h"
#include "Sim/Assets/TextureSettings.h"
#include "Sim/Core/Log.h"
#include "Sim/Core/TaskPool.h"

namespace sim::assets {

TextureBakery::TextureBakery(std::filesystem::path cacheDir, TaskPool* tasks)
    : cacheDir_(std::move(cacheDir)), tasks_(tasks) {}

BakedTextureRef TextureBakery::Request(const std::filesystem::path& source) {
    if (source.empty()) {
        return BakedTextureRef{BakeState::Failed, {}};
    }
    const std::string key = source.string();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(key);
        if (found != entries_.end()) {
            return found->second;
        }
        // Dicatat sebagai `Pending` **sebelum** tugasnya diantre. Tanpa itu,
        // permintaan kedua pada frame yang sama tidak menemukan apa pun dan
        // mengantre tugas kedua untuk berkas yang sama — dua encoder BC7
        // menghabiskan seluruh inti untuk menghasilkan berkas yang identik.
        entries_.emplace(key, BakedTextureRef{BakeState::Pending, {}});
    }

    if (tasks_ != nullptr) {
        tasks_->Submit([this, key]() { Bake(key); });
        return BakedTextureRef{BakeState::Pending, {}};
    }

    Bake(key);
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_[key];
}

void TextureBakery::Bake(const std::string& key) {
    const std::filesystem::path source(key);

    TextureSettings settings;
    LoadTextureSettings(settings, source);
    const BakeResult result = BakeTexture(source, settings, cacheDir_);

    BakedTextureRef entry;
    if (result.ok) {
        entry.state = BakeState::Ready;
        entry.path = result.path;
    } else {
        entry.state = BakeState::Failed;
        SIM_WARN("Assets", "cannot bake texture {}: {}", key, result.error);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    entries_[key] = std::move(entry);
}

void TextureBakery::Invalidate(const std::filesystem::path& source) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(source.string());
}

std::size_t TextureBakery::PendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t pending = 0;
    for (const auto& [key, entry] : entries_) {
        if (entry.state == BakeState::Pending) {
            ++pending;
        }
    }
    return pending;
}

}  // namespace sim::assets
