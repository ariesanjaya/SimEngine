#pragma once

#include "Sim/Core/Math.h"
#include "Sim/RHI/Buffer.h"
#include "Sim/RHI/Device.h"
#include "Sim/RHI/Pipeline.h"
#include "Sim/Render/Frustum.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace sim::render {

/// Frustum culling di GPU yang menghasilkan perintah gambar (G6).
///
/// **Yang pindah bukan penyaringannya melainkan keputusannya.** Sampai G6, CPU
/// yang memutuskan apa yang digambar: `Gather` mengujinya terhadap frustum lalu
/// `SplitRuns` memecah daftar ruas menjadi potongan-potongan yang hanya memuat
/// yang lolos. Biayanya tumbuh linear terhadap isi adegan, dan ia dibayar di
/// thread yang juga harus merekam command buffer.
///
/// Di sini keputusan itu menjadi sebuah buffer perintah: satu
/// `VkDrawIndexedIndirectCommand` per permukaan, `instanceCount` nol untuk yang
/// tersaring. CPU tinggal menyebut rentangnya — satu
/// `vkCmdDrawIndexedIndirect` per ruas, berapa pun banyaknya permukaan di
/// dalamnya.
///
/// **Keluarannya tidak bergantung pada urutan thread.** Tiap permukaan menulis
/// slot tetapnya sendiri, jadi tidak ada satu pun atomic dan tidak ada
/// pemadatan. Yang ditukar: GPU tetap melewati perintah yang `instanceCount`-nya
/// nol. Yang didapat: dua jalan dari binary yang sama menghasilkan gambar yang
/// sama persis, yaitu syarat setiap perbandingan gambar di repo ini.
class DrawCull {
public:
    /// Kotak dunia sebuah permukaan. Harus sama persis dengan `Bounds` di
    /// `Shaders/draw_cull.comp.slang`.
    ///
    /// Pusat dan setengah-lebar, bukan min/max: `Frustum::Intersects`
    /// menghitung keduanya dari min/max setiap kali dipanggil, dan menghitungnya
    /// sekali di sini membuat kedua sisi memakai angka yang sama persis.
    struct GpuBounds {
        Vec4 centre{0.0f};
        Vec4 extent{0.0f};
    };

    /// Rentang indeks ruas mesh sebuah permukaan. Harus sama persis dengan
    /// `Surface` di shader.
    struct GpuSurface {
        uint32_t indexCount = 0;
        uint32_t firstIndex = 0;
    };

    /// Sisi grup kerja. Sama dengan `numthreads` di shader.
    static constexpr uint32_t kGroupSize = 64;
    /// Sama dengan banyaknya slot frame `VulkanRenderer`. Dikunci
    /// `static_assert` di sana.
    static constexpr uint32_t kSlots = 3;

    DrawCull() = default;
    ~DrawCull() { Destroy(); }

    DrawCull(const DrawCull&) = delete;
    DrawCull& operator=(const DrawCull&) = delete;

    bool Create(rhi::Device& device, const std::filesystem::path& shaderDirectory);
    void Destroy();

    bool IsValid() const { return pipeline_.IsValid(); }

    /// Menulis frustum dan daftar permukaan untuk slot ini, lalu menyiapkan
    /// buffer perintahnya. Mengembalikan false bila buffernya gagal dibuat —
    /// pemanggil lalu menggambar lewat jalur CPU.
    ///
    /// `bounds` dan `surfaces` harus sama panjang: keduanya diindeks nomor
    /// permukaan yang sama.
    bool Upload(uint32_t slot, const Frustum& frustum, std::span<const GpuBounds> bounds,
                std::span<const GpuSurface> surfaces);

    /// Dispatch-nya. Barrier ke dan dari pass ini disimpulkan frame graph.
    void Record(VkCommandBuffer cmd, uint32_t slot) const;

    /// Buffer perintah slot ini, siap diserahkan ke `vkCmdDrawIndexedIndirect`.
    VkBuffer CommandBuffer(uint32_t slot) const { return slots_[slot].commands.buffer; }
    /// Banyaknya permukaan yang perintahnya sah di slot ini.
    uint32_t SurfaceCount(uint32_t slot) const { return slots_[slot].surfaceCount; }

private:
    /// Buffer device-local. Isinya ditulis dispatch dan dibaca tahap
    /// `DRAW_INDIRECT` pada frame yang sama; CPU tidak pernah menyentuhnya.
    struct DeviceBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkDeviceSize bytes = 0;
    };

    struct Slot {
        rhi::DynamicBuffer params;
        rhi::DynamicBuffer bounds;
        rhi::DynamicBuffer surfaces;
        DeviceBuffer commands;
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint32_t surfaceCount = 0;
    };

    bool CreateCommandBuffer(DeviceBuffer& target, VkDeviceSize bytes);
    void DestroyDeviceBuffer(DeviceBuffer& target);
    /// **Per slot, bukan sekaligus.** Memperbarui descriptor set yang masih
    /// dipakai command buffer yang belum selesai adalah perilaku tak
    /// terdefinisi, dan slot yang lain memang bisa sedang berjalan.
    void WriteSlotDescriptors(uint32_t slot);

    rhi::Device* device_ = nullptr;
    rhi::ComputePipeline pipeline_;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::array<Slot, kSlots> slots_;
};

}  // namespace sim::render
