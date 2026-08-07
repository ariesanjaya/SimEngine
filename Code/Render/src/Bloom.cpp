#include "Sim/Render/Bloom.h"

#include "Sim/Render/ToneMap.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace sim::render {
namespace {

/// Bobot kelompok tapis 13 cuplikan: tengah 0,5 dan empat sudut 0,125.
constexpr float kCentreWeight = 0.5f;
constexpr float kCornerWeight = 0.125f;

Vec3 Fetch(const BloomLevel& level, int32_t x, int32_t y) {
    const int32_t cx = std::clamp(x, 0, static_cast<int32_t>(level.width) - 1);
    const int32_t cy = std::clamp(y, 0, static_cast<int32_t>(level.height) - 1);
    return level.At(static_cast<uint32_t>(cx), static_cast<uint32_t>(cy));
}

/// Rata-rata sebuah kelompok empat cuplikan, dengan atau tanpa pembobotan Karis.
Vec3 GroupAverage(const std::array<Vec3, 4>& samples, bool karis) {
    if (!karis) {
        return (samples[0] + samples[1] + samples[2] + samples[3]) * 0.25f;
    }
    // Bobot 1/(1+luma) per cuplikan. **Bukan pemotongan nilai maksimum:**
    // memotong menghilangkan energi sorotan sungguhan sekaligus, sedangkan
    // pembobotan ini hanya menurunkan pengaruh piksel yang berdiri sendiri.
    Vec3 sum(0.0f);
    float total = 0.0f;
    for (const Vec3& sample : samples) {
        const float weight = 1.0f / (1.0f + Luminance(sample));
        sum += sample * weight;
        total += weight;
    }
    return total > 0.0f ? sum / total : Vec3(0.0f);
}

}  // namespace

uint32_t BloomChain::LevelsFor(uint32_t width, uint32_t height, uint32_t minSize) {
    uint32_t levels = 1;
    uint32_t w = width;
    uint32_t h = height;
    while (w / 2 >= minSize && h / 2 >= minSize) {
        w /= 2;
        h /= 2;
        ++levels;
    }
    return levels;
}

float BloomChain::SoftThreshold(float luminance, float threshold, float knee) {
    // Kurva berlutut Karis: kuadratik di dalam lutut, linier di luarnya, dan
    // turunannya menyambung di kedua ujung. Ambang tajam membuat permukaan yang
    // luminansinya melintasi ambang berkedip antara berpendar dan tidak.
    const float k = std::max(knee, 1e-4f);
    const float soft = std::clamp(luminance - threshold + k, 0.0f, 2.0f * k);
    const float contribution =
        std::max(soft * soft / (4.0f * k), luminance - threshold);
    return luminance > 1e-6f ? std::max(contribution, 0.0f) / luminance : 0.0f;
}

float BloomChain::DownsampleWeightSum() {
    return kCentreWeight + 4.0f * kCornerWeight;
}

BloomLevel BloomChain::Downsample(const BloomLevel& source, bool karis) {
    BloomLevel result;
    result.width = std::max(source.width / 2u, 1u);
    result.height = std::max(source.height / 2u, 1u);
    result.pixels.assign(static_cast<std::size_t>(result.width) * result.height, Vec3(0.0f));

    for (uint32_t y = 0; y < result.height; ++y) {
        for (uint32_t x = 0; x < result.width; ++x) {
            const auto sx = static_cast<int32_t>(x) * 2;
            const auto sy = static_cast<int32_t>(y) * 2;
            // Tiga belas cuplikan: sembilan pada kisi genap dan empat di antara.
            const auto at = [&](int32_t dx, int32_t dy) { return Fetch(source, sx + dx, sy + dy); };
            const Vec3 a = at(-2, -2), b = at(0, -2), c = at(2, -2);
            const Vec3 dd = at(-2, 0), e = at(0, 0), f = at(2, 0);
            const Vec3 g = at(-2, 2), h = at(0, 2), i = at(2, 2);
            const Vec3 j = at(-1, -1), k = at(1, -1), l = at(-1, 1), m = at(1, 1);

            const Vec3 centre = GroupAverage({j, k, l, m}, karis);
            const Vec3 topLeft = GroupAverage({a, b, dd, e}, karis);
            const Vec3 topRight = GroupAverage({b, c, e, f}, karis);
            const Vec3 bottomLeft = GroupAverage({dd, e, g, h}, karis);
            const Vec3 bottomRight = GroupAverage({e, f, h, i}, karis);

            result.At(x, y) = centre * kCentreWeight +
                              (topLeft + topRight + bottomLeft + bottomRight) * kCornerWeight;
        }
    }
    return result;
}

void BloomChain::UpsampleInto(BloomLevel& target, const BloomLevel& source, float scatter) {
    const float blend = std::clamp(scatter, 0.0f, 1.0f);
    for (uint32_t y = 0; y < target.height; ++y) {
        for (uint32_t x = 0; x < target.width; ++x) {
            const auto sx = static_cast<int32_t>(x / 2);
            const auto sy = static_cast<int32_t>(y / 2);
            // Tapis tenda 3×3, bobot 1-2-1 di kedua arah, berjumlah tepat satu.
            Vec3 sum(0.0f);
            static const float kTent[3] = {0.25f, 0.5f, 0.25f};
            for (int32_t dy = -1; dy <= 1; ++dy) {
                for (int32_t dx = -1; dx <= 1; ++dx) {
                    sum += Fetch(source, sx + dx, sy + dy) * (kTent[dx + 1] * kTent[dy + 1]);
                }
            }
            // Dipadu, bukan ditambahkan: yang ditambahkan menaikkan energi
            // seluruh gambar, dan energi itu lalu diukur eksposur otomatis.
            target.At(x, y) = glm::mix(target.At(x, y), sum, blend);
        }
    }
}

BloomLevel BloomChain::Build(const BloomLevel& scene, const BloomSettings& settings) {
    BloomLevel head = scene;
    for (Vec3& pixel : head.pixels) {
        pixel *= SoftThreshold(Luminance(pixel), settings.threshold, settings.knee);
    }

    std::vector<BloomLevel> chain;
    chain.push_back(std::move(head));
    const uint32_t levels = LevelsFor(scene.width, scene.height);
    for (uint32_t level = 1; level < levels; ++level) {
        chain.push_back(Downsample(chain.back(), level == 1));
    }
    for (std::size_t level = chain.size() - 1; level > 0; --level) {
        UpsampleInto(chain[level - 1], chain[level], settings.scatter);
    }
    return chain.front();
}

BloomLevel BloomChain::Composite(const BloomLevel& scene, const BloomLevel& bloom,
                                 float strength) {
    BloomLevel result = scene;
    const float scale = std::max(strength, 0.0f);
    for (std::size_t i = 0; i < result.pixels.size() && i < bloom.pixels.size(); ++i) {
        result.pixels[i] = scene.pixels[i] + bloom.pixels[i] * scale;
    }
    return result;
}

}  // namespace sim::render
