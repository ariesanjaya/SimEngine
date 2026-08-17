#include "Sim/RHI/Swapchain.h"
#include <vector>
#include <string>
#include <cstring>
#include "Sim/RHI/Buffer.h"

#include "Sim/Core/Assert.h"

#include <algorithm>

namespace sim::rhi {
namespace {

VkSurfaceFormatKHR PickSurfaceFormat(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface) {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &count, formats.data());
    SIM_VERIFY(!formats.empty(), "Surface reported no formats");

    // UNORM, bukan SRGB: ImGui menggambar dengan warna yang sudah dalam ruang
    // sRGB, jadi konversi otomatis oleh swapchain SRGB akan membuat UI terlihat
    // pudar. Konversi ruang warna untuk konten 3D dilakukan di post-process (E8).
    for (const VkSurfaceFormatKHR& format : formats) {
        if ((format.format == VK_FORMAT_B8G8R8A8_UNORM ||
             format.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    return formats.front();
}

VkPresentModeKHR PickPresentMode(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
                                 bool vsync) {
    if (vsync) {
        return VK_PRESENT_MODE_FIFO_KHR;  // dijamin selalu tersedia
    }
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &count, modes.data());

    for (VkPresentModeKHR wanted : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
        if (std::find(modes.begin(), modes.end(), wanted) != modes.end()) {
            return wanted;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

}  // namespace

Swapchain::~Swapchain() {
    Destroy();
}

bool Swapchain::Create(Device& device, VkSurfaceKHR surface, uint32_t width, uint32_t height,
                       bool vsync) {
    device_ = &device;
    surface_ = surface;
    vsync_ = vsync;

    VkBool32 presentSupported = VK_FALSE;
    SIM_VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(device.PhysicalDevice(),
                                                      device.GraphicsQueueFamily(), surface,
                                                      &presentSupported));
    if (presentSupported != VK_TRUE) {
        SIM_CRITICAL("RHI", "Graphics queue family cannot present to this surface");
        return false;
    }

    surfaceFormat_ = PickSurfaceFormat(device.PhysicalDevice(), surface);
    presentMode_ = PickPresentMode(device.PhysicalDevice(), surface, vsync);

    return CreateSwapchain(width, height) && CreateRenderPass() && CreateFramebuffers() &&
           CreateSyncObjects();
}

bool Swapchain::CreateSwapchain(uint32_t width, uint32_t height) {
    VkSurfaceCapabilitiesKHR capabilities{};
    SIM_VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_->PhysicalDevice(), surface_,
                                                           &capabilities));

    // currentExtent 0xFFFFFFFF berarti "ukuran ditentukan swapchain", yang
    // terjadi di Wayland. Di kasus itu kita pakai ukuran jendela dari platform.
    if (capabilities.currentExtent.width != UINT32_MAX) {
        extent_ = capabilities.currentExtent;
    } else {
        extent_.width = std::clamp(width, capabilities.minImageExtent.width,
                                   capabilities.maxImageExtent.width);
        extent_.height = std::clamp(height, capabilities.minImageExtent.height,
                                    capabilities.maxImageExtent.height);
    }
    if (extent_.width == 0 || extent_.height == 0) {
        return false;  // jendela diminimalkan
    }

    minImageCount_ = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0) {
        minImageCount_ = std::min(minImageCount_, capabilities.maxImageCount);
    }

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface_;
    info.minImageCount = minImageCount_;
    info.imageFormat = surfaceFormat_.format;
    info.imageColorSpace = surfaceFormat_.colorSpace;
    info.imageExtent = extent_;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // TRANSFER_SRC dipakai `CaptureLastPresented` — tangkapan layar penuh untuk
    // agen MCP. **Diminta hanya bila permukaannya mengizinkan**, dan hasilnya
    // diingat: memintanya tanpa memeriksa membuat pembuatan swapchain gagal
    // seluruhnya di perangkat yang tidak mendukungnya, yaitu menukar sebuah
    // fitur diagnostik dengan editor yang tidak bisa menggambar apa pun.
    canCapture_ = (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    if (canCapture_) {
        info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    } else {
        SIM_WARN("RHI", "permukaan ini tidak mengizinkan TRANSFER_SRC — editor.screenshot mati");
    }
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = presentMode_;
    info.clipped = VK_TRUE;
    // Swapchain lama dipakai sebagai basis supaya driver bisa memakai ulang
    // memorinya; ini yang membuat resize tidak berkedip hitam.
    info.oldSwapchain = swapchain_;

    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    const VkResult result =
        vkCreateSwapchainKHR(device_->Handle(), &info, nullptr, &newSwapchain);
    if (result != VK_SUCCESS) {
        SIM_ERROR("RHI", "vkCreateSwapchainKHR failed: {}", ResultToString(result));
        return false;
    }
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_->Handle(), swapchain_, nullptr);
    }
    swapchain_ = newSwapchain;

    uint32_t imageCount = 0;
    SIM_VK_CHECK(vkGetSwapchainImagesKHR(device_->Handle(), swapchain_, &imageCount, nullptr));
    images_.resize(imageCount);
    SIM_VK_CHECK(
        vkGetSwapchainImagesKHR(device_->Handle(), swapchain_, &imageCount, images_.data()));

    imageViews_.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = images_[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = surfaceFormat_.format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        SIM_VK_CHECK(vkCreateImageView(device_->Handle(), &viewInfo, nullptr, &imageViews_[i]));
    }
    return true;
}

bool Swapchain::CreateRenderPass() {
    if (renderPass_ != VK_NULL_HANDLE) {
        return true;  // format tidak berubah saat resize
    }

    VkAttachmentDescription color{};
    color.format = surfaceFormat_.format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &color;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
    SIM_VK_CHECK(vkCreateRenderPass(device_->Handle(), &info, nullptr, &renderPass_));
    return true;
}

bool Swapchain::CreateFramebuffers() {
    framebuffers_.resize(imageViews_.size());
    for (std::size_t i = 0; i < imageViews_.size(); ++i) {
        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = renderPass_;
        info.attachmentCount = 1;
        info.pAttachments = &imageViews_[i];
        info.width = extent_.width;
        info.height = extent_.height;
        info.layers = 1;
        SIM_VK_CHECK(vkCreateFramebuffer(device_->Handle(), &info, nullptr, &framebuffers_[i]));
    }
    return true;
}

bool Swapchain::CreateSyncObjects() {
    // Semaphore render-finished per image, bukan per frame in-flight.
    renderFinished_.resize(images_.size());
    for (VkSemaphore& semaphore : renderFinished_) {
        if (semaphore != VK_NULL_HANDLE) {
            continue;
        }
        VkSemaphoreCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        SIM_VK_CHECK(vkCreateSemaphore(device_->Handle(), &info, nullptr, &semaphore));
    }

    if (frames_[0].commandPool != VK_NULL_HANDLE) {
        return true;  // sudah dibuat, tidak perlu diulang saat resize
    }

    for (FrameSync& frame : frames_) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = device_->GraphicsQueueFamily();
        SIM_VK_CHECK(vkCreateCommandPool(device_->Handle(), &poolInfo, nullptr, &frame.commandPool));

        VkCommandBufferAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.commandPool = frame.commandPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        SIM_VK_CHECK(
            vkAllocateCommandBuffers(device_->Handle(), &allocateInfo, &frame.commandBuffer));

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        // SIGNALED supaya frame pertama tidak menunggu fence yang tidak pernah
        // di-submit.
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        SIM_VK_CHECK(vkCreateFence(device_->Handle(), &fenceInfo, nullptr, &frame.inFlight));

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        SIM_VK_CHECK(
            vkCreateSemaphore(device_->Handle(), &semaphoreInfo, nullptr, &frame.imageAvailable));
    }
    return true;
}

bool Swapchain::Resize(uint32_t width, uint32_t height) {
    device_->WaitIdle();

    for (VkFramebuffer framebuffer : framebuffers_) {
        vkDestroyFramebuffer(device_->Handle(), framebuffer, nullptr);
    }
    framebuffers_.clear();
    for (VkImageView view : imageViews_) {
        vkDestroyImageView(device_->Handle(), view, nullptr);
    }
    imageViews_.clear();

    const std::size_t oldImageCount = renderFinished_.size();
    if (!CreateSwapchain(width, height)) {
        return false;
    }
    // Jumlah image bisa berubah setelah rebuild; semaphore lama yang berlebih
    // dibuang, yang kurang ditambah di CreateSyncObjects().
    if (renderFinished_.size() > images_.size()) {
        for (std::size_t i = images_.size(); i < oldImageCount; ++i) {
            vkDestroySemaphore(device_->Handle(), renderFinished_[i], nullptr);
        }
    }
    renderFinished_.resize(images_.size(), VK_NULL_HANDLE);

    return CreateFramebuffers() && CreateSyncObjects();
}

bool Swapchain::BeginFrame(Frame& outFrame) {
    FrameSync& sync = frames_[frameIndex_];

    SIM_VK_CHECK(vkWaitForFences(device_->Handle(), 1, &sync.inFlight, VK_TRUE, UINT64_MAX));

    uint32_t imageIndex = 0;
    const VkResult acquire = vkAcquireNextImageKHR(
        device_->Handle(), swapchain_, UINT64_MAX, sync.imageAvailable, VK_NULL_HANDLE,
        &imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        return false;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        SIM_ERROR("RHI", "vkAcquireNextImageKHR failed: {}", ResultToString(acquire));
        return false;
    }

    // Fence baru di-reset setelah acquire berhasil. Me-reset lebih awal akan
    // membuat fence tidak pernah di-signal kalau kita keluar lewat jalur
    // OUT_OF_DATE di atas, dan frame berikutnya menggantung selamanya.
    SIM_VK_CHECK(vkResetFences(device_->Handle(), 1, &sync.inFlight));
    SIM_VK_CHECK(vkResetCommandPool(device_->Handle(), sync.commandPool, 0));

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    SIM_VK_CHECK(vkBeginCommandBuffer(sync.commandBuffer, &beginInfo));

    outFrame.commandBuffer = sync.commandBuffer;
    outFrame.imageIndex = imageIndex;
    currentFrame_ = outFrame;
    return true;
}

void Swapchain::BeginRenderPass(const Frame& frame, const std::array<float, 4>& clearColor) {
    VkClearValue clear{};
    clear.color = {{clearColor[0], clearColor[1], clearColor[2], clearColor[3]}};

    VkRenderPassBeginInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    info.renderPass = renderPass_;
    info.framebuffer = framebuffers_[frame.imageIndex];
    info.renderArea.extent = extent_;
    info.clearValueCount = 1;
    info.pClearValues = &clear;
    vkCmdBeginRenderPass(frame.commandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
}

void Swapchain::EndRenderPass(const Frame& frame) {
    vkCmdEndRenderPass(frame.commandBuffer);
}

bool Swapchain::EndFrame(const Frame& frame) {
    FrameSync& sync = frames_[frameIndex_];
    SIM_VK_CHECK(vkEndCommandBuffer(sync.commandBuffer));

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &sync.imageAvailable;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &sync.commandBuffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderFinished_[frame.imageIndex];
    SIM_VK_CHECK(vkQueueSubmit(device_->GraphicsQueue(), 1, &submit, sync.inFlight));

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished_[frame.imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &frame.imageIndex;

    const VkResult result = vkQueuePresentKHR(device_->GraphicsQueue(), &present);
    // Dicatat sebelum cabang galat: OUT_OF_DATE dan SUBOPTIMAL keduanya tetap
    // mempresentasikan image ini, dan isinya tetap yang terakhir terlihat.
    lastPresented_ = frame.imageIndex;
    frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        return false;
    }
    if (result != VK_SUCCESS) {
        SIM_ERROR("RHI", "vkQueuePresentKHR failed: {}", ResultToString(result));
        return false;
    }
    return true;
}

void Swapchain::DestroySwapchainResources() {
    for (VkFramebuffer framebuffer : framebuffers_) {
        vkDestroyFramebuffer(device_->Handle(), framebuffer, nullptr);
    }
    framebuffers_.clear();
    for (VkImageView view : imageViews_) {
        vkDestroyImageView(device_->Handle(), view, nullptr);
    }
    imageViews_.clear();
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_->Handle(), swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

void Swapchain::Destroy() {
    if (device_ == nullptr || device_->Handle() == VK_NULL_HANDLE) {
        return;
    }
    device_->WaitIdle();

    for (VkSemaphore semaphore : renderFinished_) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_->Handle(), semaphore, nullptr);
        }
    }
    renderFinished_.clear();

    for (FrameSync& frame : frames_) {
        if (frame.imageAvailable != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_->Handle(), frame.imageAvailable, nullptr);
        }
        if (frame.inFlight != VK_NULL_HANDLE) {
            vkDestroyFence(device_->Handle(), frame.inFlight, nullptr);
        }
        if (frame.commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_->Handle(), frame.commandPool, nullptr);
        }
        frame = FrameSync{};
    }

    DestroySwapchainResources();

    if (renderPass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device_->Handle(), renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
    }
    device_ = nullptr;
}


bool Swapchain::CaptureLastPresented(std::vector<uint8_t>& outRgba, uint32_t& outWidth,
                                     uint32_t& outHeight, std::string& error) {
    if (!canCapture_) {
        error = "this surface does not allow reading swapchain images back";
        return false;
    }
    if (lastPresented_ >= images_.size()) {
        error = "no frame has been presented yet";
        return false;
    }
    // Hanya dua tata letak yang benar-benar muncul di swapchain desktop. Yang
    // lain ditolak dengan menyebut namanya, bukan disalin lalu ditafsirkan
    // salah — gambar dengan kanal tertukar terlihat seperti bug rendering, dan
    // yang mencarinya akan mencarinya di renderer.
    const VkFormat format = surfaceFormat_.format;
    const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
    const bool rgba = format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB;
    if (!bgra && !rgba) {
        error = "swapchain format " + std::to_string(static_cast<int>(format)) +
                " is not one of BGRA8/RGBA8";
        return false;
    }

    const uint32_t width = extent_.width;
    const uint32_t height = extent_.height;
    const VkDeviceSize bytes = VkDeviceSize(width) * height * 4;

    DynamicBuffer staging;
    if (!staging.Create(*device_, VK_BUFFER_USAGE_TRANSFER_DST_BIT, bytes)) {
        error = "cannot allocate a staging buffer for the screenshot";
        return false;
    }

    // Menunggu GPU diam supaya penyalinan tidak berlomba dengan frame yang
    // masih berjalan. Boleh mahal: lihat catatan di headernya.
    device_->WaitIdle();

    VkImage image = images_[lastPresented_];
    const auto barrier = [&](VkCommandBuffer cmd, VkImageLayout from, VkImageLayout to,
                             VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                             VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
        VkImageMemoryBarrier2 memory{};
        memory.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        memory.srcStageMask = srcStage;
        memory.srcAccessMask = srcAccess;
        memory.dstStageMask = dstStage;
        memory.dstAccessMask = dstAccess;
        memory.oldLayout = from;
        memory.newLayout = to;
        memory.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memory.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memory.image = image;
        memory.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &memory;
        vkCmdPipelineBarrier2(cmd, &dependency);
    };

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};

    VkCommandBuffer cmd = device_->BeginOneShot();
    barrier(cmd, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT);
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.Handle(), 1,
                           &region);
    // Dikembalikan ke PRESENT_SRC: image ini akan dipakai lagi oleh frame
    // berikutnya, dan yang menemukannya dalam tata letak lain adalah
    // validation layer di tengah frame yang tidak ada hubungannya.
    barrier(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, VK_ACCESS_2_NONE);
    device_->EndOneShot(cmd);

    const uint8_t* source = static_cast<const uint8_t*>(staging.Mapped());
    if (source == nullptr) {
        error = "the staging buffer is not mapped";
        return false;
    }

    outRgba.resize(static_cast<std::size_t>(bytes));
    if (rgba) {
        std::memcpy(outRgba.data(), source, static_cast<std::size_t>(bytes));
    } else {
        for (std::size_t i = 0; i < static_cast<std::size_t>(bytes); i += 4) {
            outRgba[i + 0] = source[i + 2];
            outRgba[i + 1] = source[i + 1];
            outRgba[i + 2] = source[i + 0];
            outRgba[i + 3] = source[i + 3];
        }
    }
    outWidth = width;
    outHeight = height;
    return true;
}

}  // namespace sim::rhi
