#pragma once

#include "Sim/RHI/Device.h"
#include "Sim/RHI/Pipeline.h"
#include "Sim/RHI/TextureCube.h"

#include <array>
#include <cstdint>
#include <filesystem>

namespace sim::render {

/// Menyaring mip peta lingkungan di GPU, dari mip 0 yang sudah diunggah.
///
/// **Ada karena peta 64² tidak cukup dan peta 256² tidak terjangkau CPU.**
/// Mip 0 adalah pantulan kekasaran nol: setiap krom, logam poles, dan permukaan
/// glossy memantulkan mip itu apa adanya, jadi 64 texel per muka adalah
/// pantulan cermin ber-resolusi 64 piksel yang diperbesar. Menaikkannya ke 256
/// mengalikan texel yang harus disaring enam belas kali, dan menaikkan
/// sampelnya bersamaan mengalikannya lagi — 33 juta cuplikan lingkungan, yaitu
/// belasan detik di CPU untuk pekerjaan yang bentuknya persis compute.
///
/// **Mip 0 tetap di CPU dan itu bukan kompromi.** Ia mencuplik
/// `IEnvironmentSampler`: langit atmosferik yang satu cuplikannya satu ray
/// march, atau sebuah HDR 4096×2048 yang baru saja didekode ke memori host.
/// Tidak satu pun dari keduanya ada di GPU saat panggangan berjalan, dan
/// memindahkannya ke sana berarti implementasi kedua untuk atmosfer — yaitu
/// persis yang `IEnvironmentSampler` ada untuk mencegahnya.
///
/// **Jalur CPU tidak dibuang.** `BakeIblCpu` dengan `firstGpuMip` nol tetap
/// menyaring seluruh rantai, dan itulah yang diuji tanpa perangkat grafis dan
/// yang dipakai membandingkan hasil kelas ini. Yang tidak punya cara memeriksa
/// dirinya sendiri adalah yang diam-diam berhenti bekerja di mesin orang lain.
class IblPrefilter {
public:
    /// Batas yang sama dengan yang dijepitkan `BakeIblCpu` ke `mipCount`.
    static constexpr uint32_t kMaxMips = 12;

    /// Sisi grup kerja, dua dimensi. Sama dengan `numthreads` di
    /// `Shaders/ibl_prefilter.comp.slang`; keduanya harus berpindah bersama.
    static constexpr uint32_t kGroupSize = 8;

    IblPrefilter() = default;
    ~IblPrefilter() { Destroy(); }

    IblPrefilter(const IblPrefilter&) = delete;
    IblPrefilter& operator=(const IblPrefilter&) = delete;

    bool Create(rhi::Device& device, const std::filesystem::path& shaderDirectory);
    void Destroy();

    bool IsValid() const { return pipeline_.IsValid(); }

    /// Menyaring mip `firstMip` sampai mip terakhir `cube`, semuanya dari mip 0.
    ///
    /// **Satu command buffer sekali pakai, dan ia menunggu queue diam.** Ini
    /// pekerjaan sekali per lingkungan, bukan per frame: menganyamnya ke dalam
    /// frame graph menuntut resource impor beserta seumur hidupnya sendiri
    /// untuk menghemat satu stall yang terjadi ketika seseorang menggeser
    /// slider langit.
    ///
    /// Mengembalikan false beserta alasannya di log bila bentuk `cube` tidak
    /// cocok — bukan diam-diam meninggalkan mip yang tidak tersentuh, yang
    /// terlihat sebagai pantulan hitam pada material paling kasar saja.
    bool Run(const rhi::TextureCube& cube, uint32_t firstMip, uint32_t sampleCount);

private:
    void ReleaseViews();

    rhi::Device* device_ = nullptr;
    rhi::ComputePipeline pipeline_;

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kMaxMips> sets_{};

    /// View milik panggangan terakhir, dipegang sampai panggangan berikutnya.
    ///
    /// **Tidak dibuang di akhir `Run`** walaupun `EndOneShot` sudah menunggu
    /// queue diam. Descriptor set-nya masih menunjuk view-view ini sampai
    /// ditulis ulang, dan lapisan validasi memperlakukan descriptor yang
    /// menunjuk view yang sudah dimusnahkan sebagai galat — walaupun tidak ada
    /// satu pun dispatch yang membacanya lagi.
    std::array<VkImageView, kMaxMips> mipViews_{};
    VkImageView sourceView_ = VK_NULL_HANDLE;
};

}  // namespace sim::render
