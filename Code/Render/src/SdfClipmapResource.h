#pragma once

#include "Sim/RHI/Buffer.h"
#include "Sim/RHI/Pipeline.h"
#include "Sim/RHI/Texture3D.h"
#include "Sim/Render/SdfVolume.h"
#include "Sim/Render/Types.h"

#include <array>
#include <filesystem>
#include <span>
#include <vector>

namespace sim::render {

/// Kaskade SDF di GPU, beserta pembaruannya dari sisi CPU.
///
/// **Geometrinya masih kotak, dan itu batas yang disengaja sejak M1.** Rencana
/// GI menyebut bake per-mesh menjadi brick sparse; selama geometrinya kotak,
/// medan jaraknya punya bentuk analitik yang tepat — jadi yang diuji di sini
/// benar-benar clipmap dan sphere tracing-nya, bukan ketelitian sebuah baker
/// yang belum ada.
///
/// **Sejak G4 kompositnya punya dua jalur.** Yang di GPU mengevaluasi medan
/// jaraknya satu voxel per thread dan menulis langsung ke tekstur kaskade —
/// tanpa larik byte di CPU, tanpa staging buffer, tanpa pengemasan per wilayah.
/// Yang di CPU tetap ada sebagai jalur mundur, dan sebagai pembanding: satu-
/// satunya cara memeriksa jalur GPU adalah membandingkan isi voxelnya byte demi
/// byte dengan hasil jalur CPU, karena satu-satunya pembaca kaskade ini adalah
/// penelusuran GI — dan GI tidak deterministik.
///
/// Jalur GPU menuntut `VK_FORMAT_R8_UNORM` bisa dipakai sebagai storage image.
/// Itu bukan jaminan spesifikasi, jadi ia ditanyakan saat pembuatan dan jalur
/// CPU yang dipakai bila jawabannya tidak.
class SdfClipmapResource {
public:
    SdfClipmapResource() = default;
    ~SdfClipmapResource() { Destroy(); }

    SdfClipmapResource(const SdfClipmapResource&) = delete;
    SdfClipmapResource& operator=(const SdfClipmapResource&) = delete;

    bool Create(rhi::Device& device, const SdfClipmapSettings& settings);
    void Destroy();

    bool IsValid() const { return textures_[0].IsValid(); }
    const SdfVolume& Volume() const { return volume_; }

    /// Menggeser kaskade mengikuti kamera lalu mengemas wilayah yang basi ke
    /// `staging`.
    ///
    /// **Tidak menyentuh queue.** Salinannya direkam `RecordUploads` ke command
    /// buffer frame. Bentuk sebelumnya memanggil `UploadRegion` per wilayah, dan
    /// tiap panggilan itu menyubmit sendiri lalu menunggu queue idle — belasan
    /// kali per frame. Yang terukur di panel Statistics: 3,98 ms untuk 16 ribu
    /// voxel, hampir seluruhnya menunggu, bukan menghitung.
    ///
    /// `staging` harus milik slot frame yang sedang direkam: isinya masih dibaca
    /// GPU sampai submit slot itu selesai. Mengembalikan banyaknya voxel yang
    /// benar-benar ditulis — angka yang diawasi terhadap anggaran 0,4 ms.
    uint64_t Update(const Vec3& cameraPosition, std::span<const MeshInstance> meshes,
                    rhi::DynamicBuffer& staging, uint32_t slot);

    /// Merekam salinan atau dispatch yang disiapkan `Update` ke command buffer
    /// frame. Jalur mana yang direkam ditentukan `SetGpuFill`.
    void RecordUploads(VkCommandBuffer cmd);

    /// Menyiapkan jalur compute. Mengembalikan false — dan meninggalkan jalur
    /// CPU aktif — bila shadernya tidak ada atau format kaskadenya tidak bisa
    /// dipakai sebagai storage image di perangkat ini.
    bool CreateGpuFill(const std::filesystem::path& shaderDirectory);

    /// Menyalakan jalur compute. Diabaikan bila `CreateGpuFill` gagal.
    void SetGpuFill(bool enabled) { gpuFill_ = enabled && fill_.IsValid(); }
    bool GpuFillActive() const { return gpuFill_; }
    bool GpuFillAvailable() const { return fill_.IsValid(); }

    /// Banyaknya slot frame yang buffer entrinya disediakan. Sama dengan
    /// `VulkanRenderer`; dikunci `static_assert` di sana.
    static constexpr uint32_t kSlots = 3;
    /// Sisi grup kerja, sama dengan `numthreads` di Shaders/sdf_fill.comp.slang.
    static constexpr uint32_t kGroupSize = 64;

    const rhi::Texture3D& Texture(uint32_t cascade) const { return textures_[cascade]; }
    uint32_t CascadeCount() const { return volume_.Clipmap().CascadeCount(); }

    /// Byte staging terbesar yang mungkin dibutuhkan satu pembaruan: seluruh
    /// isi setiap kaskade, yaitu yang terjadi saat kamera melompat lebih jauh
    /// daripada lebar clipmap-nya sendiri.
    VkDeviceSize StagingBytes() const;

private:
    /// Satu salinan yang menunggu direkam.
    struct PendingCopy {
        uint32_t cascade = 0;
        glm::uvec3 offset{0};
        glm::uvec3 extent{0};
        VkDeviceSize sourceOffset = 0;
    };

    /// Satu dispatch yang menunggu direkam: kotak texel beserta voxel dunia
    /// yang bersesuaian dengannya.
    struct PendingFill {
        uint32_t cascade = 0;
        glm::uvec3 texelMin{0};
        glm::ivec3 worldMin{0};
        glm::uvec3 extent{0};
    };

    bool WriteFillDescriptors();
    /// Menunjuk ulang set entri **satu slot** ke buffer barunya.
    ///
    /// Terpisah dari `WriteFillDescriptors` karena keduanya dipanggil pada saat
    /// yang sangat berbeda: yang itu saat start, ketika belum ada satu pun
    /// command buffer yang berjalan; yang ini di tengah frame, ketika slot lain
    /// bisa saja masih dibaca GPU.
    void WriteEntryDescriptor(uint32_t slot);
    void RecordFills(VkCommandBuffer cmd);

    rhi::Device* device_ = nullptr;
    SdfVolume volume_;
    std::array<rhi::Texture3D, kMaxSdfCascades> textures_;
    std::vector<SdfClipmap::TexelBox> boxes_;
    std::vector<uint8_t> scratch_;
    std::vector<PendingCopy> pending_;
    VkBuffer pendingSource_ = VK_NULL_HANDLE;
    // Dipakai ulang tiap frame supaya medan jaraknya tidak mengalokasi vektor
    // baru setiap kali kamera bergerak satu voxel.
    BoxSceneField field_;

    // --- jalur compute (G4) ---
    rhi::ComputePipeline fill_;
    VkDescriptorSetLayout cascadeLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout entryLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool fillPool_ = VK_NULL_HANDLE;
    /// Satu set per kaskade: storage image-nya.
    std::array<VkDescriptorSet, kMaxSdfCascades> cascadeSets_{};
    /// Satu set dan satu buffer per slot frame: entri medan jaraknya, yang
    /// berganti tiap frame.
    std::array<VkDescriptorSet, kSlots> entrySets_{};
    std::array<rhi::DynamicBuffer, kSlots> entryBuffers_;
    std::vector<BoxSceneField::GpuEntry> gpuEntries_;
    std::vector<PendingFill> pendingFills_;
    uint32_t fillSlot_ = 0;
    bool storageCapable_ = false;
    uint32_t fillEntryCount_ = 0;
    bool gpuFill_ = false;
};

}  // namespace sim::render
