#pragma once

#include "Sim/ImGuiIntegration/TextureBridge.h"
#include "Sim/RHI/Device.h"

#include <filesystem>
#include <string>

struct SDL_Window;
union SDL_Event;

namespace sim::imguix {

struct ImGuiLayerDesc {
    SDL_Window* window = nullptr;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    uint32_t minImageCount = 2;
    uint32_t imageCount = 3;

    /// Lokasi penyimpanan layout dock. Kosong berarti layout tidak disimpan.
    std::filesystem::path iniPath;
    /// Berkas font TTF opsional; kalau kosong atau tidak ada, dipakai font
    /// bawaan ImGui.
    std::filesystem::path fontPath;
    float fontSize = 16.0f;

    /// Font ikon yang di-merge ke font teks, sehingga ikon bisa ditulis
    /// langsung di dalam string biasa. Kosong berarti tanpa ikon.
    std::filesystem::path iconFontPath;
    /// Rentang codepoint ikon. Di luar rentang ini glyph diambil dari font teks.
    unsigned int iconRangeMin = 0;
    unsigned int iconRangeMax = 0;
};

/// Pemilik konteks Dear ImGui beserta backend SDL3 + Vulkan.
///
/// Docking dan multi-viewport dinyalakan di sini. Multi-viewport berarti panel
/// yang diseret keluar dockspace menjadi jendela OS tersendiri, lengkap dengan
/// swapchain-nya sendiri — itulah yang membuat panel bisa dipindah ke monitor
/// kedua.
class ImGuiLayer {
public:
    bool Initialize(rhi::Device& device, const ImGuiLayerDesc& desc);
    void Shutdown();

    bool ProcessEvent(const SDL_Event& event);

    void BeginFrame();

    /// Menyudahi frame dan menyusun draw list.
    ///
    /// WAJIB dipanggil setiap frame setelah UI digambar, termasuk ketika frame
    /// jendela utama batal — lihat catatan pada RenderPlatformWindows().
    void EndFrame();

    /// Merekam draw list jendela utama ke command buffer. Hanya dipanggil bila
    /// frame swapchain utama benar-benar jadi.
    void RenderDrawData(VkCommandBuffer commandBuffer);

    /// Membuat, memperbarui, dan mem-present jendela multi-viewport.
    ///
    /// Juga WAJIB setiap frame, bahkan ketika frame utama batal. Jendela
    /// platform untuk viewport baru dibuat di sini; melewatkannya satu frame
    /// saja membuat ImGui melihat viewport tanpa jendela dan langsung membuang
    /// viewport itu — panel yang seharusnya mengambang di monitor lain akan
    /// tertarik kembali ke jendela utama.
    void RenderPlatformWindows();

    void SetMinImageCount(uint32_t minImageCount);

    /// Menyesuaikan ukuran gaya ke skala monitor tempat jendela utama berada.
    /// Dipanggil tiap frame; hanya bekerja bila skalanya benar-benar berubah.
    void UpdateDisplayScale(float scale);

    TextureBridge& Textures() { return textures_; }
    float CurrentScale() const { return currentScale_; }

private:
    void ConfigureIo(const ImGuiLayerDesc& desc);
    void LoadFonts(const ImGuiLayerDesc& desc);

    rhi::Device* device_ = nullptr;
    TextureBridge textures_;
    std::string iniPathStorage_;
    float currentScale_ = 1.0f;
    bool initialized_ = false;
};

}  // namespace sim::imguix
