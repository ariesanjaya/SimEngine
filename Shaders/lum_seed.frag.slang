// Petak awal pengukuran luminansi: gambar HDR seukuran berapa pun menjadi
// log-luminansi 256×256.
//
// **Petaknya berukuran tetap, bukan seukuran viewport.** Rantai penurunan 2×2
// atas ukuran ganjil harus membuang satu baris atau menimbang tiga texel, dan
// keduanya memasukkan bias yang bergantung ukuran jendela — eksposur yang
// bergeser sedikit saat pemisah dock digeser adalah cacat yang tidak akan
// pernah dicurigai siapa pun.

#include "tonemap_common.slang"

struct Push {
    /// Petak yang benar-benar digambar di dalam gambar HDR, dalam uv. Gambar
    /// dialokasikan lebih besar daripada viewport, dan sisanya tidak pernah
    /// diisi — merata-ratakannya ikut berarti mengukur memori yang belum ditulis.
    float2 viewportUvMax;
    int2 seedSize;
};

[[vk::push_constant]] ConstantBuffer<Push> pc;
[[vk::binding(0, 0)]] Sampler2D uScene;

static const float kMinLuminance = 1e-4;

[shader("fragment")]
float main(float4 position : SV_Position) : SV_Target {
    float2 cell = pc.viewportUvMax / float2(pc.seedSize);
    float2 origin = float2(int2(position.xy)) * cell;

    // Empat kali empat cuplikan bilinear di dalam petaknya. **Bukan satu.**
    // Satu cuplikan per texel berarti 65 ribu piksel yang dibaca dari 800 ribu
    // yang ada, dan yang tidak terbaca adalah yang menentukan: sebuah lampu
    // kecil dan terang jatuh di antara cuplikan, lalu muncul kembali begitu
    // kamera bergeser satu piksel — eksposur yang berkedip tanpa sebab yang
    // terlihat.
    float sum = 0.0;
    [unroll]
    for (int y = 0; y < 4; ++y) {
        [unroll]
        for (int x = 0; x < 4; ++x) {
            float2 uv = origin + (float2(x, y) + 0.5) * cell * 0.25;
            float3 color = uScene.SampleLevel(uv, 0.0).rgb;
            // Lantai luminansi. log2(0) adalah negatif tak hingga, dan satu
            // piksel hitam sudah cukup meracuni seluruh rata-rata — yang muncul
            // sebagai layar hitam atau putih penuh, bukan sebagai galat.
            sum += log2(max(luminanceOf(color), kMinLuminance));
        }
    }
    return sum / 16.0;
}
