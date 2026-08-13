#pragma once

#include "Sim/RHI/Texture3D.h"
#include "Sim/Render/VolumeTexture.h"

#include <vector>

namespace sim::render {

/// Sebuah volume `.vdb` yang sudah berada di GPU.
///
/// **Diunggah sekali, bukan tiap frame** — dan itu yang membedakannya dari
/// `SdfClipmapResource` di sebelahnya. Clipmap menulis ulang lempeng tepinya
/// setiap kamera bergerak, jadi ia memakai jalur `RecordRegionCopy` yang
/// direkam ke command buffer frame. Volume aset tidak berubah sama sekali
/// setelah dimuat, jadi `UploadRegion` yang menyubmit sendiri adalah bentuk
/// yang benar: ia dibayar sekali, di waktu muat.
class VolumeResource {
public:
    /// Menyandikan `grid` lalu mengunggahnya. Mengembalikan false bila gridnya
    /// kosong atau tekstur tidak bisa dibuat.
    bool Create(rhi::Device& device, const VolumeGrid& grid, VolumeTextureFormat format);
    void Destroy();

    bool IsValid() const { return texture_.IsValid(); }
    const VolumeTextureDesc& Desc() const { return desc_; }
    const rhi::Texture3D& Texture() const { return texture_; }

    /// Kotak volume di ruang lokalnya, sudah termasuk setengah voxel di tiap
    /// sisi — nilainya disimpan di pusat voxel, jadi kotak pusat-ke-pusat
    /// memotong separuh voxel terluar. Angka yang sama dipakai `IntersectBox`
    /// di acuan CPU-nya.
    void LocalBounds(Vec3& outMin, Vec3& outMax) const;

private:
    rhi::Texture3D texture_;
    VolumeTextureDesc desc_;
};

}  // namespace sim::render
