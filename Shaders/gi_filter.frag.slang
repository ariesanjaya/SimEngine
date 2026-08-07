// Satu lintasan à-trous atas SH probe.
//
// **Disaring di kisi probe, bukan di resolusi layar.** Derau yang terlihat
// adalah derau per-probe — ubin 16×16 piksel yang terangnya berbeda dari
// tetangganya — dan setiap piksel toh sudah menginterpolasi empat probe. Menyaring
// di resolusi layar berarti menyaring 800 ribu piksel untuk menghapus derau yang
// hanya punya 3600 derajat kebebasan.
//
// **À-trous, bukan Gaussian bertingkat.** Lintasan ke-n melompat 2ⁿ texel dengan
// kernel yang sama, jadi dua lintasan menjangkau dua puluh probe dengan lima
// puluh pengambilan — sementara satu Gaussian selebar itu menuntut ratusan. Yang
// membuat lompatannya sah adalah bobot bilateralnya: bobot yang melintasi
// permukaan bernilai nol.
//
// Rumusnya harus sama dengan `AtrousPass` di `Denoise.cpp`.

#include "gi_common.slang"

struct FilterPush {
    // xy ukuran kisi probe, z lompatan lintasan ini, w tidak dipakai
    int4 grid;
    // x jarak bidang (meter), y kosinus normal minimum
    float4 weights;
};

[[vk::push_constant]] ConstantBuffer<FilterPush> pc;

[[vk::binding(0, 0)]] Sampler2D inputShR;
[[vk::binding(1, 0)]] Sampler2D inputShG;
[[vk::binding(2, 0)]] Sampler2D inputShB;
/// xyz posisi dunia probe, w normal tersandi. Nol pada w berarti probe itu
/// jatuh di langit dan tidak punya permukaan untuk dipijak.
[[vk::binding(3, 0)]] Sampler2D inputSurface;

struct FilterOut {
    float4 r : SV_Target0;
    float4 g : SV_Target1;
    float4 b : SV_Target2;
};

/// Kernel B-spline satu dimensi: 1, 4, 6, 4, 1 dibagi 16.
static const float kBSpline[5] = {1.0 / 16.0, 4.0 / 16.0, 6.0 / 16.0, 4.0 / 16.0, 1.0 / 16.0};

/// Harus sama persis dengan `unpackNormal` di gi_probe.frag.slang.
float3 unpackFilterNormal(float packed) {
    uint bits = uint((packed - 1.0) * 16777216.0 + 0.5);
    float2 oct = float2(float(bits & 4095u), float((bits >> 12) & 4095u)) / 4095.0;
    return octDecode(oct);
}

[shader("fragment")]
FilterOut main(float4 position : SV_Position) {
    int2 centre = int2(position.xy);
    float4 centreSurface = inputSurface.Load(int3(centre, 0));

    FilterOut output;
    output.r = inputShR.Load(int3(centre, 0));
    output.g = inputShG.Load(int3(centre, 0));
    output.b = inputShB.Load(int3(centre, 0));
    if (centreSurface.w == 0.0) {
        return output;  // probe di langit: tidak ada yang bisa disaring
    }

    float3 centreNormal = unpackFilterNormal(centreSurface.w);
    int step = max(pc.grid.z, 1);

    float4 sumR = float4(0.0);
    float4 sumG = float4(0.0);
    float4 sumB = float4(0.0);
    float sumWeight = 0.0;

    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            int2 sample = centre + int2(dx, dy) * step;
            if (any(sample < int2(0)) || any(sample >= pc.grid.xy)) {
                continue;
            }
            float4 surface = inputSurface.Load(int3(sample, 0));
            if (surface.w == 0.0) {
                continue;
            }
            // Bobot bilateral: jarak tegak lurus bidang probe pusat, dan
            // kesamaan normal. Yang melintasi permukaan bernilai nol.
            float plane = abs(dot(surface.xyz - centreSurface.xyz, centreNormal));
            if (plane > pc.weights.x) {
                continue;
            }
            float cosine = dot(centreNormal, unpackFilterNormal(surface.w));
            if (cosine < pc.weights.y) {
                continue;
            }

            float weight = kBSpline[dx + 2] * kBSpline[dy + 2];
            sumR += inputShR.Load(int3(sample, 0)) * weight;
            sumG += inputShG.Load(int3(sample, 0)) * weight;
            sumB += inputShB.Load(int3(sample, 0)) * weight;
            sumWeight += weight;
        }
    }

    if (sumWeight <= 1e-6) {
        return output;
    }
    // Dinormalkan oleh bobot yang benar-benar dipakai, bukan oleh satu:
    // tetangga yang ditolak tidak boleh menggelapkan hasilnya, ia harus tidak
    // ikut sama sekali.
    float inverse = 1.0 / sumWeight;
    output.r = sumR * inverse;
    output.g = sumG * inverse;
    output.b = sumB * inverse;
    return output;
}
