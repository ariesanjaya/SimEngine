// Keadaan bayangan per frame, dipakai bersama pass forward.
//
// **Tata letaknya ABI dengan `ShadowUniforms` di VulkanRenderer.cpp.** Selisih
// satu sisipan tidak menghasilkan galat apa pun — hanya bayangan yang jatuh di
// tempat yang salah, yang mudah dikira masalah matriks cascade.
//
// Seluruh anggotanya berukuran dan berjajar kelipatan 16 byte: hanya `float4`,
// `float4x4`, dan larik keduanya. **Itu bukan kebetulan melainkan yang membuat
// ABI ini selamat dari porting GLSL ke Slang** — std140, std430, dan tata letak
// skalar sepakat persis pada tipe-tipe itu, jadi tidak ada aturan tata letak
// yang bisa menggeser satu medan pun. Menambahkan `float3` atau skalar tunggal
// di sini akan mematahkan sifat itu, dan `static_assert` di sisi C++ tidak bisa
// melihatnya.

#ifndef SHADOW_COMMON_SLANG
#define SHADOW_COMMON_SLANG

static const int kMaxCascades = 4;

struct ShadowParams {
    float4x4 cascadeViewProj[kMaxCascades];
    float4 cascadeSplitFar;    // batas jauh tiap cascade, ruang pandang
    float4 cascadeBlendBegin;  // awal pita campur tiap cascade
    float4 cascadeTexelSize;   // ukuran dunia satu texel tiap cascade
    float4 lightDirection;     // xyz: dari permukaan ke cahaya, w: jumlah cascade
    float4 cameraPosition;     // xyz: posisi, w: 1 kalau bayangan menyala
    float4 sunRadiance;        // rgb: warna * intensitas * eksposur
    float4 cameraForward;      // xyz: arah pandang, w: kekuatan bias normal
    float4 clusterCounts;      // xyz: tilesX, tilesY, slices; w: jumlah lampu
    float4 clusterDepth;       // x: skala irisan, y: bias irisan, z: near, w: far
    float4 viewportSize;       // xy: piksel
    // Kaskade SDF clipmap. xyz titik asal dunia, w ukuran voxel.
    float4 sdfOrigin[4];
    // x resolusi, y jumlah kaskade, z lebar pita dalam voxel, w langkah maksimum
    float4 sdfParams;
    // Dunia -> clip. Dipakai lapis screen-space untuk memproyeksikan sinar ke
    // layar. Di UBO, bukan di push constant: batas push constant yang dijamin
    // Vulkan hanya 128 byte, dan dua matriks saja sudah menghabiskannya.
    float4x4 viewProj;
    // Clip -> dunia. Dipakai penelusur screen-space memulihkan posisi dunia dari
    // sebuah texel depth, dan itulah yang membuat uji ketebalan bisa diukur
    // dalam meter. Di UBO supaya penelusurnya tidak bergantung pada push
    // constant pemakainya — dan ia dipakai lebih dari satu pass.
    float4x4 invViewProj;
    // x ketebalan yang diandaikan (meter), y dorongan awal (meter),
    // z langkah maksimum screen-space, w jumlah tingkat HiZ. Nol pada w berarti
    // lapis screen-space mati.
    float4 screenTrace;
    // x kapasitas cache (pangkat dua), y ukuran sel terhalus, z jarak LOD,
    // w langkah probing maksimum. Kapasitas nol berarti cache mati.
    float4 cacheParams;
    // x frame akumulasi, y frame sebelum entri boleh direbut, z nomor frame.
    float4 cacheDecay;
    // Dunia -> clip frame SEBELUMNYA. Inilah yang mengikat riwayat probe ke
    // dunia alih-alih ke piksel.
    float4x4 previousViewProj;
    // x jendela akumulasi (frame), y jarak bidang untuk menolak riwayat (meter),
    // z kosinus normal minimum, w skala penjepitan riwayat.
    float4 denoise;
};

[[vk::binding(0, 0)]] ConstantBuffer<ShadowParams> shadowParams;
[[vk::binding(1, 0)]] Sampler2DArrayShadow shadowMap;

/// Cascade untuk sebuah kedalaman pandang, beserta bobot campur ke penerusnya.
///
/// Bentuknya harus sama dengan `ChooseCascade` di sisi C++ — keduanya diuji
/// terhadap kumpulan cascade yang sama, dan rumus yang berselisih menghasilkan
/// pita tempat CPU dan GPU tidak sepakat cascade mana yang berlaku.
int chooseCascade(float viewDepth, out float blendWeight) {
    int count = int(shadowParams.lightDirection.w);
    blendWeight = 0.0;
    for (int i = 0; i < count; ++i) {
        if (viewDepth <= shadowParams.cascadeSplitFar[i]) {
            if (i + 1 < count && viewDepth > shadowParams.cascadeBlendBegin[i]) {
                float span = shadowParams.cascadeSplitFar[i] - shadowParams.cascadeBlendBegin[i];
                blendWeight = span > 1e-6 ? (viewDepth - shadowParams.cascadeBlendBegin[i]) / span
                                          : 0.0;
            }
            return i;
        }
    }
    return count - 1;
}

/// Satu pengambilan bayangan dengan PCF 3x3.
float sampleCascade(int cascade, float3 worldPosition, float3 normal) {
    // **Bias normal, bukan bias depth konstan.** Bias depth konstan adalah angka
    // ajaib yang harus disetel ulang untuk tiap cascade — yang cukup di cascade
    // dekat menghasilkan peter-panning di cascade jauh, dan sebaliknya
    // menghasilkan acne. Menggeser titik sampel sepanjang normal sebanyak satu
    // texel dunia menyesuaikan diri sendiri terhadap resolusi tiap cascade.
    float texel = shadowParams.cascadeTexelSize[cascade];
    float3 offset = normal * (texel * shadowParams.cameraForward.w);
    float4 lightPosition =
        mul(shadowParams.cascadeViewProj[cascade], float4(worldPosition + offset, 1.0));

    float3 projected = lightPosition.xyz / lightPosition.w;
    float2 uv = projected.xy * 0.5 + 0.5;
    // Di luar peta berarti tidak ada yang menghalangi — bukan gelap. Menganggap
    // luar-peta sebagai bayangan membuat seluruh dunia di luar cascade terakhir
    // menjadi hitam.
    if (any(uv < float2(0.0)) || any(uv > float2(1.0)) || projected.z <= 0.0 ||
        projected.z >= 1.0) {
        return 1.0;
    }

    uint width, height, layers, levels;
    shadowMap.GetDimensions(0, width, height, layers, levels);
    float2 texelStep = 1.0 / float2(width, height);
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float2 tap = uv + float2(x, y) * texelStep;
            sum += shadowMap.SampleCmp(float3(tap, float(cascade)), projected.z);
        }
    }
    return sum / 9.0;
}

/// Faktor bayangan 0..1 untuk sebuah titik. 1 berarti tersinari penuh.
float sampleShadow(float3 worldPosition, float3 normal) {
    if (shadowParams.cameraPosition.w < 0.5) {
        return 1.0;
    }
    float viewDepth = dot(worldPosition - shadowParams.cameraPosition.xyz,
                          shadowParams.cameraForward.xyz);
    float blendWeight;
    int cascade = chooseCascade(viewDepth, blendWeight);

    float shadow = sampleCascade(cascade, worldPosition, normal);
    // Pita campur diambil dari kedua cascade, bukan dari salah satunya. Tanpa
    // ini perpindahan cascade adalah garis tajam tempat resolusi bayangan
    // berubah mendadak — dan garis itu bergerak bersama kamera, yang justru
    // membuatnya lebih terlihat daripada bayangan kasarnya sendiri.
    if (blendWeight > 0.0) {
        shadow = lerp(shadow, sampleCascade(cascade + 1, worldPosition, normal), blendWeight);
    }
    return shadow;
}

#endif
