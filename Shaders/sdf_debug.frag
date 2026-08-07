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
layout(set = 0, binding = 10) uniform sampler2D hiZ;

layout(location = 0) in vec3 vNearPoint;
layout(location = 1) in vec3 vFarPoint;
layout(location = 0) out vec4 outColor;

const int kViewNormal = 2;
const int kViewMarchSteps = 5;
const int kViewTraceLayers = 6;

const int kLayerNone = 0;
const int kLayerScreen = 1;
const int kLayerSdf = 2;
const int kLayerSky = 3;

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

// --- Lapis screen-space -----------------------------------------------------
//
// **Rumusnya harus sama dengan `TraceScreenSpace` di sisi C++**, sama seperti
// sphere tracing di atas. Yang di CPU adalah acuan yang diuji terhadap depth
// buffer sintetis; yang di sini adalah yang benar-benar dipakai.

struct ScreenHit {
    bool hit;
    bool leftScreen;
    float distance;
    vec3 position;
    int steps;
};

/// Ukuran sepetak yang sah pada sebuah tingkat, dengan pembagian dibulatkan ke
/// bawah — aturan mip Vulkan, sama dengan `HiZPyramid` di sisi C++.
ivec2 hizLevelSize(int level) {
    ivec2 size = ivec2(shadowParams.viewportSize.xy);
    for (int i = 0; i < level; ++i) {
        size = max(ivec2(1), size / 2);
    }
    return size;
}

float hizSample(int level, vec2 uv, ivec2 size) {
    ivec2 texel = clamp(ivec2(floor(uv * vec2(size))), ivec2(0), size - ivec2(1));
    return texelFetch(hiZ, texel, level).r;
}

vec3 unproject(vec2 uv, float depth) {
    vec4 point = pc.invViewProj * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return point.xyz / point.w;
}

/// Parameter saat sinar meninggalkan sel yang memuatnya pada tingkat ini.
float nextCellBoundary(vec2 uv0, vec2 duv, ivec2 size, float s) {
    vec2 scale = vec2(size);
    vec2 point = (uv0 + duv * s) * scale;
    vec2 cell = floor(point);

    float next = 1e30;
    for (int axis = 0; axis < 2; ++axis) {
        float step = duv[axis] * scale[axis];
        if (abs(step) < 1e-9) {
            continue;
        }
        float edge = step > 0.0 ? cell[axis] + 1.0 : cell[axis];
        next = min(next, (edge - uv0[axis] * scale[axis]) / step);
    }
    if (next > 1e29) {
        return 1e30;
    }
    // Dorongan sepersekian sel: tanpa itu, pembulatan menaruh titik berikutnya
    // tepat di batas dan sel yang sama diuji lagi sampai anggaran habis.
    return next + (1.0 / max(length(duv * scale), 1e-9)) / 128.0;
}

ScreenHit traceScreen(vec3 origin, vec3 direction, float tMax) {
    ScreenHit result;
    result.hit = false;
    result.leftScreen = true;
    result.distance = 0.0;
    result.position = origin;
    result.steps = 0;

    int levelCount = int(shadowParams.screenTrace.w);
    if (levelCount <= 0) {
        return result;
    }
    float thickness = shadowParams.screenTrace.x;
    float bias = min(shadowParams.screenTrace.y, tMax * 0.5);
    int maxSteps = int(shadowParams.screenTrace.z);

    vec4 clipStart = shadowParams.viewProj * vec4(origin + direction * bias, 1.0);
    vec4 clipEnd = shadowParams.viewProj * vec4(origin + direction * tMax, 1.0);
    // Titik dengan w tidak positif tidak punya proyeksi yang berarti: koordinat
    // layarnya adalah pantulan di seberang layar, dan penelusur yang memakainya
    // menelusuri tempat yang salah dengan penuh keyakinan.
    const float minW = 1e-4;
    if (clipStart.w < minW && clipEnd.w < minW) {
        return result;
    }
    if (clipStart.w < minW || clipEnd.w < minW) {
        float t = (minW - clipStart.w) / (clipEnd.w - clipStart.w);
        vec4 crossing = clipStart + (clipEnd - clipStart) * t;
        if (clipStart.w >= minW) {
            clipEnd = crossing;
        } else {
            clipStart = crossing;
        }
    }

    vec3 ndcStart = clipStart.xyz / clipStart.w;
    vec3 ndcEnd = clipEnd.xyz / clipEnd.w;
    vec2 uv0 = ndcStart.xy * 0.5 + 0.5;
    vec2 duv = (ndcEnd.xy * 0.5 + 0.5) - uv0;
    float z0 = ndcStart.z;
    float dz = ndcEnd.z - ndcStart.z;

    float sMin = 0.0;
    float sMax = 1.0;
    for (int axis = 0; axis < 2; ++axis) {
        if (abs(duv[axis]) < 1e-9) {
            if (uv0[axis] < 0.0 || uv0[axis] > 1.0) {
                return result;
            }
            continue;
        }
        float a = (0.0 - uv0[axis]) / duv[axis];
        float b = (1.0 - uv0[axis]) / duv[axis];
        sMin = max(sMin, min(a, b));
        sMax = min(sMax, max(a, b));
    }
    if (sMin > sMax) {
        return result;
    }

    int level = 0;
    float s = sMin;
    for (int step = 0; step < maxSteps; ++step) {
        result.steps = step + 1;
        ivec2 size = hizLevelSize(level);
        float sNext = min(nextCellBoundary(uv0, duv, size, s), sMax);
        float sceneZ = hizSample(level, uv0 + duv * ((s + sNext) * 0.5), size);

        float zEnter = z0 + s * dz;
        float zExit = z0 + sNext * dz;
        // Yang dibandingkan yang terkecil di sepanjang potongan ini: sinar GI
        // boleh mengarah balik ke kamera, dan pada sinar seperti itu depth
        // justru naik.
        if (min(zEnter, zExit) < sceneZ) {
            if (level > 0) {
                --level;
                continue;
            }
            // Uji ketebalan di tempat sinar MASUK piksel ini, bukan keluarnya.
            // Sinar yang masuk dalam keadaan sudah di belakang permukaan tidak
            // memotongnya — ia lewat di belakangnya.
            if (zEnter < sceneZ) {
                vec2 entryUv = uv0 + duv * s;
                if (distance(unproject(entryUv, zEnter), unproject(entryUv, sceneZ)) >
                    thickness) {
                    s = sNext;
                    if (s >= sMax) {
                        return result;
                    }
                    continue;
                }
            }
            float sHit = abs(dz) > 1e-9 ? clamp((sceneZ - z0) / dz, s, sNext) : s;
            result.hit = true;
            result.leftScreen = false;
            result.position = unproject(uv0 + duv * sHit, sceneZ);
            result.distance = dot(result.position - origin, direction);
            return result;
        }

        s = sNext;
        if (s >= sMax) {
            return result;
        }
        level = min(level + 1, levelCount - 1);
    }
    result.leftScreen = false;
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

    // **Jenjang: layar dulu, lalu SDF, lalu langit.** Urutannya bukan soal biaya
    // melainkan ketelitian — depth buffer punya resolusi geometri sungguhan,
    // sedangkan voxel SDF terhalus sepuluh sentimeter. Meleset di lapis pertama
    // bukan jawaban: sinar yang keluar layar berarti "layar tidak tahu", dan
    // menyamakannya dengan "tidak ada apa-apa" menghasilkan lubang gelap tepat
    // di tepi layar.
    ScreenHit screen = traceScreen(origin, direction, pc.params.y);
    SdfHit hit;
    int layer;
    if (screen.hit) {
        hit.hit = true;
        hit.distance = screen.distance;
        hit.position = screen.position;
        hit.steps = screen.steps;
        layer = kLayerScreen;
    } else {
        hit = traceSdf(origin, direction, pc.params.y);
        // Langkah kedua lapis dijumlahkan: yang diawasi anggaran adalah biaya
        // satu sinar, dan sinar yang gagal di layar lalu berjalan penuh di SDF
        // adalah sinar yang paling mahal — persis yang harus terlihat di
        // heatmap.
        hit.steps += screen.steps;
        layer = hit.hit ? kLayerSdf : kLayerSky;
    }

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
