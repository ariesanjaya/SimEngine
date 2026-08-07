#pragma once

#include "Sim/RHI/Texture.h"
#include "Sim/RHI/TextureCube.h"
#include "Sim/Render/Ibl.h"

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
