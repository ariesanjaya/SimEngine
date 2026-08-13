#include "Sim/Terrain/TerrainPicking.h"

#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>

namespace sim::terrain {
namespace {

/// Selisih tinggi sinar terhadap permukaan pada parameter `t`.
///
/// Positif berarti sinar masih di atas tanah. Nol adalah perpotongannya, dan
/// itulah satu-satunya besaran yang dicari seluruh berkas ini.
float Above(const Terrain& terrain, const Vec3& origin, const Vec3& direction, float t) {
    const Vec3 at = origin + direction * t;
    return at.y - terrain.HeightAtWorld(at.x, at.z);
}

/// Memotong sinar terhadap kotak terrain, dan mengembalikan false bila ia
/// melewatinya sama sekali.
///
/// Kotaknya dilebihkan sedikit ke atas dan ke bawah: `minHeight`/`maxHeight`
/// adalah rentang yang **bisa** disimpan, dan sinar yang lewat persis di
/// batasnya tetap harus diuji.
bool ClipToBounds(const Terrain& terrain, const Vec3& origin, const Vec3& direction, float maxT,
                  float& enter, float& exit) {
    const TerrainDesc& desc = terrain.Desc();
    const Vec3 boxMin(0.0f, desc.minHeight - 1.0f, 0.0f);
    const Vec3 boxMax(terrain.WorldWidth(), desc.maxHeight + 1.0f, terrain.WorldDepth());

    enter = 0.0f;
    exit = maxT;
    for (int axis = 0; axis < 3; ++axis) {
        const float from = origin[axis];
        const float along = direction[axis];
        if (std::abs(along) < 1e-9f) {
            // Sejajar sumbu ini: entah selalu di dalam, entah tidak akan pernah.
            if (from < boxMin[axis] || from > boxMax[axis]) {
                return false;
            }
            continue;
        }
        float near = (boxMin[axis] - from) / along;
        float far = (boxMax[axis] - from) / along;
        if (near > far) {
            std::swap(near, far);
        }
        enter = std::max(enter, near);
        exit = std::min(exit, far);
        if (enter > exit) {
            return false;
        }
    }
    return true;
}

}  // namespace

TerrainHit RaycastTerrain(const Terrain& terrain, const Vec3& origin, const Vec3& direction,
                          float maxDistance) {
    TerrainHit result;

    const float length = glm::length(direction);
    if (!(length > 0.0f) || !(maxDistance > 0.0f)) {
        // Arah bernorma nol ditolak alih-alih ditebak: menebaknya berarti
        // sebuah bug pemanggil muncul sebagai kursor yang menempel di satu
        // titik, dan itu terbaca sebagai bug terrain.
        return result;
    }
    const Vec3 ray = direction / length;

    float enter = 0.0f;
    float exit = 0.0f;
    if (!ClipToBounds(terrain, origin, ray, maxDistance, enter, exit)) {
        return result;
    }

    // Yang berpangkal di bawah tanah tidak menembak apa pun. Diperiksa di
    // pangkalnya sendiri, bukan di titik masuk kotak: yang berdiri di dalam
    // bukit sudah di bawah permukaan sebelum kotaknya dipotong.
    if (Above(terrain, origin, ray, 0.0f) < 0.0f) {
        return result;
    }

    // Setengah jarak sampel. Heightmap tidak bisa menyatakan fitur yang lebih
    // sempit daripada satu sampel, jadi langkah setengahnya tidak melewatkan
    // apa pun yang benar-benar ada di dalam datanya — sementara langkah sebesar
    // satu sampel bisa menyeberangi sebuah punggungan secara diagonal.
    const float step = std::max(terrain.Desc().sampleSpacing * 0.5f, 1e-3f);

    float previousT = enter;
    float previousAbove = Above(terrain, origin, ray, previousT);
    if (previousAbove < 0.0f) {
        // Sudah di bawah tepat di titik masuk: perpotongannya ada di antara
        // pangkal dan titik itu.
        previousT = 0.0f;
        previousAbove = Above(terrain, origin, ray, 0.0f);
    }

    for (float t = previousT + step; t <= exit + step; t += step) {
        const float clamped = std::min(t, exit);
        const float above = Above(terrain, origin, ray, clamped);
        if (above <= 0.0f) {
            // Bagi dua di antara yang terakhir di atas dan yang pertama di
            // bawah. Dua puluh iterasi menyempitkan selang selebar satu sampel
            // menjadi sepersejuta-nya — jauh di bawah yang bisa dibedakan mata
            // maupun yang bisa disimpan heightmap-nya.
            float low = previousT;
            float high = clamped;
            for (int i = 0; i < 20; ++i) {
                const float mid = (low + high) * 0.5f;
                if (Above(terrain, origin, ray, mid) > 0.0f) {
                    low = mid;
                } else {
                    high = mid;
                }
            }
            result.hit = true;
            result.distance = high;
            result.position = origin + ray * high;
            // Tingginya diambil dari permukaannya, bukan dari sinar: keduanya
            // sama dalam batas bagi dua, dan yang dipakai brush adalah titik di
            // atas tanah — bukan titik yang hampir di atas tanah.
            result.position.y = terrain.HeightAtWorld(result.position.x, result.position.z);
            return result;
        }
        previousT = clamped;
        if (clamped >= exit) {
            break;
        }
    }
    return result;
}

}  // namespace sim::terrain
