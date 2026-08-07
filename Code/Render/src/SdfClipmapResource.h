#pragma once

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

    /// Menggeser kaskade mengikuti kamera lalu mengunggah wilayah yang basi.
    ///
    /// `meshes` dipakai membangun medan jaraknya. Mengembalikan banyaknya voxel
    /// yang benar-benar ditulis — angka yang dipakai membuktikan pembaruan
    /// parsial memang parsial, dan yang akan diawasi terhadap anggaran 0,4 ms.
    uint64_t Update(const Vec3& cameraPosition, std::span<const MeshInstance> meshes);

    const rhi::Texture3D& Texture(uint32_t cascade) const { return textures_[cascade]; }
    uint32_t CascadeCount() const { return volume_.Clipmap().CascadeCount(); }

private:
    rhi::Device* device_ = nullptr;
    SdfVolume volume_;
    std::array<rhi::Texture3D, kMaxSdfCascades> textures_;
    std::vector<SdfClipmap::TexelBox> boxes_;
    std::vector<uint8_t> scratch_;
};

}  // namespace sim::render
