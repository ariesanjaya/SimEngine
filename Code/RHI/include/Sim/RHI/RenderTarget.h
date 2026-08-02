#pragma once

#include "Sim/RHI/Device.h"

#include <cstdint>

namespace sim::rhi {

/// Target render offscreen (color + depth) yang hasilnya bisa dipakai sebagai
/// tekstur di UI.
///
/// Inilah yang dipakai panel Viewport: renderer menggambar ke sini, lalu
/// ImGuiIntegration::TextureBridge mengubah image view-nya menjadi ImTextureID.
/// finalLayout color sengaja SHADER_READ_ONLY_OPTIMAL supaya tidak perlu
/// transisi layout manual setiap frame.
class RenderTarget {
public:
    RenderTarget() = default;
    ~RenderTarget();

    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;

    bool Create(Device& device, uint32_t width, uint32_t height);
    void Destroy();

    /// Membangun ulang bila ukurannya berbeda. Mengembalikan true bila ada
    /// perubahan, sehingga pemanggil tahu ImTextureID lamanya sudah basi.
    bool Resize(uint32_t width, uint32_t height);

    bool IsValid() const { return colorImage_ != VK_NULL_HANDLE; }

    /// Ukuran yang diminta pemanggil — inilah area yang benar-benar digambar.
    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }

    /// Ukuran gambar yang sebenarnya dialokasikan, selalu >= ukuran diminta.
    uint32_t AllocatedWidth() const { return allocatedWidth_; }
    uint32_t AllocatedHeight() const { return allocatedHeight_; }

    /// Koordinat tekstur pojok kanan-bawah dari area yang terpakai. UI harus
    /// menggambar dengan UV ini, bukan (1,1), karena sisa gambar tidak diisi.
    float UvMaxU() const;
    float UvMaxV() const;

    VkRenderPass RenderPass() const { return renderPass_; }
    VkFramebuffer Framebuffer() const { return framebuffer_; }
    VkImageView ColorView() const { return colorView_; }
    VkSampler Sampler() const { return sampler_; }
    VkFormat ColorFormat() const { return kColorFormat; }

private:
    static constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;

    /// Ukuran alokasi dibulatkan ke kelipatan ini.
    ///
    /// Inilah yang membuat menyeret pemisah dock tidak lagi menghancurkan dan
    /// membangun ulang dua gambar GPU setiap frame: selama ukuran baru masih
    /// muat di alokasi yang ada, target dipakai ulang apa adanya dan hanya
    /// area gambarnya yang berubah. Harganya sedikit memori yang menganggur.
    static constexpr uint32_t kAllocationGranularity = 128;

    bool CreateRenderPass();
    bool CreateAttachments();
    VkFormat PickDepthFormat() const;

    Device* device_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t allocatedWidth_ = 0;
    uint32_t allocatedHeight_ = 0;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;

    VkImage colorImage_ = VK_NULL_HANDLE;
    VmaAllocation colorAllocation_ = VK_NULL_HANDLE;
    VkImageView colorView_ = VK_NULL_HANDLE;

    VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
    VkImage depthImage_ = VK_NULL_HANDLE;
    VmaAllocation depthAllocation_ = VK_NULL_HANDLE;
    VkImageView depthView_ = VK_NULL_HANDLE;
};

}  // namespace sim::rhi
