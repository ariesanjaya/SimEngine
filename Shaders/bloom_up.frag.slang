// Satu tingkat penaikan bloom: tapis tenda 3×3, lalu dipadu ke tingkat ini.
//
// **Dipadu, bukan ditambahkan.** Bloom yang menambahkan tiap tingkat menaikkan
// energi seluruh gambar — dan energi itu lalu diukur eksposur otomatis, yang
// menurunkan eksposur, yang menurunkan bloom. Keduanya saling mengejar dan yang
// terlihat adalah kecerahan yang bergoyang pelan tanpa sebab yang kelihatan.

struct Push {
    float2 sourceTexel;
    /// Petak sumber yang terisi sebagai pecahan alokasinya — sebuah skala, bukan
    /// batas. Lihat catatan yang sama di bloom_down.frag.slang.
    float2 sourceUvScale;
    /// x: sebaran 0..1
    float4 params;
};

[[vk::push_constant]] ConstantBuffer<Push> pc;
[[vk::binding(0, 0)]] Sampler2D uSource;   ///< tingkat yang lebih kecil
[[vk::binding(1, 0)]] Sampler2D uTarget;   ///< tingkat ini, sebelum dipadu

/// `uv` di sini dalam koordinat petak terpakai (0..1). Diskalakan ke uv gambar
/// sumbernya, lalu dijepit setengah texel ke dalam supaya cuplikan bilinear di
/// tepi terakhir tidak menjangkau texel pertama di luar petak itu.
float3 fetch(float2 uv) {
    float2 limit = pc.sourceUvScale - 0.5 * pc.sourceTexel;
    return uSource.SampleLevel(clamp(uv * pc.sourceUvScale, 0.5 * pc.sourceTexel, limit),
                               0.0).rgb;
}

[shader("fragment")]
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 t = pc.sourceTexel / max(pc.sourceUvScale, 1e-6);
    // Bobot 1-2-1 di kedua arah, berjumlah tepat satu.
    float3 sum = fetch(uv + float2(-1, -1) * t) * 0.0625 +
                 fetch(uv + float2( 0, -1) * t) * 0.1250 +
                 fetch(uv + float2( 1, -1) * t) * 0.0625 +
                 fetch(uv + float2(-1,  0) * t) * 0.1250 +
                 fetch(uv)                      * 0.2500 +
                 fetch(uv + float2( 1,  0) * t) * 0.1250 +
                 fetch(uv + float2(-1,  1) * t) * 0.0625 +
                 fetch(uv + float2( 0,  1) * t) * 0.1250 +
                 fetch(uv + float2( 1,  1) * t) * 0.0625;
    float3 here = uTarget.Load(int3(int2(position.xy), 0)).rgb;
    return float4(lerp(here, sum, saturate(pc.params.x)), 1.0);
}
