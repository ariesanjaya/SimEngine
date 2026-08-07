#version 450

#include "shadow_common.glsl"
#include "gi_trace.glsl"

// Sphere tracing SDF clipmap, dan tampilan diagnostiknya.
//
// **Rumusnya harus sama dengan `SdfTraceBackend` di sisi C++.** Yang di CPU
// adalah acuan kebenaran dan sudah diuji terhadap medan analitik; yang di sini
// adalah yang benar-benar dipakai. Dua rumus yang ditulis terpisah akan
// berselisih tepat di kasus yang paling sulit dilihat — dan kriteria selesai M2
// justru menuntut keduanya cocok pada uji ray tunggal.

layout(push_constant) uniform Push {
    mat4 invViewProj;
    vec4 cameraPos;
    vec4 params;  // x jenis debug view, y jangkauan trace maksimum
} pc;

layout(location = 0) in vec3 vNearPoint;
layout(location = 1) in vec3 vFarPoint;
layout(location = 0) out vec4 outColor;

const int kViewNormal = 2;
const int kViewMarchSteps = 5;
const int kViewTraceLayers = 6;

/// Normal dari gradien medan jarak, beda hingga tengah.
vec3 sdfNormal(int cascade, vec3 position) {
    float h = cascadeVoxelSize(cascade) * 0.5;
    return normalize(vec3(
        sampleCascade(cascade, position + vec3(h, 0, 0)) -
            sampleCascade(cascade, position - vec3(h, 0, 0)),
        sampleCascade(cascade, position + vec3(0, h, 0)) -
            sampleCascade(cascade, position - vec3(0, h, 0)),
        sampleCascade(cascade, position + vec3(0, 0, h)) -
            sampleCascade(cascade, position - vec3(0, 0, h))));
}

/// Peta warna untuk heatmap: biru → hijau → kuning → merah.
vec3 heat(float t) {
    t = clamp(t, 0.0, 1.0);
    return clamp(vec3(t * 3.0 - 1.4, 1.4 - abs(t * 3.0 - 1.5), 1.0 - t * 3.0), 0.0, 1.0);
}

void main() {
    vec3 origin = vNearPoint;
    vec3 direction = normalize(vFarPoint - vNearPoint);

    // **Jenjang: layar dulu, lalu SDF, lalu langit.** Urutannya bukan soal biaya
    // melainkan ketelitian — depth buffer punya resolusi geometri sungguhan,
    // sedangkan voxel SDF terhalus sepuluh sentimeter. Meleset di lapis pertama
    // bukan jawaban: sinar yang keluar layar berarti "layar tidak tahu", dan
    // menyamakannya dengan "tidak ada apa-apa" menghasilkan lubang gelap tepat
    // di tepi layar.
    TieredHit hit = traceTiered(origin, direction, pc.params.y);
    int layer = hit.layer;

    int view = int(pc.params.x);
    if (view == kViewTraceLayers) {
        // Hijau lapis layar, biru SDF, abu-abu langit. Jenjang yang tidak
        // terlihat adalah jenjang yang diam-diam berhenti dipakai.
        if (layer == kLayerScreen) {
            outColor = vec4(0.25, 0.85, 0.35, 1.0);
        } else if (layer == kLayerSdf) {
            outColor = vec4(0.30, 0.50, 0.95, 1.0);
        } else {
            outColor = vec4(0.16, 0.16, 0.18, 1.0);
        }
        return;
    }
    if (view == kViewMarchSteps) {
        // **Inilah alat yang paling sering dipakai.** Ia satu-satunya yang
        // menunjukkan di mana sphere tracing kehabisan anggaran sebelum sampai
        // ke permukaan — dan itu gejala yang tidak terlihat sama sekali pada
        // gambar akhirnya.
        float fraction = float(hit.steps) / max(shadowParams.sdfParams.w, 1.0);
        outColor = vec4(heat(fraction), 1.0);
        return;
    }
    if (view == kViewNormal) {
        if (!hit.hit) {
            outColor = vec4(0.05, 0.05, 0.06, 1.0);
            return;
        }
        int cascade = cascadeFor(hit.position);
        vec3 normal = cascade >= 0 ? sdfNormal(cascade, hit.position) : vec3(0.0, 1.0, 0.0);
        outColor = vec4(normal * 0.5 + 0.5, 1.0);
        return;
    }

    // Sisanya: jarak yang ditelusuri, abu-abu. Inilah yang dibandingkan
    // berdampingan dengan depth buffer raster untuk kriteria selesai M1.
    if (!hit.hit) {
        outColor = vec4(0.05, 0.05, 0.06, 1.0);
        return;
    }
    float shade = 1.0 - clamp(hit.distance / max(pc.params.y, 1e-3), 0.0, 1.0);
    outColor = vec4(vec3(shade), 1.0);
}
