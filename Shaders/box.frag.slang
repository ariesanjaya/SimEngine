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

float4 boxBaseColor(BoxVarying input) {
    return baseColorTexture.Sample(baseColorSampler, input.uv);
}

#include "box_shading.slang"
