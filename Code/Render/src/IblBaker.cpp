#include "IblBaker.h"

#include "IblPrefilter.h"

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
/// di rentang terang saja. Cubemap 256 piksel dengan lima mip 8,0 MiB;
/// menghemat separuhnya tidak sebanding dengan risiko itu, dan yang
/// memilikinya satu per lingkungan — bukan satu per objek.
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
    // Permintaan di luar rantai tidak menyisakan apa pun untuk GPU, dan
    // jawabannya lalu seluruhnya di CPU — bukan sebuah galat, dan bukan rantai
    // yang sebagian kosong tanpa ada yang akan mengisinya. Nol tetap berarti
    // "tidak ada", satu-satunya artinya, jadi mip 0 tidak pernah bisa diminta
    // dari GPU lewat pintu ini.
    const uint32_t firstGpuMip =
        (settings.firstGpuMip == 0 || settings.firstGpuMip >= mipCount) ? 0u
                                                                       : settings.firstGpuMip;
    baked.firstGpuMip = firstGpuMip;
    // Mip yang benar-benar disaring di sini. Tanpa GPU itu seluruhnya.
    const uint32_t cpuMipCount = firstGpuMip == 0 ? mipCount : firstGpuMip;
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
    // untuk langit atmosferik satu cuplikan adalah satu ray march. Dari mip 0
    // ia pencarian tekstur, dan mip 0 memang sudah berisi lingkungan yang sama
    // — dicuplik sekali per texel, bukan ratusan kali.
    //
    // **Itu tetap tidak cukup pada ukuran bawaan.** Mip 1..4 sebuah cube 256²
    // adalah 130.560 texel dikali 256 sampel, yaitu 33 juta pencarian: terukur
    // 57,5 detik untuk seluruh panggangan (Debug, HDRI 4K) lewat jalur ini,
    // lawan 0,9 detik ketika `IblPrefilter` yang mengerjakannya. Jalur ini
    // karena itu jalur acuan dan jalur mundur, bukan jalur yang dipakai editor.
    CubemapEnvironment base;
    base.size = cubeSize;

    std::size_t at = 0;
    for (uint32_t mip = 0; mip < mipCount; ++mip) {
        const uint32_t extent = std::max(cubeSize >> mip, 1u);
        if (mip >= cpuMipCount) {
            // Diserahkan ke `IblPrefilter`. Texelnya tetap dilewati, bukan
            // dipotong dari `texels`: unggahannya menuntut rantai utuh, dan nol
            // yang tergambar hitam jauh lebih mudah dikenali sebagai dispatch
            // yang tidak berjalan daripada memori yang belum ditulis siapa pun.
            at += static_cast<std::size_t>(extent) * extent * kCubeFaceCount * 4;
            continue;
        }
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
    // Storage usage hanya untuk yang masih berutang mip. Memintanya selalu
    // menutup pilihan tata letak driver untuk cubemap yang isinya tidak pernah
    // berubah lagi sesudah diunggah — mayoritasnya.
    if (!out.prefiltered.Create(device, baked.cubeSize, baked.mipCount, kCubeFormat,
                                kCubeBytesPerTexel,
                                {bytes, baked.cubeTexels.size() * sizeof(float)},
                                baked.firstGpuMip > 0)) {
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
             const IblBakeSettings& settings, BakedIbl& out, IblPrefilter* prefilter) {
    const auto started = std::chrono::steady_clock::now();

    IblBakeSettings effective = settings;
    // **Jalur GPU diminta hanya kalau ada yang bisa menjalankannya.** Menyetel
    // `firstGpuMip` lalu tidak menyerahkan pemanggangnya menghasilkan cubemap
    // yang mipnya nol — pantulan hitam pada material kasar saja, yaitu bentuk
    // kegagalan yang paling lama tidak dikenali. Yang tanpa pemanggang jatuh
    // kembali ke CPU, dan itu lambat tetapi benar.
    if (prefilter == nullptr || !prefilter->IsValid()) {
        effective.firstGpuMip = 0;
    }

    const IblBakeCpu baked = BakeIblCpu(environment, effective);
    const DfgLut lut = BakeDfgLut(effective.dfgSize, effective.dfgSamples);
    if (!UploadIbl(device, baked, lut, out)) {
        return false;
    }
    if (baked.firstGpuMip > 0 &&
        !prefilter->Run(out.prefiltered, baked.firstGpuMip, effective.prefilterSamples)) {
        // Mip di atasnya tinggal nol, dan itu bukan gambar yang bisa dipakai.
        // Yang gagal melepasnya seluruhnya; pemanggilnya jatuh ke lingkungan
        // kosong, yang jujur.
        out.Destroy();
        return false;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    SIM_INFO("Render", "IBL baked in {} ms (cube {}x{}, {} mips, {} on GPU, DFG {})",
             elapsed.count(), baked.cubeSize, baked.cubeSize, baked.mipCount,
             baked.firstGpuMip > 0 ? baked.mipCount - baked.firstGpuMip : 0u, lut.size);
    return true;
}

}  // namespace sim::render
