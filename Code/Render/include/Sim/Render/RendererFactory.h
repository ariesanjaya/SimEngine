#pragma once

#include "Sim/Render/IViewportRenderer.h"

#include <filesystem>
#include <memory>

// Hanya deklarasi maju: berkas ini dipakai composition root (Apps/SimEditor),
// dan tidak boleh menyeret header Vulkan ke siapa pun yang meng-include-nya.
namespace sim::rhi {
class Device;
class ITextureRegistry;
}  // namespace sim::rhi

namespace sim::render {

struct StubRendererDesc {
    /// Folder berisi berkas .spv hasil kompilasi shader.
    std::filesystem::path shaderDirectory;
    uint32_t initialWidth = 1280;
    uint32_t initialHeight = 720;
};

/// Renderer sementara untuk fase editor (E1..E7): clear color + grid
/// prosedural. Cukup untuk menjalankan seluruh alur kerja panel; diganti
/// VulkanRenderer di E8.
std::unique_ptr<IViewportRenderer> CreateStubRenderer(rhi::Device& device,
                                                      rhi::ITextureRegistry& textures,
                                                      const StubRendererDesc& desc);

}  // namespace sim::render
