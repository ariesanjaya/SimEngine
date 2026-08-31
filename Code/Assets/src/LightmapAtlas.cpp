#include "Sim/Assets/LightmapAtlas.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace sim::assets {
namespace {

uint32_t RoundUpPowerOfTwo(uint32_t value) {
    uint32_t result = 1;
    while (result < value) {
        result <<= 1;
    }
    return result;
}

}  // namespace

float MeshWorldArea(const MeshData& mesh, const Mat4& transform) {
    if (!mesh.IsValid()) {
        return 0.0f;
    }
    double total = 0.0;
    for (std::size_t at = 0; at + 2 < mesh.indices.size(); at += 3) {
        const Vec3 a = Vec3(transform * Vec4(mesh.vertices[mesh.indices[at]].position, 1.0f));
        const Vec3 b = Vec3(transform * Vec4(mesh.vertices[mesh.indices[at + 1]].position, 1.0f));
        const Vec3 c = Vec3(transform * Vec4(mesh.vertices[mesh.indices[at + 2]].position, 1.0f));
        total += 0.5 * glm::length(glm::cross(b - a, c - a));
    }
    return static_cast<float>(total);
}

uint32_t LightmapChartSide(float worldArea, float texelsPerMeter, uint32_t minSide,
                           uint32_t maxSide) {
    if (!(worldArea > 0.0f) || !(texelsPerMeter > 0.0f)) {
        return minSide;
    }
    // Luas dunia → sisi dalam meter → sisi dalam texel. Akar kuadratnya karena
    // yang disetel pengarang adalah texel **per meter**, bukan texel per meter
    // persegi: yang kedua membuat menggandakan ukuran sebuah benda melipatkan
    // kerapatannya empat kali.
    const float sideMeters = std::sqrt(worldArea);
    const auto texels = static_cast<uint32_t>(std::ceil(sideMeters * texelsPerMeter));
    return std::clamp(RoundUpPowerOfTwo(std::max(texels, 1u)), minSide, maxSide);
}

LightmapAtlasLayout PackLightmapAtlas(std::vector<LightmapChart> charts, uint32_t maxSide) {
    LightmapAtlasLayout layout;
    layout.charts = std::move(charts);
    if (layout.charts.empty()) {
        return layout;
    }

    uint64_t area = 0;
    uint32_t largest = 1;
    for (const LightmapChart& chart : layout.charts) {
        area += static_cast<uint64_t>(chart.side) * chart.side;
        largest = std::max(largest, chart.side);
    }

    // Tebakan awal: atlas persegi yang luasnya cukup, dinaikkan ke pangkat dua,
    // dan sedikitnya sebesar petak terbesar. Yang tidak muat menaikkannya sekali
    // lagi — pemaketan rak meninggalkan celah, jadi luas saja tidak menjamin.
    auto side = RoundUpPowerOfTwo(
        std::max(largest, static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(area))))));
    // **Dijepit ke batasnya sejak tebakan awal, bukan hanya saat tumbuh.**
    // Tanpa ini `maxSide` cuma membatasi pertumbuhan: pemanggil yang meminta
    // atlas paling besar 256 mendapat 1024 karena tebakan awalnya sudah di sana,
    // dan anggaran memori yang ia sebut diabaikan tanpa satu pun galat.
    side = std::min(side, std::max(RoundUpPowerOfTwo(maxSide), 1u));

    // Terbesar dulu: rak yang dimulai dengan petak kecil tidak bisa menampung
    // petak besar sesudahnya, dan tingginya terlanjur terpakai.
    std::vector<uint32_t> order(layout.charts.size());
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&layout](uint32_t lhs, uint32_t rhs) {
        return layout.charts[lhs].side > layout.charts[rhs].side;
    });

    while (true) {
        for (LightmapChart& chart : layout.charts) {
            chart.placed = false;
        }
        uint32_t penX = 0;
        uint32_t penY = 0;
        uint32_t shelfHeight = 0;
        uint32_t dropped = 0;

        for (const uint32_t index : order) {
            LightmapChart& chart = layout.charts[index];
            if (chart.side == 0) {
                ++dropped;
                continue;
            }
            if (penX + chart.side > side) {
                penX = 0;
                penY += shelfHeight;
                shelfHeight = 0;
            }
            if (penY + chart.side > side) {
                ++dropped;
                continue;
            }
            chart.x = penX;
            chart.y = penY;
            chart.placed = true;
            penX += chart.side;
            shelfHeight = std::max(shelfHeight, chart.side);
        }

        if (dropped == 0 || side >= maxSide) {
            layout.width = side;
            layout.height = side;
            layout.dropped = dropped;
            break;
        }
        side <<= 1;
    }

    layout.usedTexels = 0;
    for (const LightmapChart& chart : layout.charts) {
        if (chart.placed) {
            layout.usedTexels += static_cast<uint64_t>(chart.side) * chart.side;
        }
    }
    const auto total = static_cast<double>(layout.width) * layout.height;
    layout.utilisation =
        total > 0.0 ? static_cast<float>(static_cast<double>(layout.usedTexels) / total) : 0.0f;
    return layout;
}

}  // namespace sim::assets
