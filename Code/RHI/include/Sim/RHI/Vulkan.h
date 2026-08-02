#pragma once

// Satu-satunya tempat header Vulkan dan VMA masuk ke SimEngine.
// Modul di luar RHI/ImGuiIntegration/Render tidak boleh meng-include ini —
// batasnya ditegakkan lewat target link di CMake.

#include <vulkan/vulkan.h>

#include <vk_mem_alloc.h>

#include "Sim/Core/Log.h"

#include <string_view>

namespace sim::rhi {

std::string_view ResultToString(VkResult result);

}  // namespace sim::rhi

/// Membungkus pemanggilan Vulkan yang mengembalikan VkResult.
///
/// Kegagalan Vulkan hampir selalu berarti kesalahan program atau perangkat yang
/// tidak memenuhi syarat, bukan kondisi yang bisa dipulihkan di tempat. Jadi
/// pola di sini adalah "catat lalu hentikan", bukan mengembalikan error yang
/// akan diabaikan pemanggil.
#define SIM_VK_CHECK(expr)                                                                     \
    do {                                                                                       \
        const VkResult _sim_vk_result = (expr);                                                \
        if (_sim_vk_result != VK_SUCCESS) [[unlikely]] {                                       \
            SIM_CRITICAL("RHI", "{}:{} — {} mengembalikan {}", __FILE__, __LINE__, #expr,      \
                         ::sim::rhi::ResultToString(_sim_vk_result));                          \
            std::abort();                                                                      \
        }                                                                                      \
    } while (false)
