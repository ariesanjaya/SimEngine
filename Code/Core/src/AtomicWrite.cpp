#include "Sim/Core/AtomicWrite.h"

#include <atomic>
#include <cstdint>
#include <string>

#include <unistd.h>

namespace sim {

std::filesystem::path UniqueTemporaryPath(const std::filesystem::path& target) {
    static std::atomic<uint64_t> counter{0};
    return target.string() + ".tmp." + std::to_string(::getpid()) + "." +
           std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

}  // namespace sim
