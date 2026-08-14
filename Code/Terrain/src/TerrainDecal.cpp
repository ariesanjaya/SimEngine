#include "Sim/Terrain/TerrainDecal.h"

#include "Sim/Terrain/TerrainMesh.h"

#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>
#include <limits>
#include <vector>

namespace sim::terrain {
namespace {

/// Apakah sebuah titik dunia jatuh di petak yang berlubang.
///
/// Lubang adalah sifat **petak**, bukan sampel — jadi yang ditanyakan adalah
/// petak yang memuat titik itu, bukan sampel terdekatnya.
bool OverHole(const Terrain& terrain, float worldX, float worldZ) {
    const float spacing = terrain.Desc().sampleSpacing;
    const int x = static_cast<int>(std::floor(worldX / spacing));
    const int y = static_cast<int>(std::floor(worldZ / spacing));
    return terrain.HoleAt(x, y);
}

}  // namespace

assets::MeshData BuildDecalMesh(const Terrain& terrain, const DecalProjection& decal) {
    assets::MeshData mesh;

    const float halfX = std::abs(decal.halfSize.x);
    const float halfZ = std::abs(decal.halfSize.y);
    if (!(halfX > 0.0f) || !(halfZ > 0.0f)) {
        return mesh;
    }

    // Rapatnya mengikuti jarak sampel terrain, bukan angka tetap. Decal yang
    // dibagi rata sepuluh kali akan melewatkan setiap gundukan yang lebih sempit
    // daripada sepersepuluh jejaknya — dan yang terlihat adalah decal yang
    // menembus tanah di satu sisi dan melayang di sisi lain.
    const float spacing = std::max(terrain.Desc().sampleSpacing, 1e-3f);
    const int limit = std::clamp(decal.maxSteps, 1, 4096);
    const int stepsX = std::clamp(static_cast<int>(std::ceil(halfX * 2.0f / spacing)), 1, limit);
    const int stepsZ = std::clamp(static_cast<int>(std::ceil(halfZ * 2.0f / spacing)), 1, limit);

    const float cosine = std::cos(decal.rotationY);
    const float sine = std::sin(decal.rotationY);

    struct Corner {
        Vec3 position;
        Vec2 uv;
        bool inside = false;
    };
    std::vector<Corner> corners;
    corners.reserve(static_cast<std::size_t>(stepsX + 1) * (stepsZ + 1));

    const float width = terrain.WorldWidth();
    const float depth = terrain.WorldDepth();

    Vec3 boundsMin(std::numeric_limits<float>::max());
    Vec3 boundsMax(std::numeric_limits<float>::lowest());

    for (int j = 0; j <= stepsZ; ++j) {
        const float v = static_cast<float>(j) / static_cast<float>(stepsZ);
        const float localZ = (v * 2.0f - 1.0f) * halfZ;
        for (int i = 0; i <= stepsX; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(stepsX);
            const float localX = (u * 2.0f - 1.0f) * halfX;

            // Jejaknya diputar terhadap Y saja. Decal terrain yang menyamping
            // tidak punya arti, dan yang memiringkannya akan mendapat jejak yang
            // menyempit tanpa ada yang menjelaskan mengapa.
            const float worldX = decal.center.x + localX * cosine - localZ * sine;
            const float worldZ = decal.center.z + localX * sine + localZ * cosine;

            Corner corner;
            corner.uv = Vec2(u, v);
            // Di luar peta bukan galat: gizmo boleh menyeret decal ke mana pun,
            // dan yang menggantung di luar tepi hanya kehilangan bagian itu.
            corner.inside = worldX >= 0.0f && worldZ >= 0.0f && worldX <= width && worldZ <= depth;
            const float height = terrain.HeightAtWorld(worldX, worldZ);
            corner.position = Vec3(worldX, height + decal.lift, worldZ);
            corners.push_back(corner);

            if (corner.inside) {
                boundsMin = glm::min(boundsMin, corner.position);
                boundsMax = glm::max(boundsMax, corner.position);
            }
        }
    }

    const std::size_t stride = static_cast<std::size_t>(stepsX) + 1;
    mesh.vertices.reserve(corners.size());
    for (const Corner& corner : corners) {
        assets::MeshVertex vertex;
        vertex.position = corner.position;
        // Normal permukaannya, bukan tegak lurus ke atas: decal di dinding
        // lembah yang dinyalakan seolah menghadap langit akan terlihat menempel
        // di kaca alih-alih di tanah.
        vertex.normal = terrain.NormalAtWorld(corner.position.x, corner.position.z);
        vertex.uv = corner.uv;
        vertex.tangent = Vec4(1.0f, 0.0f, 0.0f, 1.0f);
        vertex.color = decal.color;
        mesh.vertices.push_back(vertex);
    }

    for (int j = 0; j < stepsZ; ++j) {
        for (int i = 0; i < stepsX; ++i) {
            const std::size_t a = static_cast<std::size_t>(j) * stride + i;
            const std::size_t b = a + 1;
            const std::size_t c = a + stride;
            const std::size_t d = c + 1;
            // Petak yang salah satu sudutnya di luar peta dibuang seluruhnya:
            // separuh petak yang direntangkan ke tepi adalah segitiga yang
            // membentang ke tempat yang tidak ditunjuk siapa pun.
            if (!corners[a].inside || !corners[b].inside || !corners[c].inside ||
                !corners[d].inside) {
                continue;
            }
            // Decal tidak menambal lubang. Yang menutupinya membuat lubang yang
            // sudah dipahat orang muncul kembali sebagai lantai berwarna.
            const float midX = (corners[a].position.x + corners[d].position.x) * 0.5f;
            const float midZ = (corners[a].position.z + corners[d].position.z) * 0.5f;
            if (OverHole(terrain, midX, midZ)) {
                continue;
            }
            mesh.indices.insert(mesh.indices.end(),
                                {static_cast<uint32_t>(a), static_cast<uint32_t>(c),
                                 static_cast<uint32_t>(b), static_cast<uint32_t>(b),
                                 static_cast<uint32_t>(c), static_cast<uint32_t>(d)});
        }
    }

    if (mesh.indices.empty()) {
        mesh.vertices.clear();
        return mesh;
    }

    mesh.parts.push_back(assets::SubMesh{0, static_cast<uint32_t>(mesh.indices.size()), -1});
    mesh.boundsMin = boundsMin;
    mesh.boundsMax = boundsMax;
    return mesh;
}

}  // namespace sim::terrain
