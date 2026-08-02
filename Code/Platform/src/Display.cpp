#include "Sim/Platform/Display.h"

#include "Sim/Core/Log.h"

#include <SDL3/SDL.h>

namespace sim::platform {

std::vector<DisplayInfo> EnumerateDisplays() {
    std::vector<DisplayInfo> result;

    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    if (ids == nullptr) {
        SIM_WARN("Platform", "SDL_GetDisplays failed: {}", SDL_GetError());
        return result;
    }

    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    result.reserve(static_cast<std::size_t>(count));

    for (int i = 0; i < count; ++i) {
        DisplayInfo info;
        info.id = ids[i];
        const char* name = SDL_GetDisplayName(ids[i]);
        info.name = name != nullptr ? name : "Display";
        info.isPrimary = ids[i] == primary;
        info.contentScale = SDL_GetDisplayContentScale(ids[i]);
        if (info.contentScale <= 0.0f) {
            info.contentScale = 1.0f;
        }

        SDL_Rect bounds{};
        if (SDL_GetDisplayBounds(ids[i], &bounds)) {
            info.position = IVec2{bounds.x, bounds.y};
            info.size = IVec2{bounds.w, bounds.h};
        }

        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(ids[i]);
        if (mode != nullptr) {
            info.refreshRate = mode->refresh_rate;
        }

        result.push_back(std::move(info));
    }

    SDL_free(ids);
    return result;
}

const DisplayInfo* DisplayContaining(const std::vector<DisplayInfo>& displays, IVec2 point) {
    for (const DisplayInfo& display : displays) {
        const IVec2 max = display.position + display.size;
        if (point.x >= display.position.x && point.x < max.x && point.y >= display.position.y &&
            point.y < max.y) {
            return &display;
        }
    }
    return nullptr;
}

float PrimaryDisplayScale() {
    const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    return scale > 0.0f ? scale : 1.0f;
}

float LowestRefreshRate(float fallback) {
    float lowest = 0.0f;
    for (const DisplayInfo& display : EnumerateDisplays()) {
        // Monitor yang melaporkan 0 diabaikan, bukan dianggap paling lambat —
        // kalau tidak, satu monitor tanpa laporan akan mengunci editor ke 0.
        if (display.refreshRate <= 1.0f) {
            continue;
        }
        if (lowest == 0.0f || display.refreshRate < lowest) {
            lowest = display.refreshRate;
        }
    }
    return lowest > 0.0f ? lowest : fallback;
}

}  // namespace sim::platform
