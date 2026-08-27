#pragma once

#include "Sim/RHI/Texture.h"
#include "Sim/RHI/TextureCube.h"
#include "Sim/Render/Ibl.h"

#include <vector>

namespace sim::render {

struct IblBakeSettings {
    /// Sisi muka cubemap prefilter pada mip 0.
    uint32_t cubeSize = 64;
    /// Banyaknya mip. Mip terakhir mewakili kekasaran 1.
    uint32_t mipCount = 5;
    uint32_t dfgSize = 64;
    /// Sampel GGX per texel prefilter, di luar mip 0 yang cuma satu pengambilan.
    uint32_t prefilterSamples = 64;
    uint32_t dfgSamples = 256;
    /// Cuplikan bola untuk proyeksi SH. **Nol berarti lewati**, dan itu dipakai
    /// pemanggang yang sudah punya SH-nya sendiri dari panggangan yang lebih
    /// sering: menghitungnya lagi di sini hanya menghasilkan angka yang sama.
    uint32_t irradianceSamples = 8192;
};

/// Hasil pembakaran IBL, siap diikat ke descriptor.
struct BakedIbl {
    Sh9 irradiance;
    rhi::TextureCube prefiltered;
    rhi::Texture2D dfg;

    bool IsValid() const { return prefiltered.IsValid() && dfg.IsValid(); }
    void Destroy();
};

/// Panggangan yang belum menyentuh GPU sama sekali.
///
/// **Dipisah karena yang mahal dan yang harus di main thread bukan bagian yang
/// sama.** Menghitung texel prefilter memakan detik dan tidak menyentuh satu
/// pun objek Vulkan; membuat tekstur dan menyalinnya menyentuh device dan
/// karena itu harus di thread yang memilikinya. Yang menggabungkan keduanya
/// memaksa memilih antara membekukan editor dan menyentuh device dari worker.
///
/// **LUT DFG sengaja tidak di sini.** Ia tidak bergantung pada lingkungan sama
/// sekali — hanya pada BRDF-nya — jadi memanggangnya ulang setiap langit
/// berubah adalah 914 ms (Debug) yang dibuang untuk menghasilkan byte yang sama
/// persis. Yang memanggangnya memanggilnya sekali, sendiri.
struct IblBakeCpu {
    Sh9 irradiance;
    uint32_t cubeSize = 0;
    uint32_t mipCount = 0;
    /// RGBA32F, seluruh mip berurutan, tiap mip enam muka berurutan — tata letak
    /// yang diminta `TextureCube::Create`.
    std::vector<float> cubeTexels;

    bool IsValid() const { return cubeSize > 0 && mipCount > 0 && !cubeTexels.empty(); }
};

/// Menghitung iradiansi SH dan peta prefilter. Aman dipanggil dari thread mana
/// pun: ia tidak menyentuh `rhi::Device`.
IblBakeCpu BakeIblCpu(const IEnvironmentSampler& environment, const IblBakeSettings& settings);

/// Mengunggah hasil `BakeIblCpu` beserta LUT DFG-nya. **Main thread.**
bool UploadIbl(rhi::Device& device, const IblBakeCpu& baked, const DfgLut& dfg, BakedIbl& out);

/// Membakar IBL sebuah lingkungan di CPU lalu mengunggahnya.
///
/// **Di CPU, dan itu keputusan sementara yang disengaja.** Membakarnya di GPU
/// menuntut pass compute beserta rantai barrier-nya sendiri, dan yang dibakar di
/// sini adalah lingkungan yang berubah paling banyak sekali per sesi. Yang
/// dibayar beberapa ratus milidetik sekali; yang didapat adalah matematika yang
/// sama persis dengan yang sudah teruji di `Tests/RenderTests.cpp`, tanpa
/// implementasi kedua.
///
/// Begitu peta lingkungan sungguhan bisa dimuat dan bisa berganti saat berjalan,
/// pembakaran GPU menjadi menarik — dan `IEnvironmentSampler` yang sama tetap
/// menjadi acuan kebenarannya.
bool BakeIbl(rhi::Device& device, const IEnvironmentSampler& environment,
             const IblBakeSettings& settings, BakedIbl& out);

}  // namespace sim::render
