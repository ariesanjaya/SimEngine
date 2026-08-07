// Penelusuran screen probe: enam belas ray per probe, diproyeksikan ke SH orde
// satu.
//
// **Satu invokasi per probe, bukan per ray.** Enam belas ray ditelusuri di dalam
// satu fragmen lalu diproyeksikan sekali; menembakkannya sebagai enam belas
// fragmen akan menuntut penjumlahan silang-fragmen yang tidak dimiliki tahap
// fragment. Keluarannya tiga lampiran — satu `float4` koefisien per kanal warna.
//
// Rumus arah ray dan basis SH-nya ada di `gi_common.slang`, cermin dari
// `ScreenProbe.cpp` yang diuji terhadap uji tungku.

#include "gi_trace.slang"
#include "gi_common.slang"

struct ProbePush {
    // xy jumlah probe, z ukuran ubin dalam piksel, w akar jumlah ray
    uint4 grid;
    // x nomor frame, y jumlah frame akumulasi, z jangkauan trace maksimum
    float4 params;
};

[[vk::push_constant]] ConstantBuffer<ProbePush> pc;

/// Normal per piksel dari depth prepass, tersandi oktahedral.
[[vk::binding(11, 0)]] Sampler2D gbufferNormal;
/// Warna yang sudah tersinari. **Inilah satu-satunya sumber radiansi yang
/// dimiliki M3.** Sinar yang mengenai lewat lapis layar membaca piksel yang
/// sudah dinaungi dan disinari di pass forward — satu pantulan hampir gratis.
/// Sinar yang mengenai lewat SDF tidak punya warna sama sekali: clipmap hanya
/// menyimpan jarak. Yang memberinya warna adalah hash grid di M4, dan sampai
/// saat itu ia menyumbang nol, bukan menebak.
[[vk::binding(12, 0)]] Sampler2D litColor;

struct ProbeSh {
    float4 r : SV_Target0;
    float4 g : SV_Target1;
    float4 b : SV_Target2;
};

[shader("fragment")]
ProbeSh main(float4 position : SV_Position) {
    uint2 probe = uint2(position.xy);
    uint probeIndex = probe.y * pc.grid.x + probe.x;

    float tile = float(pc.grid.z);
    float2 viewport = shadowParams.viewportSize.xy;
    // Pusat ubin, dijepit ke dalam viewport: ubin terakhir sebuah layar yang
    // lebarnya bukan kelipatan ubin hanya terisi sebagian, dan pusat ubin itu
    // jatuh di luar layar.
    float2 pixel = min((float2(probe) + 0.5) * tile, viewport - 0.5);
    float2 uv = pixel / viewport;

    ProbeSh output;
    output.r = float4(0.0);
    output.g = float4(0.0);
    output.b = float4(0.0);

    // Depth dari tingkat nol piramida HiZ, yang memang salinan penuh depth
    // buffer — bukan dari tekstur kedua yang harus dijaga tetap sinkron.
    float depth = hiZ.Load(int3(int2(pixel), 0)).r;
    if (depth <= 0.0) {
        // Probe jatuh di langit: tidak ada permukaan untuk dipijak, dan
        // iradiansinya tidak berarti apa-apa. Nol, bukan warna langit — yang
        // membaca probe ini akan menolaknya lewat bobot, dan mengisinya dengan
        // sesuatu yang tampak masuk akal justru menyembunyikan penolakan itu.
        return output;
    }

    float4 world = mul(shadowParams.invViewProj, float4(uv * 2.0 - 1.0, depth, 1.0));
    float3 origin = world.xyz / world.w;

    uint rayCount = pc.grid.w * pc.grid.w;
    uint frame = uint(pc.params.x);
    uint accumulation = uint(pc.params.y);
    float tMax = pc.params.z;

    // **Sinar yang radiansinya tidak diketahui dibuang, bukan dihitung nol.**
    // Sinar yang mengenai lewat SDF tidak punya warna sampai hash grid M4, dan
    // menghitungnya sebagai hitam bukan sekadar membuat gambarnya lebih gelap —
    // ia menjadikan jenjang penelusuran itu sendiri sumber derau: permukaan yang
    // sama mengembalikan warnanya kalau lapis layar menjawab dan nol kalau SDF
    // yang menjawab, dan mana yang menjawab berubah tiap frame karena arahnya
    // berjitter. Membuangnya menaksir iradiansi dari sinar yang benar-benar
    // diketahui; menghitungnya nol menaksir sesuatu yang tidak ada.
    uint known = 0u;
    for (uint ray = 0u; ray < rayCount; ++ray) {
        float3 direction =
            giProbeRayDirection(ray, frame, probeIndex, pc.grid.w, accumulation);
        TieredHit hit = traceTiered(origin, direction, tMax);

        float3 radiance;
        if (hit.layer == kLayerSdf) {
            continue;  // radiansinya belum ada; menunggu hash grid M4
        }
        if (hit.layer == kLayerScreen) {
            // **Koordinat texel, bukan uv ternormalisasi.** `RenderTarget`
            // mengalokasi lebih besar daripada yang digambar dan hanya membangun
            // ulang saat melewati ambang, jadi viewport menempati sepetak di
            // pojok kiri-atas image. uv [0,1] atas image itu mendarat di tempat
            // yang sama sekali lain — dan yang terbaca adalah sisa frame lama,
            // yang tampak seperti cahaya yang masuk akal tapi datang dari
            // tempat yang salah.
            int2 texel = int2(hit.uv * viewport);
            radiance = litColor.Load(int3(texel, 0)).rgb;
        } else {
            radiance = giSky(direction);
        }

        float4 basis = giShBasis(direction);
        output.r += basis * radiance.r;
        output.g += basis * radiance.g;
        output.b += basis * radiance.b;
        ++known;
    }

    if (known == 0u) {
        return output;
    }
    // Bobot sampel bola seragam: 4π/N, dengan N sinar yang benar-benar dipakai.
    float weight = 12.56637061 / float(known);
    output.r *= weight;
    output.g *= weight;
    output.b *= weight;
    return output;
}
