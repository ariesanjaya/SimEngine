// Sphere tracing SDF clipmap, dan tampilan diagnostiknya.
//
// **Rumusnya harus sama dengan `SdfTraceBackend` di sisi C++.** Yang di CPU
// adalah acuan kebenaran dan sudah diuji terhadap medan analitik; yang di sini
// adalah yang benar-benar dipakai. Dua rumus yang ditulis terpisah akan
// berselisih tepat di kasus yang paling sulit dilihat — dan kriteria selesai M2
// justru menuntut keduanya cocok pada uji ray tunggal.

#include "gi_trace.slang"
#include "sdf_debug_common.slang"

static const int kViewNormal = 2;
static const int kViewIrradiance = 3;
static const int kViewMarchSteps = 5;
static const int kViewTraceLayers = 6;

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

/// Peta warna untuk heatmap: biru → hijau → kuning → merah.
float3 heat(float t) {
    t = clamp(t, 0.0, 1.0);
    return clamp(float3(t * 3.0 - 1.4, 1.4 - abs(t * 3.0 - 1.5), 1.0 - t * 3.0), 0.0, 1.0);
}

[shader("fragment")]
float4 main(SdfDebugVarying input) : SV_Target {
    float3 origin = input.nearPoint;
    float3 direction = normalize(input.farPoint - input.nearPoint);

    // **Jenjang: layar dulu, lalu SDF, lalu langit.** Urutannya bukan soal biaya
    // melainkan ketelitian — depth buffer punya resolusi geometri sungguhan,
    // sedangkan voxel SDF terhalus sepuluh sentimeter.
    TieredHit hit = traceTiered(origin, direction, pc.params.y);
    int layer = hit.layer;

    int view = int(pc.params.x);
    if (view == kViewIrradiance) {
        // Iradiansi mentah, sebelum denoise. Dibaca dari piksel yang benar-benar
        // tergambar, bukan dari hasil penelusuran di atas: yang diperlihatkan
        // adalah apa yang akan diterima permukaan di layar, dan itulah yang
        // dipakai shading di M6.
        float2 pixel = input.position.xy;
        float depth = hiZ.Load(int3(int2(pixel), 0)).r;
        if (depth <= 0.0) {
            return float4(0.02, 0.02, 0.03, 1.0);
        }
        float2 uv = pixel / shadowParams.viewportSize.xy;
        float4 world = mul(shadowParams.invViewProj, float4(uv * 2.0 - 1.0, depth, 1.0));
        float3 position = world.xyz / world.w;
        float3 normal = octDecode(gbufferNormal.Load(int3(int2(pixel), 0)).rg);
        float3 irradiance = resolveProbes(pixel, position, normal, pc.params.z);
        // Dibagi pi supaya yang tampil radiansi permukaan Lambert ber-albedo 1,
        // bukan iradiansi mentah yang skalanya pi kali lebih besar.
        return float4(irradiance / 3.14159265, 1.0);
    }
    if (view == kViewTraceLayers) {
        // Hijau lapis layar, biru SDF, abu-abu langit. Jenjang yang tidak
        // terlihat adalah jenjang yang diam-diam berhenti dipakai.
        if (layer == kLayerScreen) {
            return float4(0.25, 0.85, 0.35, 1.0);
        }
        if (layer == kLayerSdf) {
            return float4(0.30, 0.50, 0.95, 1.0);
        }
        return float4(0.16, 0.16, 0.18, 1.0);
    }
    if (view == kViewMarchSteps) {
        // **Inilah alat yang paling sering dipakai.** Ia satu-satunya yang
        // menunjukkan di mana sphere tracing kehabisan anggaran sebelum sampai
        // ke permukaan — dan itu gejala yang tidak terlihat sama sekali pada
        // gambar akhirnya.
        float fraction = float(hit.steps) / max(shadowParams.sdfParams.w, 1.0);
        return float4(heat(fraction), 1.0);
    }
    if (view == kViewNormal) {
        if (!hit.hit) {
            return float4(0.05, 0.05, 0.06, 1.0);
        }
        int cascade = cascadeFor(hit.position);
        float3 normal =
            cascade >= 0 ? sdfSurfaceNormal(cascade, hit.position) : float3(0.0, 1.0, 0.0);
        return float4(normal * 0.5 + 0.5, 1.0);
    }

    // Sisanya: jarak yang ditelusuri, abu-abu. Inilah yang dibandingkan
    // berdampingan dengan depth buffer raster untuk kriteria selesai M1.
    if (!hit.hit) {
        return float4(0.05, 0.05, 0.06, 1.0);
    }
    float shade = 1.0 - clamp(hit.distance / max(pc.params.y, 1e-3), 0.0, 1.0);
    return float4(shade, shade, shade, 1.0);
}
