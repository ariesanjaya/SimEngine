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
#include "gi_cache.slang"

struct ProbePush {
    // xy jumlah probe, z ukuran ubin dalam piksel, w akar jumlah ray
    uint4 grid;
    // x nomor frame, y jumlah frame akumulasi, z jangkauan trace maksimum,
    // w bias titik asal sepanjang normal, dalam voxel kaskade setempat
    float4 params;
};

[[vk::push_constant]] ConstantBuffer<ProbePush> pc;

/// Riwayat probe frame sebelumnya, dan permukaan tempat riwayat itu duduk.
///
/// **Ping-pong tekstur, membalik keputusan M3 yang memadu lewat blend unit.**
/// Blending hanya bisa memadu di texel yang sama; reproyeksi justru membaca
/// texel yang lain — titik dunia yang sama ada di ubin yang berbeda begitu
/// kamera bergerak. Salinannya dibuat sesudah pass ini, jadi descriptor-nya
/// tetap dan tidak ada pertanyaan "yang mana yang berlaku sekarang".
[[vk::binding(17, 0)]] Sampler2D historyShR;
[[vk::binding(18, 0)]] Sampler2D historyShG;
[[vk::binding(19, 0)]] Sampler2D historyShB;
/// xyz posisi dunia probe, w normal tersandi oktahedral ke dua setengah-float.
[[vk::binding(20, 0)]] Sampler2D historySurface;

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
    /// xyz posisi dunia probe, w normal tersandi. Ditulis supaya frame
    /// berikutnya bisa memutuskan apakah riwayat ini masih berlaku baginya.
    float4 surface : SV_Target3;
};

/// Normal dikemas ke mantissa sebuah float di [1,2), dua belas bit per sumbu.
///
/// **Bukan `f32tof16`.** Intrinsik itu memancarkan kapabilitas SPIR-V Float16
/// dan Int16, dan keduanya menuntut fitur perangkat yang tidak ada di baseline —
/// syarat perangkat baru demi mengemas sebuah normal yang hanya dipakai sebagai
/// bobot. Dan `asfloat` atas pola bit sembarang bisa menghasilkan NaN, yang
/// payload-nya tidak dijamin selamat melewati tekstur.
///
/// Nilainya selalu di [1,2): eksponennya tetap, jadi tidak ada pola bit yang
/// bisa menjadi inf atau NaN. Nol berarti "tidak ada permukaan di sini".
float packNormal(float3 normal) {
    float2 oct = saturate(octEncode(normal));
    uint x = uint(oct.x * 4095.0 + 0.5);
    uint y = uint(oct.y * 4095.0 + 0.5);
    return 1.0 + float(x + y * 4096u) / 16777216.0;
}

float3 unpackNormal(float packed) {
    uint bits = uint((packed - 1.0) * 16777216.0 + 0.5);
    float2 oct = float2(float(bits & 4095u), float((bits >> 12) & 4095u)) / 4095.0;
    return octDecode(oct);
}

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
    float3 surfaceNormal = octDecode(gbufferNormal.Load(int3(int2(pixel), 0)).rg);
    // **Yang disimpan titik permukaannya, bukan titik asal sinar.** Riwayat dan
    // bobot probe keduanya menolak lewat jarak tegak lurus bidang permukaan, dan
    // jarak itu diukur ke permukaan — menyimpan titik yang sudah digeser membuat
    // setiap probe tampak melayang sejauh biasnya di atas dirinya sendiri.
    output.surface = float4(origin, packNormal(surfaceNormal));

    // **Sinar berangkat sedikit di atas permukaan, bukan dari permukaannya.**
    // Sphere tracing yang mulai tepat di permukaan berhenti pada langkah
    // pertama: jarak di sana nol, ambang berhentinya setengah voxel. Arah
    // sinarnya tidak menolong — yang menunjuk lurus menjauh pun mengenai, pada
    // jarak nol. Lapis layar sudah punya pergeseran seperti ini di sepanjang
    // arah sinar; lapis SDF tidak punya apa pun, dan `traceTiered` meneruskan
    // titik asal yang sama ke keduanya.
    // Bias dalam voxel **kaskade tempat probe ini berdiri**, bukan dalam meter.
    // Lihat catatan di sisi renderer: satu bias untuk seluruh clipmap berarti
    // probe di kaskade terkasar terjepit di ambang, dan probe yang terjepit
    // tidak punya satu pun sinar yang bisa dijawab medannya.
    int originCascade = cascadeFor(origin);
    float originVoxel = originCascade >= 0 ? cascadeVoxelSize(originCascade)
                                           : shadowParams.sdfOrigin[0].w;
    float3 traceOrigin = origin + surfaceNormal * (pc.params.w * originVoxel);

    uint rayCount = pc.grid.w * pc.grid.w;
    uint frame = uint(pc.params.x);
    uint jitterPeriod = uint(pc.params.y);
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
            giProbeRayDirection(ray, frame, probeIndex, pc.grid.w, jitterPeriod);
        TieredHit hit = traceTiered(traceOrigin, direction, tMax);
        if (hit.unknown) {
            // Tidak ada lapis yang punya jawaban untuk sinar ini. Membuangnya
            // menaksir iradiansi dari sinar yang benar-benar diketahui;
            // menghitungnya sebagai langit menaksir sesuatu yang tidak ada.
            continue;
        }

        float3 cameraPosition = shadowParams.cameraPosition.xyz;

        float3 radiance;
        if (hit.layer == kLayerSdf) {
            // **Inilah yang dibuka M4.** Di M3 sinar ini dibuang dari penaksir
            // karena clipmap hanya menyimpan jarak. Sekarang cache tahu
            // radiansi permukaan yang pernah terlihat di layar, jadi permukaan
            // yang sudah lewat di depan kamera tetap menyumbang cahaya setelah
            // ia keluar layar. Yang belum pernah terlihat tetap dibuang — "tidak
            // tahu" masih bukan "hitam".
            int cascade = cascadeFor(hit.position);
            if (cascade < 0) {
                continue;
            }
            // Cadangan normal: menghadap balik ke sinar. Sebuah permukaan
            // yang dikenai sinar memang menghadap ke arah datangnya, dan itu
            // tebakan yang jauh lebih baik daripada sumbu tetap mana pun.
            float3 hitNormal = sdfSurfaceNormal(cascade, hit.position, -direction);
            float3 cached;
            if (giCacheQuery(hit.position, hitNormal, cameraPosition, cached)) {
                radiance = cached;
            } else if (shadowParams.giBounce.w > 0.5) {
                // **"Tidak tahu" ditaksir, bukan lagi dibuang.** Membuangnya
                // benar selama satu-satunya sumber radiansi adalah layar: yang
                // tidak pernah terlihat memang tidak diketahui warnanya. Tetapi
                // di dalam ruang tertutup hampir *setiap* sinar mengenai
                // permukaan seperti itu, jadi yang tersisa untuk ditaksir tidak
                // ada sama sekali — dan probe yang tidak punya satu pun sinar
                // yang diketahui mengembalikan nol. Serambi Sponza karena itu
                // hitam pekat justru ketika GI dinyalakan.
                radiance = giGuessedBounce(hit.position, hitNormal);
            } else {
                continue;
            }
        } else if (hit.layer == kLayerScreen) {
            // **Koordinat texel, bukan uv ternormalisasi.** `RenderTarget`
            // mengalokasi lebih besar daripada yang digambar dan hanya membangun
            // ulang saat melewati ambang, jadi viewport menempati sepetak di
            // pojok kiri-atas image. uv [0,1] atas image itu mendarat di tempat
            // yang sama sekali lain — dan yang terbaca adalah sisa frame lama,
            // yang tampak seperti cahaya yang masuk akal tapi datang dari
            // tempat yang salah.
            int2 texel = int2(hit.uv * viewport);
            radiance = litColor.Load(int3(texel, 0)).rgb;
            // **Yang dilihat layar disimpan supaya bisa dipakai saat ia tidak
            // lagi terlihat.** Normalnya dibaca dari G-buffer di piksel yang
            // sama, bukan ditebak dari arah sinar: permukaan yang dikenai
            // menghadap ke arahnya sendiri, dan arah sinar hanya kebetulan
            // datang dari sana.
            float3 hitNormal = octDecode(gbufferNormal.Load(int3(texel, 0)).rg);
            giCacheInsert(hit.position, hitNormal, cameraPosition, radiance,
                          uint(shadowParams.cacheDecay.z));
        } else {
            radiance = giSky(direction);
        }

        float4 basis = giShBasis(direction);
        output.r += basis * radiance.r;
        output.g += basis * radiance.g;
        output.b += basis * radiance.b;
        ++known;
    }

    // **Probe tanpa satu pun sinar yang diketahui mempertahankan riwayatnya,
    // bukan mengembalikan nol.** Ia terjadi di sudut sempit, tempat titik asal
    // sinar terjepit di ambang medan jarak walaupun sudah digeser sepanjang
    // normalnya. Mengembalikan nol berarti sepetak hitam yang mengikuti kamera;
    // mempertahankan riwayat berarti sudut itu tetap seterang terakhir kali ia
    // punya jawaban. Yang tidak punya riwayat pun tetap nol — di sana memang
    // belum ada yang bisa dikatakan.
    const bool sampled = known > 0u;
    if (sampled) {
        // Bobot sampel bola seragam: 4π/N, dengan N sinar yang benar-benar
        // dipakai.
        float weight = 12.56637061 / float(known);
        output.r *= weight;
        output.g *= weight;
        output.b *= weight;
    }

    // --- Reproyeksi riwayat --------------------------------------------------
    //
    // **Riwayat diikat ke dunia, bukan ke piksel.** Sampai M4 seluruh riwayat
    // dibuang begitu kamera berpindah, karena piksel yang sama menunjuk
    // permukaan yang berbeda. Di sini titik dunia yang sama dicari di tempatnya
    // berada frame lalu.
    float window = max(shadowParams.denoise.x, 1.0);
    float alpha = 1.0;

    float4 previousClip = mul(shadowParams.previousViewProj, float4(origin, 1.0));
    if (previousClip.w > 1e-6) {
        float2 previousUv = (previousClip.xy / previousClip.w) * 0.5 + 0.5;
        if (all(previousUv >= 0.0) && all(previousUv <= 1.0)) {
            float2 previousPixel = previousUv * viewport;
            int2 previousProbe = int2(clamp(previousPixel / tile - 0.5 + 0.5, float2(0.0),
                                            float2(pc.grid.xy) - 1.0));

            float4 previousSurface = historySurface.Load(int3(previousProbe, 0));
            float3 previousNormal = unpackNormal(previousSurface.w);
            // Ditolak lewat jarak tegak lurus bidang dan kesamaan normal — sama
            // dengan bobot interpolasi probe, dan alasannya sama: dua titik di
            // lantai yang sama boleh berjauhan, dua titik terpisah dinding tidak
            // boleh berdekatan.
            float plane = abs(dot(previousSurface.xyz - origin, surfaceNormal));
            float cosine = dot(previousNormal, surfaceNormal);
            bool usable = previousSurface.w != 0.0 && plane <= shadowParams.denoise.y &&
                          cosine >= shadowParams.denoise.z;
            if (usable) {
                float4 historyR = historyShR.Load(int3(previousProbe, 0));
                float4 historyG = historyShG.Load(int3(previousProbe, 0));
                float4 historyB = historyShB.Load(int3(previousProbe, 0));

                // **Penjepitan riwayat memutus pertukaran respons lawan derau.**
                // Jendela pendek merespons cepat tapi berderau; jendela panjang
                // sebaliknya. Selama riwayatnya sejalan dengan sampel frame ini
                // ia dipertahankan utuh; begitu adegannya benar-benar berubah —
                // lampu dimatikan — riwayat yang sudah tidak sejalan dijepit
                // masuk ke rentang baru dalam satu frame, bukan meluruh
                // perlahan sebagai ghosting.
                // Penjepitan hanya berlaku bila ada sampel untuk menjepitnya:
                // menjepit riwayat terhadap nol yang bukan hasil pengukuran
                // adalah cara lain menghapus riwayat itu.
                float scale = sampled ? shadowParams.denoise.w : 1e30;
                float4 spreadR = abs(output.r) * scale + 1e-3;
                float4 spreadG = abs(output.g) * scale + 1e-3;
                float4 spreadB = abs(output.b) * scale + 1e-3;
                historyR = clamp(historyR, output.r - spreadR, output.r + spreadR);
                historyG = clamp(historyG, output.g - spreadG, output.g + spreadG);
                historyB = clamp(historyB, output.b - spreadB, output.b + spreadB);

                alpha = sampled ? 1.0 / window : 0.0;
                output.r = lerp(historyR, output.r, alpha);
                output.g = lerp(historyG, output.g, alpha);
                output.b = lerp(historyB, output.b, alpha);
            }
        }
    }
    return output;
}
