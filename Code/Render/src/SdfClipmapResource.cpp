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

uint64_t SdfClipmapResource::Update(const Vec3& cameraPosition,
                                    std::span<const MeshInstance> meshes) {
    if (!IsValid()) {
        return 0;
    }
    std::vector<Mat4> inverses;
    std::vector<Vec3> halfExtents;
    std::vector<float> scales;
    const SdfVolume::DistanceField field =
        MakeBoxSceneField(meshes, inverses, halfExtents, scales);

    const SdfScrollResult scroll = volume_.Clipmap().Scroll(cameraPosition);
    if (scroll.regions.empty()) {
        return 0;
    }
    volume_.ResetWriteCount();
    volume_.Fill(scroll, field);

    // Voxel yang baru ditulis disalin ke GPU wilayah demi wilayah. Pembagian
    // toroidalnya sudah dilakukan `SplitWrapped`, jadi setiap kotak di sini
    // dijamin tidak melewati tepi tekstur — dan `UploadRegion` menolak yang
    // melewatinya alih-alih menjepit, supaya kelalaian di sini tidak
    // tersembunyi di balik gambar yang hampir benar.
    for (const SdfScrollRegion& region : scroll.regions) {
        volume_.Clipmap().SplitWrapped(region, boxes_);
        for (const SdfClipmap::TexelBox& box : boxes_) {
            const glm::uvec3 size = box.max - box.min;
            scratch_.resize(static_cast<std::size_t>(size.x) * size.y * size.z);
            std::size_t at = 0;
            for (uint32_t z = box.min.z; z < box.max.z; ++z) {
                for (uint32_t y = box.min.y; y < box.max.y; ++y) {
                    for (uint32_t x = box.min.x; x < box.max.x; ++x) {
                        scratch_[at++] = volume_.At(region.cascade, {x, y, z});
                    }
                }
            }
            textures_[region.cascade].UploadRegion(
                box.min, size,
                {reinterpret_cast<const std::byte*>(scratch_.data()), scratch_.size()});
        }
    }
    return volume_.WrittenVoxels();
}

}  // namespace sim::render
