// Penetapan lampu punctual ke cluster, satu thread per cluster (G4).
//
// **Acuannya `AssignLights` di `Code/Render/src/LightCluster.cpp`, dan keduanya
// harus sepakat.** Yang di CPU punya test tanpa GPU — uji bola-kotak dan uji
// kerucut-bola diuji sendiri-sendiri di `SimRenderTests` — jadi yang di sini
// adalah salinannya, bukan penafsirannya. Dua implementasi yang harus sepakat
// adalah cara termurah menemukan yang mana yang salah; dua implementasi yang
// ditulis dari dua pemahaman adalah cara termahal.
//
// **Tata letak keluarannya berjarak tetap, bukan padat.** Yang di CPU memadatkan
// daftarnya — offset tiap cluster mengikuti panjang cluster sebelumnya — dan itu
// menuntut pemindaian berurutan yang tidak punya bentuk paralel. Di sini tiap
// cluster memiliki peta blok selebar `maxLightsPerCluster` miliknya sendiri,
// sehingga tidak ada satu pun atomic di jalur utama. Yang dibaca fragment shader
// tetap `uint2(offset, count)` yang sama persis, jadi sisi pembacanya tidak
// berubah satu baris pun.

struct Params {
    /// tilesX, tilesY, slices, banyaknya lampu
    uint4 counts;
    /// tanHalfX, tanHalfY, nearZ, farZ
    float4 grid;
    /// maxLightsPerCluster, sisanya belum dipakai
    uint4 limits;
};

/// Cermin dari `ViewLight` di `ClusterAssign::Upload`. Sudah dalam ruang
/// pandang: memindahkan enam belas lampu sekali di CPU lebih murah daripada
/// memindahkannya 3.456 kali di sini.
struct ViewLight {
    /// xyz posisi ruang pandang, w jangkauan
    float4 positionRange;
    /// xyz arah pancar (ternormalisasi), w kosinus setengah sudut luar
    float4 directionCosOuter;
    /// x: 1 untuk spot, 0 untuk point
    float4 flags;
};

[[vk::binding(0, 0)]] ConstantBuffer<Params> params;
[[vk::binding(1, 0)]] StructuredBuffer<ViewLight> lights;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint2> ranges;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> indices;
/// Satu angka: banyaknya cluster yang daftarnya terpotong. Dibaca CPU satu frame
/// belakangan, semata untuk peringatan — tanpa ini pemotongan menjadi diam, dan
/// yang terlihat hanyalah lampu yang hilang di sudut tertentu saja.
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> overflow;

/// Salinan `ClusterGrid::SliceBounds`.
float2 sliceBounds(uint slice) {
    float count = float(params.counts.z);
    float from = float(min(slice, params.counts.z)) / count;
    float to = float(min(slice + 1, params.counts.z)) / count;
    float ratio = params.grid.w / params.grid.z;
    return float2(params.grid.z * pow(ratio, from), params.grid.z * pow(ratio, to));
}

/// Salinan `ClusterGrid::ClusterBounds`. Baris 0 adalah baris ATAS layar, jadi
/// Y-nya dibalik — sama seperti di CPU, dan kegagalannya sama: cahaya yang
/// terpotong tepat di batas ubin, persegi bertepi tegak lurus di ruang layar.
void clusterBounds(uint x, uint y, uint slice, out float3 boxMin, out float3 boxMax) {
    float2 depth = sliceBounds(slice);
    float tilesX = float(params.counts.x);
    float tilesY = float(params.counts.y);

    float x0 = float(x) / tilesX * 2.0 - 1.0;
    float x1 = float(x + 1) / tilesX * 2.0 - 1.0;
    float y0 = 1.0 - float(y + 1) / tilesY * 2.0;
    float y1 = 1.0 - float(y) / tilesY * 2.0;

    float tanX = params.grid.x;
    float tanY = params.grid.y;

    // Ubin melebar bersama kedalaman, jadi batas melintangnya diambil dari
    // bidang **jauh** irisan — di sanalah ubinnya paling lebar.
    boxMin.x = min(x0 * tanX * depth.x, x0 * tanX * depth.y);
    boxMax.x = max(x1 * tanX * depth.x, x1 * tanX * depth.y);
    boxMin.y = min(y0 * tanY * depth.x, y0 * tanY * depth.y);
    boxMax.y = max(y1 * tanY * depth.x, y1 * tanY * depth.y);
    boxMin.z = depth.x;
    boxMax.z = depth.y;
}

/// Salinan `SphereIntersectsAabb`.
bool sphereIntersectsAabb(float3 centre, float radius, float3 boxMin, float3 boxMax) {
    float3 closest = clamp(centre, boxMin, boxMax);
    float3 delta = centre - closest;
    return dot(delta, delta) <= radius * radius;
}

/// Salinan `ConeIntersectsSphere`.
bool coneIntersectsSphere(float3 apex, float3 direction, float range, float cosOuterAngle,
                          float3 centre, float radius) {
    float3 toSphere = centre - apex;
    float distanceSq = dot(toSphere, toSphere);
    float reach = range + radius;
    if (distanceSq > reach * reach) {
        return false;
    }
    if (distanceSq <= radius * radius) {
        return true;
    }

    float cosAngle = clamp(cosOuterAngle, -1.0, 1.0);
    float sinAngle = sqrt(max(1.0 - cosAngle * cosAngle, 0.0));

    if (sinAngle > 1e-4) {
        float3 shiftedApex = apex - direction * (radius / sinAngle);
        float3 toCentre = centre - shiftedApex;
        float axial = dot(toCentre, direction);
        if (axial <= 0.0) {
            return false;
        }
        return axial * axial >= dot(toCentre, toCentre) * cosAngle * cosAngle;
    }
    float axial = clamp(dot(toSphere, direction), 0.0, range);
    float3 delta = toSphere - direction * axial;
    return dot(delta, delta) <= radius * radius;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    uint total = params.counts.x * params.counts.y * params.counts.z;
    uint cluster = thread.x;
    if (cluster >= total) {
        return;
    }

    // Kebalikan dari `ClusterGrid::IndexOf`: (slice * tilesY + y) * tilesX + x.
    uint x = cluster % params.counts.x;
    uint rest = cluster / params.counts.x;
    uint y = rest % params.counts.y;
    uint slice = rest / params.counts.y;

    float3 boxMin;
    float3 boxMax;
    clusterBounds(x, y, slice, boxMin, boxMax);
    float3 centre = (boxMin + boxMax) * 0.5;
    float boxRadius = length((boxMax - boxMin) * 0.5);

    uint stride = max(params.limits.x, 1u);
    uint base = cluster * stride;
    uint count = 0;
    bool truncated = false;

    for (uint i = 0; i < params.counts.w; ++i) {
        ViewLight light = lights[i];
        float3 position = light.positionRange.xyz;
        float range = max(light.positionRange.w, 0.0);
        if (!sphereIntersectsAabb(position, range, boxMin, boxMax)) {
            continue;
        }
        if (light.flags.x > 0.5) {
            // Longgar ke arah yang aman: cluster yang sesekali menyalakan lampu
            // yang tidak mengenainya jauh lebih murah daripada lampu yang
            // sesekali padam.
            if (!coneIntersectsSphere(position, light.directionCosOuter.xyz, range,
                                      light.directionCosOuter.w, centre, boxRadius)) {
                continue;
            }
        }
        if (count >= stride) {
            truncated = true;
            break;
        }
        indices[base + count] = i;
        ++count;
    }

    ranges[cluster] = uint2(base, count);
    if (truncated) {
        uint previous;
        InterlockedAdd(overflow[0], 1u, previous);
    }
}
