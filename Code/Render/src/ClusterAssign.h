#pragma once

#include "Sim/Core/Math.h"
#include "Sim/RHI/Buffer.h"
#include "Sim/RHI/Device.h"
#include "Sim/RHI/Pipeline.h"
#include "Sim/Render/LightCluster.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace sim::render {

/// Penetapan lampu punctual ke cluster, di GPU (G4).
///
/// **Yang pindah hanyalah penetapannya.** `ClusterGrid` tetap bebas Vulkan dan
/// tetap dibangun di CPU — ia yang mendefinisikan bentuk kisinya, dan seluruh
/// test tanpa-GPU-nya bersandar pada itu. Yang pindah adalah 3.456 uji
/// bola-terhadap-kotak yang sebelumnya dikerjakan satu core setiap frame.
///
/// **Jalur CPU tidak dihapus.** Ia jalur mundur dan, lebih penting, pembanding:
/// dua implementasi yang harus sepakat adalah cara termurah menemukan yang mana
/// yang salah. Sakelarnya ada di panel Statistics.
///
/// Keluarannya berjarak tetap, bukan padat: tiap cluster memiliki blok selebar
/// `maxLightsPerCluster` miliknya sendiri, sehingga tidak ada satu pun atomic di
/// jalur utama. Yang dibaca fragment shader tetap `uint2(offset, count)` yang
/// sama, jadi sisi pembacanya tidak berubah sama sekali.
class ClusterAssign {
public:
    /// Harus sama persis dengan `ViewLight` di
    /// `Shaders/cluster_assign.comp.slang`.
    struct GpuViewLight {
        /// xyz posisi ruang pandang, w jangkauan
        Vec4 positionRange{0.0f};
        /// xyz arah pancar ternormalisasi, w kosinus setengah sudut luar
        Vec4 directionCosOuter{0.0f, 0.0f, 1.0f, 1.0f};
        /// x: 1 untuk spot, 0 untuk point
        Vec4 flags{0.0f};
    };

    /// Sisi grup kerja. Sama dengan `numthreads` di
    /// `Shaders/cluster_assign.comp.slang`.
    static constexpr uint32_t kGroupSize = 64;
    /// Sama dengan banyaknya slot frame `VulkanRenderer`.
    ///
    /// **Dikunci `static_assert` di sana, bukan dipercayakan pada komentar
    /// ini.** Angka yang meleset di sini tidak menghasilkan galat apa pun: ia
    /// menghasilkan pembacaan di luar batas larik slot, yaitu VkBuffer sampah
    /// yang diserahkan ke descriptor — dan yang terlihat adalah device lost
    /// beberapa frame kemudian, jauh dari sebabnya.
    static constexpr uint32_t kSlots = 3;

    ClusterAssign() = default;
    ~ClusterAssign() { Destroy(); }

    ClusterAssign(const ClusterAssign&) = delete;
    ClusterAssign& operator=(const ClusterAssign&) = delete;

    bool Create(rhi::Device& device, const std::filesystem::path& shaderDirectory);
    void Destroy();

    bool IsValid() const { return pipeline_.IsValid(); }

    /// Menyiapkan buffer keluaran untuk kisi sebesar ini. Mengembalikan true
    /// bila buffer-nya dibuat ulang — pemanggil lalu harus menulis ulang
    /// descriptor yang menunjuknya.
    bool Adopt(uint32_t clusterCount, uint32_t maxLightsPerCluster);

    /// Menulis parameter kisi dan lampu ruang pandang untuk slot ini.
    ///
    /// Lampunya dipindahkan ke ruang pandang di sini, bukan di shader:
    /// memindahkan enam belas lampu sekali di CPU lebih murah daripada
    /// memindahkannya 3.456 kali di GPU — dan itu perhitungan yang sama dengan
    /// yang membuat jalur CPU melakukannya lebih dulu juga.
    void Upload(uint32_t slot, const ClusterGrid& grid, const Mat4& view,
                std::span<const ClusterLight> lights, uint32_t maxLightsPerCluster);

    /// Dispatch-nya. Barrier ke dan dari pass ini disimpulkan frame graph.
    void Record(VkCommandBuffer cmd, uint32_t slot) const;

    /// Banyaknya cluster yang daftarnya terpotong pada jalan terakhir slot ini.
    ///
    /// Dibaca dari memori host-visible yang ditulis shader. Sah hanya sesudah
    /// submit slot itu selesai — yaitu tepat pada saat `VulkanRenderer`
    /// memakainya kembali, yang sudah menunggu fence-nya.
    uint32_t Overflowed(uint32_t slot) const;

    VkBuffer RangeBuffer(uint32_t slot) const { return slots_[slot].ranges.buffer; }
    VkBuffer IndexBuffer(uint32_t slot) const { return slots_[slot].indices.buffer; }

private:
    /// Buffer device-local. Keluaran dispatch dibaca fragment shader pada frame
    /// yang sama dan tidak pernah disentuh CPU, jadi menaruhnya di memori
    /// host-visible hanya menyeretnya lewat PCIe dua kali.
    struct DeviceBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkDeviceSize bytes = 0;
    };

    struct Slot {
        rhi::DynamicBuffer params;
        rhi::DynamicBuffer lights;
        /// Satu `uint`. Host-visible karena CPU yang membacanya, dan hanya
        /// cluster yang benar-benar terpotong yang menyentuhnya — pada adegan
        /// yang lampunya lebih sedikit daripada batas per-cluster, tidak ada
        /// satu pun.
        rhi::DynamicBuffer overflow;
        DeviceBuffer ranges;
        DeviceBuffer indices;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };

    bool CreateDeviceBuffer(DeviceBuffer& target, VkDeviceSize bytes);
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
    /// Dipakai ulang tiap frame; alokasi per frame untuk enam belas lampu
    /// adalah alokasi yang tidak perlu ada.
    std::vector<GpuViewLight> viewLights_;
    uint32_t clusterCount_ = 0;
    uint32_t stride_ = 0;
};

}  // namespace sim::render
