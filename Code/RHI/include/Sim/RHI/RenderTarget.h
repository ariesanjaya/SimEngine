#pragma once

#include "Sim/RHI/Device.h"

#include <cstdint>
#include <string>
#include <vector>

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
    /// Image mentahnya. Dibutuhkan barrier layout: barrier bekerja pada image,
    /// bukan pada view — dan renderer yang memakai dynamic rendering harus
    /// memindahkan target ini dari SHADER_READ_ONLY ke COLOR_ATTACHMENT dan
    /// kembali setiap frame.
    VkImage ColorImage() const { return colorImage_; }
    VkImage DepthImage() const { return depthImage_; }
    VkImageView DepthView() const { return depthView_; }
    VkFormat DepthFormat() const { return depthFormat_; }
    VkSampler Sampler() const { return sampler_; }
    VkFormat ColorFormat() const { return kColorFormat; }

    /// Menyalin isi target ini ke RGBA8 di memori CPU.
    ///
    /// **Jalan keluar yang tidak lewat jendela.** Sampai ini ada, satu-satunya
    /// cara agen mendapat gambar adalah `Swapchain::CaptureLastPresented` — dan
    /// tanpa jendela tidak ada swapchain, jadi setiap tool yang mengembalikan
    /// gambar mati di mode headless. Ini juga yang membuat `viewport.capture`
    /// tidak lagi harus memotret seluruh jendela lalu memotongnya: yang keluar
    /// dari sini sudah persis area yang digambar.
    ///
    /// Yang disalin hanya `Width() x Height()`, bukan seluruh alokasi. Sisa
    /// gambar tidak pernah diisi, dan mengembalikannya berarti pinggiran berisi
    /// piksel dari ukuran viewport sebelumnya.
    ///
    /// **Menunggu GPU diam.** Boleh mahal: yang memanggilnya adalah tool
    /// diagnostik yang dipakai sekali-sekali, bukan jalur frame.
    bool ReadPixels(std::vector<uint8_t>& outRgba, uint32_t& outWidth, uint32_t& outHeight,
                    std::string& error);

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
