#version 450

#include "shadow_common.glsl"

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

layout(set = 0, binding = 7) uniform sampler3D sdfCascade0;
layout(set = 0, binding = 8) uniform sampler3D sdfCascade1;
layout(set = 0, binding = 9) uniform sampler3D sdfCascade2;

layout(location = 0) in vec3 vNearPoint;
layout(location = 1) in vec3 vFarPoint;
layout(location = 0) out vec4 outColor;

const int kViewNormal = 2;
const int kViewMarchSteps = 5;

float cascadeVoxelSize(int cascade) { return shadowParams.sdfOrigin[cascade].w; }

/// Kaskade terhalus yang memuat sebuah titik, atau -1.
int cascadeFor(vec3 world) {
    int count = int(shadowParams.sdfParams.y);
    float resolution = shadowParams.sdfParams.x;
    for (int i = 0; i < count; ++i) {
        vec3 origin = shadowParams.sdfOrigin[i].xyz;
        float voxel = shadowParams.sdfOrigin[i].w;
        vec3 local = world - origin;
        float span = voxel * resolution;
        // Sedikit di dalam tepinya: sampel trilinear membaca voxel tetangga,
        // dan tetangga di tepi kaskade adalah milik sisi seberangnya.
        if (all(greaterThanEqual(local, vec3(voxel))) &&
            all(lessThanEqual(local, vec3(span - voxel)))) {
            return i;
        }
    }
    return -1;
}

float sampleCascade(int cascade, vec3 world) {
    float voxel = shadowParams.sdfOrigin[cascade].w;
    float resolution = shadowParams.sdfParams.x;
    // Koordinat tekstur toroidal: dibagi resolusi lalu dibiarkan membungkus oleh
    // sampler REPEAT. Titik asalnya tidak dikurangkan — justru itu inti clipmap,
    // sebuah voxel dunia selalu memetakan ke texel yang sama.
    vec3 uvw = (world / voxel - vec3(0.5)) / resolution;
    // Cabang, bukan pemilihan lewat ternary: GLSL tidak mengizinkan sampler
    // sebagai nilai ekspresi. Larik sampler akan lebih rapi, tapi ia menuntut
    // indeks yang seragam di seluruh subgroup — dan indeks kaskade di sini
    // memang berbeda antar-piksel.
    float encoded;
    if (cascade == 0) {
        encoded = texture(sdfCascade0, uvw).r;
    } else if (cascade == 1) {
        encoded = texture(sdfCascade1, uvw).r;
    } else {
        encoded = texture(sdfCascade2, uvw).r;
    }
    float band = voxel * shadowParams.sdfParams.z;
    return (encoded - 0.5) * band * 2.0;
}

struct SdfHit {
    bool hit;
    float distance;
    vec3 position;
    int steps;
};

SdfHit traceSdf(vec3 origin, vec3 direction, float tMax) {
    SdfHit result;
    result.hit = false;
    result.distance = 0.0;
    result.position = origin;
    result.steps = 0;

    int maxSteps = int(shadowParams.sdfParams.w);
    float travelled = 0.0;
    for (int step = 0; step < maxSteps; ++step) {
        vec3 position = origin + direction * travelled;
        int cascade = cascadeFor(position);
        if (cascade < 0) {
            result.steps = step;
            return result;
        }
        float distance = sampleCascade(cascade, position);
        float voxel = cascadeVoxelSize(cascade);
        if (distance < voxel * 0.5) {
            result.hit = true;
            result.distance = travelled;
            result.position = position;
            result.steps = step + 1;
            return result;
        }
        // Langkah minimum seperempat voxel: tanpa itu, permukaan yang jaraknya
        // hampir nol tapi belum melewati ambang membuat lingkaran berputar
        // menghabiskan anggaran langkah tanpa maju.
        travelled += max(distance, voxel * 0.25);
        if (travelled > tMax) {
            result.steps = step + 1;
            return result;
        }
    }
    result.steps = maxSteps;
    return result;
}

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
    SdfHit hit = traceSdf(origin, direction, pc.params.y);

    int view = int(pc.params.x);
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
