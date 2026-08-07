#include "Sim/Render/Denoise.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace sim::render {
namespace {

/// Kernel B-spline satu dimensi: 1, 4, 6, 4, 1 dibagi 16.
constexpr std::array<float, 5> kBSpline{1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f,
                                        1.0f / 16.0f};

}  // namespace

ProbeReprojection ReprojectProbe(const Vec3& worldPosition, const Mat4& previousViewProj,
                                 const glm::uvec2& viewport, uint32_t tileSize) {
    ProbeReprojection result;
    if (viewport.x == 0 || viewport.y == 0 || tileSize == 0) {
        return result;
    }
    const Vec4 clip = previousViewProj * Vec4(worldPosition, 1.0f);
    // Di belakang bidang dekat frame lalu: koordinat layarnya adalah pantulan
    // titik itu di seberang layar, dan riwayat yang diambil dari sana adalah
    // riwayat tempat yang sama sekali lain.
    if (clip.w <= 1e-6f) {
        return result;
    }
    const Vec3 ndc = Vec3(clip) / clip.w;
    const Vec2 uv(ndc.x * 0.5f + 0.5f, ndc.y * 0.5f + 0.5f);
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) {
        return result;
    }

    const Vec2 pixel(uv.x * static_cast<float>(viewport.x),
                     uv.y * static_cast<float>(viewport.y));
    const float tile = static_cast<float>(tileSize);
    // Dikurangi setengah ubin karena probe duduk di pusat ubinnya — aturan yang
    // sama dengan `ProbeGrid::TileCoordinate`, dan dua aturan yang berselisih
    // setengah ubin menghasilkan riwayat yang selalu diambil dari tetangga.
    result.tile = {pixel.x / tile - 0.5f, pixel.y / tile - 0.5f};
    result.onScreen = true;
    return result;
}

float AtrousKernel(int dx, int dy) {
    if (dx < -2 || dx > 2 || dy < -2 || dy > 2) {
        return 0.0f;
    }
    return kBSpline[static_cast<std::size_t>(dx + 2)] *
           kBSpline[static_cast<std::size_t>(dy + 2)];
}

void AtrousPass(std::span<const Vec3> input, std::span<const Vec3> positions,
                std::span<const Vec3> normals, std::span<const uint8_t> valid,
                const glm::uvec2& size, uint32_t stride, const AtrousSettings& settings,
                std::span<Vec3> output) {
    const std::size_t count = static_cast<std::size_t>(size.x) * size.y;
    if (input.size() < count || output.size() < count || positions.size() < count ||
        normals.size() < count || valid.size() < count) {
        return;
    }
    const auto step = static_cast<int>(std::max(stride, 1u));

    for (uint32_t y = 0; y < size.y; ++y) {
        for (uint32_t x = 0; x < size.x; ++x) {
            const std::size_t centre = static_cast<std::size_t>(y) * size.x + x;
            if (valid[centre] == 0) {
                output[centre] = input[centre];
                continue;
            }

            Vec3 sum(0.0f);
            float weightSum = 0.0f;
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    const int sx = static_cast<int>(x) + dx * step;
                    const int sy = static_cast<int>(y) + dy * step;
                    if (sx < 0 || sy < 0 || sx >= static_cast<int>(size.x) ||
                        sy >= static_cast<int>(size.y)) {
                        continue;
                    }
                    const std::size_t sample =
                        static_cast<std::size_t>(sy) * size.x + static_cast<std::size_t>(sx);
                    if (valid[sample] == 0) {
                        continue;
                    }

                    // Bobot bilateral: jarak tegak lurus bidang piksel pusat dan
                    // kesamaan normal. Yang melintasi permukaan bernilai nol, dan
                    // itulah yang membuat lompatan besar à-trous tetap sah.
                    const float plane =
                        std::abs(glm::dot(positions[sample] - positions[centre], normals[centre]));
                    if (plane > settings.planeDistance) {
                        continue;
                    }
                    const float cosine = glm::dot(normals[centre], normals[sample]);
                    if (cosine < settings.normalCosine) {
                        continue;
                    }

                    const float weight = AtrousKernel(dx, dy);
                    sum += input[sample] * weight;
                    weightSum += weight;
                }
            }
            // Dinormalkan oleh jumlah bobot yang benar-benar dipakai, bukan oleh
            // satu: tetangga yang ditolak tidak boleh menggelapkan hasilnya, ia
            // harus tidak ikut sama sekali.
            output[centre] = weightSum > 1e-6f ? sum / weightSum : input[centre];
        }
    }
}

uint32_t FramesToRespond(uint32_t maxFrames, float fraction) {
    const float target = std::clamp(fraction, 0.0f, 0.999f);
    const uint32_t window = std::max(maxFrames, 1u);
    // **Dimulai dari riwayat yang sudah penuh, bukan dari kosong.** Yang
    // ditanyakan kriteria M5 adalah berapa lama GI merespons lampu yang berubah
    // di tengah adegan yang sudah menyatu — bukan berapa lama ia terisi pertama
    // kali. Memodelkannya dari kosong membuat jawabannya selalu satu frame,
    // karena sampel pertama pada jendela yang masih kosong memang menggantikan
    // seluruhnya. Angka yang benar tidak akan pernah terlihat.
    float value = 0.0f;
    for (uint32_t frame = 0; frame < 10000; ++frame) {
        value += (1.0f - value) / static_cast<float>(window);
        if (value >= target) {
            return frame + 1;
        }
    }
    return 10000;
}

Vec3 ClampHistory(const Vec3& history, const Vec3& mean, const Vec3& deviation, float scale) {
    const Vec3 spread = glm::max(deviation, Vec3(0.0f)) * std::max(scale, 0.0f);
    return glm::clamp(history, mean - spread, mean + spread);
}

}  // namespace sim::render
