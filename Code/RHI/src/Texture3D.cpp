#include "Sim/RHI/Texture3D.h"

#include "Sim/Core/Log.h"
#include "Sim/RHI/Buffer.h"

namespace sim::rhi {

Texture3D::~Texture3D() {
    Destroy();
}

bool Texture3D::Create(Device& device, const glm::uvec3& extent, VkFormat format,
                       uint32_t bytesPerTexel) {
    if (extent.x == 0 || extent.y == 0 || extent.z == 0 || bytesPerTexel == 0) {
        return false;
    }
    Destroy();
    device_ = &device;
    extent_ = extent;
    bytesPerTexel_ = bytesPerTexel;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_3D;
    imageInfo.format = format;
    imageInfo.extent = {extent.x, extent.y, extent.z};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(device_->Allocator(), &imageInfo, &allocationInfo, &image_, &allocation_,
                       nullptr) != VK_SUCCESS) {
        SIM_ERROR("RHI", "cannot allocate a {}x{}x{} volume texture", extent.x, extent.y,
                  extent.z);
        image_ = VK_NULL_HANDLE;
        Destroy();
        return false;
    }

    // Dipindahkan ke SHADER_READ_ONLY sejak dibuat, bukan pada unggahan
    // pertama. Pass yang membacanya sebelum ada satu wilayah pun diunggah akan
    // membaca image dalam layout yang tidak sah — dan clipmap memang punya
    // kaskade yang tidak tersentuh selama kamera tidak bergerak.
    VkCommandBuffer cmd = device_->BeginOneShot();
    Transition(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    device_->EndOneShot(cmd);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    SIM_VK_CHECK(vkCreateImageView(device_->Handle(), &viewInfo, nullptr, &view_));

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    // REPEAT, bukan CLAMP. Pengalamatan clipmap memang toroidal: koordinat yang
    // melewati tepi harus muncul kembali di sisi seberangnya, dan sampler yang
    // menjepit akan mengoles texel tepi sepanjang seluruh sisi.
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    SIM_VK_CHECK(vkCreateSampler(device_->Handle(), &samplerInfo, nullptr, &sampler_));
    return true;
}

void Texture3D::Transition(VkCommandBuffer cmd, VkImageLayout from, VkImageLayout to) {
    const bool toTransfer = to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = toTransfer ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                      : VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.srcAccessMask = toTransfer ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
                                       : VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = toTransfer ? VK_PIPELINE_STAGE_2_COPY_BIT
                                      : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = toTransfer ? VK_ACCESS_2_TRANSFER_WRITE_BIT
                                       : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    if (from == VK_IMAGE_LAYOUT_UNDEFINED) {
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_NONE;
    }
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image_;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

bool Texture3D::UploadRegion(const glm::uvec3& offset, const glm::uvec3& extent,
                             std::span<const std::byte> texels) {
    if (!IsValid() || extent.x == 0 || extent.y == 0 || extent.z == 0) {
        return false;
    }
    const std::size_t needed =
        static_cast<std::size_t>(extent.x) * extent.y * extent.z * bytesPerTexel_;
    if (texels.size() < needed) {
        SIM_ERROR("RHI", "volume upload needs {} bytes but got {}", needed, texels.size());
        return false;
    }
    if (offset.x + extent.x > extent_.x || offset.y + extent.y > extent_.y ||
        offset.z + extent.z > extent_.z) {
        // Ditolak, bukan dijepit. Sub-wilayah yang melewati tepi berarti
        // pemanggilnya belum memecah wilayah toroidalnya — dan menjepitnya di
        // sini akan menyembunyikan kesalahan itu di balik gambar yang hampir
        // benar.
        SIM_ERROR("RHI", "volume upload region leaves the texture");
        return false;
    }

    DynamicBuffer staging;
    if (!staging.Create(*device_, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, needed) ||
        !staging.Write(texels.data(), needed)) {
        return false;
    }

    VkCommandBuffer cmd = device_->BeginOneShot();
    Transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {static_cast<int32_t>(offset.x), static_cast<int32_t>(offset.y),
                          static_cast<int32_t>(offset.z)};
    region.imageExtent = {extent.x, extent.y, extent.z};
    vkCmdCopyBufferToImage(cmd, staging.Handle(), image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &region);

    Transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    device_->EndOneShot(cmd);
    return true;
}

void Texture3D::Destroy() {
    if (device_ == nullptr) {
        return;
    }
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_->Handle(), sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    if (view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_->Handle(), view_, nullptr);
        view_ = VK_NULL_HANDLE;
    }
    if (image_ != VK_NULL_HANDLE) {
        vmaDestroyImage(device_->Allocator(), image_, allocation_);
        image_ = VK_NULL_HANDLE;
        allocation_ = VK_NULL_HANDLE;
    }
    extent_ = glm::uvec3(0);
}

}  // namespace sim::rhi
