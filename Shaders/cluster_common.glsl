// Penyaringan lampu berbasis cluster, sisi shader.
//
// **Rumus irisannya harus sama persis dengan `ClusterGrid` di sisi C++.**
// Keduanya menghitung irisan kedalaman yang sama, dan dua rumus yang setara
// secara matematis tapi ditulis berbeda akan berselisih satu irisan di tepinya —
// yang terlihat sebagai lampu yang hilang tepat pada jarak tertentu. Karena itu
// skala dan bias-nya dihitung CPU dan dikirim apa adanya, bukan diturunkan ulang
// di sini dari near dan far.

struct GpuLight {
    vec4 positionRange;      // xyz posisi, w jangkauan
    vec4 directionCosOuter;  // xyz arah pancar, w kosinus sudut luar
    vec4 colorCosInner;      // rgb warna * intensitas, w kosinus sudut dalam
    vec4 kind;               // x: 0 point, 1 spot
};

layout(set = 0, binding = 2) readonly buffer Lights {
    GpuLight lights[];
} lightBuffer;

layout(set = 0, binding = 3) readonly buffer ClusterRanges {
    uvec2 ranges[];
} clusterRanges;

layout(set = 0, binding = 4) readonly buffer ClusterIndices {
    uint indices[];
} clusterIndices;

/// Peredupan jarak dengan jendela.
///
/// **Kuadrat terbalik saja tidak pernah mencapai nol**, jadi setiap lampu akan
/// menerangi seluruh dunia dengan nilai yang sangat kecil — dan `range` tidak
/// akan berarti apa-apa selain kebohongan yang dipakai penyaringan cluster.
/// Jendelanya membuat jangkauan benar-benar berakhir tanpa tepi yang tajam.
float distanceAttenuation(float distanceSq, float range) {
    float factor = distanceSq / max(range * range, 1e-6);
    float smoothFactor = clamp(1.0 - factor * factor, 0.0, 1.0);
    return (smoothFactor * smoothFactor) / max(distanceSq, 1e-4);
}

/// Cluster untuk sebuah fragmen. `viewDepth` jarak sepanjang sumbu pandang.
uint clusterOf(vec2 fragCoord, float viewDepth) {
    uvec3 counts = uvec3(shadowParams.clusterCounts.xyz);
    vec2 tile = fragCoord / max(shadowParams.viewportSize.xy, vec2(1.0));
    uvec2 xy = uvec2(clamp(tile, vec2(0.0), vec2(0.999)) * vec2(counts.xy));

    float slice = log(max(viewDepth, shadowParams.clusterDepth.z)) *
                      shadowParams.clusterDepth.x + shadowParams.clusterDepth.y;
    uint z = uint(clamp(slice, 0.0, float(counts.z) - 1.0));
    return (z * counts.y + xy.y) * counts.x + xy.x;
}

/// Menjumlahkan lampu punctual yang mengenai sebuah fragmen.
///
/// Difus saja. Ini shader sementara untuk geometri kotak — model shading
/// sungguhnya OpenPBR lewat pipeline material, dan menulis pendekatan kedua di
/// sini melanggar aturan yang dipegang seluruh E7.1.
vec3 accumulateClusteredLights(vec2 fragCoord, vec3 worldPosition, vec3 normal, float viewDepth) {
    uint cluster = clusterOf(fragCoord, viewDepth);
    uvec2 range = clusterRanges.ranges[cluster];

    vec3 sum = vec3(0.0);
    for (uint i = 0u; i < range.y; ++i) {
        GpuLight light = lightBuffer.lights[clusterIndices.indices[range.x + i]];
        vec3 toLight = light.positionRange.xyz - worldPosition;
        float distanceSq = dot(toLight, toLight);
        float attenuation = distanceAttenuation(distanceSq, light.positionRange.w);
        if (attenuation <= 0.0) {
            continue;
        }
        vec3 direction = toLight * inversesqrt(max(distanceSq, 1e-8));
        float ndotl = max(dot(normal, direction), 0.0);
        if (ndotl <= 0.0) {
            continue;
        }
        if (light.kind.x > 0.5) {
            // Kerucut spot: memudar antara kosinus sudut luar dan sudut dalam.
            float cosAngle = dot(-direction, light.directionCosOuter.xyz);
            float cone = clamp((cosAngle - light.directionCosOuter.w) /
                                   max(light.colorCosInner.w - light.directionCosOuter.w, 1e-4),
                               0.0, 1.0);
            attenuation *= cone * cone;
        }
        sum += light.colorCosInner.rgb * (ndotl * attenuation);
    }
    return sum;
}
