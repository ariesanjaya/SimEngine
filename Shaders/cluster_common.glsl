// Penyaringan lampu berbasis cluster, sisi shader.
//
// **Rumus irisannya harus sama persis dengan `ClusterGrid` di sisi C++.**
// Keduanya menghitung irisan kedalaman yang sama, dan dua rumus yang setara
// secara matematis tapi ditulis berbeda akan berselisih satu irisan di tepinya —
// yang terlihat sebagai lampu yang hilang tepat pada jarak tertentu. Karena itu
// skala dan bias-nya dihitung CPU dan dikirim apa adanya, bukan diturunkan ulang
// di sini dari near dan far.

struct GpuLight {
    vec4 positionInvRangeSq;  // xyz posisi, w 1/range^2
    vec4 directionCosOuter;   // xyz arah pancar, w kosinus sudut luar
    vec4 colorCosInner;       // rgb warna * intensitas, w kosinus sudut dalam
    // x: 0 point / 1 spot, y: jarak minimum kuadrat,
    // z: indeks entri atlas pertama (-1 tanpa bayangan), w: bias normal (dunia)
    vec4 kind;
};

/// Satu muka bayangan di dalam atlas.
struct GpuShadowFace {
    mat4 viewProjection;
    vec4 tile;  // xy sudut kiri-atas dalam UV atlas, zw ukurannya
};

layout(set = 0, binding = 5) readonly buffer ShadowFaces {
    GpuShadowFace faces[];
} shadowFaces;

layout(set = 0, binding = 6) uniform sampler2DShadow shadowAtlas;

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
int shadowFaceOf(vec3 fromLight, bool spot) {
    if (spot) {
        return 0;
    }
    vec3 a = abs(fromLight);
    if (a.x >= a.y && a.x >= a.z) {
        return fromLight.x > 0.0 ? 0 : 1;
    }
    if (a.y >= a.z) {
        return fromLight.y > 0.0 ? 2 : 3;
    }
    return fromLight.z > 0.0 ? 4 : 5;
}

/// Faktor bayangan sebuah lampu punctual. 1 berarti tersinari penuh.
float sampleAtlasShadow(GpuLight light, vec3 worldPosition, vec3 normal, vec3 fromLight) {
    int first = int(light.kind.z);
    if (first < 0) {
        return 1.0;
    }
    // **Bias normal, sama dengan cascade.** Di sini ia dalam satuan dunia dan
    // dihitung CPU dari ukuran ubin dan jangkauan lampu, karena ubin yang lebih
    // kecil menuntut pergeseran yang lebih besar — dan ukuran ubin berubah
    // setiap kali lampunya mendekat.
    vec3 offset = normal * light.kind.w;
    int face = first + shadowFaceOf(fromLight, light.kind.x > 0.5);
    GpuShadowFace entry = shadowFaces.faces[face];

    vec4 clip = entry.viewProjection * vec4(worldPosition + offset, 1.0);
    if (clip.w <= 0.0) {
        return 1.0;
    }
    vec3 projected = clip.xyz / clip.w;
    vec2 local = projected.xy * 0.5 + 0.5;
    if (any(lessThan(local, vec2(0.0))) || any(greaterThan(local, vec2(1.0))) ||
        projected.z <= 0.0 || projected.z >= 1.0) {
        // Di luar frustum muka ini berarti tidak ada yang menghalangi. Untuk
        // spot itu daerah di luar berkasnya, yang toh sudah nol karena kerucut.
        return 1.0;
    }

    // Dijepit ke dalam ubin dengan sisa setengah texel: PCF mengambil tetangga,
    // dan tetangga di tepi ubin adalah milik lampu lain — bayangan lampu
    // sebelah lalu bocor masuk sebagai garis di tepi berkas.
    vec2 texel = 1.0 / vec2(textureSize(shadowAtlas, 0));
    vec2 inset = texel * 0.5 / max(entry.tile.zw, vec2(1e-6));
    local = clamp(local, inset, vec2(1.0) - inset);
    vec2 uv = entry.tile.xy + local * entry.tile.zw;

    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            sum += texture(shadowAtlas, vec3(uv + vec2(x, y) * texel, projected.z));
        }
    }
    return sum / 9.0;
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
/// **Yang dikembalikan iradiansi E(x), bukan radiance.** Ia sudah dikalikan
/// `n·l` tapi belum dibagi pi dan belum dikalikan albedo — pemakainya yang
/// berutang keduanya. Membaginya di sini akan membuat nama fungsinya berbohong,
/// dan tidak membaginya di pemakai menghasilkan difus yang terlalu terang
/// sebesar faktor pi.
///
/// Itu bukan kemungkinan yang jauh: `evaluateOpenPBR_IBL` di openpbr.slang
/// **membagi pi**, sedangkan `box.frag` di bawah tidak — karena seluruh
/// ekspresi `box.frag` memang ad-hoc dan bukan model shading. Begitu jalur
/// punctual ini pindah ke openpbr.slang, skala intensitasnya bergeser sebesar
/// pi dan setiap lampu yang sudah disetel harus disetel ulang. Yang membuat
/// kesalahan pi mahal adalah ia tidak pernah terlihat salah; yang
/// menghentikannya adalah menuliskan satuannya di tempat orang membacanya.
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
        vec3 toLight = light.positionInvRangeSq.xyz - worldPosition;
        float distanceSq = dot(toLight, toLight);
        float attenuation =
            distanceAttenuation(distanceSq, light.positionInvRangeSq.w, light.kind.y);
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
        // Bayangan diambil terakhir: ia yang paling mahal, dan setiap
        // penolakan di atas menghematnya utuh.
        attenuation *= sampleAtlasShadow(light, worldPosition, normal, -toLight);
        sum += light.colorCosInner.rgb * (ndotl * attenuation);
    }
    return sum;
}
