#pragma once

#include "Sim/RHI/Device.h"
#include "Sim/Render/Bloom.h"
#include "Sim/Render/Types.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
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
/// **Pengukurannya rantai reduksi grafis, bukan histogram compute.** Saat ini
/// ditulis, belum ada satu pun compute pipeline di engine — yang pertama akan
/// membawa serta storage buffer, barrier compute, dan seluruh jalur yang belum
/// pernah dijalankan sekali pun. Fondasi itu sudah ada sekarang (G3), jadi
/// alasannya berubah: rantai reduksi memakai bentuk yang sudah berjalan di
/// `DepthPyramid`, rata-rata geometrik yang dihasilkannya justru yang
/// diinginkan, dan menukarnya dengan histogram compute adalah pekerjaan yang
/// harus dibuka dengan angka — bukan dengan ketersediaan jalurnya.
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

    /// Menyalin gambar HDR adegan ke float RGBA di memori CPU — **radiance
    /// linier, sebelum eksposur dan sebelum pemetaan nada.**
    ///
    /// **Ada karena tangkapan 8-bit tidak bisa menjawab pertanyaan yang halus.**
    /// Dua gambar yang berselisih setengah tingkat kuantisasi terbaca sebagai
    /// sama, dan sebuah peta selisih yang dikuatkan mengubah tangga kuantisasi
    /// itu sendiri menjadi pola yang tampak seperti temuan. Yang keluar dari
    /// sini tidak punya tangga: ia angka yang sama yang dilihat pass tone
    /// mapping.
    ///
    /// Empat float per texel, `width × height`, urutan RGBA. `currentLayout`
    /// adalah tata letak gambar itu saat ini — yang melacaknya graph frame,
    /// bukan kelas ini, dan menebaknya berarti membaca isi yang tak
    /// terdefinisi. `UNDEFINED` ditolak alih-alih dijawab dengan nol.
    ///
    /// **Menunggu GPU diam.** Boleh mahal: yang memanggilnya alat diagnostik.
    bool ReadScene(std::vector<float>& outRgba, uint32_t width, uint32_t height,
                   VkImageLayout currentLayout, std::string& error);

    /// Membangun rantai bloom dari gambar HDR. Dijalankan sesudah pass adegan
    /// dan sebelum `RecordResolve`.
    void RecordBloom(VkCommandBuffer cmd, uint32_t viewportWidth, uint32_t viewportHeight,
                     const BloomSettings& settings);

    /// Mengukur luminansi adegan lalu memajukan eksposur satu langkah waktu.
    /// Dijalankan sesudah seluruh pass adegan dan sebelum `RecordResolve`.
    void RecordMeter(VkCommandBuffer cmd, uint32_t viewportWidth, uint32_t viewportHeight,
                     const PostProcessSettings& settings, float deltaSeconds);

    /// Menggambar penyelesaiannya. Pemanggil yang membuka dan menutup
    /// `vkCmdBeginRendering` atas target tampilan — lampiran adalah urusan
    /// graph frame, dan membiarkannya di satu tempat saja adalah alasan graph
    /// itu ada.
    void RecordResolve(VkCommandBuffer cmd, bool enabled, const BloomSettings& bloom);

private:
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    bool CreatePipeline(const std::filesystem::path& shaderDirectory, const char* fragment,
                        VkDescriptorSetLayout layout, uint32_t pushSize, VkFormat colorFormat,
                        VkPipelineLayout& outLayout, VkPipeline& outPipeline,
                        VkShaderModule vertex);
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
    /// Segitiga penutup layar yang juga menghasilkan uv. Bloom mencuplik di
    /// antara texel, jadi ia butuh koordinat pecahan — bukan hanya posisi piksel.
    VkShaderModule vertexUv_ = VK_NULL_HANDLE;
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
    VkDescriptorSetLayout threeSourceLayout_ = VK_NULL_HANDLE;

    /// Dua gambar berantai mip, bukan satu. Penaikan membaca tingkat yang lebih
    /// kecil **dan** tingkat ini sebelum dipadu; membaca serta menulis
    /// subresource yang sama di dalam satu draw adalah perilaku yang tidak
    /// terdefinisi, dan hasilnya bukan galat melainkan pendaran yang kadang
    /// benar.
    Image bloomDown_;
    Image bloomUp_;
    std::vector<VkImageView> bloomDownLevels_;
    std::vector<VkImageView> bloomUpLevels_;
    std::vector<VkDescriptorSet> bloomDownSets_;
    std::vector<VkDescriptorSet> bloomUpSets_;
    uint32_t bloomLevels_ = 0;
    uint32_t bloomWidth_ = 0;
    uint32_t bloomHeight_ = 0;

    VkPipelineLayout bloomDownLayout_ = VK_NULL_HANDLE;
    VkPipeline bloomDownPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout bloomUpLayout_ = VK_NULL_HANDLE;
    VkPipeline bloomUpPipeline_ = VK_NULL_HANDLE;

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
