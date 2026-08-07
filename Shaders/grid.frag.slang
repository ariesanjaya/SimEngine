#include "grid_common.slang"

struct VertexOut {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 nearPoint;
    [[vk::location(1)]] float3 farPoint;
};

// Kuat garis grid pada skala tertentu, dengan anti-alias.
//
// fwidth() memberi seberapa cepat koordinat berubah antar-piksel tetangga;
// membagi dengannya membuat lebar garis tetap ~1 piksel berapa pun jaraknya.
// Tanpa ini, grid jauh berubah jadi bidang abu-abu penuh moiré.
float gridStrength(float2 planeCoord, float cellSize) {
    float2 coord = planeCoord / cellSize;
    float2 derivative = fwidth(coord);
    float2 grid = abs(frac(coord - 0.5) - 0.5) / max(derivative, float2(1e-6));
    return 1.0 - min(min(grid.x, grid.y), 1.0);
}

[shader("fragment")]
float4 main(VertexOut input) : SV_Target {
    // Perpotongan sinar dengan bidang y = 0.
    float denom = input.farPoint.y - input.nearPoint.y;
    if (abs(denom) < 1e-6) {
        discard;                       // sinar sejajar bidang tanah
    }
    float t = -input.nearPoint.y / denom;
    if (t <= 0.0 || t >= 1.0) {
        discard;                       // bidang ada di belakang kamera
    }

    float3 world = input.nearPoint + t * (input.farPoint - input.nearPoint);

    float cell = max(pc.params.x, 0.001);
    float fine = gridStrength(world.xz, cell);
    float coarse = gridStrength(world.xz, cell * 10.0);

    float3 color = lerp(float3(0.32), float3(0.46), coarse);
    float alpha = max(fine * 0.35, coarse * 0.65);

    // Sumbu utama diberi warna yang sama dengan gizmo: X merah, Z biru.
    // Ambangnya memakai fwidth supaya tebalnya tetap sekitar satu piksel.
    float2 axisWidth = fwidth(world.xz) * max(pc.params.z, 1.0);
    if (abs(world.z) < axisWidth.y) {
        color = float3(0.85, 0.29, 0.31);
        alpha = max(alpha, 0.85);
    }
    if (abs(world.x) < axisWidth.x) {
        color = float3(0.31, 0.51, 0.93);
        alpha = max(alpha, 0.85);
    }

    // Pudar mengikuti jarak supaya horizon tidak berubah jadi kabut aliasing.
    float viewDistance = length(world - pc.cameraPos.xyz);
    float fade = 1.0 - clamp(viewDistance / max(pc.params.y, 1.0), 0.0, 1.0);
    alpha *= fade * fade;

    if (alpha < 0.002) {
        discard;
    }
    return float4(color, alpha);
}
