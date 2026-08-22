// Penyaringan lampu berbasis cluster, sisi shader.
//
// **Rumus irisannya harus sama persis dengan `ClusterGrid` di sisi C++.**
// Keduanya menghitung irisan kedalaman yang sama, dan dua rumus yang setara
// secara matematis tapi ditulis berbeda akan berselisih satu irisan di tepinya —
// yang terlihat sebagai lampu yang hilang tepat pada jarak tertentu. Karena itu
// skala dan bias-nya dihitung CPU dan dikirim apa adanya, bukan diturunkan ulang
// di sini dari near dan far.

#ifndef CLUSTER_COMMON_SLANG
#define CLUSTER_COMMON_SLANG

#include "shadow_common.slang"

struct GpuLight {
    float4 positionInvRangeSq;  // xyz posisi, w 1/range^2
    float4 directionCosOuter;   // xyz arah pancar, w kosinus sudut luar
    float4 colorCosInner;       // rgb warna * intensitas, w kosinus sudut dalam
    // x: 0 point / 1 spot, y: jarak minimum kuadrat,
    // z: indeks entri atlas pertama (-1 tanpa bayangan), w: bias normal (dunia)
    float4 kind;
};

/// Satu muka bayangan di dalam atlas.
struct GpuShadowFace {
    float4x4 viewProjection;
    float4 tile;  // xy sudut kiri-atas dalam UV atlas, zw ukurannya
};

[[vk::binding(2, 0)]] StructuredBuffer<GpuLight> lightBuffer;
[[vk::binding(3, 0)]] StructuredBuffer<uint2> clusterRanges;
[[vk::binding(4, 0)]] StructuredBuffer<uint> clusterIndices;
[[vk::binding(5, 0)]] StructuredBuffer<GpuShadowFace> shadowFaces;
[[vk::binding(6, 0)]] Sampler2DShadow shadowAtlas;

/// Peredupan jarak dengan jendela.
///
/// **Kuadrat terbalik saja tidak pernah mencapai nol**, jadi setiap lampu akan
/// menerangi seluruh dunia dengan nilai yang sangat kecil — dan `range` tidak
/// akan berarti apa-apa selain kebohongan yang dipakai penyaringan cluster.
/// Jendelanya membuat jangkauan benar-benar berakhir tanpa tepi yang tajam,
/// dan nol tepat di `range` itulah yang membuat penyaringan bola berjari-jari
/// `range` bersifat eksak, bukan pendekatan.
///
/// **Jendelanya kuartik — `(1 - (d/r)^4)^2` — dan itu bentuk Frostbite, bukan
/// bentuk Unity.** Unity dan turunannya memakai `(1 - (d/r)^2)^2`, yang meredup
/// lebih awal: pada setengah jangkauan selisihnya 1,6x dan pada 0,7 jangkauan
/// 2,2x. Keduanya sah dan keduanya nol di `range`; yang tidak sah adalah
/// mencampur angka intensitas yang di-author untuk salah satunya ke yang lain.
/// Kalau suatu saat lampu terasa terlalu terang setelah diimpor dari Unity,
/// inilah sebabnya — dan menukar jendelanya bukan perbaikan melainkan pindah
/// konvensi, yang menuntut setiap lampu yang sudah ada disetel ulang.
///
/// `invRangeSq` dan `minDistanceSq` datang jadi dari CPU. Tidak ada kuadrat
/// maupun pembagian di sini yang bisa ditulis berbeda dari sisi C++.
float distanceAttenuation(float distanceSq, float invRangeSq, float minDistanceSq) {
    float factor = distanceSq * invRangeSq;
    float smoothFactor = clamp(1.0 - factor * factor, 0.0, 1.0);
    // Jarak dijepit ke jari-jari sumber: cahaya tidak pernah lebih dekat
    // daripada permukaan lampunya sendiri. Tanpa batas ini lampu yang tertanam
    // di dalam geometri — hal yang lazim — menghasilkan bercak putih hangus.
    return (smoothFactor * smoothFactor) / max(distanceSq, minDistanceSq);
}

/// Muka atlas untuk sebuah arah dari lampu ke permukaan.
///
/// Spot hanya punya satu muka. Point punya enam, dan yang dipilih adalah muka
/// yang sumbunya paling searah — sama dengan cara perangkat keras memilih muka
/// cubemap, hanya dilakukan tangan karena atlas bukan cubemap.
int shadowFaceOf(float3 fromLight, bool spot) {
    if (spot) {
        return 0;
    }
    float3 a = abs(fromLight);
    if (a.x >= a.y && a.x >= a.z) {
        return fromLight.x > 0.0 ? 0 : 1;
    }
    if (a.y >= a.z) {
        return fromLight.y > 0.0 ? 2 : 3;
    }
    return fromLight.z > 0.0 ? 4 : 5;
}

/// Faktor bayangan sebuah lampu punctual. 1 berarti tersinari penuh.
float sampleAtlasShadow(GpuLight light, float3 worldPosition, float3 normal, float3 fromLight) {
    int first = int(light.kind.z);
    if (first < 0) {
        return 1.0;
    }
    // **Bias normal, sama dengan cascade.** Di sini ia dalam satuan dunia dan
    // dihitung CPU dari ukuran ubin dan jangkauan lampu, karena ubin yang lebih
    // kecil menuntut pergeseran yang lebih besar — dan ukuran ubin berubah
    // setiap kali lampunya mendekat.
    float3 offset = normal * light.kind.w;
    int face = first + shadowFaceOf(fromLight, light.kind.x > 0.5);
    GpuShadowFace entry = shadowFaces[face];

    float4 clip = mul(entry.viewProjection, float4(worldPosition + offset, 1.0));
    if (clip.w <= 0.0) {
        return 1.0;
    }
    float3 projected = clip.xyz / clip.w;
    float2 local = projected.xy * 0.5 + 0.5;
    if (any(local < float2(0.0)) || any(local > float2(1.0)) || projected.z <= 0.0 ||
        projected.z >= 1.0) {
        // Di luar frustum muka ini berarti tidak ada yang menghalangi. Untuk
        // spot itu daerah di luar berkasnya, yang toh sudah nol karena kerucut.
        return 1.0;
    }

    // Dijepit ke dalam ubin dengan sisa setengah texel: PCF mengambil tetangga,
    // dan tetangga di tepi ubin adalah milik lampu lain — bayangan lampu
    // sebelah lalu bocor masuk sebagai garis di tepi berkas.
    uint width, height, levels;
    shadowAtlas.GetDimensions(0, width, height, levels);
    float2 texel = 1.0 / float2(width, height);
    float2 inset = texel * 0.5 / max(entry.tile.zw, float2(1e-6));
    local = clamp(local, inset, float2(1.0) - inset);
    float2 uv = entry.tile.xy + local * entry.tile.zw;

    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            sum += shadowAtlas.SampleCmp(uv + float2(x, y) * texel, projected.z);
        }
    }
    return sum / 9.0;
}

/// Apakah adegan ini punya sumber cahaya sama sekali.
///
/// **Dipakai memutuskan apakah suku ambient penambal berlaku.** Sebuah konstanta
/// ambient yang menyala tanpa satu pun lampu adalah cahaya yang tidak dimiliki
/// entity mana pun — tidak muncul di Outliner, tidak bisa disunting, dan tidak
/// bisa dimatikan. Yang mencarinya tidak akan menemukannya.
///
/// Matahari dibaca dari radiansinya, bukan dari arahnya: arah selalu ada (ia
/// dipakai cascade dan LUT langit apa pun keadaannya), sedangkan radiansi nol
/// berarti tidak ada directional di scene.
bool anyLightInScene() {
    return dot(shadowParams.sunRadiance.rgb, float3(1.0, 1.0, 1.0)) > 0.0 ||
           shadowParams.clusterCounts.w > 0.5;
}

/// Cluster untuk sebuah fragmen. `viewDepth` jarak sepanjang sumbu pandang.
uint clusterOf(float2 fragCoord, float viewDepth) {
    uint3 counts = uint3(shadowParams.clusterCounts.xyz);
    float2 tile = fragCoord / max(shadowParams.viewportSize.xy, float2(1.0));
    uint2 xy = uint2(clamp(tile, float2(0.0), float2(0.999)) * float2(counts.xy));

    float slice = log(max(viewDepth, shadowParams.clusterDepth.z)) *
                      shadowParams.clusterDepth.x + shadowParams.clusterDepth.y;
    uint z = uint(clamp(slice, 0.0, float(counts.z) - 1.0));
    return (z * counts.y + xy.y) * counts.x + xy.x;
}

/// Menjumlahkan lampu punctual yang mengenai sebuah fragmen.
///
/// **Yang dikembalikan iradiansi E(x), bukan radiance.** Ia sudah dikalikan
/// `n·l` tapi belum dibagi pi dan belum dikalikan albedo — pemakainya yang
/// berutang keduanya. Membaginya di sini akan membuat nama fungsinya berbohong,
/// dan tidak membaginya di pemakai menghasilkan difus yang terlalu terang
/// sebesar faktor pi.
///
/// Itu bukan kemungkinan yang jauh: `evaluateOpenPBR_IBL` di openpbr.slang
/// **membagi pi**, sedangkan `box.frag.slang` di bawah tidak — karena seluruh
/// ekspresi `box.frag.slang` memang ad-hoc dan bukan model shading. Begitu jalur
/// punctual ini pindah ke openpbr.slang, skala intensitasnya bergeser sebesar
/// pi dan setiap lampu yang sudah disetel harus disetel ulang. Yang membuat
/// kesalahan pi mahal adalah ia tidak pernah terlihat salah; yang
/// menghentikannya adalah menuliskan satuannya di tempat orang membacanya.
///
/// Difus saja. Ini shader sementara untuk geometri kotak — model shading
/// sungguhnya OpenPBR lewat pipeline material, dan menulis pendekatan kedua di
/// sini melanggar aturan yang dipegang seluruh E7.1.
/// Satu lampu cluster yang sudah lolos seluruh penolakan, siap dinilai BRDF.
///
/// **`radiance` belum dikalikan n·l**, dan itu disengaja: model shading yang
/// mengambilnya — `evaluateOpenPBR` — sudah menghitung sukunya sendiri, dan
/// mengalikannya dua kali menggelapkan setiap lampu titik tanpa satu pun tanda.
/// Yang memakai jalur ad-hoc di `box.frag` mengalikannya sendiri, sekali.
struct ClusterLightSample {
    /// Dari permukaan ke lampu, sudah dinormalkan.
    float3 direction;
    /// Warna x intensitas, sudah diredam jarak, kerucut, dan bayangan.
    float3 radiance;
};

/// Rentang indeks lampu untuk cluster sebuah fragmen.
uint2 clusterLightRange(float2 fragCoord, float viewDepth) {
    return clusterRanges[clusterOf(fragCoord, viewDepth)];
}

/// Lampu ke-`i` di dalam sebuah rentang, atau false bila ia tidak menyumbang.
///
/// **Satu tempat untuk peredupan, kerucut, dan bayangan.** Dua pemakainya —
/// jalur ad-hoc `box.frag` dan pipeline material — memanggil yang sama persis,
/// jadi lampu tidak bisa meredup berbeda tergantung shader mana yang menggambar.
bool clusterLightAt(uint2 range, uint i, float3 worldPosition, float3 normal,
                    out ClusterLightSample result) {
    result.direction = float3(0.0, 1.0, 0.0);
    result.radiance = float3(0.0);

    GpuLight light = lightBuffer[clusterIndices[range.x + i]];
    float3 toLight = light.positionInvRangeSq.xyz - worldPosition;
    float distanceSq = dot(toLight, toLight);
    float attenuation =
        distanceAttenuation(distanceSq, light.positionInvRangeSq.w, light.kind.y);
    if (attenuation <= 0.0) {
        return false;
    }
    float3 direction = toLight * rsqrt(max(distanceSq, 1e-8));
    // Penolakan pakai normal geometri, bukan normal peta: yang di belakang
    // permukaan tetap di belakang, dan normal map tidak boleh membuat sebuah
    // lampu menembus dinding.
    if (dot(normal, direction) <= 0.0) {
        return false;
    }
    if (light.kind.x > 0.5) {
        // Kerucut spot: memudar antara kosinus sudut luar dan sudut dalam.
        float cosAngle = dot(-direction, light.directionCosOuter.xyz);
        float cone = clamp((cosAngle - light.directionCosOuter.w) /
                               max(light.colorCosInner.w - light.directionCosOuter.w, 1e-4),
                           0.0, 1.0);
        attenuation *= cone * cone;
    }
    // Bayangan diambil terakhir: ia yang paling mahal, dan setiap penolakan di
    // atas menghematnya utuh.
    attenuation *= sampleAtlasShadow(light, worldPosition, normal, -toLight);

    result.direction = direction;
    result.radiance = light.colorCosInner.rgb * attenuation;
    return true;
}

/// Iradiansi E dari seluruh lampu cluster. Dipakai jalur ad-hoc `box.frag`.
float3 accumulateClusteredLights(float2 fragCoord, float3 worldPosition, float3 normal,
                                 float viewDepth) {
    uint2 range = clusterLightRange(fragCoord, viewDepth);
    float3 sum = float3(0.0);
    for (uint i = 0u; i < range.y; ++i) {
        ClusterLightSample light;
        if (!clusterLightAt(range, i, worldPosition, normal, light)) {
            continue;
        }
        sum += light.radiance * max(dot(normal, light.direction), 0.0);
    }
    return sum;
}

#endif
