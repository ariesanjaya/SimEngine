#pragma once

#include "Sim/RHI/Device.h"
#include "Sim/RHI/Texture.h"
#include "Sim/RHI/Texture3D.h"
#include "Sim/Render/Atmosphere.h"
#include "Sim/Render/Types.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

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

    /// Sisi LUT aerial perspective dan banyaknya slice-nya.
    ///
    /// **32×32 di layar sudah cukup, dan itu bukan penghematan yang dipaksakan.**
    /// Yang disimpan LUT ini berubah mulus terhadap arah pandang — tidak ada satu
    /// pun tepi tajam di dalamnya — sementara yang membuat kabut tampak salah
    /// adalah tepi pada arah **jarak**, tempat ia bertemu geometri. Resolusi
    /// karena itu dihabiskan pada slice, bukan pada layar.
    static constexpr uint32_t kAerialSize = 32;
    static constexpr uint32_t kAerialSlices = 32;

    bool Create(rhi::Device& device, const std::filesystem::path& shaderDirectory,
                VkFormat sceneFormat);
    void Destroy();
    void AdoptLayouts();

    /// Menunjuk ulang descriptor kabut ke depth buffer yang sedang berlaku.
    ///
    /// **Wajib dipanggil ulang tiap kali target dialokasi ulang.** Descriptor
    /// yang masih menunjuk image depth yang lama tidak menghasilkan galat apa
    /// pun — ia membaca memori yang sudah dilepas, dan yang terlihat adalah
    /// kabut yang bentuknya mengikuti adegan beberapa frame yang lalu.
    void AdoptDepth(VkImageView depthView, VkSampler depthSampler);

    bool IsValid() const { return transmittance_.image != VK_NULL_HANDLE; }
    bool AerialIsValid() const { return aerial_.image != VK_NULL_HANDLE && hasDepth_; }

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

    /// Membangun LUT aerial perspective untuk kamera frame ini.
    ///
    /// `maxDistanceKm` adalah jangkauan LUT-nya. **Diturunkan dari bidang jauh
    /// kamera, bukan dipatok tetap.** Acuannya memakai 4 km per slice — masuk
    /// akal untuk adegan seluas planet, dan salah untuk adegan sepanjang dua
    /// kilometer: seluruh yang terlihat lalu jatuh ke dalam slice pertama, dan
    /// kabutnya menjadi satu nilai tetap yang tidak berubah terhadap jarak sama
    /// sekali.
    ///
    /// `hazeDensity` mengalikan aerosol Mie. Satu berarti udara seperti yang
    /// dimodelkan Bruneton.
    void RecordAerialLut(VkCommandBuffer cmd, const Mat4& invViewProj, float cameraHeightKm,
                         const Vec3& sunDirection, float maxDistanceKm, float hazeDensity);

    /// Mengomposit kabut ke gambar adegan lewat blending. Pemanggil yang membuka
    /// dan menutup rendering atas gambar HDR itu.
    void RecordAerialApply(VkCommandBuffer cmd, const Mat4& invViewProj,
                           const Vec3& cameraPosition, float maxDistanceKm, float intensity);

    /// Memasang peta lingkungan HDR equirectangular sebagai sumber langit.
    ///
    /// **Sumber langit kedua, bukan pengganti.** Atmosfer Bruneton menghitung
    /// langitnya dari fisika — itu yang membuat matahari terbenam benar dan awan
    /// volumetrik punya udara untuk ditinggali — dan membayarnya dengan empat
    /// pass LUT ditambah raymarch. HDRI membayar **satu cuplikan tekstur**, dan
    /// sudah berisi awan yang difoto sungguhan. Yang tidak bisa dilakukannya
    /// hanyalah berubah terhadap waktu.
    ///
    /// Tidak melakukan apa-apa bila `path` sama dengan yang sedang terpasang;
    /// pemanggil boleh memanggilnya tiap frame. Mengembalikan true bila ada peta
    /// yang siap dipakai sesudahnya.
    bool SetHdri(const std::filesystem::path& path);

    bool HdriIsValid() const { return hdri_.IsValid(); }

    /// Menggambar HDRI-nya. Pemanggil yang membuka dan menutup rendering.
    void RecordHdriDraw(VkCommandBuffer cmd, const Mat4& invViewProj, float intensity,
                        float rotationRadians);

    /// Sisi ubin derau awan. Bentuk 64³ dan rincian 32³.
    ///
    /// **Bukan 128³ seperti acuannya.** Acuan membangkitkannya di compute
    /// shader, tempat 128³ berharga beberapa milidetik; di CPU angka itu
    /// berharga 1,3 detik yang dibayar setiap kali editor dibuka. 64³ berharga
    /// 218 ms — dan yang hilang pada awan berskala kilometer adalah rincian yang
    /// toh sudah dikikis volume rincian.
    static constexpr uint32_t kCloudShapeSize = 64;
    static constexpr uint32_t kCloudDetailSize = 32;

    /// Membangkitkan volume derau dan mengunggahnya. Dipanggil sekali, dan
    /// **terpisah dari `Create`**: ia memakan ratusan milidetik, dan pemanggil
    /// berhak memutuskan kapan membayarnya.
    bool CreateClouds(const std::filesystem::path& shaderDirectory, VkFormat sceneFormat);

    bool CloudsAreValid() const { return cloudPipeline_ != VK_NULL_HANDLE && hasDepth_; }

    /// Menggambar awan ke gambar adegan lewat blending yang sama dengan kabut.
    /// Pemanggil yang membuka dan menutup rendering.
    void RecordClouds(VkCommandBuffer cmd, const Mat4& invViewProj, const Vec3& cameraPosition,
                      float cameraHeightKm, const Vec3& sunDirection, const Vec3& sunRadiance,
                      float intensity, const CloudSettings& settings, float timeSeconds);

private:
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    bool CreateImage(Image& target, uint32_t width, uint32_t height);
    /// Gambar 3D beserta satu view 3D untuk mencuplik dan satu view 2D per slice
    /// untuk menggambar.
    ///
    /// **Dua bentuk view atas satu image, dan itulah yang membuat pendekatan
    /// grafis ini mungkin.** `VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT` membuat
    /// tiap slice sebuah image 2D yang sah sebagai lampiran warna, sementara
    /// view 3D di atas image yang sama memberi penyaringan trilinear gratis saat
    /// dicuplik — termasuk **antar-slice**, yang justru arah tempat kabut paling
    /// mudah menjadi pita.
    bool CreateAerialImage();
    bool CreatePipeline(const std::filesystem::path& shaderDirectory, const char* fragment,
                        VkDescriptorSetLayout setLayout, uint32_t pushSize, VkFormat format,
                        VkShaderModule vertex, VkPipelineLayout& outLayout,
                        VkPipeline& outPipeline, bool blendEnabled = false);
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
    /// LUT aerial perspective. `view` di sini adalah view 3D untuk mencuplik;
    /// yang digambar adalah `aerialSlices_`.
    Image aerial_;
    std::array<VkImageView, kAerialSlices> aerialSlices_{};

    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSetLayout, 3> setLayouts_{};
    VkDescriptorSet multiscatterSet_ = VK_NULL_HANDLE;
    VkDescriptorSet skyViewSet_ = VK_NULL_HANDLE;
    VkDescriptorSet drawSet_ = VK_NULL_HANDLE;
    VkDescriptorSet aerialLutSet_ = VK_NULL_HANDLE;
    VkDescriptorSet aerialApplySet_ = VK_NULL_HANDLE;
    VkDescriptorSet cloudSet_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout cloudSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool cloudPool_ = VK_NULL_HANDLE;
    rhi::Texture3D cloudShape_;
    rhi::Texture3D cloudDetail_;
    /// Peta lingkungan HDR, beserta jalur yang sedang terpasang. Jalurnya
    /// disimpan supaya `SetHdri` boleh dipanggil tiap frame tanpa memuat ulang
    /// berkas enam megabyte enam puluh kali per detik.
    rhi::Texture2D hdri_;
    std::string hdriPath_;
    /// Sampler tersendiri: equirect **membungkus di U dan menjepit di V**, dan
    /// sampler bawaan `Texture2D` menjepit keduanya — yang meninggalkan jahitan
    /// tegak selebar satu texel membelah langit dari zenit ke nadir.
    VkSampler hdriSampler_ = VK_NULL_HANDLE;
    VkDescriptorSet hdriSet_ = VK_NULL_HANDLE;
    VkSampler cloudSampler_ = VK_NULL_HANDLE;
    VkImageView depthView_ = VK_NULL_HANDLE;
    VkSampler depthSampler_ = VK_NULL_HANDLE;
    bool hasDepth_ = false;

    VkPipelineLayout transmittanceLayout_ = VK_NULL_HANDLE;
    VkPipeline transmittancePipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout multiscatterLayout_ = VK_NULL_HANDLE;
    VkPipeline multiscatterPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout skyViewLayout_ = VK_NULL_HANDLE;
    VkPipeline skyViewPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout drawLayout_ = VK_NULL_HANDLE;
    VkPipeline drawPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout aerialLutLayout_ = VK_NULL_HANDLE;
    VkPipeline aerialLutPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout aerialApplyLayout_ = VK_NULL_HANDLE;
    VkPipeline aerialApplyPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout cloudLayout_ = VK_NULL_HANDLE;
    VkPipeline cloudPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout hdriLayout_ = VK_NULL_HANDLE;
    VkPipeline hdriPipeline_ = VK_NULL_HANDLE;

    /// Transmitansi dibangun sekali; sisanya saat mataharinya bergerak cukup
    /// jauh. Ambangnya bukan "berubah sama sekali": matahari yang digerakkan
    /// slider bergeser sedikit tiap frame, dan membangun ulang untuk pergeseran
    /// sekecil itu berarti membangun ulang tiap frame juga.
    bool transmittanceReady_ = false;
    Vec3 lastSunDirection_{0.0f};
    bool hasLastSun_ = false;
};

}  // namespace sim::render
