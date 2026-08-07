#include "SdfClipmapResource.h"

#include "Sim/Core/Log.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sim::render {
namespace {

constexpr VkFormat kSdfFormat = VK_FORMAT_R8_UNORM;

}  // namespace

bool SdfClipmapResource::Create(rhi::Device& device, const SdfClipmapSettings& settings) {
    Destroy();
    device_ = &device;
    volume_.Configure(settings);

    const uint32_t resolution = volume_.Clipmap().Settings().resolution;
    const glm::uvec3 extent(resolution);
    for (uint32_t cascade = 0; cascade < volume_.Clipmap().CascadeCount(); ++cascade) {
        if (!textures_[cascade].Create(device, extent, kSdfFormat, 1)) {
            Destroy();
            return false;
        }
    }
    scratch_.reserve(static_cast<std::size_t>(resolution) * resolution);
    return true;
}

void SdfClipmapResource::Destroy() {
    for (rhi::Texture3D& texture : textures_) {
        texture.Destroy();
    }
    device_ = nullptr;
}

VkDeviceSize SdfClipmapResource::StagingBytes() const {
    const uint64_t resolution = volume_.Clipmap().Settings().resolution;
    return static_cast<VkDeviceSize>(resolution * resolution * resolution *
                                     volume_.Clipmap().CascadeCount());
}

uint64_t SdfClipmapResource::Update(const Vec3& cameraPosition,
                                    std::span<const MeshInstance> meshes,
                                    rhi::DynamicBuffer& staging) {
    pending_.clear();
    pendingSource_ = VK_NULL_HANDLE;
    if (!IsValid()) {
        return 0;
    }

    const SdfScrollResult scroll = volume_.Clipmap().Scroll(cameraPosition);
    if (scroll.regions.empty()) {
        return 0;
    }
    // Medan jaraknya dibangun setelah `Scroll`, bukan sebelum: frame yang tidak
    // menggeser satu kaskade pun — yaitu kebanyakan frame saat kamera diam —
    // tidak perlu membalik satu matriks pun.
    field_.Build(meshes);
    volume_.ResetWriteCount();
    volume_.Fill(scroll, field_);

    if (!staging.Reserve(StagingBytes())) {
        return volume_.WrittenVoxels();
    }
    pendingSource_ = staging.Handle();

    // Voxel yang baru ditulis dikemas ke satu buffer staging, satu wilayah demi
    // satu wilayah. Pembagian toroidalnya sudah dilakukan `SplitWrapped`, jadi
    // setiap kotak di sini dijamin tidak melewati tepi tekstur — dan
    // `RecordRegionCopy` menolak yang melewatinya alih-alih menjepit, supaya
    // kelalaian di sini tidak tersembunyi di balik gambar yang hampir benar.
    const uint32_t resolution = volume_.Clipmap().Settings().resolution;
    VkDeviceSize at = 0;
    for (const SdfScrollRegion& region : scroll.regions) {
        volume_.Clipmap().SplitWrapped(region, boxes_);
        for (const SdfClipmap::TexelBox& box : boxes_) {
            const glm::uvec3 size = box.max - box.min;
            const std::size_t bytes = static_cast<std::size_t>(size.x) * size.y * size.z;
            scratch_.resize(bytes);
            // Baris demi baris, bukan voxel demi voxel: sebuah kotak texel rapat
            // pada sumbu X, jadi tiap barisnya satu salinan.
            const std::span<const uint8_t> source = volume_.Data(region.cascade);
            std::size_t cursor = 0;
            for (uint32_t z = box.min.z; z < box.max.z; ++z) {
                for (uint32_t y = box.min.y; y < box.max.y; ++y) {
                    const std::size_t rowStart =
                        (static_cast<std::size_t>(z) * resolution + y) * resolution + box.min.x;
                    std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(rowStart), size.x,
                                scratch_.begin() + static_cast<std::ptrdiff_t>(cursor));
                    cursor += size.x;
                }
            }
            if (!staging.WriteAt(at, scratch_.data(), bytes)) {
                SIM_ERROR("Render", "SDF staging buffer is too small for {} bytes at {}", bytes,
                          at);
                break;
            }
            pending_.push_back({region.cascade, box.min, size, at});
            at += bytes;
        }
    }
    return volume_.WrittenVoxels();
}

void SdfClipmapResource::RecordUploads(VkCommandBuffer cmd) {
    if (pending_.empty() || pendingSource_ == VK_NULL_HANDLE) {
        return;
    }
    // Barrier per tekstur, bukan per wilayah. Sebuah kaskade yang menerima tiga
    // lempeng dan delapan potongan toroidal tetap hanya berpindah layout dua
    // kali.
    std::array<bool, kMaxSdfCascades> touched{};
    for (const PendingCopy& copy : pending_) {
        touched[copy.cascade] = true;
    }
    for (uint32_t cascade = 0; cascade < kMaxSdfCascades; ++cascade) {
        if (touched[cascade]) {
            textures_[cascade].RecordTransition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        }
    }
    for (const PendingCopy& copy : pending_) {
        textures_[copy.cascade].RecordRegionCopy(cmd, pendingSource_, copy.sourceOffset,
                                                 copy.offset, copy.extent);
    }
    for (uint32_t cascade = 0; cascade < kMaxSdfCascades; ++cascade) {
        if (touched[cascade]) {
            textures_[cascade].RecordTransition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }
    pending_.clear();
    pendingSource_ = VK_NULL_HANDLE;
}

}  // namespace sim::render
