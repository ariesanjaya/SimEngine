#pragma once

#include "Sim/RHI/Device.h"
#include "Sim/RHI/TextureRegistry.h"

#include <array>
#include <cstdint>
#include <vector>

namespace sim::imguix {

/// Mengubah gambar GPU menjadi handle yang bisa digambar Dear ImGui.
///
/// Bagian yang penting di sini adalah penundaan pembebasan. Saat panel Viewport
/// diubah ukurannya atau ditutup, descriptor set-nya masih dirujuk oleh command
/// buffer frame-frame yang belum selesai di GPU. Membebaskannya langsung
/// menghasilkan use-after-free yang muncul sebagai crash acak — biasanya justru
/// saat pengguna menyeret panel ke monitor lain, karena di situ resize dan
/// rebuild atlas font terjadi bersamaan.
///
/// Karena itu Release() hanya mengantrikan; pembebasan sesungguhnya terjadi
/// setelah `kDeferredFrames` kali BeginFrame().
class TextureBridge final : public rhi::ITextureRegistry {
public:
    /// Harus >= jumlah frame in-flight swapchain.
    static constexpr std::size_t kDeferredFrames = 3;

    void Initialize(rhi::Device& device);
    void Shutdown();

    /// Dipanggil sekali per frame, sebelum panel menggambar.
    void BeginFrame();

    uint64_t Acquire(VkImageView imageView, VkSampler sampler) override;
    void Release(uint64_t handle) override;

    std::size_t LiveCount() const { return liveCount_; }

private:
    rhi::Device* device_ = nullptr;
    std::array<std::vector<uint64_t>, kDeferredFrames> pendingRelease_{};
    std::size_t slot_ = 0;
    std::size_t liveCount_ = 0;
};

}  // namespace sim::imguix
