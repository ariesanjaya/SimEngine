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

float SdfGrid::SampleLocal(const Vec3& local) const {
    if (Empty() || voxelSize <= 0.0f) {
        return band;
    }

    return detail::TrilinearSample(*this, origin, voxelSize, local);
}

}  // namespace sim
