#include "Sim/RHI/Texture.h"

#include "Sim/Core/Assert.h"
#include "Sim/Core/Log.h"
#include "Sim/RHI/Buffer.h"

#include <span>
#include <utility>
#include <vector>

namespace sim::rhi {
namespace {

/// Barrier untuk memindahkan layout image.
void TransitionLayout(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                      VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage,
                      VkAccessFlags sourceAccess, VkAccessFlags destinationAccess,
                      uint32_t levelCount = 1) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    // Seluruh level sekaligus. Barrier yang hanya menyebut level 0 meninggalkan
    // sisanya dalam layout `UNDEFINED` — validation layer menyebutnya, dan yang
    // tergambar tanpa validation layer adalah mip yang isinya sampah.
    barrier.subresourceRange.levelCount = levelCount;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;

    vkCmdPipelineBarrier(cmd, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
}

/// Apakah sebuah `VkFormat` membawa blok terkompresi.
///
/// Rentangnya berurutan menurut spesifikasi Vulkan — BC1 sampai BC7 — jadi satu
/// perbandingan cukup, dan tidak ada tabel yang bisa ketinggalan satu baris.
bool IsBlockCompressed(VkFormat format) {
    return format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_BC7_SRGB_BLOCK;
}

}  // namespace

Texture2D::~Texture2D() {
    Destroy();
}

Texture2D::Texture2D(Texture2D&& other) noexcept {
    *this = std::move(other);
}

Texture2D& Texture2D::operator=(Texture2D&& other) noexcept {
    if (this != &other) {
        Destroy();
        device_ = other.device_;
        image_ = other.image_;
        allocation_ = other.allocation_;
        view_ = other.view_;
        sampler_ = other.sampler_;
        width_ = other.width_;
        height_ = other.height_;
        gpuBytes_ = other.gpuBytes_;
        levels_ = other.levels_;

        other.device_ = nullptr;
        other.image_ = VK_NULL_HANDLE;
        other.allocation_ = VK_NULL_HANDLE;
        other.view_ = VK_NULL_HANDLE;
        other.sampler_ = VK_NULL_HANDLE;
        other.width_ = 0;
        other.height_ = 0;
        other.gpuBytes_ = 0;
        other.levels_ = 1;
    }
    return *this;
}

bool Texture2D::CreateFromRgba(Device& device, uint32_t width, uint32_t height,
                               const void* pixels) {
    // UNORM, bukan SRGB: thumbnail digambar oleh ImGui yang menulis langsung ke
    // swapchain tanpa koreksi gamma. Memakai SRGB di sini membuat gambarnya
    // tampak lebih terang daripada aslinya di berkas.
    return Create(device, width, height, VK_FORMAT_R8G8B8A8_UNORM, 4, pixels);
}

bool Texture2D::Create(Device& device, uint32_t width, uint32_t height, VkFormat format,
                       uint32_t bytesPerTexel, const void* texels) {
    if (width == 0 || height == 0 || bytesPerTexel == 0 || texels == nullptr) {
        return false;
    }
    Destroy();
    device_ = &device;
    width_ = width;
    height_ = height;
    levels_ = 1;

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * bytesPerTexel;
    gpuBytes_ = bytes;

    // Staging buffer hidup hanya selama fungsi ini. Menyimpannya untuk dipakai
    // ulang akan menahan memori host sebesar thumbnail terbesar yang pernah
    // dibuat, sepanjang sesi.
    DynamicBuffer staging;
    if (!staging.Create(device, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, bytes) ||
        !staging.Write(texels, bytes)) {
        SIM_ERROR("RHI", "Cannot stage {}x{} texture upload", width, height);
        Destroy();
        return false;
    }

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    // Device-local: setelah diunggah, tekstur ini hanya dibaca shader.
    allocationInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    const VkResult created = vmaCreateImage(device_->Allocator(), &imageInfo, &allocationInfo,
                                            &image_, &allocation_, nullptr);
    if (created != VK_SUCCESS) {
        SIM_ERROR("RHI", "vmaCreateImage failed: {}", ResultToString(created));
        image_ = VK_NULL_HANDLE;
        Destroy();
        return false;
    }

    VkCommandBuffer cmd = device_->BeginOneShot();
    TransitionLayout(cmd, image_, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, staging.Handle(), image_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    TransitionLayout(cmd, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_ACCESS_SHADER_READ_BIT);
    device_->EndOneShot(cmd);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = imageInfo.format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    SIM_VK_CHECK(vkCreateImageView(device_->Handle(), &viewInfo, nullptr, &view_));

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    // CLAMP_TO_EDGE: thumbnail digambar utuh tanpa pengulangan, dan REPEAT akan
    // memperlihatkan tepi seberang saat gambarnya diskala sedikit meleset.
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    SIM_VK_CHECK(vkCreateSampler(device_->Handle(), &samplerInfo, nullptr, &sampler_));

    return true;
}

bool Texture2D::CreateFromKtx2(Device& device, const Ktx2Texture& texture) {
    if (!texture.IsValid()) {
        return false;
    }
    const VkFormat format = static_cast<VkFormat>(texture.format);
    if (IsBlockCompressed(format) && !device.SupportsBlockCompression()) {
        // Ditolak dengan menyebutnya, bukan diunggah dan diharap. Mengunggah
        // blok BCn ke perangkat yang tidak mendukungnya bukan galat yang
        // dilaporkan Vulkan — yang keluar adalah gambar teracak.
        SIM_ERROR("RHI", "device has no block compression; cannot upload format {}",
                  static_cast<uint32_t>(texture.format));
        return false;
    }

    Destroy();
    device_ = &device;
    width_ = texture.width;
    height_ = texture.height;
    levels_ = static_cast<uint32_t>(texture.levels.size());

    // Seluruh level disalin ke satu staging buffer, dan salinannya rapat —
    // offset di dalam berkas tidak dipakai apa adanya karena spesifikasi KTX2
    // menyisipkan padding penyelarasan di antara level.
    std::vector<VkDeviceSize> offsets(texture.levels.size(), 0);
    VkDeviceSize total = 0;
    for (std::size_t level = 0; level < texture.levels.size(); ++level) {
        const std::span<const uint8_t> bytes = texture.LevelBytes(level);
        if (bytes.empty()) {
            SIM_ERROR("RHI", "KTX2 level {} is empty or out of range", level);
            Destroy();
            return false;
        }
        offsets[level] = total;
        total += bytes.size();
    }
    gpuBytes_ = total;

    DynamicBuffer staging;
    if (!staging.Create(device, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, total)) {
        SIM_ERROR("RHI", "cannot stage {} bytes for {}x{} texture", total, width_, height_);
        Destroy();
        return false;
    }
    for (std::size_t level = 0; level < texture.levels.size(); ++level) {
        const std::span<const uint8_t> bytes = texture.LevelBytes(level);
        if (!staging.WriteAt(offsets[level], bytes.data(), bytes.size())) {
            SIM_ERROR("RHI", "cannot stage KTX2 level {}", level);
            Destroy();
            return false;
        }
    }

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {width_, height_, 1};
    imageInfo.mipLevels = levels_;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    const VkResult created = vmaCreateImage(device_->Allocator(), &imageInfo, &allocationInfo,
                                            &image_, &allocation_, nullptr);
    if (created != VK_SUCCESS) {
        SIM_ERROR("RHI", "vmaCreateImage failed: {}", ResultToString(created));
        image_ = VK_NULL_HANDLE;
        Destroy();
        return false;
    }

    VkCommandBuffer cmd = device_->BeginOneShot();
    TransitionLayout(cmd, image_, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT, levels_);

    std::vector<VkBufferImageCopy> regions(texture.levels.size());
    for (std::size_t level = 0; level < texture.levels.size(); ++level) {
        VkBufferImageCopy& region = regions[level];
        region = {};
        region.bufferOffset = offsets[level];
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = static_cast<uint32_t>(level);
        region.imageSubresource.layerCount = 1;
        // Dimensi levelnya, bukan dimensi level nol digeser. Keduanya sama untuk
        // ukuran kelipatan dua dan berbeda untuk yang lain, dan yang berbeda
        // menghasilkan `VUID-vkCmdCopyBufferToImage-pRegions` — atau, tanpa
        // validation layer, mip yang isinya bergeser.
        region.imageExtent = {texture.levels[level].width, texture.levels[level].height, 1};
        // Nol berarti "rapat menurut imageExtent". Ia benar untuk format blok
        // maupun format biasa, dan menghitungnya sendiri di sini berarti
        // menyalin lagi aturan pembulatan blok 4x4.
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
    }
    vkCmdCopyBufferToImage(cmd, staging.Handle(), image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(regions.size()), regions.data());

    TransitionLayout(cmd, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_ACCESS_SHADER_READ_BIT, levels_);
    device_->EndOneShot(cmd);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = levels_;
    viewInfo.subresourceRange.layerCount = 1;
    SIM_VK_CHECK(vkCreateImageView(device_->Handle(), &viewInfo, nullptr, &view_));

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    // REPEAT: tekstur material diulang sepanjang mesh. Lihat catatan di
    // headernya soal kenapa `Create` justru clamp.
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxLod = static_cast<float>(levels_);
    SIM_VK_CHECK(vkCreateSampler(device_->Handle(), &samplerInfo, nullptr, &sampler_));

    return true;
}

void Texture2D::Destroy() {
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
    width_ = 0;
    height_ = 0;
    gpuBytes_ = 0;
    levels_ = 1;
}

}  // namespace sim::rhi
