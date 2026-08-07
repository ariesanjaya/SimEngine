// Eksposur satu texel: dari log-luminansi rata-rata menjadi pengali eksposur,
// dengan adaptasi terhadap waktu.
//
// **Rumusnya harus sama dengan `AutoExposure` dan `ExposureAdaptation` di
// `Code/Render/src/ToneMap.cpp`** — yang di CPU diuji, yang di sini terlihat.

struct Push {
    /// x: 1 saat manual, 0 saat otomatis
    /// y: EV100 manual
    /// z: kompensasi, stop
    /// w: langkah waktu, detik
    float4 params;
    /// x: tetapan waktu menuju terang, y: menuju gelap
    float4 adaptation;
};

[[vk::push_constant]] ConstantBuffer<Push> pc;
[[vk::binding(0, 0)]] Sampler2D uLuminance;
[[vk::binding(1, 0)]] Sampler2D uPrevious;

[shader("fragment")]
float main(float4 position : SV_Position) : SV_Target {
    float averageLuminance = exp2(uLuminance.Load(int3(0, 0, 0)).r);
    // S = 100, K = 12,5 → EV100 = log2(L · 8).
    float measured = log2(max(averageLuminance, 1e-6) * 8.0);
    float ev100 = pc.params.x > 0.5 ? pc.params.y : measured;
    float target = 1.0 / max(1.2 * exp2(ev100 - pc.params.z), 1e-9);

    float previous = uPrevious.Load(int3(0, 0, 0)).r;
    // Nilai bukan-positif berarti belum ada riwayat: frame pertama mendarat di
    // sasarannya, tidak beradaptasi menuju ke sana. Memulai dari nilai lain
    // berarti setiap kali viewport dibuka adegannya menyala dari gelap.
    // Ditulis sebagai `!(previous > 0)` supaya NaN ikut tertangkap — dan NaN
    // yang menular di sini akan bertahan selamanya lewat riwayatnya sendiri.
    if (!(previous > 0.0)) {
        return target;
    }

    // Eksposur turun berarti adegan menjadi lebih terang; tetapan waktunya
    // dipilih dari arah kecerahan, bukan dari arah pengalinya.
    float tau = target < previous ? pc.adaptation.x : pc.adaptation.y;
    float blend = tau <= 1e-4 ? 1.0 : 1.0 - exp(-max(pc.params.w, 0.0) / tau);
    return previous + (target - previous) * blend;
}
