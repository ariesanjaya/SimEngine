#include "IblBaker.h"

#include "Sim/Core/Log.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace sim::render {
namespace {

/// RGBA32F. Bukan RGBA16F, dan itu pilihan yang sadar.
///
/// Setengah presisi akan memangkas separuh memorinya, tapi menuntut konversi
/// float→half di CPU — dan konversi yang ditulis tangan adalah tempat yang
/// bagus untuk kesalahan yang muncul sebagai lingkungan yang sedikit salah warna
/// di rentang terang saja. Cubemap 64 piksel dengan lima mip hanya 400 KB;
/// menghemat 200 KB tidak sebanding dengan itu.
constexpr VkFormat kCubeFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr uint32_t kCubeBytesPerTexel = 16;

/// RG32F: LUT DFG hanya punya dua kanal, dan keduanya butuh presisi penuh di
/// dekat nol tempat suku bias hidup.
constexpr VkFormat kDfgFormat = VK_FORMAT_R32G32_SFLOAT;
constexpr uint32_t kDfgBytesPerTexel = 8;

}  // namespace

void BakedIbl::Destroy() {
    prefiltered.Destroy();
    dfg.Destroy();
    irradiance = Sh9{};
}

bool BakeIbl(rhi::Device& device, const IEnvironmentSampler& environment,
             const IblBakeSettings& settings, BakedIbl& out) {
    const auto started = std::chrono::steady_clock::now();

    const uint32_t cubeSize = std::max(settings.cubeSize, 1u);
    const uint32_t mipCount = std::clamp(settings.mipCount, 1u, 12u);

    out.irradiance = ProjectIrradiance(environment, settings.irradianceSamples);

    // Satu blok berisi seluruh mip berurutan, tiap mip enam muka berurutan —
    // tata letak yang diminta `TextureCube::Create`, dan yang membuat
    // penyalinannya butuh region sebanyak mip saja.
    std::vector<float> texels(
        rhi::TextureCube::TexelBytes(cubeSize, mipCount, kCubeBytesPerTexel) / sizeof(float),
        0.0f);

    std::size_t at = 0;
    for (uint32_t mip = 0; mip < mipCount; ++mip) {
        const uint32_t extent = std::max(cubeSize >> mip, 1u);
        const float roughness = RoughnessForMip(mip, mipCount);
        // Mip 0 adalah cermin: satu pengambilan per texel, bukan integral yang
        // sampelnya menyebar. Menyaringnya di sana hanya membuat pantulan tajam
        // selalu sedikit buram.
        const uint32_t samples = mip == 0 ? 1u : std::max(settings.prefilterSamples, 1u);

        for (int face = 0; face < kCubeFaceCount; ++face) {
            for (uint32_t y = 0; y < extent; ++y) {
                for (uint32_t x = 0; x < extent; ++x) {
                    // Tengah texel, sama alasannya dengan LUT DFG.
                    const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(extent);
                    const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(extent);
                    const Vec3 direction = CubeFaceDirection(face, u, v);
                    const Vec3 radiance =
                        mip == 0 ? environment.Sample(direction)
                                 : PrefilterSpecular(environment, direction, roughness, samples);
                    texels[at++] = radiance.x;
                    texels[at++] = radiance.y;
                    texels[at++] = radiance.z;
                    texels[at++] = 1.0f;
                }
            }
        }
    }

    const auto* bytes = reinterpret_cast<const std::byte*>(texels.data());
    if (!out.prefiltered.Create(device, cubeSize, mipCount, kCubeFormat, kCubeBytesPerTexel,
                                {bytes, texels.size() * sizeof(float)})) {
        out.Destroy();
        return false;
    }

    const DfgLut lut = BakeDfgLut(settings.dfgSize, settings.dfgSamples);
    if (!out.dfg.Create(device, lut.size, lut.size, kDfgFormat, kDfgBytesPerTexel,
                        lut.data.data())) {
        out.Destroy();
        return false;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    SIM_INFO("Render", "IBL baked in {} ms (cube {}x{}, {} mips, DFG {})", elapsed.count(),
             cubeSize, cubeSize, mipCount, lut.size);
    return true;
}

}  // namespace sim::render
