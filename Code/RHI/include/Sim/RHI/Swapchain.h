#pragma once

#include "Sim/RHI/Device.h"

#include <array>
#include <cstdint>
#include <vector>

namespace sim::rhi {

/// Swapchain + render pass + sinkronisasi frame untuk satu jendela.
///
/// Ditulis sendiri (tidak memakai helper ImGui_ImplVulkanH_Window seperti pada
/// sdl3_vulkan.cpp) supaya modul RHI tidak bergantung pada ImGui, dan supaya
/// E8 bisa menambah pass sendiri tanpa melawan helper contoh.
///
/// Catatan sinkronisasi: semaphore "image available" dialokasikan per frame
/// in-flight, sementara "render finished" per image swapchain. Memakai
/// per-frame untuk keduanya adalah kesalahan umum yang memicu peringatan
/// validation layer ketika jumlah image != jumlah frame in-flight.
class Swapchain {
public:
    static constexpr uint32_t kFramesInFlight = 2;

    struct Frame {
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        uint32_t imageIndex = 0;
    };

    Swapchain() = default;
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    bool Create(Device& device, VkSurfaceKHR surface, uint32_t width, uint32_t height, bool vsync);
    void Destroy();

    /// Bangun ulang swapchain untuk ukuran baru. Aman dipanggil dengan ukuran
    /// yang sama (mis. setelah VK_ERROR_OUT_OF_DATE_KHR tanpa resize).
    bool Resize(uint32_t width, uint32_t height);

    /// Mulai frame. Mengembalikan false bila swapchain perlu dibangun ulang —
    /// pemanggil harus melewati frame ini tanpa memanggil EndFrame().
    bool BeginFrame(Frame& outFrame);
    void BeginRenderPass(const Frame& frame, const std::array<float, 4>& clearColor);
    void EndRenderPass(const Frame& frame);
    /// Submit + present. Mengembalikan false bila swapchain perlu dibangun ulang.
    bool EndFrame(const Frame& frame);

    VkRenderPass RenderPass() const { return renderPass_; }
    VkFormat ImageFormat() const { return surfaceFormat_.format; }
    uint32_t ImageCount() const { return static_cast<uint32_t>(images_.size()); }
    uint32_t MinImageCount() const { return minImageCount_; }
    uint32_t Width() const { return extent_.width; }
    uint32_t Height() const { return extent_.height; }

private:
    bool CreateSwapchain(uint32_t width, uint32_t height);
    bool CreateRenderPass();
    bool CreateFramebuffers();
    bool CreateSyncObjects();
    void DestroySwapchainResources();

    Device* device_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkSurfaceFormatKHR surfaceFormat_{};
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D extent_{};
    uint32_t minImageCount_ = 2;
    bool vsync_ = true;

    std::vector<VkImage> images_;
    std::vector<VkImageView> imageViews_;
    std::vector<VkFramebuffer> framebuffers_;
    /// Per image swapchain — lihat catatan sinkronisasi di atas.
    std::vector<VkSemaphore> renderFinished_;

    struct FrameSync {
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
    };
    std::array<FrameSync, kFramesInFlight> frames_{};
    uint32_t frameIndex_ = 0;
};

}  // namespace sim::rhi
