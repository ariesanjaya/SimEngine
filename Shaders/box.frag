#version 450

#include "shadow_common.glsl"
#include "cluster_common.glsl"

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec3 inWorldPosition;
layout(location = 3) flat in uint inFlags;

/// Harus sama dengan `kInstanceReceiveShadows` di VulkanRenderer.cpp.
const uint kReceiveShadows = 1u;

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
    // Yang tidak menerima bayangan tetap dihitung n·l-nya — ia tersinari
    // seperti biasa, hanya tidak pernah tergelapkan oleh yang lain.
    float shadow = (inFlags & kReceiveShadows) != 0u ? sampleShadow(inWorldPosition, normal)
                                                     : 1.0;
    float viewDepth = dot(inWorldPosition - shadowParams.cameraPosition.xyz,
                          shadowParams.cameraForward.xyz);
    // `punctual` adalah iradiansi E(x). Ia dijumlahkan ke suku ad-hoc di
    // sebelahnya tanpa dibagi pi — dan itu konsisten hanya karena seluruh
    // ekspresi ini memang ad-hoc. Jalur yang benar (albedo/pi * E) ada di
    // openpbr.slang, dan ke sanalah lampu punctual pindah begitu pipeline
    // material menggantikan shader ini.
    vec3 punctual = accumulateClusteredLights(gl_FragCoord.xy, inWorldPosition, normal, viewDepth);
    // Radiance matahari datang dari scene sekarang — warna dikali intensitas
    // dikali eksposur — bukan lagi angka 0,75 yang ditulis di sini.
    vec3 lit = inColor.rgb * (0.25 + shadowParams.sunRadiance.rgb * (ndotl * shadow) + punctual);
    outColor = vec4(lit, inColor.a);
}
