// Satu tingkat penurunan bloom: tapis 13 cuplikan (Jimenez).
//
// **Bobotnya harus sama dengan `BloomChain::Downsample` di sisi C++**, dan
// jumlahnya harus tepat satu. Jumlah yang meleset sedikit saja mengalikan gambar
// pada setiap tingkat; pada tujuh tingkat, selisih satu persen menjadi tujuh
// persen — pergeseran kecerahan yang tampak seperti bloom yang "terlalu kuat"
// alih-alih seperti tapis yang salah.

#include "tonemap_common.slang"

struct Push {
    float2 sourceTexel;    ///< 1 / ukuran **alokasi** sumber
    /// Petak sumber yang benar-benar terisi, sebagai pecahan alokasinya.
    ///
    /// **Sebuah skala, bukan sebuah batas.** Bentuk pertama saya menjepit uv ke
    /// nilai ini, dan itu salah dengan cara yang halus: segitiga penutup layar
    /// menghasilkan uv 0..1 atas petak yang **digambar**, sedangkan petak
    /// terpakai sumbernya hanya sebagian dari alokasinya. Menjepit membuat
    /// separuh keluaran membaca baris yang sama berulang-ulang — yang terlihat
    /// adalah adegan yang teregang dan terulang di dalam pendarannya, bukan
    /// galat.
    float2 sourceUvScale;
    /// x: bukan nol pada penurunan pertama — pembobotan Karis menyala di sana.
    /// y: ambang luminansi, z: lebar lutut. Keduanya hanya dipakai saat x != 0.
    float4 params;
};

[[vk::push_constant]] ConstantBuffer<Push> pc;
[[vk::binding(0, 0)]] Sampler2D uSource;

/// `uv` di sini dalam koordinat petak terpakai (0..1). Diskalakan ke uv gambar
/// sumbernya, lalu dijepit setengah texel ke dalam supaya cuplikan bilinear di
/// tepi terakhir tidak menjangkau texel pertama di luar petak itu.
float3 fetch(float2 uv) {
    float2 limit = pc.sourceUvScale - 0.5 * pc.sourceTexel;
    return uSource.SampleLevel(clamp(uv * pc.sourceUvScale, 0.5 * pc.sourceTexel, limit),
                               0.0).rgb;
}

/// Ambang berlutut lembut. Ambang tajam membuat permukaan yang luminansinya
/// melintasi ambang berkedip antara berpendar dan tidak — dan permukaan seperti
/// itu justru yang bergerak pelan di layar.
float3 threshold(float3 color) {
    float luminance = luminanceOf(color);
    float knee = max(pc.params.z, 1e-4);
    float soft = clamp(luminance - pc.params.y + knee, 0.0, 2.0 * knee);
    float contribution = max(soft * soft / (4.0 * knee), luminance - pc.params.y);
    return luminance > 1e-6 ? color * (max(contribution, 0.0) / luminance) : float3(0.0);
}

/// Rata-rata sebuah kelompok empat cuplikan. Pembobotan 1/(1+luma) menjinakkan
/// satu piksel yang jauh lebih terang daripada tetangganya — tanpa itu ia
/// mendominasi kelompoknya dan muncul sebagai bintik berpendar yang berkedip
/// begitu kamera bergeser satu piksel. **Bukan penjepitan nilai maksimum:**
/// menjepit menghilangkan energi sorotan sungguhan sekaligus.
float3 groupAverage(float3 a, float3 b, float3 c, float3 e, bool karis) {
    if (!karis) {
        return (a + b + c + e) * 0.25;
    }
    float wa = 1.0 / (1.0 + luminanceOf(a));
    float wb = 1.0 / (1.0 + luminanceOf(b));
    float wc = 1.0 / (1.0 + luminanceOf(c));
    float we = 1.0 / (1.0 + luminanceOf(e));
    return (a * wa + b * wb + c * wc + e * we) / max(wa + wb + wc + we, 1e-6);
}

[shader("fragment")]
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    bool first = pc.params.x > 0.5;
    // Offset tap dalam satuan uv **petak**, bukan uv gambar: `fetch` yang
    // menskalakannya. Memakai texel gambar di sini akan membuat jangkauan
    // tapisnya mengecil sebanding petak terpakai — pendaran yang menyempit saat
    // jendela dikecilkan.
    float2 t = pc.sourceTexel / max(pc.sourceUvScale, 1e-6);

    float3 a = fetch(uv + float2(-2, -2) * t);
    float3 b = fetch(uv + float2( 0, -2) * t);
    float3 c = fetch(uv + float2( 2, -2) * t);
    float3 dd = fetch(uv + float2(-2,  0) * t);
    float3 e = fetch(uv);
    float3 f = fetch(uv + float2( 2,  0) * t);
    float3 g = fetch(uv + float2(-2,  2) * t);
    float3 h = fetch(uv + float2( 0,  2) * t);
    float3 i = fetch(uv + float2( 2,  2) * t);
    float3 j = fetch(uv + float2(-1, -1) * t);
    float3 k = fetch(uv + float2( 1, -1) * t);
    float3 l = fetch(uv + float2(-1,  1) * t);
    float3 m = fetch(uv + float2( 1,  1) * t);

    if (first) {
        a = threshold(a); b = threshold(b); c = threshold(c);
        dd = threshold(dd); e = threshold(e); f = threshold(f);
        g = threshold(g); h = threshold(h); i = threshold(i);
        j = threshold(j); k = threshold(k); l = threshold(l); m = threshold(m);
    }

    float3 result = groupAverage(j, k, l, m, first) * 0.5 +
                    groupAverage(a, b, dd, e, first) * 0.125 +
                    groupAverage(b, c, e, f, first) * 0.125 +
                    groupAverage(dd, e, g, h, first) * 0.125 +
                    groupAverage(e, f, h, i, first) * 0.125;
    return float4(result, 1.0);
}
