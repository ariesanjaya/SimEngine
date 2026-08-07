#pragma once

#include "Sim/RHI/Device.h"
#include "Sim/Render/Types.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace sim::render {

/// Rantai post-process: target warna HDR, pengukuran luminansi, eksposur yang
/// beradaptasi, dan penyelesaian ACES ke target tampilan.
///
/// **Target warnanya pindah ke sini, dan itu perubahan yang menyeluruh.**
/// Sebelumnya seluruh pass menggambar langsung ke gambar 8-bit yang disampel
/// ImGui, dan itu memaksa dua hal yang keduanya salah: radiance sungguhan harus
/// dikalikan `ViewportDesc::exposure` supaya tidak terpotong putih, dan tidak
/// ada satu pun tempat untuk melakukan apa pun terhadap warna sesudah adegan
/// selesai. Sekarang adegan menggambar ke `R16G16B16A16_SFLOAT` dengan radiance
/// apa adanya, dan pass ini yang memetakannya ke layar.
///
/// **Pengukurannya rantai reduksi grafis, bukan histogram compute.** Bukan
/// karena histogram lebih buruk melainkan karena tidak ada satu pun compute
/// pipeline di engine ini: yang pertama akan membawa serta storage buffer,
/// barrier compute, dan seluruh jalur yang belum pernah dijalankan sekali pun.
/// Rantai reduksi memakai bentuk yang sudah berjalan di `DepthPyramid`, dan
/// rata-rata geometrik yang dihasilkannya justru yang diinginkan.
class PostProcess {
public:
    /// Sisi petak awal pengukuran. Sama dengan `LogLuminanceReducer::kSeedSize`.
    static constexpr uint32_t kSeedSize = 256;
    static constexpr VkFormat kSceneFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    bool Create(rhi::Device& device, const std::filesystem::path& shaderDirectory,
                VkFormat outputFormat);
    void Destroy();

    /// Menyesuaikan diri dengan ukuran alokasi target tampilan. Mengembalikan
    /// true bila gambar HDR-nya dibuat ulang — pemanggil lalu harus menulis
    /// ulang descriptor yang menunjuknya.
    bool Adopt(uint32_t allocatedWidth, uint32_t allocatedHeight);

    /// Memindahkan gambar yang baru dibuat ke layout yang dijanjikan
    /// descriptor-nya, dan **mengosongkan kedua texel eksposur**. Nol berarti
    /// "belum ada riwayat", dan itu satu-satunya cara pass eksposur tahu bahwa
    /// frame ini harus mendarat alih-alih beradaptasi. Memori yang belum ditulis
    /// bisa berisi apa saja, termasuk angka yang tampak masuk akal.
    void AdoptLayouts();

    bool IsValid() const { return sceneImage_ != VK_NULL_HANDLE; }

    VkImage SceneImage() const { return sceneImage_; }
    VkImageView SceneView() const { return sceneView_; }
    VkSampler Sampler() const { return sampler_; }

    /// Mengukur luminansi adegan lalu memajukan eksposur satu langkah waktu.
    /// Dijalankan sesudah seluruh pass adegan dan sebelum `RecordResolve`.
    void RecordMeter(VkCommandBuffer cmd, uint32_t viewportWidth, uint32_t viewportHeight,
                     const PostProcessSettings& settings, float deltaSeconds);

    /// Menggambar penyelesaiannya. Pemanggil yang membuka dan menutup
    /// `vkCmdBeginRendering` atas target tampilan — lampiran adalah urusan
    /// graph frame, dan membiarkannya di satu tempat saja adalah alasan graph
    /// itu ada.
    void RecordResolve(VkCommandBuffer cmd, bool enabled);

private:
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    bool CreatePipeline(const std::filesystem::path& shaderDirectory, const char* fragment,
                        VkDescriptorSetLayout layout, uint32_t pushSize, VkFormat colorFormat,
                        VkPipelineLayout& outLayout, VkPipeline& outPipeline);
    bool CreateSetLayout(uint32_t bindingCount, VkDescriptorSetLayout& outLayout);
    void DestroyImages();
    void WriteSets();
    void Transition(VkCommandBuffer cmd, VkImage image, uint32_t baseMip, VkImageLayout from,
                    VkImageLayout to, bool toAttachment);
    void DrawInto(VkCommandBuffer cmd, VkImageView target, uint32_t width, uint32_t height,
                  VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSet set,
                  const void* push, uint32_t pushSize);

    rhi::Device* device_ = nullptr;
    VkFormat outputFormat_ = VK_FORMAT_UNDEFINED;
    VkShaderModule vertex_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    /// NEAREST, dipakai rantai reduksi dan pembacaan satu texel. Reduksi
    /// mengambil texel per texel lewat `Load`, dan penyaringan bilinear di
    /// tengahnya hanya akan menambahkan tapisan kedua yang tidak diminta.
    VkSampler pointSampler_ = VK_NULL_HANDLE;

    Image scene_;
    VkImage sceneImage_ = VK_NULL_HANDLE;
    VkImageView sceneView_ = VK_NULL_HANDLE;
    uint32_t allocatedWidth_ = 0;
    uint32_t allocatedHeight_ = 0;

    /// Rantai log-luminansi, `kSeedSize` sampai 1×1.
    Image luminance_;
    std::vector<VkImageView> luminanceLevels_;
    uint32_t luminanceLevels_Count = 0;

    /// Dua texel eksposur bergantian: pass frame ini membaca yang satu dan
    /// menulis yang lain. Membaca dan menulis texel yang sama di dalam satu
    /// draw adalah perilaku yang tidak terdefinisi, dan hasilnya bukan galat
    /// melainkan eksposur yang kadang benar.
    std::array<Image, 2> exposure_;
    uint32_t exposureIndex_ = 0;

    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout oneSourceLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout twoSourceLayout_ = VK_NULL_HANDLE;

    VkDescriptorSet seedSet_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> reduceSets_;
    std::array<VkDescriptorSet, 2> exposureSets_{};
    std::array<VkDescriptorSet, 2> resolveSets_{};

    VkPipelineLayout seedLayout_ = VK_NULL_HANDLE;
    VkPipeline seedPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout reduceLayout_ = VK_NULL_HANDLE;
    VkPipeline reducePipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout exposureLayout_ = VK_NULL_HANDLE;
    VkPipeline exposurePipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout resolveLayout_ = VK_NULL_HANDLE;
    VkPipeline resolvePipeline_ = VK_NULL_HANDLE;
};

}  // namespace sim::render
