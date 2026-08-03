#include "Sim/Terrain/TerrainBrush.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace sim::terrain {
namespace {

uint32_t Hash(uint32_t value) {
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    value ^= value >> 16;
    return value;
}

/// Noise nilai pada kisi sampel, halus antar sel.
float ValueNoise(uint32_t seed, float x, float y) {
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const float tx = x - fx;
    const float ty = y - fy;
    const auto corner = [seed](float cx, float cy) {
        const auto ix = static_cast<uint32_t>(static_cast<int32_t>(cx) + 32768);
        const auto iy = static_cast<uint32_t>(static_cast<int32_t>(cy) + 32768);
        return static_cast<float>(Hash(seed ^ Hash(ix * 0x9e3779b9U + iy)) >> 8) *
                   (2.0f / 16777216.0f) -
               1.0f;
    };
    // Smoothstep pada kedua sumbu: interpolasi linear meninggalkan lipatan yang
    // terlihat jelas sebagai kisi persegi pada terrain.
    const float sx = tx * tx * (3.0f - 2.0f * tx);
    const float sy = ty * ty * (3.0f - 2.0f * ty);
    const float a = corner(fx, fy);
    const float b = corner(fx + 1.0f, fy);
    const float c = corner(fx, fy + 1.0f);
    const float d = corner(fx + 1.0f, fy + 1.0f);
    return (a * (1.0f - sx) + b * sx) * (1.0f - sy) + (c * (1.0f - sx) + d * sx) * sy;
}

/// Salinan sebuah rentang, dilebarkan satu sampel ke tiap arah.
///
/// Smooth membaca tetangga. Membacanya langsung dari terrain berarti sampel yang
/// sudah ditulis di iterasi ini ikut terbaca, sehingga hasilnya bergantung pada
/// urutan penelusuran — dan brush yang sama menghasilkan bukit yang miring ke
/// arah penelusurannya.
struct Snapshot {
    SampleRect rect;
    std::vector<float> heights;

    float At(int x, int y) const {
        const int cx = std::clamp(x, rect.x0, rect.x1 - 1);
        const int cy = std::clamp(y, rect.y0, rect.y1 - 1);
        return heights[static_cast<std::size_t>((cy - rect.y0) * rect.Width() + (cx - rect.x0))];
    }
};

Snapshot Capture(const Terrain& terrain, const SampleRect& area) {
    Snapshot snapshot;
    snapshot.rect.x0 = std::max(0, area.x0 - 1);
    snapshot.rect.y0 = std::max(0, area.y0 - 1);
    snapshot.rect.x1 = std::min(terrain.SamplesX(), area.x1 + 1);
    snapshot.rect.y1 = std::min(terrain.SamplesY(), area.y1 + 1);
    snapshot.heights.resize(static_cast<std::size_t>(snapshot.rect.Width()) *
                            static_cast<std::size_t>(snapshot.rect.Height()));
    std::size_t at = 0;
    for (int y = snapshot.rect.y0; y < snapshot.rect.y1; ++y) {
        for (int x = snapshot.rect.x0; x < snapshot.rect.x1; ++x) {
            snapshot.heights[at++] = terrain.HeightAt(x, y);
        }
    }
    return snapshot;
}

}  // namespace

const char* ToString(BrushKind kind) {
    switch (kind) {
        case BrushKind::Raise: return "Raise";
        case BrushKind::Lower: return "Lower";
        case BrushKind::Flatten: return "Flatten";
        case BrushKind::Smooth: return "Smooth";
        case BrushKind::Noise: return "Noise";
    }
    return "Unknown";
}

float BrushWeight(const Brush& brush, float distance) {
    if (brush.radius <= 0.0f || distance >= brush.radius) {
        return 0.0f;
    }
    const float t = distance / brush.radius;
    // `falloff` menentukan seberapa lebar dataran penuh di tengah. Dengan 0,
    // seluruh lingkaran berbobot penuh dan tepinya tajam — itu yang dibutuhkan
    // untuk memahat bentuk bersudut, dan tidak bisa didapat dari kurva yang
    // selalu melembut.
    const float inner = 1.0f - std::clamp(brush.falloff, 0.0f, 1.0f);
    if (t <= inner) {
        return 1.0f;
    }
    if (inner >= 1.0f) {
        return 1.0f;
    }
    const float u = (t - inner) / (1.0f - inner);
    return 1.0f - u * u * (3.0f - 2.0f * u);
}

void ApplyDab(Terrain& terrain, const Brush& brush, float worldX, float worldZ, float dt) {
    if (dt <= 0.0f || brush.radius <= 0.0f) {
        return;
    }
    const SampleRect rect = terrain.RectForCircle(worldX, worldZ, brush.radius);
    if (rect.Empty()) {
        return;
    }
    const float spacing = terrain.Desc().sampleSpacing;
    const bool needsNeighbours = brush.kind == BrushKind::Smooth;
    const Snapshot snapshot = needsNeighbours ? Capture(terrain, rect) : Snapshot{};

    for (int y = rect.y0; y < rect.y1; ++y) {
        for (int x = rect.x0; x < rect.x1; ++x) {
            const float dx = static_cast<float>(x) * spacing - worldX;
            const float dz = static_cast<float>(y) * spacing - worldZ;
            const float weight = BrushWeight(brush, std::sqrt(dx * dx + dz * dz));
            if (weight <= 0.0f) {
                continue;
            }
            const float amount = brush.strength * dt * weight;
            const float current = terrain.HeightAt(x, y);
            float next = current;
            switch (brush.kind) {
                case BrushKind::Raise: next = current + amount; break;
                case BrushKind::Lower: next = current - amount; break;
                case BrushKind::Flatten:
                    next = current + (brush.targetHeight - current) * std::min(1.0f, amount);
                    break;
                case BrushKind::Smooth: {
                    const float average =
                        (snapshot.At(x - 1, y) + snapshot.At(x + 1, y) + snapshot.At(x, y - 1) +
                         snapshot.At(x, y + 1)) *
                        0.25f;
                    next = current + (average - current) * std::min(1.0f, amount);
                    break;
                }
                case BrushKind::Noise: {
                    const float nx = static_cast<float>(x) * spacing * brush.noiseFrequency;
                    const float nz = static_cast<float>(y) * spacing * brush.noiseFrequency;
                    next = current + ValueNoise(brush.seed, nx, nz) * amount;
                    break;
                }
            }
            terrain.SetHeightAt(x, y, next);
        }
    }
}

void ApplyRamp(Terrain& terrain, const Brush& brush, const Vec3& start, const Vec3& end) {
    const Vec3 axis(end.x - start.x, 0.0f, end.z - start.z);
    const float lengthSq = axis.x * axis.x + axis.z * axis.z;
    if (lengthSq <= 1e-6f || brush.radius <= 0.0f) {
        return;
    }

    const float spacing = terrain.Desc().sampleSpacing;
    const float minX = std::min(start.x, end.x) - brush.radius;
    const float maxX = std::max(start.x, end.x) + brush.radius;
    const float minZ = std::min(start.z, end.z) - brush.radius;
    const float maxZ = std::max(start.z, end.z) + brush.radius;
    const SampleRect rect{
        std::max(0, static_cast<int>(std::floor(minX / spacing))),
        std::max(0, static_cast<int>(std::floor(minZ / spacing))),
        std::min(terrain.SamplesX(), static_cast<int>(std::ceil(maxX / spacing)) + 1),
        std::min(terrain.SamplesY(), static_cast<int>(std::ceil(maxZ / spacing)) + 1)};

    for (int y = rect.y0; y < rect.y1; ++y) {
        for (int x = rect.x0; x < rect.x1; ++x) {
            const float px = static_cast<float>(x) * spacing;
            const float pz = static_cast<float>(y) * spacing;
            // Proyeksi ke ruas garis, dijepit ke ujungnya: tanpa penjepitan,
            // ramp merambat tak berhingga melewati kedua ujungnya.
            const float t = std::clamp(
                ((px - start.x) * axis.x + (pz - start.z) * axis.z) / lengthSq, 0.0f, 1.0f);
            const float cx = start.x + axis.x * t;
            const float cz = start.z + axis.z * t;
            const float dx = px - cx;
            const float dz = pz - cz;
            const float weight = BrushWeight(brush, std::sqrt(dx * dx + dz * dz));
            if (weight <= 0.0f) {
                continue;
            }
            const float target = start.y + (end.y - start.y) * t;
            const float current = terrain.HeightAt(x, y);
            terrain.SetHeightAt(x, y, current + (target - current) * weight);
        }
    }
}

void ApplyThermalErosion(Terrain& terrain, const SampleRect& area, int iterations,
                         float talusDegrees, float strength) {
    if (iterations <= 0 || area.Empty()) {
        return;
    }
    const float spacing = terrain.Desc().sampleSpacing;
    // Sudut talus jadi beda tinggi maksimum antar sampel bertetangga. Di atas
    // itu material longsor; di bawahnya lereng dianggap stabil.
    const float talus = std::tan(glm::radians(std::clamp(talusDegrees, 0.0f, 89.0f))) * spacing;
    const float rate = std::clamp(strength, 0.0f, 0.5f);

    for (int pass = 0; pass < iterations; ++pass) {
        const Snapshot before = Capture(terrain, area);
        for (int y = area.y0; y < area.y1; ++y) {
            for (int x = area.x0; x < area.x1; ++x) {
                const float here = before.At(x, y);
                float moved = 0.0f;
                const std::array<std::pair<int, int>, 4> neighbours{
                    {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}}};
                for (const auto& [nx, ny] : neighbours) {
                    const float drop = here - before.At(nx, ny);
                    if (drop > talus) {
                        moved += (drop - talus) * rate * 0.25f;
                    }
                }
                if (moved > 0.0f) {
                    terrain.SetHeightAt(x, y, here - moved);
                }
            }
        }
        // Sisi penerimanya dikerjakan terpisah supaya material yang turun tidak
        // langsung ikut longsor lagi dalam iterasi yang sama — longsoran
        // berantai dalam satu langkah membuat hasilnya bergantung urutan
        // penelusuran, bukan pada bentuk terrainnya.
        for (int y = area.y0; y < area.y1; ++y) {
            for (int x = area.x0; x < area.x1; ++x) {
                const float here = before.At(x, y);
                float gained = 0.0f;
                const std::array<std::pair<int, int>, 4> neighbours{
                    {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}}};
                for (const auto& [nx, ny] : neighbours) {
                    const float drop = before.At(nx, ny) - here;
                    if (drop > talus) {
                        gained += (drop - talus) * rate * 0.25f;
                    }
                }
                if (gained > 0.0f) {
                    terrain.SetHeightAt(x, y, terrain.HeightAt(x, y) + gained);
                }
            }
        }
    }
}

}  // namespace sim::terrain
