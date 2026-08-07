// Penelusuran berjenjang: depth buffer layar, lalu clipmap SDF, lalu langit.
//
// **Satu implementasi, dipakai pass debug dan pass probe.** Dua salinan rumus
// yang sama adalah dua rumus yang akan berselisih — dan selisih antara apa yang
// dilihat alat diagnostik dan apa yang dipakai menggambar adalah selisih yang
// paling mahal, karena alatnya lalu berbohong justru saat paling dibutuhkan.
//
// Rumusnya harus sama dengan `SdfTraceBackend` dan `TraceScreenSpace` di sisi
// C++, yang keduanya diuji terhadap medan analitik dan depth buffer sintetis.

#ifndef GI_TRACE_GLSL
#define GI_TRACE_GLSL

#include "gi_common.glsl"

layout(set = 0, binding = 7) uniform sampler3D sdfCascade0;
layout(set = 0, binding = 8) uniform sampler3D sdfCascade1;
layout(set = 0, binding = 9) uniform sampler3D sdfCascade2;
layout(set = 0, binding = 10) uniform sampler2D hiZ;

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
    /// Tempat perpotongannya di layar. Dipakai membaca radiansi permukaan yang
    /// dikenai dari buffer warna yang sudah tersinari.
    vec2 uv;
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
    vec4 point = shadowParams.invViewProj * vec4(uv * 2.0 - 1.0, depth, 1.0);
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
    result.uv = vec2(0.0);
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
            result.uv = uv0 + duv * sHit;
            result.position = unproject(result.uv, sceneZ);
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


const int kLayerNone = 0;
const int kLayerScreen = 1;
const int kLayerSdf = 2;
const int kLayerSky = 3;

/// Hasil satu penelusuran berjenjang.
struct TieredHit {
    bool hit;
    int layer;
    float distance;
    vec3 position;
    vec2 uv;      // hanya berarti untuk lapis layar
    int steps;
};

TieredHit traceTiered(vec3 origin, vec3 direction, float tMax) {
    TieredHit result;
    ScreenHit screen = traceScreen(origin, direction, tMax);
    if (screen.hit) {
        result.hit = true;
        result.layer = kLayerScreen;
        result.distance = screen.distance;
        result.position = screen.position;
        result.uv = screen.uv;
        result.steps = screen.steps;
        return result;
    }

    // Meleset di layar bukan jawaban: sinar yang keluar layar berarti "layar
    // tidak tahu", bukan "tidak ada apa-apa di sana".
    SdfHit sdf = traceSdf(origin, direction, tMax);
    result.hit = sdf.hit;
    result.layer = sdf.hit ? kLayerSdf : kLayerSky;
    result.distance = sdf.distance;
    result.position = sdf.position;
    result.uv = vec2(0.0);
    // Langkah kedua lapis dijumlahkan: yang diawasi anggaran adalah biaya satu
    // sinar, dan sinar yang gagal di layar lalu berjalan penuh di SDF adalah
    // sinar yang paling mahal.
    result.steps = sdf.steps + screen.steps;
    return result;
}

/// Langit analitik sementara.
///
/// **Nilainya menyalin nilai bawaan `GradientSky` di `Ibl.h`**, dan itu memang
/// duplikasi yang disengaja untuk sementara: langit sungguhan datang di E8.8
/// bersama atmosfer, dan sampai saat itu satu-satunya alternatif adalah
/// mengangkut tiga warna lewat UBO untuk sesuatu yang akan dibuang.
vec3 giSky(vec3 direction) {
    const vec3 zenith = vec3(0.18, 0.32, 0.62);
    const vec3 horizon = vec3(0.62, 0.66, 0.72);
    const vec3 ground = vec3(0.14, 0.13, 0.12);
    float t = direction.y;
    return t >= 0.0 ? mix(horizon, zenith, sqrt(t)) : mix(horizon, ground, sqrt(-t));
}

#endif
