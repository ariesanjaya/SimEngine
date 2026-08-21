// Penelusuran berjenjang: depth buffer layar, lalu clipmap SDF, lalu langit.
//
// **Satu implementasi, dipakai pass debug dan pass probe.** Dua salinan rumus
// yang sama adalah dua rumus yang akan berselisih — dan selisih antara apa yang
// dilihat alat diagnostik dan apa yang dipakai menggambar adalah selisih yang
// paling mahal, karena alatnya lalu berbohong justru saat paling dibutuhkan.
//
// Rumusnya harus sama dengan `SdfTraceBackend` dan `TraceScreenSpace` di sisi
// C++, yang keduanya diuji terhadap medan analitik dan depth buffer sintetis.

#ifndef GI_TRACE_SLANG
#define GI_TRACE_SLANG

#include "gi_common.slang"
#include "shadow_common.slang"
// Hanya parameterisasi LUT-nya yang dipakai dari sini — tidak ada satu pun
// binding di dalamnya, jadi ia tidak menuntut apa pun dari set descriptor
// pemakainya.
#include "sky_common.slang"

/// Entri grid medan jarak: transformasi dan kotak batasnya, cermin dari
/// `BakedSceneField::GpuGrid`. Dipakai mencari grid mana yang memuat sebuah
/// titik, dan di mana titik itu jatuh di dalamnya.
struct GiGrid {
    float4x4 inverse;
    /// x skala terkecil, y anisotropi, z pita grid, w slot tekstur
    float4 scaleBandSlot;
    /// xyz kotak batas dunia terkecil, w sisi voxel grid
    float4 boundsMinVoxel;
    float4 boundsMax;
    /// xyz pusat voxel (0,0,0) di ruang lokal
    float4 origin;
    /// xyz ukuran grid dalam voxel
    float4 size;
};

static const int kGiMaxGrids = 16;

[[vk::binding(22, 0)]] StructuredBuffer<GiGrid> giGrids;
/// Albedo per grid: rgb warna linear, alfa 1 berarti voxel itu punya material.
///
/// **Warna diambil dari grid per-mesh, bukan dari kaskade clipmap.** Kaskade
/// terkasar bervoxel 1,6 m; warna yang dirata-rata sejauh itu adalah warna yang
/// salah di mana-mana. Grid tetap 12,7 cm berapa pun jauhnya dari kamera.
[[vk::binding(23, 0)]] Texture3D<float4> giGridAlbedo[kGiMaxGrids];

/// LUT sky-view, langit yang sama dengan yang tergambar di belakang adegan.
/// Isinya radiansi atmosfer tanpa `skyIntensity`; pengalinya ada di
/// `shadowParams.skyParams.x`, sama seperti di pass gambar.
[[vk::binding(21, 0)]] Sampler2D giSkyView;

[[vk::binding(7, 0)]] Sampler3D sdfCascade0;
[[vk::binding(8, 0)]] Sampler3D sdfCascade1;
[[vk::binding(9, 0)]] Sampler3D sdfCascade2;
#include "gi_depth.slang"

float cascadeVoxelSize(int cascade) { return shadowParams.sdfOrigin[cascade].w; }

/// Kaskade terhalus yang memuat sebuah titik, atau -1.
int cascadeFor(float3 world) {
    int count = int(shadowParams.sdfParams.y);
    float resolution = shadowParams.sdfParams.x;
    for (int i = 0; i < count; ++i) {
        float3 origin = shadowParams.sdfOrigin[i].xyz;
        float voxel = shadowParams.sdfOrigin[i].w;
        float3 local = world - origin;
        float span = voxel * resolution;
        // Sedikit di dalam tepinya: sampel trilinear membaca voxel tetangga,
        // dan tetangga di tepi kaskade adalah milik sisi seberangnya.
        if (all(local >= float3(voxel)) &&
            all(local <= float3(span - voxel))) {
            return i;
        }
    }
    return -1;
}

float sampleCascadeSdf(int cascade, float3 world) {
    float voxel = shadowParams.sdfOrigin[cascade].w;
    float resolution = shadowParams.sdfParams.x;
    // Koordinat tekstur toroidal: dibagi resolusi lalu dibiarkan membungkus oleh
    // sampler REPEAT. Titik asalnya tidak dikurangkan — justru itu inti clipmap,
    // sebuah voxel dunia selalu memetakan ke texel yang sama.
    float3 uvw = (world / voxel - float3(0.5)) / resolution;
    // Cabang, bukan pemilihan lewat ternary: GLSL tidak mengizinkan sampler
    // sebagai nilai ekspresi. Larik sampler akan lebih rapi, tapi ia menuntut
    // indeks yang seragam di seluruh subgroup — dan indeks kaskade di sini
    // memang berbeda antar-piksel.
    float encoded;
    if (cascade == 0) {
        encoded = sdfCascade0.SampleLevel(uvw, 0).r;
    } else if (cascade == 1) {
        encoded = sdfCascade1.SampleLevel(uvw, 0).r;
    } else {
        encoded = sdfCascade2.SampleLevel(uvw, 0).r;
    }
    float band = voxel * shadowParams.sdfParams.z;
    return (encoded - 0.5) * band * 2.0;
}

struct SdfHit {
    bool hit;
    float distance;
    float3 position;
    int steps;
    /// Medan tidak punya jawaban untuk sinar ini — titik asalnya sudah berada
    /// di ambang. **Berbeda dari "tidak mengenai apa-apa"**, dan bedanya
    /// sebesar seluruh langit.
    bool unknown;
};

SdfHit traceSdf(float3 origin, float3 direction, float tMax) {
    SdfHit result;
    result.hit = false;
    result.distance = 0.0;
    result.position = origin;
    result.steps = 0;
    result.unknown = false;

    int maxSteps = int(shadowParams.sdfParams.w);

    // **Sinar mengabaikan permukaannya sendiri sepanjang satu voxel pertama.**
    // Titik asal probe sudah digeser satu voxel sepanjang normalnya, dan itu
    // tetap tidak cukup: permukaan nol medan jarak sendiri meleset sampai
    // setengah voxel grid mesh-nya ditambah setengah voxel kaskadenya. Terukur
    // di lantai serambi Sponza — medan di titik asal membaca 0,49 voxel,
    // sementara ambang "mengenai" 0,5. Setiap sinar dari probe itu lalu
    // mengenai sesuatu di langkah pertama, ke arah mana pun, dan probe yang
    // seluruh sinarnya begitu tidak punya apa pun untuk ditaksir: 91% bobot
    // probe lantai depan datang dari probe mati seperti itu.
    //
    // Melewatinya, bukan membuang sinarnya: sinar yang mengarah menjauh
    // memang harus melanjutkan perjalanannya, dan yang mengarah ke dalam
    // mengenai permukaannya satu voxel kemudian. Yang hilang oklusi pada skala
    // satu voxel — kontak — dan itu jauh lebih murah daripada probe yang mati.
    int firstCascade = cascadeFor(origin);
    float selfSkip = firstCascade >= 0 ? cascadeVoxelSize(firstCascade) : 0.0;

    float travelled = 0.0;
    for (int step = 0; step < maxSteps; ++step) {
        float3 position = origin + direction * travelled;
        int cascade = cascadeFor(position);
        if (cascade < 0) {
            result.steps = step;
            return result;
        }
        float distance = sampleCascadeSdf(cascade, position);
        float voxel = cascadeVoxelSize(cascade);
        // **Titik asal yang sudah berada di dalam medan tidak bisa dilayani
        // medan itu.** Sphere tracing menjawab "berapa jauh sampai permukaan"
        // dari luar; dari dalam, jawabannya nol di langkah pertama ke arah mana
        // pun — termasuk arah yang lurus keluar. Yang tampak: sebuah permukaan
        // di jarak nol pada keenam belas sinar sebuah probe.
        //
        // Ini bukan kemungkinan teoretis melainkan keadaan Sponza. Medan
        // adegan menyusun tiap mesh sebagai **kotak batasnya** (`BoxSceneField`),
        // dan Sponza satu mesh berkotak 36×20×24 m: seluruh gedung, halaman
        // dan serambinya, berada *di dalam* satu kotak pejal. 84% voxel kaskade
        // terhalus terbaca "jauh di dalam benda". Setiap sinar probe lalu
        // mengenai sesuatu di jarak nol, tidak satu pun sampai ke langit, dan
        // yang menyinari adegan tinggal tebakan atas permukaan yang tidak ada.
        //
        // Melewatkan lapis ini mengembalikan sinarnya ke langit — jawaban yang
        // benar untuk "medan ini tidak tahu", dan yang salah hanya kalau
        // medannya memang tahu. Oklusi yang hilang sebagiannya masih dijawab
        // lapis layar. Yang mengembalikannya seluruhnya adalah medan jarak yang
        // benar-benar mengikuti segitiga, dan itu M1 di rencana GI.
        //
        // **Yang benar-benar di dalam benda tetap tidak punya jawaban.** Sphere
        // tracing menjawab "berapa jauh sampai permukaan" dari luar; dari
        // dalam, jawabannya nol ke arah mana pun. Itu berbeda dari titik yang
        // hanya tersenggol ambang, yang dijawab `selfSkip` di atas.
        if (step == 0 && distance < 0.0) {
            result.steps = 1;
            result.unknown = true;
            return result;
        }
        if (distance < voxel * 0.5 && travelled >= selfSkip) {
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
    float3 position;
    /// Tempat perpotongannya di layar. Dipakai membaca radiansi permukaan yang
    /// dikenai dari buffer warna yang sudah tersinari.
    float2 uv;
    int steps;
};

/// Ukuran sepetak yang sah pada sebuah tingkat, dengan pembagian dibulatkan ke
/// bawah — aturan mip Vulkan, sama dengan `HiZPyramid` di sisi C++.
int2 hizLevelSize(int level) {
    int2 size = int2(shadowParams.viewportSize.xy);
    for (int i = 0; i < level; ++i) {
        size = max(int2(1), size / 2);
    }
    return size;
}

float hizSample(int level, float2 uv, int2 size) {
    int2 texel = clamp(int2(floor(uv * float2(size))), int2(0), size - int2(1));
    return hiZ.Load(int3(texel, level)).r;
}

float3 unproject(float2 uv, float depth) {
    float4 point = mul(shadowParams.invViewProj, float4(uv * 2.0 - 1.0, depth, 1.0));
    return point.xyz / point.w;
}

/// Parameter saat sinar meninggalkan sel yang memuatnya pada tingkat ini.
float nextCellBoundary(float2 uv0, float2 duv, int2 size, float s) {
    float2 scale = float2(size);
    float2 point = (uv0 + duv * s) * scale;
    float2 cell = floor(point);

    float next = 1e30;
    for (int axis = 0; axis < 2; ++axis) {
        float advance = duv[axis] * scale[axis];
        if (abs(advance) < 1e-9) {
            continue;
        }
        float edge = advance > 0.0 ? cell[axis] + 1.0 : cell[axis];
        next = min(next, (edge - uv0[axis] * scale[axis]) / advance);
    }
    if (next > 1e29) {
        return 1e30;
    }
    // Dorongan sepersekian sel: tanpa itu, pembulatan menaruh titik berikutnya
    // tepat di batas dan sel yang sama diuji lagi sampai anggaran habis.
    return next + (1.0 / max(length(duv * scale), 1e-9)) / 128.0;
}

ScreenHit traceScreen(float3 origin, float3 direction, float tMax) {
    ScreenHit result;
    result.hit = false;
    result.leftScreen = true;
    result.distance = 0.0;
    result.position = origin;
    result.uv = float2(0.0);
    result.steps = 0;

    int levelCount = int(shadowParams.screenTrace.w);
    if (levelCount <= 0) {
        return result;
    }
    float thickness = shadowParams.screenTrace.x;
    float bias = min(shadowParams.screenTrace.y, tMax * 0.5);
    int maxSteps = int(shadowParams.screenTrace.z);

    float4 clipStart = mul(shadowParams.viewProj, float4(origin + direction * bias, 1.0));
    float4 clipEnd = mul(shadowParams.viewProj, float4(origin + direction * tMax, 1.0));
    // Titik dengan w tidak positif tidak punya proyeksi yang berarti: koordinat
    // layarnya adalah pantulan di seberang layar, dan penelusur yang memakainya
    // menelusuri tempat yang salah dengan penuh keyakinan.
    const float minW = 1e-4;
    if (clipStart.w < minW && clipEnd.w < minW) {
        return result;
    }
    if (clipStart.w < minW || clipEnd.w < minW) {
        float t = (minW - clipStart.w) / (clipEnd.w - clipStart.w);
        float4 crossing = clipStart + (clipEnd - clipStart) * t;
        if (clipStart.w >= minW) {
            clipEnd = crossing;
        } else {
            clipStart = crossing;
        }
    }

    float3 ndcStart = clipStart.xyz / clipStart.w;
    float3 ndcEnd = clipEnd.xyz / clipEnd.w;
    float2 uv0 = ndcStart.xy * 0.5 + 0.5;
    float2 duv = (ndcEnd.xy * 0.5 + 0.5) - uv0;
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
        int2 size = hizLevelSize(level);
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
                float2 entryUv = uv0 + duv * s;
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


/// Normal dari gradien medan jarak, beda hingga tengah.
///
/// **`fallback` bukan kerapian melainkan penangkal NaN.** Gradien bisa tepat
/// nol: di dalam pita yang terjepit ke nilai maksimumnya, di luar kaskade, dan
/// di sudut tempat dua permukaan bertemu. `normalize` atas vektor nol
/// menghasilkan NaN, dan NaN tidak berhenti di piksel tempat ia lahir — satu
/// sinar saja meracuni seluruh SH sebuah probe, karena koefisiennya
/// dijumlahkan. Yang terlihat: sepetak layar yang gelap gulita padahal
/// tetangganya baik-baik saja, dan sumbernya sebuah pembagian dengan nol
/// beberapa lapis di bawahnya. Ditemukan begitu — serambi Sponza hitam pekat
/// sementara `float3(0.5)` di tempat yang sama tampil rata.
static float3 sdfSurfaceNormal(int cascade, float3 position, float3 fallback) {
    float h = cascadeVoxelSize(cascade) * 0.5;
    float3 gradient = float3(
        sampleCascadeSdf(cascade, position + float3(h, 0, 0)) -
            sampleCascadeSdf(cascade, position - float3(h, 0, 0)),
        sampleCascadeSdf(cascade, position + float3(0, h, 0)) -
            sampleCascadeSdf(cascade, position - float3(0, h, 0)),
        sampleCascadeSdf(cascade, position + float3(0, 0, h)) -
            sampleCascadeSdf(cascade, position - float3(0, 0, h)));
    float length2 = dot(gradient, gradient);
    return length2 > 1e-12 ? gradient * rsqrt(length2) : fallback;
}

static const int kLayerNone = 0;
static const int kLayerScreen = 1;
static const int kLayerSdf = 2;
static const int kLayerSky = 3;

/// Hasil satu penelusuran berjenjang.
struct TieredHit {
    bool hit;
    int layer;
    /// Tidak ada lapis yang punya jawaban. Yang menerimanya membuang sinar ini
    /// dari penaksir alih-alih menghitungnya sebagai langit.
    bool unknown;
    float distance;
    float3 position;
    float2 uv;      // hanya berarti untuk lapis layar
    int steps;
};

TieredHit traceTiered(float3 origin, float3 direction, float tMax) {
    TieredHit result;
    ScreenHit screen = traceScreen(origin, direction, tMax);
    result.unknown = false;
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
    // **"Medan tidak tahu" bukan "sinar sampai ke langit".** Keduanya sama-sama
    // "tidak mengenai apa-apa" dari luar, dan menyamakannya berarti sebuah
    // probe yang titik asalnya terjepit di permukaan menerima langit penuh dari
    // keenam belas sinarnya — iradiansi terbuka lebar untuk sebuah titik yang
    // justru sedang menempel di dinding. Ditemukan lewat tampilan ketampakan
    // langit: lantai serambi terbaca 100% melihat langit, sementara satu
    // tangkapan dari lantai yang sama menghadap ke atas memperlihatkan lubang
    // langit yang hanya sepersepuluh bidang pandangnya.
    result.unknown = sdf.unknown;
    result.layer = sdf.hit ? kLayerSdf : kLayerSky;
    result.distance = sdf.distance;
    result.position = sdf.position;
    result.uv = float2(0.0);
    // Langkah kedua lapis dijumlahkan: yang diawasi anggaran adalah biaya satu
    // sinar, dan sinar yang gagal di layar lalu berjalan penuh di SDF adalah
    // sinar yang paling mahal.
    result.steps = sdf.steps + screen.steps;
    return result;
}

/// Langit cadangan: gradien analitik.
///
/// **Nilainya menyalin nilai bawaan `GradientSky` di `Ibl.h`.** Ia dipakai
/// hanya ketika tidak ada atmosfer — langit dimatikan, atau sumbernya sebuah
/// HDRI yang belum terikat ke pass ini. Sampai E8.8 ia satu-satunya langit yang
/// dikenal GI, dan itulah cacatnya: adegan disinari langit yang bukan langit
/// yang tergambar di belakangnya.
float3 giSkyGradient(float3 direction) {
    const float3 zenith = float3(0.18, 0.32, 0.62);
    const float3 horizon = float3(0.62, 0.66, 0.72);
    const float3 ground = float3(0.14, 0.13, 0.12);
    float t = direction.y;
    return t >= 0.0 ? lerp(horizon, zenith, sqrt(t)) : lerp(horizon, ground, sqrt(-t));
}

/// Langit yang menyinari adegan, yaitu **langit yang sama dengan yang
/// tergambar**: LUT sky-view Bruneton, dicuplik dengan parameterisasi yang
/// sama persis dengan `sky_draw.frag`.
///
/// Satu cuplikan tekstur 192×108 per sinar yang lolos. Menelusuri ulang
/// atmosfernya di sini akan mengulang pekerjaan yang sudah dibayar frame ini,
/// enam belas kali per probe.
float3 giSky(float3 direction) {
    if (shadowParams.giParams.w > 0.5) {
        return float3(1.0);  // uji tungku: langit seragam berintensitas satu
    }
    float intensity = shadowParams.skyParams.x;
    if (intensity <= 0.0) {
        return giSkyGradient(direction);
    }

    // Kerangka planet: +Z ke atas dari pusat planet, sementara sumbu atas
    // adegan +Y. Ditukar di sini dan hanya di sini — sama seperti pass gambar.
    float viewHeight = kBottomRadius + max(shadowParams.skyParams.y, kRadiusOffset);
    float3 up = float3(0.0, 0.0, 1.0);
    float3 worldDirection = float3(direction.x, -direction.z, direction.y);
    float3 sunDirection = float3(shadowParams.lightDirection.x, -shadowParams.lightDirection.z,
                                 shadowParams.lightDirection.y);

    float viewZenithCos = dot(worldDirection, up);
    float viewZenithAngle = acos(clamp(viewZenithCos, -1.0, 1.0));
    float3 sideDirection = normalize(worldDirection - up * viewZenithCos);
    float3 sunSide = normalize(sunDirection - up * dot(sunDirection, up));
    float lightViewAngle = acos(clamp(dot(sideDirection, sunSide), -1.0, 1.0));

    float3 origin = float3(0.0, 0.0, viewHeight);
    bool intersectsGround = raySphereNearest(origin, worldDirection, kBottomRadius) >= 0.0;
    float2 lutUv = skyViewParamsToUv(float2(viewZenithAngle, lightViewAngle),
                                     shadowParams.skyParams.zw, viewHeight, intersectsGround);
    // **Tanpa cakram matahari.** Pass gambar menambahkannya supaya matahari
    // terlihat; menambahkannya di sini akan menghitung cahaya matahari dua
    // kali — sinar langsungnya sudah dinilai lampu directional, dan sebuah
    // sinar probe yang kebetulan mengarah ke cakramnya akan menyumbang
    // radiansi ribuan kali lipat tetangganya, yang terlihat sebagai bintik
    // terang yang berpindah tiap frame.
    return giSkyView.SampleLevel(lutUv, 0.0).rgb * intensity;
}

/// Radiansi keluar yang **ditebak** untuk sebuah permukaan clipmap SDF yang
/// belum dikenal cache radiansi.
///
/// **Penutup rekursi, dan tanpanya ruang tertutup hitam pekat.** Cache hanya
/// mengenal permukaan yang pernah terlihat layar; di dalam serambi, hampir
/// setiap sinar probe mengenai batu yang tidak pernah masuk layar. Membuang
/// semuanya berarti tidak ada satu pun sinar yang diketahui, dan iradiansinya
/// nol — bukan gelap, melainkan hitam.
///
/// Yang ditaksir di sini satu pantulan penuh: matahari lewat peta bayangan yang
/// sama dengan yang dipakai shading, ditambah langit. Albedo-nya tebakan
/// (`kBounceAlbedo` di renderer); yang bergantung padanya kecerahan pantulan,
/// bukan ada-tidaknya.
/// Albedo sebuah titik dunia, dibaca dari grid yang memuatnya.
///
/// Mengembalikan tebakan tetap `giBounce.rgb` bila tidak ada grid yang memuat
/// titiknya, atau bila voxelnya tidak punya material — di luar pita, tempat
/// tidak ada permukaan yang bisa ditanyai warnanya.
///
/// **Tetangga terdekat, bukan trilinear.** Warna permukaan tidak berpadu: batu
/// bertemu kain di sebuah tepi, bukan di sebuah gradien. Memadukannya
/// menghasilkan sepetak warna yang tidak dimiliki bahan mana pun, tepat di
/// tempat yang paling diperhatikan mata.
float3 giAlbedoAt(float3 world) {
    if (shadowParams.giParams.w > 0.5) {
        return float3(1.0);  // uji tungku: albedo satu di mana-mana
    }
    int count = min(int(shadowParams.giParams.z), kGiMaxGrids);
    for (int g = 0; g < count; ++g) {
        GiGrid grid = giGrids[g];
        if (any(world < grid.boundsMinVoxel.xyz) || any(world > grid.boundsMax.xyz)) {
            continue;
        }
        float3 local = mul(grid.inverse, float4(world, 1.0)).xyz;
        int3 texel = int3(round((local - grid.origin.xyz) / grid.boundsMinVoxel.w));
        if (any(texel < int3(0)) || any(texel >= int3(grid.size.xyz))) {
            continue;
        }
        int slot = int(grid.scaleBandSlot.w);
        float4 albedo = giGridAlbedo[NonUniformResourceIndex(slot)].Load(int4(texel, 0));
        if (albedo.a < 0.5) {
            continue;
        }
        return albedo.rgb;
    }
    return shadowParams.giBounce.rgb;
}

/// `localIrradiance` adalah iradiansi yang diterima probe yang menembakkan sinar
/// ini, dari frame sebelumnya.
///
/// **Separuh bola yang bukan langit bukan kegelapan — ia permukaan lain.**
/// Bentuk sebelumnya berhenti pada `albedo * langit * 0,5`, dan itu gagal uji
/// tungku yang menjadi kriteria selesai M4: di bawah cahaya seragam
/// berintensitas satu dengan albedo satu, setiap permukaan seharusnya
/// berradiansi satu, dan rumus itu menjawab setengah. Yang hilang persis
/// separuh yang lain — permukaan tetangga, yang di bawah tungku juga
/// berradiansi satu.
///
/// Taksiran radiansi tetangga: albedo dikali iradiansi probe ini sendiri dibagi
/// π. Ia menutup rekursinya lewat suku yang **bertambah tiap frame** —
/// pantulan kedua, ketiga, dan seterusnya masuk sebagai deret yang konvergen
/// karena albedo selalu di bawah satu. Di bawah tungku titik tetapnya tepat
/// satu.
float3 giGuessedBounce(float3 position, float3 normal, float3 localIrradiance) {
    float3 albedo = giAlbedoAt(position);
    float ndotl = max(dot(normal, shadowParams.lightDirection.xyz), 0.0);
    float sun = ndotl > 0.0 ? sampleShadow(position, normal) * ndotl : 0.0;
    if (shadowParams.giParams.w > 0.5) {
        sun = 0.0;  // uji tungku: tidak ada matahari, hanya cahaya seragam
    }

    // Berapa bagian bola yang dianggap langit. Sebuah permukaan yang menempel
    // di dinding paling banyak melihat separuh bola; sisanya adegan.
    const float kSkyClosure = 0.5;
    float3 nearby = albedo * max(localIrradiance, float3(0.0)) / kPi;
    return albedo * (shadowParams.sunRadiance.rgb * (sun / kPi) +
                     giSky(normal) * kSkyClosure + nearby * (1.0 - kSkyClosure));
}

#endif
