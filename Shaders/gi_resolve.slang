// Resolve probe ke piksel: iradiansi sebuah permukaan dari empat probe
// tetangganya.
//
// **Satu implementasi, dipakai pass debug dan pass forward.** Yang diperlihatkan
// tampilan Irradiance harus persis yang dipakai menggambar — alat diagnostik
// yang berselisih dengan jalur sesungguhnya berbohong justru saat paling
// dibutuhkan.

#ifndef GI_RESOLVE_SLANG
#define GI_RESOLVE_SLANG

#include "gi_common.slang"
#include "shadow_common.slang"
#include "gi_depth.slang"

[[vk::binding(11, 0)]] Sampler2D gbufferNormal;
[[vk::binding(13, 0)]] Sampler2D probeShR;
[[vk::binding(14, 0)]] Sampler2D probeShG;
[[vk::binding(15, 0)]] Sampler2D probeShB;

/// Iradiansi sebuah piksel dari empat probe tetangganya.
///
/// **Bilinear dikali bilateral, lalu dinormalkan.** Bilinear saja mencampur
/// probe yang duduk di permukaan lain — cahaya lantai merembes ke dinding di
/// sepanjang sudut ruangan. Bilateral saja membuang kehalusan dan
/// memperlihatkan kisi ubin. Yang dinormalkan jumlah bobotnya, bukan
/// jumlah probenya: probe yang ditolak tidak boleh menggelapkan hasilnya,
/// ia harus tidak ikut sama sekali.
float3 resolveProbes(float2 pixel, float3 world, float3 normal, float tile) {
    float2 viewport = shadowParams.viewportSize.xy;
    int2 counts = int2((viewport + tile - 1.0) / tile);

    // Dikurangi setengah ubin karena probe duduk di pusat ubinnya, bukan di
    // pojoknya — tanpa itu seluruh interpolasi bergeser setengah ubin.
    float2 tileCoord = pixel / tile - 0.5;
    float2 base = floor(tileCoord);
    float2 f = tileCoord - base;

    float4 sumR = float4(0.0);
    float4 sumG = float4(0.0);
    float4 sumB = float4(0.0);
    float sumWeight = 0.0;

    for (int corner = 0; corner < 4; ++corner) {
        float2 offset = float2(float(corner & 1), float((corner >> 1) & 1));
        int2 probe = clamp(int2(base + offset), int2(0), counts - 1);

        float2 probePixel = min((float2(probe) + 0.5) * tile, viewport - 0.5);
        float probeDepth = hiZ.Load(int3(int2(probePixel), 0)).r;
        if (probeDepth <= 0.0) {
            continue;  // probe jatuh di langit: tidak ada permukaan untuk dipijak
        }
        float2 probeUv = probePixel / viewport;
        float4 probeWorld =
            mul(shadowParams.invViewProj, float4(probeUv * 2.0 - 1.0, probeDepth, 1.0));
        float3 probePosition = probeWorld.xyz / probeWorld.w;
        float3 probeNormal = octDecode(gbufferNormal.Load(int3(int2(probePixel), 0)).rg);

        float bilinear = lerp(1.0 - f.x, f.x, offset.x) * lerp(1.0 - f.y, f.y, offset.y);
        float weight = bilinear * giProbeWeight(probePosition, probeNormal, world, normal,
                                                0.5, 0.5);
        if (weight <= 0.0) {
            continue;
        }
        sumR += probeShR.Load(int3(probe, 0)) * weight;
        sumG += probeShG.Load(int3(probe, 0)) * weight;
        sumB += probeShB.Load(int3(probe, 0)) * weight;
        sumWeight += weight;
    }

    if (sumWeight <= 1e-6) {
        return float3(0.0);
    }
    float inverse = 1.0 / sumWeight;
    return giEvaluateSh(sumR * inverse, sumG * inverse, sumB * inverse, normal);
}


/// Iradiansi sebuah piksel layar, atau nol bila tidak ada probe yang berlaku.
float3 giIrradianceAt(float2 pixel, float3 world, float3 normal, float tile) {
    return resolveProbes(pixel, world, normal, tile);
}

#endif
