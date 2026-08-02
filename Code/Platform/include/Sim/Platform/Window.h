#pragma once

#include "Sim/Core/Math.h"

#include <cstdint>
#include <string>

struct SDL_Window;

namespace sim::platform {

struct WindowDesc {
    std::string title = "SimEngine";
    int width = 1600;
    int height = 900;
    bool resizable = true;
    bool maximized = false;
};

/// Jendela SDL3 dengan dukungan surface Vulkan.
///
/// Sengaja tipis: yang tidak dipakai editor tidak diekspos. Jendela sekunder
/// untuk panel yang ditarik keluar dibuat dan dikelola oleh backend
/// multi-viewport ImGui, bukan oleh kelas ini.
class Window {
public:
    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    bool Create(const WindowDesc& desc);
    void Destroy();

    void Show();
    void CenterOnPrimaryDisplay();

    /// Ukuran dalam piksel sesungguhnya (bukan koordinat logis). Inilah yang
    /// dipakai untuk swapchain — memakai ukuran logis di layar HiDPI membuat
    /// gambar buram.
    UVec2 PixelSize() const;

    bool IsMinimized() const;

    /// Skala konten monitor tempat jendela ini berada saat ini. Berubah ketika
    /// jendela dipindah antar-monitor dengan DPI berbeda.
    float DisplayScale() const;

    SDL_Window* Handle() const { return window_; }
    uint32_t Id() const;

private:
    SDL_Window* window_ = nullptr;
};

/// Inisialisasi subsistem SDL yang dibutuhkan editor. Aman dipanggil sekali.
bool InitPlatform();
void ShutdownPlatform();

}  // namespace sim::platform
