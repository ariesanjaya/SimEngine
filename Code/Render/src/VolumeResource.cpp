#include "VolumeResource.h"

#include "Sim/Core/Log.h"

namespace sim::render {

bool VolumeResource::Create(rhi::Device& device, const VolumeGrid& grid,
                            VolumeTextureFormat format) {
    Destroy();

    std::vector<std::byte> bytes;
    if (!EncodeVolume(grid, format, bytes, desc_)) {
        SIM_WARN("Render", "volume '{}' is empty; nothing to upload", grid.name);
        return false;
    }

    const VkFormat vulkanFormat =
        format == VolumeTextureFormat::R16Unorm ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;
    const glm::uvec3 extent(desc_.sizeX, desc_.sizeY, desc_.sizeZ);
    if (!texture_.Create(device, extent, vulkanFormat,
                         static_cast<uint32_t>(desc_.BytesPerTexel()))) {
        SIM_WARN("Render", "cannot create a {}x{}x{} volume texture for '{}'", desc_.sizeX,
                 desc_.sizeY, desc_.sizeZ, grid.name);
        desc_ = VolumeTextureDesc{};
        return false;
    }

    if (!texture_.UploadRegion(glm::uvec3(0), extent, bytes)) {
        SIM_WARN("Render", "cannot upload volume '{}'", grid.name);
        Destroy();
        return false;
    }

    SIM_INFO("Render", "volume '{}' uploaded: {}x{}x{}, {} KB, values {:.4g}..{:.4g}", grid.name,
             desc_.sizeX, desc_.sizeY, desc_.sizeZ, bytes.size() / 1024,
             static_cast<double>(desc_.bias),
             static_cast<double>(desc_.bias + desc_.scale));
    return true;
}

void VolumeResource::Destroy() {
    texture_.Destroy();
    desc_ = VolumeTextureDesc{};
}

void VolumeResource::LocalBounds(Vec3& outMin, Vec3& outMax) const {
    // Lewat `VolumeGrid` supaya aturan setengah-voxel-nya punya satu definisi.
    // Wireframe di editor memakai yang sama; dua salinan akan bergeser
    // sendiri-sendiri, dan kotak bantu yang tidak cocok dengan yang dijejaki
    // adalah kotak yang berbohong tepat ketika ia paling dibutuhkan.
    VolumeGrid shape;
    shape.origin = desc_.origin;
    shape.voxelSize = desc_.voxelSize;
    shape.sizeX = desc_.sizeX;
    shape.sizeY = desc_.sizeY;
    shape.sizeZ = desc_.sizeZ;
    shape.PaddedLocalBounds(outMin, outMax);
}

}  // namespace sim::render
