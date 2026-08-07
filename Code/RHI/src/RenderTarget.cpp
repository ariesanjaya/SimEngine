#include "Sim/RHI/RenderTarget.h"

#include "Sim/Core/Assert.h"

#include <array>

namespace sim::rhi {
namespace {

uint32_t RoundUp(uint32_t value, uint32_t granularity) {
    return ((value + granularity - 1) / granularity) * granularity;
}

}  // namespace

RenderTarget::~RenderTarget() {
    Destroy();
}

float RenderTarget::UvMaxU() const {
    return allocatedWidth_ == 0 ? 1.0f
                                : static_cast<float>(width_) / static_cast<float>(allocatedWidth_);
}

float RenderTarget::UvMaxV() const {
    return allocatedHeight_ == 0
               ? 1.0f
               : static_cast<float>(height_) / static_cast<float>(allocatedHeight_);
}

VkFormat RenderTarget::PickDepthFormat() const {
    for (VkFormat candidate : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT,
                               VK_FORMAT_D24_UNORM_S8_UINT}) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(device_->PhysicalDevice(), candidate, &properties);
        if ((properties.optimalTilingFeatures &
             VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
            return candidate;
        }
    }
    return VK_FORMAT_D32_SFLOAT;
}

bool RenderTarget::Create(Device& device, uint32_t width, uint32_t height) {
    device_ = &device;
    width_ = width;
    height_ = height;
    allocatedWidth_ = RoundUp(width, kAllocationGranularity);
    allocatedHeight_ = RoundUp(height, kAllocationGranularity);
    depthFormat_ = PickDepthFormat();

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    // CLAMP_TO_EDGE: tanpa ini, pembulatan setengah piksel di tepi panel
    // menghasilkan garis tipis dari sisi berlawanan.
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 1.0f;
    SIM_VK_CHECK(vkCreateSampler(device_->Handle(), &samplerInfo, nullptr, &sampler_));

    return CreateRenderPass() && CreateAttachments();
}

bool RenderTarget::CreateRenderPass() {
    std::array<VkAttachmentDescription, 2> attachments{};

    attachments[0].format = kColorFormat;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    attachments[1].format = depthFormat_;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    // Dua dependency: satu memastikan pembacaan fragment shader frame
    // sebelumnya selesai sebelum kita menulis lagi, satu memastikan tulisan
    // kita selesai sebelum ImGui membacanya sebagai tekstur.
    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = static_cast<uint32_t>(attachments.size());
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = static_cast<uint32_t>(dependencies.size());
    info.pDependencies = dependencies.data();
    SIM_VK_CHECK(vkCreateRenderPass(device_->Handle(), &info, nullptr, &renderPass_));
    return true;
}

bool RenderTarget::CreateAttachments() {
    if (allocatedWidth_ == 0 || allocatedHeight_ == 0) {
        return false;
    }

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kColorFormat;
    imageInfo.extent = {allocatedWidth_, allocatedHeight_, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    // TRANSFER_SRC dibutuhkan tool MCP viewport.capture (lihat docs/PLAN-AI.md)
    // untuk menyalin hasil render ke buffer yang bisa dibaca CPU.
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    SIM_VK_CHECK(vmaCreateImage(device_->Allocator(), &imageInfo, &allocationInfo, &colorImage_,
                                &colorAllocation_, nullptr));

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = colorImage_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kColorFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    SIM_VK_CHECK(vkCreateImageView(device_->Handle(), &viewInfo, nullptr, &colorView_));

    imageInfo.format = depthFormat_;
    // SAMPLED ikut: piramida HiZ membaca depth buffer ini sebagai tekstur.
    // Tanpa itu, lapis screen-space GI tidak punya apa pun untuk ditelusuri —
    // dan kekurangannya muncul sebagai galat pembuatan image view, jauh dari
    // tempat yang sebenarnya membutuhkannya.
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    SIM_VK_CHECK(vmaCreateImage(device_->Allocator(), &imageInfo, &allocationInfo, &depthImage_,
                                &depthAllocation_, nullptr));

    viewInfo.image = depthImage_;
    viewInfo.format = depthFormat_;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    SIM_VK_CHECK(vkCreateImageView(device_->Handle(), &viewInfo, nullptr, &depthView_));

    const std::array<VkImageView, 2> attachments{colorView_, depthView_};
    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = renderPass_;
    framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    framebufferInfo.pAttachments = attachments.data();
    framebufferInfo.width = allocatedWidth_;
    framebufferInfo.height = allocatedHeight_;
    framebufferInfo.layers = 1;
    SIM_VK_CHECK(
        vkCreateFramebuffer(device_->Handle(), &framebufferInfo, nullptr, &framebuffer_));
    return true;
}

bool RenderTarget::Resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || (width == width_ && height == height_)) {
        return false;
    }

    width_ = width;
    height_ = height;

    const uint32_t neededWidth = RoundUp(width, kAllocationGranularity);
    const uint32_t neededHeight = RoundUp(height, kAllocationGranularity);

    // Jalur cepat, dan inilah yang membuat menyeret pemisah dock terasa mulus:
    // selama ukuran baru masih muat dan alokasinya belum terlalu berlebih,
    // tidak ada apa pun yang dialokasikan ulang. Yang berubah hanya area yang
    // digambar dan UV yang dipakai UI.
    const bool fits = neededWidth <= allocatedWidth_ && neededHeight <= allocatedHeight_;
    const bool notWasteful = allocatedWidth_ <= neededWidth * 2 &&
                             allocatedHeight_ <= neededHeight * 2;
    if (fits && notWasteful) {
        return false;
    }

    // Alokasi ulang jarang terjadi sekarang (hanya saat melewati batas 128 px
    // atau saat panel mengecil drastis), jadi menunggu device idle di sini
    // tidak lagi terasa.
    SIM_DEBUG_LOG("RHI", "RenderTarget reallocated {}x{} -> {}x{} (requested {}x{})",
                  allocatedWidth_, allocatedHeight_, neededWidth, neededHeight, width, height);

    device_->WaitIdle();
    allocatedWidth_ = neededWidth;
    allocatedHeight_ = neededHeight;

    if (framebuffer_ != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device_->Handle(), framebuffer_, nullptr);
        framebuffer_ = VK_NULL_HANDLE;
    }
    if (colorView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_->Handle(), colorView_, nullptr);
        colorView_ = VK_NULL_HANDLE;
    }
    if (depthView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_->Handle(), depthView_, nullptr);
        depthView_ = VK_NULL_HANDLE;
    }
    if (colorImage_ != VK_NULL_HANDLE) {
        vmaDestroyImage(device_->Allocator(), colorImage_, colorAllocation_);
        colorImage_ = VK_NULL_HANDLE;
        colorAllocation_ = VK_NULL_HANDLE;
    }
    if (depthImage_ != VK_NULL_HANDLE) {
        vmaDestroyImage(device_->Allocator(), depthImage_, depthAllocation_);
        depthImage_ = VK_NULL_HANDLE;
        depthAllocation_ = VK_NULL_HANDLE;
    }

    return CreateAttachments();
}

void RenderTarget::Destroy() {
    if (device_ == nullptr || device_->Handle() == VK_NULL_HANDLE) {
        return;
    }
    device_->WaitIdle();

    if (framebuffer_ != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device_->Handle(), framebuffer_, nullptr);
        framebuffer_ = VK_NULL_HANDLE;
    }
    if (colorView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_->Handle(), colorView_, nullptr);
        colorView_ = VK_NULL_HANDLE;
    }
    if (depthView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_->Handle(), depthView_, nullptr);
        depthView_ = VK_NULL_HANDLE;
    }
    if (colorImage_ != VK_NULL_HANDLE) {
        vmaDestroyImage(device_->Allocator(), colorImage_, colorAllocation_);
        colorImage_ = VK_NULL_HANDLE;
    }
    if (depthImage_ != VK_NULL_HANDLE) {
        vmaDestroyImage(device_->Allocator(), depthImage_, depthAllocation_);
        depthImage_ = VK_NULL_HANDLE;
    }
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_->Handle(), sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    if (renderPass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device_->Handle(), renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
    }
    device_ = nullptr;
}

}  // namespace sim::rhi
