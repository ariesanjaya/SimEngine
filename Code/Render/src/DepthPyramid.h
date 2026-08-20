#pragma once

#include "Sim/RHI/Device.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace sim::render {

/// Apa yang diringkas tiap texel piramida.
///
/// **Dua pertanyaan yang berlawanan, dan karena itu dua piramida.**
/// Penelusuran sinar menanyakan permukaan **terdekat** di sebuah sel: sinar
/// boleh melompatinya hanya kalau ia masih di depan yang terdekat. Occlusion
/// culling menanyakan yang **terjauh**: sebuah benda tertutup hanya kalau
/// seluruh petaknya sudah terisi sesuatu yang lebih dekat, dan yang menentukan
/// adalah texel yang paling jauh di antaranya. Satu piramida tidak bisa
/// menjawab keduanya — meringkas dua kali dari satu depth buffer jauh lebih
/// murah daripada menjawab salah satunya dengan angka yang salah.
enum class DepthReduce : uint8_t {
    /// Maksimum pada reversed-Z. Dipakai penelusuran GI.
    Nearest,
    /// Minimum pada reversed-Z. Dipakai occlusion culling.
    Farthest,
};

/// Piramida depth hierarkis di GPU, dibangun ulang tiap frame dari depth buffer
/// viewport.
///
/// **Image-nya seukuran alokasi target, tapi yang diisi hanya sepetak seukuran
/// viewport.** `RenderTarget` mengalokasi lebih besar daripada yang digambar dan
/// hanya membangun ulang saat ukurannya melewati ambang, justru supaya menyeret
/// pemisah dock tidak mengalokasi ulang apa pun. Piramida ini mengikuti aturan
/// yang sama: ukuran tiap tingkat diturunkan dari ukuran viewport frame ini, dan
/// texel di luar petak itu tidak pernah dibaca. Mengalokasi ulang piramida tiap
/// frame akan menghapus penghematan itu, dan menandai texel di luar petak
/// sebagai sah akan mencampurkan isi frame lama ke tepi gambar.
///
/// Perpindahan layout tiap mip diurus di sini, bukan oleh frame graph. Graph
/// melacak resource sebagai satu kesatuan, sementara pembangunan piramida
/// menulis mip demi mip sambil membaca mip sebelumnya — yaitu resource yang sama
/// dalam dua layout sekaligus.
class DepthPyramid {
public:
    bool Create(rhi::Device& device, const std::filesystem::path& shaderDirectory,
                DepthReduce reduce = DepthReduce::Nearest);
    void Destroy();

    /// Menyesuaikan diri dengan image depth target. Mengembalikan true bila
    /// piramidanya dibuat ulang — pemanggil lalu harus menulis ulang descriptor
    /// yang menunjuknya.
    bool Adopt(uint32_t allocatedWidth, uint32_t allocatedHeight, VkImageView depthView,
               VkSampler depthSampler);

    /// Memindahkan seluruh mip ke layout yang dijanjikan descriptor-nya, sekali.
    /// Alasannya sama dengan `ProbeField::AdoptLayouts`: pass forward
    /// mendeklarasikannya sejak M6, dan Vulkan menuntut layoutnya cocok saat
    /// submit — bukan saat shader benar-benar membacanya.
    void AdoptLayouts();

    /// Merekam pembangunan seluruh tingkat untuk viewport seukuran ini.
    /// Sesudahnya seluruh mip ada dalam layout `SHADER_READ_ONLY_OPTIMAL`.
    void Record(VkCommandBuffer cmd, uint32_t viewportWidth, uint32_t viewportHeight);

    bool IsValid() const { return image_ != VK_NULL_HANDLE; }
    VkImageView View() const { return view_; }
    VkSampler Sampler() const { return sampler_; }

    /// Banyaknya tingkat untuk sebuah ukuran viewport, dengan pembagian
    /// dibulatkan ke atas — aturan yang sama dengan `HiZPyramid` di CPU.
    static uint32_t LevelsFor(uint32_t width, uint32_t height);

private:
    void DestroyImage();

    rhi::Device* device_ = nullptr;
    DepthReduce reduce_ = DepthReduce::Nearest;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    /// Seluruh mip, dipakai penelusur.
    VkImageView view_ = VK_NULL_HANDLE;
    /// Satu view per mip, dipakai sebagai lampiran saat membangun.
    std::vector<VkImageView> levelViews_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t levels_ = 0;

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    /// Sumber tiap tingkat: depth buffer untuk tingkat nol, mip sebelumnya untuk
    /// sisanya.
    std::vector<VkDescriptorSet> sources_;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace sim::render
