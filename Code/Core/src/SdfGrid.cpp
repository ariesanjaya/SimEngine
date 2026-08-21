#include "Sim/Core/SdfGrid.h"

#include "GridSample.h"

#include <algorithm>
#include <cmath>

namespace sim {

float SdfGrid::At(int32_t x, int32_t y, int32_t z) const {
    if (x < 0 || y < 0 || z < 0 || static_cast<uint32_t>(x) >= sizeX ||
        static_cast<uint32_t>(y) >= sizeY || static_cast<uint32_t>(z) >= sizeZ) {
        return band;
    }
    const std::size_t at = (static_cast<std::size_t>(z) * sizeY + static_cast<std::size_t>(y)) *
                               sizeX +
                           static_cast<std::size_t>(x);
    return distances[at];
}

Vec3 SdfGrid::AlbedoAt(int32_t x, int32_t y, int32_t z, const Vec3& fallback) const {
    if (!HasAlbedo() || x < 0 || y < 0 || z < 0 || static_cast<uint32_t>(x) >= sizeX ||
        static_cast<uint32_t>(y) >= sizeY || static_cast<uint32_t>(z) >= sizeZ) {
        return fallback;
    }
    const std::size_t at = (static_cast<std::size_t>(z) * sizeY + static_cast<std::size_t>(y)) *
                               sizeX +
                           static_cast<std::size_t>(x);
    const uint8_t index = materials[at];
    if (index == 0 || index >= palette.size()) {
        return fallback;
    }
    return palette[index];
}

Vec3 SdfGrid::AlbedoLocal(const Vec3& local, const Vec3& fallback) const {
    if (!HasAlbedo() || voxelSize <= 0.0f) {
        return fallback;
    }
    const Vec3 position = (local - origin) / voxelSize;
    return AlbedoAt(static_cast<int32_t>(std::lround(position.x)),
                    static_cast<int32_t>(std::lround(position.y)),
                    static_cast<int32_t>(std::lround(position.z)), fallback);
}

float SdfGrid::SampleLocal(const Vec3& local) const {
    if (Empty() || voxelSize <= 0.0f) {
        return band;
    }

    return detail::TrilinearSample(*this, origin, voxelSize, local);
}

}  // namespace sim
