#include "box_varyings.slang"
#include "cluster_common.slang"
#include "gi_resolve.slang"

/// Harus sama dengan `kInstanceReceiveShadows` di VulkanRenderer.cpp.

/// Dipakai saat GI mati. **Penambal, dan disebut penambal.** Ia yang membuat
/// bentuk kotak terbaca tanpa GI, dan ia pula yang harus hilang begitu GI
/// menyala — dua sumber cahaya tak-langsung yang dijumlahkan berarti adegan
/// yang selalu terlalu terang.
static const float kFallbackAmbient = 0.25;

/// Albedo ruas ini, di set material — **nomor set dan binding yang sama persis
/// dengan yang dihasilkan kompiler graph material.** Ruas tanpa tekstur
/// mendapat putih 1x1, yaitu nilai satuan perkalian.
///
/// Tekstur dan sampler terpisah, bukan `Sampler2D` gabungan: itu bentuk yang
/// ditulis kompiler graph, dan menyamakannya sekarang berarti pipeline material
/// nanti mengganti shader ini tanpa menyentuh satu pun descriptor.
[[vk::binding(1, 2)]] Texture2D<float4> baseColorTexture;
[[vk::binding(2, 2)]] SamplerState baseColorSampler;

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
    float shadow = (input.flags & kFlagReceiveShadows) != 0u
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
    // **Iradiansi tak-langsung datang dari screen probe, bukan dari angka
    // tetap.** Sampai M5 suku ini adalah konstanta 0,25 — penambal supaya bentuk
    // kotaknya terbaca. Konstanta itu juga yang membuat perubahan pencahayaan
    // hampir tidak menggerakkan gambar, karena ia mendominasi apa pun yang
    // datang dari lampu; menggantikannya dengan GI adalah seluruh guna M6.
    //
    // Dibagi pi karena `giIrradianceAt` mengembalikan iradiansi E, sedangkan
    // yang dijumlahkan di sini radiansi keluar permukaan Lambert ber-albedo
    // `input.color`. Kesalahan pi tidak pernah terlihat salah — ia hanya membuat
    // seluruh adegan terlalu terang sebesar faktor tetap, yang paling mudah
    // dikira masalah eksposur.
    float3 indirect = float3(kFallbackAmbient);
    if (shadowParams.giParams.x > 0.5) {
        indirect = giIrradianceAt(input.position.xy, input.worldPosition, normal,
                                  shadowParams.giParams.y) / 3.14159265;
    }
    // Radiance matahari datang dari scene sekarang — warna dikali intensitas
    // dikali eksposur — bukan lagi angka 0,75 yang ditulis di sini.
    // **Dikalikan, bukan menggantikan.** Ruas tanpa tekstur membawa putih, dan
    // putih tidak mengubah apa pun — jadi setiap mesh yang sudah ada tergambar
    // persis seperti sebelum jalur ini ada.
    float4 albedo = baseColorTexture.Sample(baseColorSampler, input.uv);
    float3 surface = input.color.rgb * albedo.rgb;
    float3 lit = surface *
                 (indirect + shadowParams.sunRadiance.rgb * (ndotl * shadow) + punctual);
    return float4(lit, input.color.a * albedo.a);
}
