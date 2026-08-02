#include "Sim/Render/ThumbnailCache.h"

#include "Sim/Assets/Thumbnail.h"
#include "Sim/Core/Log.h"
#include "Sim/Core/TaskPool.h"
#include "Sim/RHI/Device.h"
#include "Sim/RHI/Texture.h"
#include "Sim/RHI/TextureRegistry.h"

#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sim::render {
namespace {

/// Jumlah unggahan GPU per frame.
///
/// Unggahannya memblokir sampai selesai, jadi membuka seluruh antrean sekaligus
/// akan menghentikan frame selama ratusan milidetik saat folder besar dibuka
/// pertama kali. Empat cukup untuk mengisi layar dalam beberapa frame tanpa
/// terasa tersendat.
constexpr std::size_t kUploadsPerFrame = 4;

class ThumbnailCache final : public IThumbnailCache {
public:
    ThumbnailCache(rhi::Device& device, rhi::ITextureRegistry& textures, TaskPool& tasks,
                   std::filesystem::path cacheDir, uint32_t size)
        : device_(device),
          textures_(textures),
          tasks_(tasks),
          cacheDir_(std::move(cacheDir)),
          size_(size) {}

    ~ThumbnailCache() override {
        // Menunggu tugas latar selesai sebelum membongkar: tugas yang masih
        // berjalan menulis ke antrean milik objek ini.
        tasks_.WaitIdle();
        device_.WaitIdle();
        for (auto& [guid, entry] : entries_) {
            if (entry.handle != kInvalidTexture) {
                textures_.Release(entry.handle);
            }
        }
    }

    TextureHandle Request(const Uuid& guid, const std::filesystem::path& path) override {
        const auto it = entries_.find(guid);
        if (it != entries_.end()) {
            return it->second.handle;
        }

        // Dicatat sebagai "sedang dikerjakan" sebelum tugasnya diantre, supaya
        // permintaan di frame berikutnya tidak menjadwalkan pekerjaan yang sama
        // berulang kali selama gambarnya belum datang.
        entries_.emplace(guid, Entry{});
        tasks_.Submit([this, guid, path]() {
            assets::ThumbnailImage image = assets::MakeThumbnail(path, cacheDir_, size_);
            const std::lock_guard<std::mutex> lock(readyMutex_);
            ready_.push_back({guid, std::move(image)});
        });
        return kInvalidTexture;
    }

    void Update() override {
        std::vector<Decoded> batch;
        {
            const std::lock_guard<std::mutex> lock(readyMutex_);
            const std::size_t count = std::min(kUploadsPerFrame, ready_.size());
            batch.insert(batch.end(), std::make_move_iterator(ready_.begin()),
                         std::make_move_iterator(ready_.begin() +
                                                 static_cast<std::ptrdiff_t>(count)));
            ready_.erase(ready_.begin(), ready_.begin() + static_cast<std::ptrdiff_t>(count));
        }

        for (Decoded& decoded : batch) {
            const auto it = entries_.find(decoded.guid);
            if (it == entries_.end()) {
                continue;
            }
            // Berkas yang bukan gambar tetap disimpan sebagai entri kosong.
            // Tanpa itu, setiap frame akan menjadwalkan dekode ulang untuk
            // berkas yang sudah pasti gagal.
            if (!decoded.image.IsValid()) {
                continue;
            }
            rhi::Texture2D texture;
            if (!texture.CreateFromRgba(device_, decoded.image.width, decoded.image.height,
                                        decoded.image.rgba.data())) {
                continue;
            }
            it->second.handle = textures_.Acquire(texture.View(), texture.Sampler());
            it->second.texture = std::move(texture);
        }
    }

    uint32_t Size() const override { return size_; }

private:
    struct Entry {
        rhi::Texture2D texture;
        TextureHandle handle = kInvalidTexture;
    };
    struct Decoded {
        Uuid guid;
        assets::ThumbnailImage image;
    };

    rhi::Device& device_;
    rhi::ITextureRegistry& textures_;
    TaskPool& tasks_;
    std::filesystem::path cacheDir_;
    uint32_t size_;

    std::unordered_map<Uuid, Entry> entries_;

    std::mutex readyMutex_;
    std::vector<Decoded> ready_;
};

}  // namespace

std::unique_ptr<IThumbnailCache> CreateThumbnailCache(rhi::Device& device,
                                                      rhi::ITextureRegistry& textures,
                                                      TaskPool& tasks,
                                                      std::filesystem::path cacheDir,
                                                      uint32_t size) {
    return std::make_unique<ThumbnailCache>(device, textures, tasks, std::move(cacheDir), size);
}

}  // namespace sim::render
