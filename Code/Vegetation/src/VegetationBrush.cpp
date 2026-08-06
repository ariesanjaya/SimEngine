#include "Sim/Vegetation/VegetationBrush.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace sim::vegetation {
namespace {

using sim::terrain::BrushWeight;

/// Persegi sel kepadatan yang tersentuh sebuah lingkaran dunia, sudah dijepit.
struct CellRect {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;

    bool Empty() const { return x1 <= x0 || y1 <= y0; }
};

CellRect RectForCircle(const DensityMap& map, float worldX, float worldZ, float radius) {
    CellRect rect;
    if (map.Width() <= 0 || map.Height() <= 0) {
        return rect;
    }
    const float inv = 1.0f / map.CellSize();
    rect.x0 = std::max(0, static_cast<int>(std::floor((worldX - radius) * inv)));
    rect.y0 = std::max(0, static_cast<int>(std::floor((worldZ - radius) * inv)));
    rect.x1 = std::min(map.Width(), static_cast<int>(std::ceil((worldX + radius) * inv)) + 1);
    rect.y1 = std::min(map.Height(), static_cast<int>(std::ceil((worldZ + radius) * inv)) + 1);
    return rect;
}

/// Membulatkan menjauhi nilai sekarang, sama seperti sentuhan cat bobot layer
/// terrain — dan dengan pertukaran yang sama: sebuah sentuhan tidak pernah
/// berakhir tanpa hasil karena pembulatan memakan seluruh perubahannya, tapi
/// pita tipis di tepi kuas terisi sedikit lebih cepat daripada yang dijanjikan
/// profilnya. Selisihnya satu tingkat dari 255, dan alternatifnya adalah kuas
/// lemah yang tidak melakukan apa-apa sama sekali.
uint8_t Converge(float current, float target, float amount) {
    const float next = current + (target - current) * amount;
    const float rounded = target > current ? std::ceil(next) : std::floor(next);
    return static_cast<uint8_t>(std::clamp(rounded, 0.0f, 255.0f));
}

}  // namespace

void ApplyDensityDab(Vegetation& vegetation, const PaintBrush& brush, int layer, float worldX,
                     float worldZ, float dt) {
    const DensityMap& map = vegetation.Density(layer);
    const CellRect rect = RectForCircle(map, worldX, worldZ, brush.radius);
    if (rect.Empty() || dt <= 0.0f) {
        return;
    }
    const float cell = map.CellSize();
    const float target = std::clamp(brush.target, 0.0f, 1.0f) * 255.0f;

    for (int y = rect.y0; y < rect.y1; ++y) {
        for (int x = rect.x0; x < rect.x1; ++x) {
            const float dx = static_cast<float>(x) * cell - worldX;
            const float dz = static_cast<float>(y) * cell - worldZ;
            const float weight = BrushWeight(brush, std::sqrt(dx * dx + dz * dz));
            if (weight <= 0.0f) {
                continue;
            }
            const float amount = std::min(1.0f, brush.strength * dt * weight);
            vegetation.PaintDensity(
                layer, x, y, Converge(static_cast<float>(map.At(x, y)), target, amount));
        }
    }
}

void ApplySmoothDab(Vegetation& vegetation, const PaintBrush& brush, int layer, float worldX,
                    float worldZ, float dt) {
    const DensityMap& map = vegetation.Density(layer);
    const CellRect rect = RectForCircle(map, worldX, worldZ, brush.radius);
    if (rect.Empty() || dt <= 0.0f) {
        return;
    }
    const float cell = map.CellSize();

    // Salinan diambil satu sel lebih lebar di setiap sisi, supaya sel di tepi
    // persegi tetap punya keempat tetangganya. Tanpa itu, tepi persegi kuas
    // meratakan dirinya terhadap dirinya sendiri dan meninggalkan bekas kotak.
    const int copyX0 = rect.x0 - 1;
    const int copyY0 = rect.y0 - 1;
    const int copyWidth = rect.x1 - rect.x0 + 2;
    const int copyHeight = rect.y1 - rect.y0 + 2;
    std::vector<uint8_t> before(static_cast<std::size_t>(copyWidth) *
                                static_cast<std::size_t>(copyHeight));
    for (int y = 0; y < copyHeight; ++y) {
        for (int x = 0; x < copyWidth; ++x) {
            before[static_cast<std::size_t>(y) * static_cast<std::size_t>(copyWidth) +
                   static_cast<std::size_t>(x)] = map.At(copyX0 + x, copyY0 + y);
        }
    }
    const auto sample = [&](int x, int y) {
        return static_cast<float>(
            before[static_cast<std::size_t>(y - copyY0) * static_cast<std::size_t>(copyWidth) +
                   static_cast<std::size_t>(x - copyX0)]);
    };

    for (int y = rect.y0; y < rect.y1; ++y) {
        for (int x = rect.x0; x < rect.x1; ++x) {
            const float dx = static_cast<float>(x) * cell - worldX;
            const float dz = static_cast<float>(y) * cell - worldZ;
            const float weight = BrushWeight(brush, std::sqrt(dx * dx + dz * dz));
            if (weight <= 0.0f) {
                continue;
            }
            const float average =
                (sample(x - 1, y) + sample(x + 1, y) + sample(x, y - 1) + sample(x, y + 1)) * 0.25f;
            const float amount = std::min(1.0f, brush.strength * dt * weight);
            vegetation.PaintDensity(layer, x, y, Converge(sample(x, y), average, amount));
        }
    }
}

std::size_t ApplyEraseDab(Vegetation& vegetation, int layer, float worldX, float worldZ,
                          float radius) {
    return vegetation.Erase(layer, worldX, worldZ, radius);
}

void ApplyPlantDab(Vegetation& vegetation, const Terrain& terrain, int layer, float worldX,
                   float worldZ) {
    if (layer < 0 || layer >= vegetation.LayerCount()) {
        return;
    }
    Rng rng(InstanceKey(worldX, worldZ));
    const float scaleRoll = rng.NextFloat();
    const float yawRoll = rng.NextFloat();
    vegetation.Plant(layer, MakeInstance(vegetation.Layer(layer), worldX, worldZ,
                                         terrain.HeightAtWorld(worldX, worldZ),
                                         terrain.NormalAtWorld(worldX, worldZ), scaleRoll,
                                         yawRoll));
}

}  // namespace sim::vegetation
