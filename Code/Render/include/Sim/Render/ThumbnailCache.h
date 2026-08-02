#pragma once

#include "Sim/Core/Uuid.h"
#include "Sim/Render/Types.h"

#include <filesystem>
#include <memory>

namespace sim {
class TaskPool;
}
namespace sim::rhi {
class Device;
class ITextureRegistry;
}

namespace sim::render {

/// Gambar pratinjau aset, siap dipakai ImGui::Image().
///
/// Sengaja tidak meng-include apa pun dari Vulkan, sama seperti
/// IViewportRenderer: panel Asset Browser hanya melihat antarmuka ini dan
/// sebuah TextureHandle.
class IThumbnailCache {
public:
    virtual ~IThumbnailCache() = default;

    /// Handle untuk sebuah aset, atau kInvalidTexture bila belum siap.
    ///
    /// Panggilan pertama menjadwalkan pembuatannya di thread latar dan langsung
    /// kembali — panel menggambar ikon pengganti sampai gambarnya datang. Aman
    /// dipanggil tiap frame untuk aset yang sama.
    ///
    /// `contentTag` harus berubah ketika ISI berkasnya berubah. GUID saja tidak
    /// cukup, dan justru tidak boleh cukup: GUID sengaja bertahan saat berkas
    /// diganti nama maupun ditimpa, sehingga cache yang berkunci GUID saja akan
    /// terus menampilkan gambar lama selamanya. Nilai yang wajar adalah campuran
    /// ukuran dan waktu ubah berkas.
    ///
    /// Selama gambar baru sedang didekode, handle LAMA tetap dikembalikan —
    /// mengosongkannya lebih dulu hanya menghasilkan kedipan.
    virtual TextureHandle Request(const Uuid& guid, const std::filesystem::path& path,
                                  uint64_t contentTag) = 0;

    /// Mengunggah thumbnail yang sudah selesai didekode. Dipanggil sekali per
    /// frame dari main thread — unggahan Vulkan tidak boleh dari thread lain.
    virtual void Update() = 0;

    /// Ukuran sisi terpanjang thumbnail dalam piksel.
    virtual uint32_t Size() const = 0;
};

/// Membuat cache thumbnail. `cacheDir` menampung hasil dekode antar sesi.
std::unique_ptr<IThumbnailCache> CreateThumbnailCache(rhi::Device& device,
                                                      rhi::ITextureRegistry& textures,
                                                      TaskPool& tasks,
                                                      std::filesystem::path cacheDir,
                                                      uint32_t size = 128);

}  // namespace sim::render
