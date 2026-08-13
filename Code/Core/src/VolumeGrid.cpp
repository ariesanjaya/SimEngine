#include "Sim/Core/VolumeGrid.h"

#include "GridSample.h"

namespace sim {

float VolumeGrid::At(int32_t x, int32_t y, int32_t z) const {
    if (x < 0 || y < 0 || z < 0 || static_cast<uint32_t>(x) >= sizeX ||
        static_cast<uint32_t>(y) >= sizeY || static_cast<uint32_t>(z) >= sizeZ) {
        return background;
    }
    const std::size_t at = (static_cast<std::size_t>(z) * sizeY + static_cast<std::size_t>(y)) *
                               sizeX +
                           static_cast<std::size_t>(x);
    return values[at];
}

float VolumeGrid::SampleLocal(const Vec3& local) const {
    if (Empty() || voxelSize <= 0.0f) {
        return background;
    }
    return detail::TrilinearSample(*this, origin, voxelSize, local);
}

void VolumeGrid::LocalBounds(Vec3& outMin, Vec3& outMax) const {
    outMin = origin;
    outMax = origin + Vec3(static_cast<float>(sizeX == 0 ? 0 : sizeX - 1),
                           static_cast<float>(sizeY == 0 ? 0 : sizeY - 1),
                           static_cast<float>(sizeZ == 0 ? 0 : sizeZ - 1)) *
                          voxelSize;
}

void VolumeGrid::PaddedLocalBounds(Vec3& outMin, Vec3& outMax) const {
    LocalBounds(outMin, outMax);
    const Vec3 pad(voxelSize * 0.5f);
    outMin -= pad;
    outMax += pad;
}

void VolumeGrid::WorldBounds(const Vec3& position, float scale, Vec3& outMin,
                             Vec3& outMax) const {
    PaddedLocalBounds(outMin, outMax);
    const float safeScale = scale > 0.0f ? scale : 1.0f;
    outMin = position + outMin * safeScale;
    outMax = position + outMax * safeScale;
}

}  // namespace sim
