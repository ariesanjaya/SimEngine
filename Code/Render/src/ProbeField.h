#pragma once

#include "Sim/RHI/Device.h"
#include "Sim/Render/ScreenProbe.h"

#include <array>
#include <cstdint>
#include <filesystem>

namespace sim::render {

/// Screen probe di GPU: normal per piksel dan radiansi probe sebagai SH.
///
/// **Akumulasi temporalnya dikerjakan blend unit, bukan ping-pong tekstur.**
/// Bentuk yang lazim menyimpan dua salinan dan bergantian membaca yang satu
/// sambil menulis yang lain — enam tekstur, dua set descriptor, dan satu
/// pertanyaan "yang mana yang berlaku sekarang" di setiap pass yang membacanya.
/// Yang dibutuhkan di sini hanyalah `dst = src·c + dst·(1−c)`, dan itu persis
/// yang dilakukan blending dengan konstanta. Konstantanya disetel per frame
/// lewat `vkCmdSetBlendConstants`, jadi ia bisa mengikuti `AccumulateProbe` di
/// sisi C++ tepat angka demi angka — termasuk 1,0 pada frame pertama, yang
/// artinya "buang seluruh riwayat".
///
/// SH orde satu, satu `float4` per kanal warna, tiga lampiran RGBA16F. Itu
/// bentuk yang bisa ditulis satu pass dengan satu invokasi per probe: enam belas
/// ray ditelusuri sekali, lalu diproyeksikan sekali.
class ProbeField {
public:
    bool Create(rhi::Device& device, const std::filesystem::path& shaderDirectory,
                VkDescriptorSetLayout frameSetLayout);
    void Destroy();

    /// Menyesuaikan diri dengan ukuran alokasi target. Mengembalikan true bila
    /// image-nya dibuat ulang — pemanggil lalu harus menulis ulang descriptor
    /// yang menunjuknya.
    bool Adopt(uint32_t allocatedWidth, uint32_t allocatedHeight, VkFormat normalFormat);

    /// Barrier target normal di kedua ujung depth prepass. Perpindahan
    /// layout-nya diurus di sini, bukan oleh frame graph: graph melacak resource
    /// yang dideklarasikan pass, dan normal ini ditulis satu pass lalu dibaca
    /// dua pass lain di dalam command buffer yang sama.
    void RecordNormalBegin(VkCommandBuffer cmd);
    void RecordNormalEnd(VkCommandBuffer cmd);

    /// Menelusuri seluruh probe dan mengakumulasikannya.
    ///
    /// `blend` adalah bobot frame ini: 1,0 berarti membuang riwayat, dan itulah
    /// yang benar pada frame pertama dan setiap kali kamera berpindah — riwayat
    /// probe terikat ke piksel, dan piksel yang sama menunjuk permukaan yang
    /// berbeda begitu kamera bergerak. Reproyeksi datang di M5.
    void Record(VkCommandBuffer cmd, VkDescriptorSet frameSet, const ProbeGrid& grid,
                uint32_t frameIndex, float blend);

    bool IsValid() const { return normalImage_ != VK_NULL_HANDLE; }
    VkImage NormalImage() const { return normalImage_; }
    VkImageView NormalView() const { return normalView_; }
    VkSampler Sampler() const { return sampler_; }
    VkImageView ShView(uint32_t channel) const { return sh_[channel].view; }
    VkImage ShImage(uint32_t channel) const { return sh_[channel].image; }

    static constexpr uint32_t kShChannels = 3;

private:
    struct Attachment {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    bool CreateAttachment(Attachment& attachment, const glm::uvec2& size, VkFormat format);
    void DestroyAttachment(Attachment& attachment);
    void DestroyImages();

    rhi::Device* device_ = nullptr;
    VkImage normalImage_ = VK_NULL_HANDLE;
    VmaAllocation normalAllocation_ = VK_NULL_HANDLE;
    VkImageView normalView_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    std::array<Attachment, kShChannels> sh_;
    glm::uvec2 allocated_{0};
    glm::uvec2 probeCapacity_{0};

    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    /// True sampai satu frame pertama selesai direkam: sebelum itu isi SH-nya
    /// belum pernah ditulis, dan layout-nya belum sah untuk dibaca.
    bool shUndefined_ = true;
};

}  // namespace sim::render
