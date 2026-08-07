#include "box_varyings.slang"
#include "cluster_common.slang"

/// Harus sama dengan `kInstanceReceiveShadows` di VulkanRenderer.cpp.
static const uint kReceiveShadows = 1u;

[shader("fragment")]
float4 main(BoxVarying input) : SV_Target {
    // Pencahayaan sementara: satu arah plus ambient. Bukan model shading —
    // OpenPBR masuk lewat pipeline material, dan menulis pendekatan kedua di
    // sini melanggar aturan yang dipegang seluruh E7.1: model shading hanya
    // boleh punya satu implementasi. Ini sekadar supaya bentuk kotaknya terbaca.
    //
    // Bayangannya sungguhan, dan itu bukan ketidakkonsistenan: yang sedang
    // diuji di sini adalah pass bayangannya, dan ia harus terlihat pada
    // geometri yang sudah ada sebelum pipeline material siap memakainya.
    float3 normal = normalize(input.normal);
    float3 lightDir = normalize(shadowParams.lightDirection.xyz);
    float ndotl = max(dot(normal, lightDir), 0.0);
    // Yang tidak menerima bayangan tetap dihitung n·l-nya — ia tersinari
    // seperti biasa, hanya tidak pernah tergelapkan oleh yang lain.
    float shadow = (input.flags & kReceiveShadows) != 0u
                       ? sampleShadow(input.worldPosition, normal)
                       : 1.0;
    float viewDepth = dot(input.worldPosition - shadowParams.cameraPosition.xyz,
                          shadowParams.cameraForward.xyz);
    // `punctual` adalah iradiansi E(x). Ia dijumlahkan ke suku ad-hoc di
    // sebelahnya tanpa dibagi pi — dan itu konsisten hanya karena seluruh
    // ekspresi ini memang ad-hoc. Jalur yang benar (albedo/pi * E) ada di
    // openpbr.slang, dan ke sanalah lampu punctual pindah begitu pipeline
    // material menggantikan shader ini.
    float3 punctual =
        accumulateClusteredLights(input.position.xy, input.worldPosition, normal, viewDepth);
    // Radiance matahari datang dari scene sekarang — warna dikali intensitas
    // dikali eksposur — bukan lagi angka 0,75 yang ditulis di sini.
    float3 lit =
        input.color.rgb * (0.25 + shadowParams.sunRadiance.rgb * (ndotl * shadow) + punctual);
    return float4(lit, input.color.a);
}
