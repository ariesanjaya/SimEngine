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

    /// Angka antara uji occlusion untuk satu permukaan. Harus sama persis
    /// dengan `CullDebug` di `Shaders/draw_cull.comp.slang`.
    ///
    /// **Ada karena menebak sudah kehabisan giliran.** Sembilan hipotesis soal
    /// kenapa uji occlusion membuang permukaan yang terlihat sudah dipatahkan
    /// satu per satu, semuanya dengan membandingkan gambar. Yang belum pernah
    /// dilihat adalah angkanya sendiri: petak layar yang dipakai, kedalaman
    /// terdekat kotaknya, dan nilai yang dibaca dari piramida.
    struct GpuCullDebug {
        /// uvMin.xy, uvMax.xy
        Vec4 rect{0.0f};
        /// nearest, farthest, tingkat, hasil. Tingkat -1 berarti ada sudut di
        /// belakang kamera; -2 berarti gugur di frustum.
        Vec4 result{0.0f};
        Vec4 centre{0.0f};
        Vec4 extent{0.0f};
    };

    /// Fase culling. Keduanya menjalankan shader yang sama.
    enum class Phase : uint32_t {
        /// Sebelum depth prepass: frustum saja.
        Frustum = 0,
        /// Sesudah prepass dan piramida occlusion: frustum dan occlusion.
        Occlusion = 1,
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
    bool Upload(uint32_t slot, const Frustum& frustum, const Mat4& viewProjection,
                uint32_t viewportWidth, uint32_t viewportHeight, uint32_t pyramidLevels,
                std::span<const GpuBounds> bounds, std::span<const GpuSurface> surfaces);

    /// Menunjuk ulang binding piramida occlusion tiap slot.
    ///
    /// Dipanggil ulang setiap kali piramidanya dibuat ulang — yaitu setiap kali
    /// target render melewati ambang alokasinya. Descriptor yang masih menunjuk
    /// image lama menunjuk memori yang sudah dibebaskan.
    void AdoptPyramid(VkImageView view, VkSampler sampler, VkImageView depthView,
                      VkSampler depthSampler);

    /// Dispatch-nya. Barrier ke dan dari pass ini disimpulkan frame graph.
    ///
    /// `debug` bukan nol menuliskan angka antara tiap permukaan; ia hanya
    /// dipakai alat diagnostik, dan tanpa itu satu pun byte tidak ditulis.
    void Record(VkCommandBuffer cmd, uint32_t slot, Phase phase, bool debug,
                uint32_t limit = 0xffffffffu, uint32_t first = 0) const;

    /// Menyalin angka antara slot ini ke `out`. Sah hanya sesudah submit slot
    /// itu selesai — pemanggil yang menunggunya.
    void ReadDebug(uint32_t slot, std::vector<GpuCullDebug>& out) const;

    /// Buffer perintah fase frustum, dipakai depth prepass.
    VkBuffer CommandBuffer(uint32_t slot) const { return slots_[slot].commands.buffer; }
    /// Buffer perintah fase occlusion, dipakai pass forward.
    VkBuffer VisibleCommandBuffer(uint32_t slot) const {
        return slots_[slot].visibleCommands.buffer;
    }
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
        DeviceBuffer visibleCommands;
        /// Host-visible: yang membacanya CPU, dan hanya saat diminta.
        rhi::DynamicBuffer debug;
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
    /// Piramida occlusion. **Bukan milik kelas ini** — ia hidup di renderer,
    /// dan yang disimpan hanya cara menunjuknya.
    VkImageView pyramidView_ = VK_NULL_HANDLE;
    VkSampler pyramidSampler_ = VK_NULL_HANDLE;
    /// Depth buffer viewport, dipakai membandingkan piramida dengan sumbernya.
    VkImageView depthView_ = VK_NULL_HANDLE;
    VkSampler depthSampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::array<Slot, kSlots> slots_;
};

}  // namespace sim::render
