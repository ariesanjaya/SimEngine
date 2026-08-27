#include "Sim/Volume/VolumeRaymarch.h"

#include <algorithm>
#include <cmath>

namespace sim::volume {

bool IntersectBox(const Vec3& origin, const Vec3& direction, const Vec3& boxMin,
                  const Vec3& boxMax, float& outNear, float& outFar) {
    // Slab method. Pembagian dengan nol disengaja: IEEE menghasilkan ±inf, dan
    // min/max di bawah menanganinya dengan benar untuk ray yang sejajar sebuah
    // sisi. Menjaganya dengan `if` justru menambah cabang yang salah di kasus
    // tepi — ray yang tepat menyentuh bidang sisi.
    const Vec3 inverse = 1.0f / direction;
    const Vec3 a = (boxMin - origin) * inverse;
    const Vec3 b = (boxMax - origin) * inverse;
    const Vec3 low = glm::min(a, b);
    const Vec3 high = glm::max(a, b);

    outNear = std::max(std::max(low.x, low.y), low.z);
    outFar = std::min(std::min(high.x, high.y), high.z);
    // `outFar >= 0` supaya kotak yang seluruhnya di belakang ray ditolak.
    return outFar >= std::max(outNear, 0.0f);
}

RaymarchResult Raymarch(const VolumeGrid& grid, const Vec3& origin, const Vec3& direction,
                        const RaymarchSettings& settings) {
    RaymarchResult result;
    if (grid.Empty() || settings.stepSize <= 0.0f) {
        return result;
    }

    Vec3 boxMin;
    Vec3 boxMax;
    grid.LocalBounds(boxMin, boxMax);
    // Setengah voxel di setiap sisi: nilainya disimpan di pusat voxel, jadi
    // kotak pusat-ke-pusat memotong separuh voxel terluar.
    const Vec3 pad(grid.voxelSize * 0.5f);
    boxMin -= pad;
    boxMax += pad;

    float tNear = 0.0f;
    float tFar = 0.0f;
    if (!IntersectBox(origin, direction, boxMin, boxMax, tNear, tFar)) {
        return result;  // ray ini tidak menyentuh volumenya sama sekali
    }
    tNear = std::max(tNear, 0.0f);

    const Vec3 unit = glm::normalize(direction);
    for (uint32_t step = 0; step < settings.maxSteps; ++step) {
        // Disampel di **tengah** langkah, bukan di tepinya. Sampel di tepi
        // menggeser seluruh hasil setengah langkah, dan pergeseran itu tumbuh
        // bersama besar langkah — sehingga menaikkan kualitas memindahkan
        // asapnya, bukan hanya menghaluskannya.
        const float distance = tNear + (static_cast<float>(step) + 0.5f) * settings.stepSize;
        if (distance >= tFar) {
            break;
        }
        result.steps = step + 1;

        const float density = grid.SampleLocal(origin + unit * distance);
        if (density <= 0.0f) {
            continue;
        }

        const float extinction = density * settings.extinction;
        const float stepTransmittance = std::exp(-extinction * settings.stepSize);

        // **Integrasi analitik di dalam langkah**, bukan `radiance * step`.
        // Bentuk naif membuat hasilnya bergantung pada besar langkah; bentuk ini
        // menjawab integral yang sama untuk langkah apa pun, jadi menaikkan
        // kualitas menghaluskan tanpa mengubah kecerahan.
        const Vec3 radiance = settings.incomingLight * settings.scatterAlbedo * density;
        const Vec3 integrated =
            (radiance - radiance * stepTransmittance) / std::max(extinction, 1e-6f);

        result.scattered += integrated * result.transmittance;
        result.transmittance *= stepTransmittance;

        if (result.transmittance < settings.minTransmittance) {
            // Sisanya tidak lagi mengubah piksel mana pun.
            break;
        }
    }
    return result;
}

}  // namespace sim::volume
