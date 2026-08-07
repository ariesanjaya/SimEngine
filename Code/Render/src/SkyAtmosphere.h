#pragma once

#include "Sim/RHI/Device.h"
#include "Sim/Render/Atmosphere.h"

#include <array>
#include <cstdint>
#include <filesystem>

namespace sim::render {

/// Langit Bruneton-Hillaire: tiga LUT dan satu pass gambar.
///
/// **Tiga LUT dengan tiga umur yang berbeda, dan itu inti rancangannya.**
/// Transmitansi hanya bergantung pada parameter atmosfer, jadi ia dibangun
/// sekali. Multiscattering bergantung pada matahari, jadi ia dibangun saat
/// matahari bergerak. Sky-view bergantung pada matahari **dan** ketinggian
/// kamera, jadi ia dibangun tiap frame — tapi ukurannya 192×108, sepersekian
/// dari satu layar. Membangun ketiganya tiap frame membuang belasan juta
/// cuplikan untuk hasil yang sama persis; membangun ketiganya sekali membuat
/// langit membeku pada matahari pertama.
class SkyAtmosphere {
public:
    static constexpr uint32_t kTransmittanceWidth = 256;
    static constexpr uint32_t kTransmittanceHeight = 64;
    static constexpr uint32_t kMultiscatterSize = 32;
    static constexpr uint32_t kSkyViewWidth = 192;
    static constexpr uint32_t kSkyViewHeight = 108;

    bool Create(rhi::Device& device, const std::filesystem::path& shaderDirectory,
                VkFormat sceneFormat);
    void Destroy();
    void AdoptLayouts();

    bool IsValid() const { return transmittance_.image != VK_NULL_HANDLE; }

    /// Membangun ulang LUT yang perlu dibangun ulang, lalu menggambar langit ke
    /// lampiran yang sedang terpasang.
    ///
    /// `cameraHeightKm` adalah ketinggian kamera di atas permukaan laut. Nol
    /// tepat di permukaan; nilai negatif dijepit — kamera di bawah permukaan
    /// planet membuat setiap perpotongan bola menjadi tidak berarti, dan yang
    /// terlihat adalah langit hitam yang berkedip.
    void RecordLuts(VkCommandBuffer cmd, const Vec3& sunDirection, float cameraHeightKm);

    /// Menggambar langitnya. Pemanggil yang membuka dan menutup rendering.
    void RecordDraw(VkCommandBuffer cmd, const Mat4& invViewProj, const Vec3& cameraPosition,
                    float cameraHeightKm, const Vec3& sunDirection, const Vec3& sunRadiance,
                    float intensity);

private:
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    bool CreateImage(Image& target, uint32_t width, uint32_t height);
    bool CreatePipeline(const std::filesystem::path& shaderDirectory, const char* fragment,
                        VkDescriptorSetLayout setLayout, uint32_t pushSize, VkFormat format,
                        VkShaderModule vertex, VkPipelineLayout& outLayout,
                        VkPipeline& outPipeline);
    void DrawInto(VkCommandBuffer cmd, const Image& target, uint32_t width, uint32_t height,
                  VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSet set,
                  const void* push, uint32_t pushSize);
    void Transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                    bool toAttachment);

    rhi::Device* device_ = nullptr;
    VkShaderModule vertex_ = VK_NULL_HANDLE;
    VkShaderModule vertexUv_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;

    Image transmittance_;
    Image multiscatter_;
    Image skyView_;

    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSetLayout, 3> setLayouts_{};
    VkDescriptorSet multiscatterSet_ = VK_NULL_HANDLE;
    VkDescriptorSet skyViewSet_ = VK_NULL_HANDLE;
    VkDescriptorSet drawSet_ = VK_NULL_HANDLE;

    VkPipelineLayout transmittanceLayout_ = VK_NULL_HANDLE;
    VkPipeline transmittancePipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout multiscatterLayout_ = VK_NULL_HANDLE;
    VkPipeline multiscatterPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout skyViewLayout_ = VK_NULL_HANDLE;
    VkPipeline skyViewPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout drawLayout_ = VK_NULL_HANDLE;
    VkPipeline drawPipeline_ = VK_NULL_HANDLE;

    /// Transmitansi dibangun sekali; sisanya saat mataharinya bergerak cukup
    /// jauh. Ambangnya bukan "berubah sama sekali": matahari yang digerakkan
    /// slider bergeser sedikit tiap frame, dan membangun ulang untuk pergeseran
    /// sekecil itu berarti membangun ulang tiap frame juga.
    bool transmittanceReady_ = false;
    Vec3 lastSunDirection_{0.0f};
    bool hasLastSun_ = false;
};

}  // namespace sim::render
