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

IblBakeCpu BakeIblCpu(const IEnvironmentSampler& environment, const IblBakeSettings& settings) {
    IblBakeCpu baked;
    const uint32_t cubeSize = std::max(settings.cubeSize, 1u);
    const uint32_t mipCount = std::clamp(settings.mipCount, 1u, 12u);
    baked.cubeSize = cubeSize;
    baked.mipCount = mipCount;
    if (settings.irradianceSamples > 0) {
        baked.irradiance = ProjectIrradiance(environment, settings.irradianceSamples);
    }

    // Satu blok berisi seluruh mip berurutan, tiap mip enam muka berurutan —
    // tata letak yang diminta `TextureCube::Create`, dan yang membuat
    // penyalinannya butuh region sebanyak mip saja.
    std::vector<float>& texels = baked.cubeTexels;
    texels.assign(
        rhi::TextureCube::TexelBytes(cubeSize, mipCount, kCubeBytesPerTexel) / sizeof(float),
        0.0f);

    // **Mip 0 lebih dulu, lalu ia yang menjadi sumber mip sisanya.**
    // Menyaring satu texel prefilter menuntut puluhan cuplikan lingkungan, dan
    // untuk langit atmosferik satu cuplikan adalah satu ray march: menyaring
    // 8160 texel dengan 64 sampel dari pencuplik analitik terukur 33 detik
    // (Debug). Dari mip 0 ia pencarian tekstur, dan mip 0 memang sudah berisi
    // lingkungan yang sama — dicuplik sekali per texel, bukan puluhan kali.
    CubemapEnvironment base;
    base.size = cubeSize;

    std::size_t at = 0;
    for (uint32_t mip = 0; mip < mipCount; ++mip) {
        const uint32_t extent = std::max(cubeSize >> mip, 1u);
        const float roughness = RoughnessForMip(mip, mipCount);
        // Mip 0 adalah cermin: satu pengambilan per texel, bukan integral yang
        // sampelnya menyebar. Menyaringnya di sana hanya membuat pantulan tajam
        // selalu sedikit buram.
        const uint32_t samples = mip == 0 ? 1u : std::max(settings.prefilterSamples, 1u);

        if (mip == 1) {
            // Mip 0 sudah lengkap di awal `texels`, jadi pencupliknya baru sah
            // di sini — bukan sebelum gelungnya berjalan.
            base.texels = texels.data();
        }
        for (int face = 0; face < kCubeFaceCount; ++face) {
            for (uint32_t y = 0; y < extent; ++y) {
                for (uint32_t x = 0; x < extent; ++x) {
                    // Tengah texel, sama alasannya dengan LUT DFG.
                    const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(extent);
                    const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(extent);
                    const Vec3 direction = CubeFaceDirection(face, u, v);
                    const Vec3 radiance =
                        mip == 0 ? environment.Sample(direction)
                                 : PrefilterSpecular(base, direction, roughness, samples);
                    texels[at++] = radiance.x;
                    texels[at++] = radiance.y;
                    texels[at++] = radiance.z;
                    texels[at++] = 1.0f;
                }
            }
        }
    }

    return baked;
}

bool UploadIbl(rhi::Device& device, const IblBakeCpu& baked, const DfgLut& dfg, BakedIbl& out) {
    if (!baked.IsValid() || dfg.size == 0) {
        return false;
    }
    out.irradiance = baked.irradiance;

    const auto* bytes = reinterpret_cast<const std::byte*>(baked.cubeTexels.data());
    if (!out.prefiltered.Create(device, baked.cubeSize, baked.mipCount, kCubeFormat,
                                kCubeBytesPerTexel,
                                {bytes, baked.cubeTexels.size() * sizeof(float)})) {
        out.Destroy();
        return false;
    }
    if (!out.dfg.Create(device, dfg.size, dfg.size, kDfgFormat, kDfgBytesPerTexel,
                        dfg.data.data())) {
        out.Destroy();
        return false;
    }
    return true;
}

bool BakeIbl(rhi::Device& device, const IEnvironmentSampler& environment,
             const IblBakeSettings& settings, BakedIbl& out) {
    const auto started = std::chrono::steady_clock::now();

    const IblBakeCpu baked = BakeIblCpu(environment, settings);
    const DfgLut lut = BakeDfgLut(settings.dfgSize, settings.dfgSamples);
    if (!UploadIbl(device, baked, lut, out)) {
        return false;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    SIM_INFO("Render", "IBL baked in {} ms (cube {}x{}, {} mips, DFG {})", elapsed.count(),
             baked.cubeSize, baked.cubeSize, baked.mipCount, lut.size);
    return true;
}

}  // namespace sim::render
