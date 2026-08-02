#version 450

layout(push_constant) uniform Push {
    mat4 invViewProj;
    vec4 cameraPos;
    vec4 params;      // x = ukuran petak, y = jarak pudar, z = tebal sumbu
} pc;

layout(location = 0) in vec3 vNearPoint;
layout(location = 1) in vec3 vFarPoint;

layout(location = 0) out vec4 outColor;

// Kuat garis grid pada skala tertentu, dengan anti-alias.
//
// fwidth() memberi seberapa cepat koordinat berubah antar-piksel tetangga;
// membagi dengannya membuat lebar garis tetap ~1 piksel berapa pun jaraknya.
// Tanpa ini, grid jauh berubah jadi bidang abu-abu penuh moiré.
float GridStrength(vec2 planeCoord, float cellSize) {
    vec2 coord = planeCoord / cellSize;
    vec2 derivative = fwidth(coord);
    vec2 grid = abs(fract(coord - 0.5) - 0.5) / max(derivative, vec2(1e-6));
    return 1.0 - min(min(grid.x, grid.y), 1.0);
}

void main() {
    // Perpotongan sinar dengan bidang y = 0.
    float denom = vFarPoint.y - vNearPoint.y;
    if (abs(denom) < 1e-6) {
        discard;                       // sinar sejajar bidang tanah
    }
    float t = -vNearPoint.y / denom;
    if (t <= 0.0 || t >= 1.0) {
        discard;                       // bidang ada di belakang kamera
    }

    vec3 world = vNearPoint + t * (vFarPoint - vNearPoint);

    float cell = max(pc.params.x, 0.001);
    float fine = GridStrength(world.xz, cell);
    float coarse = GridStrength(world.xz, cell * 10.0);

    vec3 color = mix(vec3(0.32), vec3(0.46), coarse);
    float alpha = max(fine * 0.35, coarse * 0.65);

    // Sumbu utama diberi warna yang sama dengan gizmo: X merah, Z biru.
    // Ambangnya memakai fwidth supaya tebalnya tetap sekitar satu piksel.
    vec2 axisWidth = fwidth(world.xz) * max(pc.params.z, 1.0);
    if (abs(world.z) < axisWidth.y) {
        color = vec3(0.85, 0.29, 0.31);
        alpha = max(alpha, 0.85);
    }
    if (abs(world.x) < axisWidth.x) {
        color = vec3(0.31, 0.51, 0.93);
        alpha = max(alpha, 0.85);
    }

    // Pudar mengikuti jarak supaya horizon tidak berubah jadi kabut aliasing.
    float distance = length(world - pc.cameraPos.xyz);
    float fade = 1.0 - clamp(distance / max(pc.params.y, 1.0), 0.0, 1.0);
    alpha *= fade * fade;

    if (alpha < 0.002) {
        discard;
    }
    outColor = vec4(color, alpha);
}
