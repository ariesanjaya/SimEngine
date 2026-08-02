#pragma once

#include "Sim/RHI/Vulkan.h"

#include <cstdint>

namespace sim::rhi {

/// Jembatan dari gambar GPU ke handle yang bisa digambar UI.
///
/// Dideklarasikan di RHI (bukan di ImGuiIntegration) supaya modul Render bisa
/// memakainya tanpa ikut bergantung pada ImGui. Implementasinya ada di
/// ImGuiIntegration::TextureBridge.
///
/// Kontrak yang wajib dipenuhi implementasi: Release() tidak boleh langsung
/// membebaskan descriptor, karena frame yang sedang in-flight mungkin masih
/// memakainya. Pembebasan harus ditunda sebanyak jumlah frame in-flight.
class ITextureRegistry {
public:
    virtual ~ITextureRegistry() = default;

    virtual uint64_t Acquire(VkImageView imageView, VkSampler sampler) = 0;
    virtual void Release(uint64_t handle) = 0;
};

}  // namespace sim::rhi
