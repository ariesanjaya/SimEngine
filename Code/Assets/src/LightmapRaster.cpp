#include "Sim/Assets/LightmapRaster.h"

#include <algorithm>
#include <cmath>

namespace sim::assets {
namespace {

float SignedArea(const Vec2& a, const Vec2& b, const Vec2& c) {
    return 0.5f * ((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y));
}

/// Koordinat barisentrik sebuah titik, dijepit ke dalam segitiganya.
///
/// **Dijepit, bukan ditolak.** Texel yang pusatnya sedikit di luar tetap
/// dirasterisasi — lihat alasannya di `RasteriseLightmap` — dan barisentrik
/// negatif akan menempatkannya di luar permukaan, melayang di udara. Yang
/// dijepit mendarat di tepi segitiganya, yaitu tempat terdekat yang benar-benar
/// permukaan.
Vec3 ClampedBarycentric(const Vec2& a, const Vec2& b, const Vec2& c, const Vec2& point) {
    const float area = SignedArea(a, b, c);
    if (std::abs(area) < 1e-20f) {
        return Vec3(1.0f, 0.0f, 0.0f);
    }
    Vec3 weights(SignedArea(point, b, c) / area, SignedArea(a, point, c) / area, 0.0f);
    weights.z = 1.0f - weights.x - weights.y;
    weights = glm::max(weights, Vec3(0.0f));
    const float total = weights.x + weights.y + weights.z;
    return total > 1e-20f ? weights / total : Vec3(1.0f, 0.0f, 0.0f);
}

}  // namespace

LightmapRaster RasteriseLightmap(const MeshData& mesh, uint32_t width, uint32_t height) {
    LightmapRaster raster;
    if (width == 0 || height == 0 || !mesh.IsValid() || !mesh.hasLightmapUv) {
        return raster;
    }
    raster.width = width;
    raster.height = height;
    raster.texels.assign(static_cast<std::size_t>(width) * height, LightmapTexel{});

    const auto widthF = static_cast<float>(width);
    const auto heightF = static_cast<float>(height);

    for (std::size_t at = 0; at + 2 < mesh.indices.size(); at += 3) {
        const MeshVertex& v0 = mesh.vertices[mesh.indices[at]];
        const MeshVertex& v1 = mesh.vertices[mesh.indices[at + 1]];
        const MeshVertex& v2 = mesh.vertices[mesh.indices[at + 2]];

        const Vec2 uv0(v0.lightmapUv.x * widthF, v0.lightmapUv.y * heightF);
        const Vec2 uv1(v1.lightmapUv.x * widthF, v1.lightmapUv.y * heightF);
        const Vec2 uv2(v2.lightmapUv.x * widthF, v2.lightmapUv.y * heightF);

        // **Kotaknya dilebarkan setengah texel ke segala arah**, dan itu yang
        // membuat segitiga yang lebih kecil daripada satu texel tetap mendapat
        // texel. Tanpa pelebaran ini pagar dan daun keluar sebagai lubang hitam
        // yang bentuknya mengikuti geometrinya.
        const Vec2 minimum = glm::min(uv0, glm::min(uv1, uv2)) - Vec2(0.5f);
        const Vec2 maximum = glm::max(uv0, glm::max(uv1, uv2)) + Vec2(0.5f);
        const int x0 = std::max(0, static_cast<int>(std::floor(minimum.x)));
        const int y0 = std::max(0, static_cast<int>(std::floor(minimum.y)));
        const int x1 = std::min(static_cast<int>(width) - 1, static_cast<int>(std::ceil(maximum.x)));
        const int y1 =
            std::min(static_cast<int>(height) - 1, static_cast<int>(std::ceil(maximum.y)));

        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const Vec2 centre(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
                const Vec3 weights = ClampedBarycentric(uv0, uv1, uv2, centre);

                LightmapTexel& texel = raster.texels[static_cast<std::size_t>(y) * width + x];
                // **Yang pertama menang, bukan yang terakhir.** Dua segitiga
                // bertetangga sama-sama menyentuh texel di jahitannya, dan
                // menulis dua kali berarti posisinya berpindah-pindah menurut
                // urutan indeks — sebuah lightmap yang berubah ketika mesh-nya
                // diurutkan ulang tanpa berubah bentuk.
                if (texel.covered) {
                    continue;
                }
                texel.position = v0.position * weights.x + v1.position * weights.y +
                                 v2.position * weights.z;
                const Vec3 normal = v0.normal * weights.x + v1.normal * weights.y +
                                    v2.normal * weights.z;
                texel.normal = glm::length(normal) > 1e-6f ? glm::normalize(normal)
                                                           : Vec3(0.0f, 1.0f, 0.0f);
                texel.covered = true;
                ++raster.coveredCount;
            }
        }
    }
    return raster;
}

void DilateLightmap(std::vector<Vec3>& values, const LightmapRaster& raster, uint32_t radius) {
    if (!raster.IsValid() || values.size() != raster.texels.size()) {
        return;
    }
    std::vector<bool> filled(raster.texels.size());
    for (std::size_t i = 0; i < raster.texels.size(); ++i) {
        filled[i] = raster.texels[i].covered;
    }

    const int width = static_cast<int>(raster.width);
    const int height = static_cast<int>(raster.height);
    for (uint32_t pass = 0; pass < radius; ++pass) {
        std::vector<bool> nextFilled = filled;
        std::vector<Vec3> next = values;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const std::size_t at = static_cast<std::size_t>(y) * width + x;
                if (filled[at]) {
                    continue;
                }
                Vec3 sum(0.0f);
                int found = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx;
                        const int ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
                            continue;
                        }
                        const std::size_t neighbour = static_cast<std::size_t>(ny) * width + nx;
                        if (!filled[neighbour]) {
                            continue;
                        }
                        sum += values[neighbour];
                        ++found;
                    }
                }
                if (found > 0) {
                    next[at] = sum / static_cast<float>(found);
                    nextFilled[at] = true;
                }
            }
        }
        values = std::move(next);
        filled = std::move(nextFilled);
    }
}

}  // namespace sim::assets
