#pragma once

#include "Sim/Core/Math.h"

#include <cmath>
#include <cstdint>

namespace sim::detail {

/// Cuplikan trilinear di atas apa pun yang punya `At(x, y, z)`.
///
/// Dipakai bersama `SdfGrid` dan `VolumeGrid`. Keduanya berbeda hanya pada apa
/// yang dijawab di luar gridnya — `+band` untuk medan jarak, nol untuk
/// kerapatan — dan perbedaan itu sudah dijawab `At` masing-masing. Menyalin
/// delapan sudut dan tujuh lerp-nya dua kali berarti dua tempat yang bisa
/// bergeser sendiri-sendiri, dan pergeseran sekecil setengah voxel di antara
/// keduanya tidak akan pernah terlihat sebagai galat.
template <typename Grid>
float TrilinearSample(const Grid& grid, const Vec3& origin, float voxelSize, const Vec3& local) {
    const Vec3 position = (local - origin) / voxelSize;
    const auto x0 = static_cast<int32_t>(std::floor(position.x));
    const auto y0 = static_cast<int32_t>(std::floor(position.y));
    const auto z0 = static_cast<int32_t>(std::floor(position.z));
    const float fx = position.x - static_cast<float>(x0);
    const float fy = position.y - static_cast<float>(y0);
    const float fz = position.z - static_cast<float>(z0);

    // Dibaca lewat `At` supaya sudut yang jatuh di luar grid ikut terjawab
    // nilai luarnya alih-alih membaca melewati ujung buffer.
    const float c000 = grid.At(x0, y0, z0);
    const float c100 = grid.At(x0 + 1, y0, z0);
    const float c010 = grid.At(x0, y0 + 1, z0);
    const float c110 = grid.At(x0 + 1, y0 + 1, z0);
    const float c001 = grid.At(x0, y0, z0 + 1);
    const float c101 = grid.At(x0 + 1, y0, z0 + 1);
    const float c011 = grid.At(x0, y0 + 1, z0 + 1);
    const float c111 = grid.At(x0 + 1, y0 + 1, z0 + 1);

    const float x00 = c000 + (c100 - c000) * fx;
    const float x10 = c010 + (c110 - c010) * fx;
    const float x01 = c001 + (c101 - c001) * fx;
    const float x11 = c011 + (c111 - c011) * fx;
    const float y0v = x00 + (x10 - x00) * fy;
    const float y1v = x01 + (x11 - x01) * fy;
    return y0v + (y1v - y0v) * fz;
}

}  // namespace sim::detail
