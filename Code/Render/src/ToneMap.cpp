#include "Sim/Render/ToneMap.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace sim::render {
namespace {

/// Matriks masuk dan keluar ACES. Ditulis sebagai baris di sini dan
/// ditransposkan saat dipakai — glm menyusun `mat3` dari kolom, dan angka-angka
/// ini diterbitkan sebagai baris. Menyalinnya apa adanya menghasilkan matriks
/// yang tertranspos, yang tidak menghasilkan galat apa pun: hanya warna yang
/// bergeser rona, sedikit, di seluruh gambar.
constexpr float kInput[9] = {
    0.59719f, 0.35458f, 0.04823f,  //
    0.07600f, 0.90834f, 0.01566f,  //
    0.02840f, 0.13383f, 0.83777f,
};

constexpr float kOutput[9] = {
    1.60475f,  -0.53108f, -0.07367f,  //
    -0.10208f, 1.10813f,  -0.00605f,  //
    -0.00327f, -0.07276f, 1.07602f,
};

Vec3 ApplyRows(const float (&m)[9], const Vec3& v) {
    return Vec3(m[0] * v.x + m[1] * v.y + m[2] * v.z,  //
                m[3] * v.x + m[4] * v.y + m[5] * v.z,  //
                m[6] * v.x + m[7] * v.y + m[8] * v.z);
}

}  // namespace

float AcesCurve(float value) {
    const float a = value * (value + 0.0245786f) - 0.000090537f;
    const float b = value * (0.983729f * value + 0.4329510f) + 0.238081f;
    return std::clamp(a / b, 0.0f, 1.0f);
}

Vec3 AcesToneMap(const Vec3& color) {
    const Vec3 inAces = ApplyRows(kInput, color);
    const Vec3 fitted(AcesCurve(inAces.x), AcesCurve(inAces.y), AcesCurve(inAces.z));
    const Vec3 result = ApplyRows(kOutput, fitted);
    return glm::clamp(result, Vec3(0.0f), Vec3(1.0f));
}

float AcesWhitePoint() {
    // Akar kurvanya pada nilai 1: (1 - 0,983729)v² - 0,4083724v - 0,2381715 = 0.
    constexpr float a = 1.0f - 0.983729f;
    constexpr float b = 0.0245786f - 0.4329510f;
    constexpr float c = -0.000090537f - 0.238081f;
    return (-b + std::sqrt(b * b - 4.0f * a * c)) / (2.0f * a);
}

float Luminance(const Vec3& color) {
    return 0.2126f * color.x + 0.7152f * color.y + 0.0722f * color.z;
}

float Ev100FromLuminance(float averageLuminance) {
    // S = 100, K = 12,5 → EV100 = log2(L · 8).
    constexpr float kCalibration = 100.0f / 12.5f;
    return std::log2(std::max(averageLuminance, 1e-6f) * kCalibration);
}

float ExposureFromEv100(float ev100) {
    const float maxLuminance = 1.2f * std::exp2(ev100);
    return 1.0f / std::max(maxLuminance, 1e-6f);
}

float AutoExposure(float averageLuminance, float compensationStops) {
    return ExposureFromEv100(Ev100FromLuminance(averageLuminance) - compensationStops);
}

uint32_t LogLuminanceReducer::LevelCount() {
    uint32_t levels = 0;
    for (uint32_t size = kSeedSize; size > 1; size /= 2) {
        ++levels;
    }
    return levels;
}

float LogLuminanceReducer::LogLuminanceOf(const Vec3& color) {
    return std::log2(std::max(Luminance(color), kMinLuminance));
}

std::vector<float> LogLuminanceReducer::ReduceLevel(const std::vector<float>& source,
                                                    uint32_t size) {
    const uint32_t half = std::max(size / 2u, 1u);
    std::vector<float> result(static_cast<std::size_t>(half) * half, 0.0f);
    for (uint32_t y = 0; y < half; ++y) {
        for (uint32_t x = 0; x < half; ++x) {
            const std::size_t base = static_cast<std::size_t>(y * 2) * size + x * 2;
            const float sum = source[base] + source[base + 1] + source[base + size] +
                              source[base + size + 1];
            result[static_cast<std::size_t>(y) * half + x] = sum * 0.25f;
        }
    }
    return result;
}

std::vector<float> LogLuminanceReducer::Seed(const std::vector<Vec3>& pixels, uint32_t width,
                                             uint32_t height) {
    std::vector<float> seed(static_cast<std::size_t>(kSeedSize) * kSeedSize, 0.0f);
    if (width == 0 || height == 0 || pixels.empty()) {
        return seed;
    }
    // **Dikumpulkan, bukan disebar.** Bentuk pertama berjalan dari piksel sumber
    // ke texel petak, dan itu meninggalkan sebagian besar petak kosong begitu
    // viewport lebih kecil daripada 256×256 — texel kosong lalu diisi lantai
    // luminansi, dan seluruh pengukuran anjlok ke lantai itu tanpa satu pun
    // galat. Berjalan dari texel ke petak sumbernya tidak punya kasus kosong:
    // setiap texel selalu punya paling tidak satu piksel di bawahnya.
    for (uint32_t ty = 0; ty < kSeedSize; ++ty) {
        const uint32_t y0 = ty * height / kSeedSize;
        const uint32_t y1 = std::max((ty + 1) * height / kSeedSize, y0 + 1);
        for (uint32_t tx = 0; tx < kSeedSize; ++tx) {
            const uint32_t x0 = tx * width / kSeedSize;
            const uint32_t x1 = std::max((tx + 1) * width / kSeedSize, x0 + 1);
            float sum = 0.0f;
            uint32_t count = 0;
            for (uint32_t y = y0; y < y1 && y < height; ++y) {
                for (uint32_t x = x0; x < x1 && x < width; ++x) {
                    sum += LogLuminanceOf(pixels[static_cast<std::size_t>(y) * width + x]);
                    ++count;
                }
            }
            seed[static_cast<std::size_t>(ty) * kSeedSize + tx] =
                count > 0 ? sum / static_cast<float>(count) : std::log2(kMinLuminance);
        }
    }
    return seed;
}

float LogLuminanceReducer::Reduce(const std::vector<Vec3>& pixels, uint32_t width,
                                  uint32_t height) {
    std::vector<float> level = Seed(pixels, width, height);
    for (uint32_t size = kSeedSize; size > 1; size /= 2) {
        level = ReduceLevel(level, size);
    }
    return std::exp2(level.front());
}

float ExposureAdaptation::Update(float target, float deltaSeconds) {
    if (!ready_) {
        Reset(target);
        return current_;
    }
    if (deltaSeconds <= 0.0f) {
        return current_;
    }
    // Eksposur naik berarti adegan menggelap; yang dikejar mata adalah
    // kecerahannya, jadi tetapan waktu dipilih dari arah kecerahan, bukan dari
    // arah pengalinya.
    const float tau = target < current_ ? brighten_ : darken_;
    const float blend = tau <= 1e-4f ? 1.0f : 1.0f - std::exp(-deltaSeconds / tau);
    current_ += (target - current_) * blend;
    return current_;
}

}  // namespace sim::render
