#pragma once

#include "Sim/RHI/Device.h"
#include "Sim/Render/ScreenProbe.h"

#include <array>
#include <cstdint>
#include <filesystem>

namespace sim::render {

/// Screen probe di GPU: normal per piksel, radiansi probe sebagai SH, riwayat
/// yang direproyeksi, dan penyaring à-trous di atasnya.
///
/// **Akumulasi temporalnya pindah dari blend unit ke shader, dan itu pembalikan
/// yang beralasan.** M3 memadu di tempat lewat `vkCmdSetBlendConstants` — tiga
/// tekstur, tanpa ping-pong, tanpa pertanyaan "yang mana yang berlaku sekarang".
/// Reproyeksi M5 mematahkannya: blending hanya bisa memadu di texel yang sama,
/// sedangkan titik dunia yang sama ada di **ubin yang berbeda** begitu kamera
/// bergerak. Riwayatnya karena itu disalin ke tekstur tersendiri sesudah tiap
/// pass, bukan dibaca lewat set descriptor kedua: salinan empat tekstur 80×45
/// lebih murah daripada satu pertanyaan yang harus dijawab di setiap pass yang
/// membacanya.
///
/// **Penyaringnya bekerja di kisi probe, bukan di resolusi layar.** Derau yang
/// terlihat adalah derau per-probe, dan setiap piksel toh sudah menginterpolasi
/// empat probe. Menyaring di resolusi layar berarti menyaring 800 ribu piksel
/// untuk menghapus derau yang hanya punya 3600 derajat kebebasan.
class ProbeField {
public:
    bool Create(rhi::Device& device, const std::filesystem::path& shaderDirectory,
                VkDescriptorSetLayout frameSetLayout);
    void Destroy();

    /// Menyesuaikan diri dengan ukuran alokasi target. Mengembalikan true bila
    /// image-nya dibuat ulang — pemanggil lalu harus menulis ulang descriptor
    /// yang menunjuknya.
    bool Adopt(uint32_t allocatedWidth, uint32_t allocatedHeight, VkFormat normalFormat);

    /// Memindahkan seluruh image ke layout yang dijanjikan descriptor-nya,
    /// sekali, saat baru dibuat.
    ///
    /// **Wajib walaupun shader-nya belum tentu membacanya.** Pass forward
    /// mendeklarasikan tekstur ini di descriptor set-nya sejak M6, dan Vulkan
    /// menuntut layout-nya cocok pada saat submit — bukan pada saat shader
    /// benar-benar mengambil sampel. Tanpa ini, frame pertama sebelum GI menyala
    /// melanggar di setiap draw. Pola yang sama dengan `AdoptShadowLayout`.
    void AdoptLayouts();

    /// Barrier target normal di kedua ujung depth prepass.
    void RecordNormalBegin(VkCommandBuffer cmd);
    void RecordNormalEnd(VkCommandBuffer cmd);

    /// Menelusuri seluruh probe, memadunya dengan riwayat yang direproyeksi,
    /// menyalin hasilnya ke riwayat, lalu menyaringnya dua lintasan.
    /// `traceRange` adalah jangkauan maksimum tiap sinar, meter.
    ///
    /// **Wajib, dan pernah lupa diisi.** Ia sempat tertinggal nol sejak pass ini
    /// lahir, dan nol tidak menghasilkan galat apa pun: `traceScreen` menyerah
    /// seketika, lalu `traceSdf` "mengenai" permukaan tempat sinarnya berangkat
    /// pada langkah nol — karena titik asalnya memang di permukaan. Setiap probe
    /// karena itu mengembalikan warna pikselnya sendiri, dan hasilnya tampak
    /// seperti GI yang masuk akal: kabur, berwarna benar di dekat dinding
    /// berwarna, dan stabil. Yang tidak dilakukannya hanyalah memantulkan
    /// cahaya.
    ///
    /// `normalBias` adalah pergeseran titik asal sinar sepanjang normal
    /// permukaan, meter. **Nol menghidupkan kembali separuh bug di atas.**
    /// Memperbaiki `traceRange` saja tidak cukup: sphere tracing yang berangkat
    /// tepat dari permukaan berhenti pada langkah pertama berapa pun jangkauan
    /// yang diberikan kepadanya, karena jarak di permukaan nol dan ambang
    /// berhentinya setengah voxel. Keduanya berbagi akar yang sama — titik asal
    /// yang duduk di permukaan — tapi hanya satu di antaranya yang bisa
    /// disembuhkan oleh jangkauan.
    void Record(VkCommandBuffer cmd, VkDescriptorSet frameSet, const ProbeGrid& grid,
                uint32_t frameIndex, float traceRange, float normalBias, bool resetHistory);

    bool IsValid() const { return normalImage_ != VK_NULL_HANDLE; }
    VkImageView NormalView() const { return normalView_; }
    VkSampler Sampler() const { return sampler_; }
    VkImageView ShView(uint32_t channel) const { return sh_[channel].view; }
    VkImageView HistoryShView(uint32_t channel) const { return historySh_[channel].view; }
    VkImageView HistorySurfaceView() const { return historySurface_.view; }

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
    bool CreateTracePipeline(const std::filesystem::path& shaderDirectory,
                             VkDescriptorSetLayout frameSetLayout);
    bool CreateFilterPipeline(const std::filesystem::path& shaderDirectory);
    void WriteFilterSets();
    void RecordFilterPass(VkCommandBuffer cmd, const glm::uvec2& counts, uint32_t stride,
                          VkDescriptorSet source, const std::array<Attachment, kShChannels>& target);
    void Transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to);

    rhi::Device* device_ = nullptr;
    VkImage normalImage_ = VK_NULL_HANDLE;
    VmaAllocation normalAllocation_ = VK_NULL_HANDLE;
    VkImageView normalView_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;

    std::array<Attachment, kShChannels> sh_;
    std::array<Attachment, kShChannels> scratch_;
    std::array<Attachment, kShChannels> historySh_;
    Attachment surface_;
    Attachment historySurface_;

    glm::uvec2 allocated_{0};
    glm::uvec2 probeCapacity_{0};

    VkPipelineLayout traceLayout_ = VK_NULL_HANDLE;
    VkPipeline tracePipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout filterSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool filterPool_ = VK_NULL_HANDLE;
    /// Dua set: satu membaca `sh_`, satu membaca `scratch_`. Lintasan pertama
    /// menulis ke scratch, yang kedua kembali ke `sh_` — jadi hasil akhirnya
    /// selalu di `sh_`, dan descriptor yang membacanya tidak pernah berubah.
    std::array<VkDescriptorSet, 2> filterSets_{};
    VkPipelineLayout filterLayout_ = VK_NULL_HANDLE;
    VkPipeline filterPipeline_ = VK_NULL_HANDLE;

    bool historyUndefined_ = true;
};

}  // namespace sim::render
