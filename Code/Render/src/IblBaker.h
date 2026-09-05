#pragma once

#include "Sim/RHI/Texture.h"
#include "Sim/RHI/TextureCube.h"
#include "Sim/Render/Ibl.h"

namespace sim::render {

class IblPrefilter;

/// Hasil pembakaran IBL, siap diikat ke descriptor.
struct BakedIbl {
    Sh9 irradiance;
    rhi::TextureCube prefiltered;
    rhi::Texture2D dfg;

    bool IsValid() const { return prefiltered.IsValid() && dfg.IsValid(); }
    void Destroy();
};

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
/// **Prefilter mip di atas nol sudah pindah ke GPU** — syarat yang disebut
/// paragraf ini terpenuhi sejak B3, dan `IblPrefilter` yang mengerjakannya.
/// Yang tersisa di CPU adalah mip 0, karena hanya di sinilah
/// `IEnvironmentSampler` bisa dicuplik; ia sekaligus tetap menjadi acuan
/// kebenaran jalur compute, lewat `IblBakeSettings::firstGpuMip` yang nol.
///
/// `prefilter` null — atau belum dibuat — berarti seluruh rantai disaring CPU.
/// Itu benar, hanya lambat: pada `cubeSize` bawaan ia belasan detik, jadi yang
/// memanggilnya dari main thread harus menyerahkan pemanggangnya.
bool BakeIbl(rhi::Device& device, const IEnvironmentSampler& environment,
             const IblBakeSettings& settings, BakedIbl& out,
             IblPrefilter* prefilter = nullptr);

}  // namespace sim::render
