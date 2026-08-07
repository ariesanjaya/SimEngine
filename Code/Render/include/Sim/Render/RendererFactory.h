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

/// Renderer Vulkan E8. Menjalankan frame graph: depth prepass, forward opaque,
/// transparan tersortir — dengan reversed-Z dan barrier yang disimpulkan graph.
///
/// Mengembalikan nullptr bila perangkatnya tidak memenuhi syarat (Vulkan 1.3),
/// dan pemanggil jatuh kembali ke `CreateStubRenderer`. Jatuh kembali, bukan
/// gagal: editor yang menolak jalan di mesin lama tidak bisa dipakai menyunting
/// data, padahal seluruh E2..E7 memang tidak menuntut renderer sungguhan.
std::unique_ptr<IViewportRenderer> CreateVulkanRenderer(rhi::Device& device,
                                                        rhi::ITextureRegistry& textures,
                                                        const StubRendererDesc& desc);

}  // namespace sim::render
