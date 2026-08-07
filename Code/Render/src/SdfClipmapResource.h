#pragma once

#include "Sim/RHI/Buffer.h"
#include "Sim/RHI/Texture3D.h"
#include "Sim/Render/SdfVolume.h"
#include "Sim/Render/Types.h"

#include <array>
#include <span>
#include <vector>

namespace sim::render {

/// Kaskade SDF di GPU, beserta pembaruannya dari sisi CPU.
///
/// **Komposit dan penyandiannya masih di CPU, dan itu batas yang disengaja
/// untuk M1.** Rencana GI menyebut bake per-mesh menjadi brick sparse, dan itu
/// menuntut importir mesh yang baru datang di E8.4. Selama geometrinya masih
/// kotak, medan jaraknya punya bentuk analitik yang tepat — jadi yang diuji di
/// sini benar-benar clipmap dan sphere tracing-nya, bukan ketelitian sebuah
/// baker yang belum ada.
///
/// Harganya jelas dan sudah diukur: unggahan lewat staging buffer per wilayah,
/// dan evaluasi medan jarak per voxel di CPU. Itu yang membatasi resolusi ke
/// 64³ untuk sekarang; 128³ yang diminta rencana menunggu komposit compute.
class SdfClipmapResource {
public:
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
                    rhi::DynamicBuffer& staging);

    /// Merekam salinan yang disiapkan `Update` ke command buffer frame.
    void RecordUploads(VkCommandBuffer cmd);

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
};

}  // namespace sim::render
