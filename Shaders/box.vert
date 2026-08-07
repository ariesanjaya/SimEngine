#version 450

// Kubus satuan yang di-instance. Geometri kubusnya tetap; yang berubah per
// instance hanya transform dan warnanya, jadi vertex buffer-nya dibuat sekali
// saat start dan tidak pernah disentuh lagi.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

// Per instance. Mat4 memakan empat lokasi atribut — itu aturan Vulkan, bukan
// pilihan.
layout(location = 2) in vec4 inRow0;
layout(location = 3) in vec4 inRow1;
layout(location = 4) in vec4 inRow2;
layout(location = 5) in vec4 inRow3;
layout(location = 6) in vec4 inColor;

layout(push_constant) uniform Push {
    mat4 viewProj;
} push;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec4 outColor;
layout(location = 2) out vec3 outWorldPosition;

void main() {
    mat4 model = mat4(inRow0, inRow1, inRow2, inRow3);
    vec4 world = model * vec4(inPosition, 1.0);

    // Normal ditransformasikan dengan matriks model apa adanya, bukan dengan
    // inverse-transpose. Sah di sini karena instance kotak hanya diputar dan
    // diskalakan seragam; skala tak seragam akan memiringkan normalnya, dan itu
    // baru diperbaiki di E8.4 ketika mesh sungguhan masuk beserta matriks
    // normalnya sendiri.
    outNormal = normalize(mat3(model) * inNormal);
    outColor = inColor;
    outWorldPosition = world.xyz;
    gl_Position = push.viewProj * world;
}
