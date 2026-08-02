#include "Sim/ImGuiIntegration/TextureBridge.h"

#include "Sim/Core/Assert.h"

#include <imgui_impl_vulkan.h>

namespace sim::imguix {

void TextureBridge::Initialize(rhi::Device& device) {
    device_ = &device;
}

void TextureBridge::BeginFrame() {
    slot_ = (slot_ + 1) % kDeferredFrames;

    // Slot yang sekarang berisi permintaan dari kDeferredFrames frame lalu —
    // sudah dijamin tidak dipakai command buffer mana pun.
    std::vector<uint64_t>& expired = pendingRelease_[slot_];
    for (uint64_t handle : expired) {
        ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(handle));
        --liveCount_;
    }
    expired.clear();
}

uint64_t TextureBridge::Acquire(VkImageView imageView, VkSampler sampler) {
    SIM_ASSERT(device_ != nullptr, "TextureBridge used before Initialize()");
    if (imageView == VK_NULL_HANDLE) {
        return 0;
    }
    VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(sampler, imageView,
                                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (set == VK_NULL_HANDLE) {
        SIM_ERROR("ImGui", "ImGui_ImplVulkan_AddTexture failed (descriptor pool exhausted?)");
        return 0;
    }
    ++liveCount_;
    return reinterpret_cast<uint64_t>(set);
}

void TextureBridge::Release(uint64_t handle) {
    if (handle == 0) {
        return;
    }
    // Diantrikan ke slot saat ini; dibebaskan saat slot ini datang lagi.
    pendingRelease_[slot_].push_back(handle);
}

void TextureBridge::Shutdown() {
    if (device_ != nullptr) {
        device_->WaitIdle();
    }
    for (std::vector<uint64_t>& queue : pendingRelease_) {
        for (uint64_t handle : queue) {
            ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(handle));
            --liveCount_;
        }
        queue.clear();
    }
    if (liveCount_ != 0) {
        SIM_WARN("ImGui", "{} UI textures still alive at shutdown", liveCount_);
    }
    device_ = nullptr;
}

}  // namespace sim::imguix
