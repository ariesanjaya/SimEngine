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
static const int kViewMarchSteps = 5;
static const int kViewTraceLayers = 6;

/// Normal dari gradien medan jarak, beda hingga tengah.
float3 sdfNormal(int cascade, float3 position) {
    float h = cascadeVoxelSize(cascade) * 0.5;
    return normalize(float3(
        sampleCascadeSdf(cascade, position + float3(h, 0, 0)) -
            sampleCascadeSdf(cascade, position - float3(h, 0, 0)),
        sampleCascadeSdf(cascade, position + float3(0, h, 0)) -
            sampleCascadeSdf(cascade, position - float3(0, h, 0)),
        sampleCascadeSdf(cascade, position + float3(0, 0, h)) -
            sampleCascadeSdf(cascade, position - float3(0, 0, h))));
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
        float3 normal = cascade >= 0 ? sdfNormal(cascade, hit.position) : float3(0.0, 1.0, 0.0);
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
