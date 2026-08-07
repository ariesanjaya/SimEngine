#include "Sim/RHI/TextureCube.h"

#include "Sim/Core/Log.h"
#include "Sim/RHI/Buffer.h"

#include <algorithm>
#include <vector>

namespace sim::rhi {

std::size_t TextureCube::TexelBytes(uint32_t size, uint32_t mipCount, uint32_t bytesPerTexel) {
    std::size_t total = 0;
    for (uint32_t mip = 0; mip < mipCount; ++mip) {
        const std::size_t extent = std::max(size >> mip, 1u);
        total += extent * extent * 6 * bytesPerTexel;
    }
    return total;
}

TextureCube::~TextureCube() {
    Destroy();
}

bool TextureCube::Create(Device& device, uint32_t size, uint32_t mipCount, VkFormat format,
                         uint32_t bytesPerTexel, std::span<const std::byte> texels) {
    if (size == 0 || mipCount == 0 || bytesPerTexel == 0) {
        return false;
    }
    const std::size_t needed = TexelBytes(size, mipCount, bytesPerTexel);
    if (texels.size() < needed) {
        // Ditolak, bukan dipotong. Data yang kurang berarti mip terakhir berisi
        // apa pun yang kebetulan ada di memori — dan itu muncul sebagai pantulan
        // yang berkilat-kilat pada material paling kasar, yang paling mudah
        // dikira masalah prefilter.
        SIM_ERROR("RHI", "cubemap upload needs {} bytes but got {}", needed, texels.size());
        return false;
    }

    Destroy();
    device_ = &device;
    size_ = size;
    mipCount_ = mipCount;

    DynamicBuffer staging;
    if (!staging.Create(device, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, needed) ||
        !staging.Write(texels.data(), needed)) {
        SIM_ERROR("RHI", "cannot stage {}x{} cubemap upload", size, size);
        Destroy();
        return false;
    }

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    // Bendera ini yang membuat enam lapisnya bisa dibaca sebagai kubus, bukan
    // sebagai larik biasa. Melupakannya menghasilkan image yang sah tapi tidak
    // bisa dibuatkan view bertipe CUBE.
    imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {size, size, 1};
    imageInfo.mipLevels = mipCount;
    imageInfo.arrayLayers = 6;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    const VkResult created = vmaCreateImage(device_->Allocator(), &imageInfo, &allocationInfo,
                                            &image_, &allocation_, nullptr);
    if (created != VK_SUCCESS) {
        SIM_ERROR("RHI", "vmaCreateImage failed for cubemap: {}", ResultToString(created));
        image_ = VK_NULL_HANDLE;
        Destroy();
        return false;
    }

    // Satu region per mip, masing-masing mencakup keenam mukanya — itulah yang
    // membuat tata letak "mip demi mip, muka demi muka" bisa disalin dengan
    // sejumlah region sebanyak mip, bukan sebanyak mip kali enam.
    std::vector<VkBufferImageCopy> regions;
    regions.reserve(mipCount);
    VkDeviceSize offset = 0;
    for (uint32_t mip = 0; mip < mipCount; ++mip) {
        const uint32_t extent = std::max(size >> mip, 1u);
        VkBufferImageCopy region{};
        region.bufferOffset = offset;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = mip;
        region.imageSubresource.layerCount = 6;
        region.imageExtent = {extent, extent, 1};
        regions.push_back(region);
        offset += static_cast<VkDeviceSize>(extent) * extent * 6 * bytesPerTexel;
    }

    VkImageSubresourceRange whole{};
    whole.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    whole.levelCount = mipCount;
    whole.layerCount = 6;

    const auto barrier = [&](VkCommandBuffer cmd, VkImageLayout from, VkImageLayout to,
                             VkPipelineStageFlags2 sourceStage, VkAccessFlags2 sourceAccess,
                             VkPipelineStageFlags2 destinationStage,
                             VkAccessFlags2 destinationAccess) {
        VkImageMemoryBarrier2 image{};
        image.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        image.srcStageMask = sourceStage;
        image.srcAccessMask = sourceAccess;
        image.dstStageMask = destinationStage;
        image.dstAccessMask = destinationAccess;
        image.oldLayout = from;
        image.newLayout = to;
        image.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image.image = image_;
        image.subresourceRange = whole;

        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &image;
        vkCmdPipelineBarrier2(cmd, &dependency);
    };

    VkCommandBuffer cmd = device_->BeginOneShot();
    barrier(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(cmd, staging.Handle(), image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(regions.size()), regions.data());
    barrier(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    device_->EndOneShot(cmd);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    viewInfo.format = format;
    viewInfo.subresourceRange = whole;
    SIM_VK_CHECK(vkCreateImageView(device_->Handle(), &viewInfo, nullptr, &view_));

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    // Interpolasi antar-mip: kekasaran yang berada di antara dua mip harus
    // menghasilkan pantulan di antara keduanya, bukan melompat.
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    SIM_VK_CHECK(vkCreateSampler(device_->Handle(), &samplerInfo, nullptr, &sampler_));
    return true;
}

void TextureCube::Destroy() {
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
    size_ = 0;
    mipCount_ = 0;
}

}  // namespace sim::rhi
