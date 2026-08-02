#pragma once

#include "Sim/Core/Log.h"

#include <cstdlib>

namespace sim::detail {

[[noreturn]] inline void AssertFailed(const char* expr, const char* file, int line,
                                      const std::string& message) {
    SIM_CRITICAL("Assert", "{}:{} — assertion failed: {} {}", file, line, expr, message);
    ::sim::Log::Shutdown();
    std::abort();
}

}  // namespace sim::detail

/// Aktif hanya di Debug. Untuk invarian yang mahal diperiksa.
#if SIM_DEBUG
#define SIM_ASSERT(expr, ...)                                                                    \
    do {                                                                                         \
        if (!(expr)) [[unlikely]] {                                                              \
            ::sim::detail::AssertFailed(#expr, __FILE__, __LINE__,                               \
                                        ::spdlog::fmt_lib::format("" __VA_ARGS__));                            \
        }                                                                                        \
    } while (false)
#else
#define SIM_ASSERT(expr, ...) ((void)0)
#endif

/// Aktif di semua konfigurasi. Untuk kondisi yang kalau salah berarti program
/// tidak boleh lanjut (mis. hasil pembuatan objek Vulkan).
#define SIM_VERIFY(expr, ...)                                                                    \
    do {                                                                                         \
        if (!(expr)) [[unlikely]] {                                                              \
            ::sim::detail::AssertFailed(#expr, __FILE__, __LINE__, ::spdlog::fmt_lib::format("" __VA_ARGS__)); \
        }                                                                                        \
    } while (false)
