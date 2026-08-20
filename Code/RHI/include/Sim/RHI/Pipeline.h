#pragma once

#include "Sim/RHI/Vulkan.h"

#include <cstdint>
#include <filesystem>
#include <span>

namespace sim::rhi {

class Device;

/// Membaca SPIR-V dari berkas lalu membuatnya menjadi `VkShaderModule`.
///
/// **Di sini, bukan di tiap pemanggil.** Modul Render punya lima salinan fungsi
/// ini yang isinya sama persis, masing-masing dengan pesan galatnya sendiri —
/// dan yang keenam akan lahir bersama pemakai compute pertama. Perbedaan di
/// antara salinan itu bukan perbedaan yang disengaja melainkan perbedaan yang
/// tidak sempat diseragamkan.
///
/// Mengembalikan `VK_NULL_HANDLE` bila berkasnya tidak ada, kosong, atau
/// panjangnya bukan kelipatan empat. Yang terakhir bukan kerewelan: `pCode`
/// dibaca sebagai larik `uint32_t`, dan berkas yang terpotong di tengah kata
/// menjadi pembacaan di luar batas jauh sebelum ia menjadi galat Vulkan.
VkShaderModule LoadShaderModule(VkDevice device, const std::filesystem::path& path);

/// Banyaknya grup kerja yang menutupi `count` elemen dengan grup selebar
/// `groupSize`.
///
/// **Pembulatan ke atas, dan konsekuensinya ada di sisi shader.** Dispatch
/// terakhir hampir selalu melampaui datanya, jadi setiap kernel harus membuang
/// invocation yang indeksnya di luar batas. Kernel yang lupa melakukannya
/// menulis di luar — pada storage image itu tulisan yang dibuang diam-diam,
/// pada storage buffer itu memori milik orang lain.
constexpr uint32_t GroupCount(uint32_t count, uint32_t groupSize) {
    // `count - 1`, bukan `count + groupSize - 1`: yang kedua meluap pada count
    // yang besar dan menghasilkan nol grup — yaitu dispatch yang tidak
    // mengerjakan apa pun tanpa satu pun galat.
    return (count == 0 || groupSize == 0) ? 0 : (count - 1) / groupSize + 1;
}

/// Apa yang dibutuhkan sebuah compute pipeline, dan tidak lebih.
///
/// Bentuknya sengaja sedatar ini. Compute pipeline tidak punya state raster,
/// blend, depth, atau format lampiran — seluruh yang membuat
/// `VkGraphicsPipelineCreateInfo` panjang tidak ada di sini — jadi sebuah
/// pembungkus yang meniru bentuk yang grafis hanya akan menyalin kekosongan.
struct ComputePipelineDesc {
    /// Berkas `.spv`. Namanya berakhiran `.comp.spv`: aturan build-nya sama
    /// dengan `.vert`/`.frag`, yaitu akhiran yang menentukan tahapnya.
    std::filesystem::path shader;
    std::span<const VkDescriptorSetLayout> setLayouts;
    /// Ukuran blok push constant, byte. Nol berarti tidak ada.
    uint32_t pushConstantBytes = 0;
    const char* entryPoint = "main";
};

/// Compute pipeline beserta layout-nya.
///
/// **Bind point-nya tidak pernah menjadi urusan pemanggil.** Satu-satunya
/// perbedaan yang benar-benar terlihat antara pipeline compute dan grafis di
/// sisi perekam adalah `VK_PIPELINE_BIND_POINT_COMPUTE`, dan itu justru
/// perbedaan yang paling mudah salah: mengikat pipeline compute di bind point
/// grafis bukan galat kompilasi melainkan dispatch yang menjalankan pipeline
/// lain. `Bind` di bawah menutup lubang itu dengan tidak menawarkannya.
///
/// **Cache-nya cache yang sama dengan yang grafis.** `VkPipelineCache` milik
/// `Device` menyimpan hasil kompilasi seluruh pipeline ke satu berkas (G2);
/// pipeline compute yang membuat cache-nya sendiri akan membayar kompilasi
/// pertamanya lagi setiap kali editor dijalankan.
class ComputePipeline {
public:
    ComputePipeline() = default;
    ~ComputePipeline();

    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;

    bool Create(Device& device, const ComputePipelineDesc& desc);
    void Destroy();

    bool IsValid() const { return pipeline_ != VK_NULL_HANDLE; }
    VkPipeline Handle() const { return pipeline_; }
    VkPipelineLayout Layout() const { return layout_; }

    /// Mengikat pipeline beserta descriptor set-nya di bind point compute.
    void Bind(VkCommandBuffer cmd, std::span<const VkDescriptorSet> sets) const;

    /// Menyerahkan blok push constant. `bytes` harus sama dengan yang
    /// dideklarasikan saat pembuatan — selisihnya dilaporkan di sini, bukan
    /// dibiarkan menjadi peringatan validation layer di tengah frame.
    void Push(VkCommandBuffer cmd, const void* data, uint32_t bytes) const;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    uint32_t pushConstantBytes_ = 0;
};

}  // namespace sim::rhi
