#include "Sim/Platform/Window.h"

#include "Sim/Core/Log.h"
#include "Sim/Platform/Display.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

namespace sim::platform {

bool InitPlatform() {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SIM_CRITICAL("Platform", "SDL_Init failed: {}", SDL_GetError());
        return false;
    }
    SIM_INFO("Platform", "SDL {}.{}.{}, video driver '{}'", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
             SDL_MICRO_VERSION, SDL_GetCurrentVideoDriver());
    return true;
}

void ShutdownPlatform() {
    SDL_Quit();
}

Window::~Window() {
    Destroy();
}

bool Window::Create(const WindowDesc& desc) {
    // SDL_WINDOW_HIDDEN: jendela ditampilkan setelah swapchain siap, supaya
    // pengguna tidak sempat melihat satu frame kosong berwarna acak.
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (desc.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (desc.maximized) {
        flags |= SDL_WINDOW_MAXIMIZED;
    }

    // Ukuran diberikan dalam koordinat logis; SDL yang mengalikan dengan skala
    // monitor. Mengalikannya sendiri akan menghasilkan jendela dua kali lipat
    // di layar HiDPI.
    window_ = SDL_CreateWindow(desc.title.c_str(), desc.width, desc.height, flags);
    if (window_ == nullptr) {
        SIM_CRITICAL("Platform", "SDL_CreateWindow failed: {}", SDL_GetError());
        return false;
    }
    return true;
}

void Window::Destroy() {
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
}

void Window::Show() {
    if (window_ != nullptr) {
        SDL_ShowWindow(window_);
    }
}

void Window::CenterOnPrimaryDisplay() {
    if (window_ != nullptr) {
        SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
}

UVec2 Window::PixelSize() const {
    if (window_ == nullptr) {
        return UVec2{0, 0};
    }
    int w = 0;
    int h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    return UVec2{w < 0 ? 0u : static_cast<uint32_t>(w), h < 0 ? 0u : static_cast<uint32_t>(h)};
}

bool Window::IsMinimized() const {
    return window_ != nullptr && (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0;
}

float Window::DisplayScale() const {
    if (window_ == nullptr) {
        return 1.0f;
    }
    const float scale = SDL_GetWindowDisplayScale(window_);
    return scale > 0.0f ? scale : 1.0f;
}

uint32_t Window::Id() const {
    return window_ != nullptr ? SDL_GetWindowID(window_) : 0;
}

}  // namespace sim::platform
