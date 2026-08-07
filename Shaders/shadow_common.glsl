// Keadaan bayangan per frame, dipakai bersama pass forward.
//
// **Tata letaknya ABI dengan `ShadowUniforms` di VulkanRenderer.cpp.** std140,
// dan selisih satu sisipan tidak menghasilkan galat apa pun — hanya bayangan
// yang jatuh di tempat yang salah, yang mudah dikira masalah matriks cascade.

const int kMaxCascades = 4;

layout(set = 0, binding = 0) uniform ShadowParams {
    mat4 cascadeViewProj[kMaxCascades];
    vec4 cascadeSplitFar;    // batas jauh tiap cascade, ruang pandang
    vec4 cascadeBlendBegin;  // awal pita campur tiap cascade
    vec4 cascadeTexelSize;   // ukuran dunia satu texel tiap cascade
    vec4 lightDirection;     // xyz: dari permukaan ke cahaya, w: jumlah cascade
    vec4 cameraPosition;     // xyz: posisi, w: 1 kalau bayangan menyala
    vec4 sunRadiance;        // rgb: warna * intensitas * eksposur
    vec4 cameraForward;      // xyz: arah pandang, w: kekuatan bias normal
    vec4 clusterCounts;      // xyz: tilesX, tilesY, slices; w: jumlah lampu
    vec4 clusterDepth;       // x: skala irisan, y: bias irisan, z: near, w: far
    vec4 viewportSize;       // xy: piksel
} shadowParams;

layout(set = 0, binding = 1) uniform sampler2DArrayShadow shadowMap;

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
float sampleCascade(int cascade, vec3 worldPosition, vec3 normal) {
    // **Bias normal, bukan bias depth konstan.** Bias depth konstan adalah angka
    // ajaib yang harus disetel ulang untuk tiap cascade — yang cukup di cascade
    // dekat menghasilkan peter-panning di cascade jauh, dan sebaliknya
    // menghasilkan acne. Menggeser titik sampel sepanjang normal sebanyak satu
    // texel dunia menyesuaikan diri sendiri terhadap resolusi tiap cascade.
    float texel = shadowParams.cascadeTexelSize[cascade];
    vec3 offset = normal * (texel * shadowParams.cameraForward.w);
    vec4 lightPosition = shadowParams.cascadeViewProj[cascade] * vec4(worldPosition + offset, 1.0);

    vec3 projected = lightPosition.xyz / lightPosition.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    // Di luar peta berarti tidak ada yang menghalangi — bukan gelap. Menganggap
    // luar-peta sebagai bayangan membuat seluruh dunia di luar cascade terakhir
    // menjadi hitam.
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) ||
        projected.z <= 0.0 || projected.z >= 1.0) {
        return 1.0;
    }

    vec2 step = 1.0 / vec2(textureSize(shadowMap, 0).xy);
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 tap = uv + vec2(x, y) * step;
            sum += texture(shadowMap, vec4(tap, float(cascade), projected.z));
        }
    }
    return sum / 9.0;
}

/// Faktor bayangan 0..1 untuk sebuah titik. 1 berarti tersinari penuh.
float sampleShadow(vec3 worldPosition, vec3 normal) {
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
        shadow = mix(shadow, sampleCascade(cascade + 1, worldPosition, normal), blendWeight);
    }
    return shadow;
}
