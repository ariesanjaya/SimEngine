#pragma once

#include "Sim/Render/IMaterialPreview.h"
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
    /// Jalur material. Diabaikan `CreateStubRenderer`, yang tidak menggambar
    /// material sama sekali.
    MaterialBindingPreference materialBinding = MaterialBindingPreference::Auto;
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

/// Preview material untuk panel Material Editor: satu mesh, satu material, satu
/// cahaya, dengan target render sendiri.
///
/// Target render sendiri itulah yang menyelesaikan tabrakan yang menahan preview
/// sejak E7.1 — panel Viewport dan preview keduanya menggambar
/// `ImGui::Image(ColorTarget())`, dan satu target berarti yang belakangan
/// menimpa yang duluan pada frame yang sama.
///
/// Mengembalikan nullptr bila perangkatnya tidak memenuhi syarat. Panel yang
/// menerimanya menampilkan pesan, bukan menolak dibuka: menyunting graph
/// material tidak menuntut preview.
///
/// `shaderDirectory` adalah folder `.spv` yang sama dengan yang diberikan ke
/// renderer viewport: preview memanggang peta lingkungannya sendiri, dan
/// penyaringnya sebuah pass compute. Folder yang salah bukan kegagalan — yang
/// terjadi peta yang lebih kecil beserta satu baris peringatan.
std::unique_ptr<IMaterialPreview> CreateMaterialPreview(rhi::Device& device,
                                                        rhi::ITextureRegistry& textures,
                                                        const std::filesystem::path& shaderDirectory,
                                                        uint32_t width = 512,
                                                        uint32_t height = 512);

}  // namespace sim::render
