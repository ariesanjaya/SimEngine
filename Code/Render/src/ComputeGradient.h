#pragma once

#include "Sim/RHI/Device.h"
#include "Sim/RHI/Pipeline.h"

#include <cstdint>
#include <filesystem>

namespace sim::render {

/// Pass compute sepele yang membuktikan jalur compute hidup (G3).
///
/// **Sebuah alat diagnostik, bukan sisa pekerjaan yang lupa dibuang.** Ia
/// dibangun sebagai kriteria selesai G3 — satu dispatch yang mengisi storage
/// image lalu ditampilkan — dan ia tetap ada setelah pemakai compute yang
/// sungguhan datang, dengan alasan yang sama seperti pemilih backend GI yang
/// terlihat sejak M0: jalur yang tidak punya cara memeriksanya sendiri adalah
/// jalur yang diam-diam berhenti bekerja pada mesin orang lain, dan yang
/// terlihat di sana bukan pesan galat melainkan fitur yang "tidak muncul".
///
/// Dua pass, bukan satu, dan pembagiannya bukan selera: compute menulis ke
/// storage image dalam layout `GENERAL`, sedangkan yang menampilkannya adalah
/// fragment shader yang membacanya sebagai tekstur. Perpindahan di antara
/// keduanya justru transisi yang dituntut setiap pemakai compute berikutnya —
/// Hi-Z, clipmap SDF, penetapan cluster — jadi transisi itulah yang harus
/// dijalankan sungguhan, bukan disimpulkan saja di test.
class ComputeGradient {
public:
    /// Sisi grup kerja, dua dimensi. Sama dengan `numthreads` di
    /// `Shaders/debug_gradient.comp.slang`; keduanya harus berpindah bersama.
    static constexpr uint32_t kGroupSize = 8;

    ComputeGradient() = default;
    ~ComputeGradient() { Destroy(); }

    ComputeGradient(const ComputeGradient&) = delete;
    ComputeGradient& operator=(const ComputeGradient&) = delete;

    /// `outputFormat` adalah format lampiran yang ditulis `RecordBlit` — yaitu
    /// target tampilan, bukan gambar HDR: pass ini berjalan **sesudah** tone
    /// mapping, supaya yang terlihat adalah warna yang ditulis shader dan bukan
    /// warna itu setelah dipetakan operator nada.
    bool Create(rhi::Device& device, const std::filesystem::path& shaderDirectory,
                VkFormat outputFormat);
    void Destroy();

    bool IsValid() const { return pipeline_.IsValid() && blitPipeline_ != VK_NULL_HANDLE; }

    /// Menyesuaikan diri dengan ukuran alokasi target. Mengembalikan true bila
    /// image-nya dibuat ulang.
    bool Adopt(uint32_t allocatedWidth, uint32_t allocatedHeight);

    /// Memindahkan image yang baru dibuat ke layout yang dijanjikan graph
    /// sebagai keadaan awalnya.
    ///
    /// Graph mengimpor image ini dalam keadaan `ShaderRead`, karena itulah
    /// keadaan yang ditinggalkan pass blit di akhir tiap frame. Itu benar untuk
    /// setiap frame kecuali yang pertama sesudah alokasi: image yang baru
    /// dibuat berada di `UNDEFINED`, dan barrier yang menyatakan sumbernya
    /// `SHADER_READ_ONLY` sedang berbohong kepada driver.
    void AdoptLayout(rhi::Device& device) const;

    VkImage Image() const { return image_; }

    /// Dispatch. Mengisi petak `width × height` di pojok kiri atas image.
    void RecordFill(VkCommandBuffer cmd, uint32_t width, uint32_t height) const;

    /// Menggambar hasilnya memenuhi lampiran yang sedang aktif. Pemanggil yang
    /// membuka dan menutup `vkCmdBeginRendering` — lampiran adalah urusan graph
    /// frame, dan membiarkannya di satu tempat saja adalah alasan graph itu ada.
    void RecordBlit(VkCommandBuffer cmd, uint32_t width, uint32_t height) const;

private:
    void DestroyImage();
    bool WriteDescriptors();

    rhi::Device* device_ = nullptr;
    rhi::ComputePipeline pipeline_;

    VkDescriptorSetLayout storageLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout sampledLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet storageSet_ = VK_NULL_HANDLE;
    VkDescriptorSet sampledSet_ = VK_NULL_HANDLE;

    VkPipelineLayout blitLayout_ = VK_NULL_HANDLE;
    VkPipeline blitPipeline_ = VK_NULL_HANDLE;

    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    uint32_t allocatedWidth_ = 0;
    uint32_t allocatedHeight_ = 0;
};

}  // namespace sim::render
