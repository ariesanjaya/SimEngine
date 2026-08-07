#version 450

#include "shadow_common.glsl"

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec3 inWorldPosition;

layout(location = 0) out vec4 outColor;

void main() {
    // Pencahayaan sementara: satu arah plus ambient. Bukan model shading —
    // OpenPBR masuk lewat pipeline material, dan menulis pendekatan kedua di
    // sini melanggar aturan yang dipegang seluruh E7.1: model shading hanya
    // boleh punya satu implementasi. Ini sekadar supaya bentuk kotaknya terbaca.
    //
    // Bayangannya sungguhan, dan itu bukan ketidakkonsistenan: yang sedang
    // diuji di sini adalah pass bayangannya, dan ia harus terlihat pada
    // geometri yang sudah ada sebelum pipeline material siap memakainya.
    vec3 normal = normalize(inNormal);
    vec3 lightDir = normalize(shadowParams.lightDirection.xyz);
    float ndotl = max(dot(normal, lightDir), 0.0);
    float shadow = sampleShadow(inWorldPosition, normal);
    vec3 lit = inColor.rgb * (0.25 + 0.75 * ndotl * shadow);
    outColor = vec4(lit, inColor.a);
}
