// Keadaan bayangan per frame, dipakai bersama pass forward.
//
// **Tata letaknya ABI dengan `ShadowUniforms` di VulkanRenderer.cpp.** Selisih
// satu sisipan tidak menghasilkan galat apa pun — hanya bayangan yang jatuh di
// tempat yang salah, yang mudah dikira masalah matriks cascade.
//
// Seluruh anggotanya berukuran dan berjajar kelipatan 16 byte: hanya `float4`,
// `float4x4`, dan larik keduanya. **Itu bukan kebetulan melainkan yang membuat
// ABI ini selamat dari porting GLSL ke Slang** — std140, std430, dan tata letak
// skalar sepakat persis pada tipe-tipe itu, jadi tidak ada aturan tata letak
// yang bisa menggeser satu medan pun. Menambahkan `float3` atau skalar tunggal
// di sini akan mematahkan sifat itu, dan `static_assert` di sisi C++ tidak bisa
// melihatnya.

#ifndef SHADOW_COMMON_SLANG
#define SHADOW_COMMON_SLANG

static const int kMaxCascades = 4;

#include "probe_grid.slang"

struct ShadowParams {
    float4x4 cascadeViewProj[kMaxCascades];
    float4 cascadeSplitFar;    // batas jauh tiap cascade, ruang pandang
    float4 cascadeBlendBegin;  // awal pita campur tiap cascade
    float4 cascadeTexelSize;   // ukuran dunia satu texel tiap cascade
    float4 lightDirection;     // xyz: dari permukaan ke cahaya, w: jumlah cascade
    float4 cameraPosition;     // xyz: posisi, w: 1 kalau bayangan menyala
    float4 sunRadiance;        // rgb: warna * intensitas * eksposur
    float4 cameraForward;      // xyz: arah pandang, w: kekuatan bias normal
    float4 clusterCounts;      // xyz: tilesX, tilesY, slices; w: jumlah lampu
    float4 clusterDepth;       // x: skala irisan, y: bias irisan, z: near, w: far
    float4 viewportSize;       // xy: piksel
    // Kaskade SDF clipmap. xyz titik asal dunia, w ukuran voxel.
    float4 sdfOrigin[4];
    // x resolusi, y jumlah kaskade, z lebar pita dalam voxel, w langkah maksimum
    float4 sdfParams;
    // Dunia -> clip. Dipakai lapis screen-space untuk memproyeksikan sinar ke
    // layar. Di UBO, bukan di push constant: batas push constant yang dijamin
    // Vulkan hanya 128 byte, dan dua matriks saja sudah menghabiskannya.
    float4x4 viewProj;
    // Clip -> dunia. Dipakai penelusur screen-space memulihkan posisi dunia dari
    // sebuah texel depth, dan itulah yang membuat uji ketebalan bisa diukur
    // dalam meter. Di UBO supaya penelusurnya tidak bergantung pada push
    // constant pemakainya — dan ia dipakai lebih dari satu pass.
    float4x4 invViewProj;
    // x ketebalan yang diandaikan (meter), y dorongan awal (meter),
    // z langkah maksimum screen-space, w jumlah tingkat HiZ. Nol pada w berarti
    // lapis screen-space mati.
    float4 screenTrace;
    // x kapasitas cache (pangkat dua), y ukuran sel terhalus, z jarak LOD,
    // w langkah probing maksimum. Kapasitas nol berarti cache mati.
    float4 cacheParams;
    // x frame akumulasi, y frame sebelum entri boleh direbut, z nomor frame.
    float4 cacheDecay;
    // Dunia -> clip frame SEBELUMNYA. Inilah yang mengikat riwayat probe ke
    // dunia alih-alih ke piksel.
    float4x4 previousViewProj;
    // x jendela akumulasi (frame), y jarak bidang untuk menolak riwayat (meter),
    // z kosinus normal minimum, w skala penjepitan riwayat.
    float4 denoise;
    // x 1 kalau iradiansi GI berlaku, y ukuran ubin probe dalam piksel,
    // z banyaknya grid medan jarak yang punya entri frame ini, w 1 saat uji
    // tungku.
    float4 giParams;
    // Langit untuk GI: x pengali radiansi, y ketinggian kamera (km), zw ukuran
    // LUT sky-view. Pengali nol berarti tidak ada atmosfer — langit mati atau
    // HDRI — dan penelusur GI kembali ke gradien cadangannya.
    float4 skyParams;
    // Penutup rekursi untuk permukaan yang belum dikenal cache radiansi:
    // xyz albedo yang ditebak, w 1 kalau tebakan itu dipakai.
    float4 giBounce;
    // Keadaan tampilan per-frame: x 1 kalau mode Unlit, y 1 kalau mode Clay,
    // **z detik sejak adegan dibuka**.
    //
    // **Di sini, bukan konstanta spesialisasi.** Konstanta spesialisasi
    // ditetapkan saat pipeline dibuat, dan pipeline pass forward dibuat satu per
    // material oleh kompiler graph — jadi sebuah bendera `kUnlit` berarti setiap
    // material di project dikompilasi dua kali, dan sakelar di viewport menunggu
    // kompilasi itu selesai. Sebuah float di blok per-frame berlaku pada frame
    // berikutnya, untuk seluruh material sekaligus, tanpa satu pun pipeline
    // dibangun ulang.
    float4 viewParams;
    // Iradiansi lingkungan panggang, sembilan koefisien SH orde dua (B1).
    //
    // **rgb dipakai, w padding.** std140 menaikkan setiap anggota larik ke
    // 16 byte, jadi float3[9] menempati ruang yang sama dengan float4[9] —
    // menuliskannya sebagai float4 membuat tata letaknya terbaca apa adanya
    // alih-alih bergantung pada aturan yang harus diingat.
    //
    // **Sembilan angka, bukan cubemap iradiansi.** Kernel lobe kosinus meredam
    // pita di atas orde dua sampai di bawah satu persen; alasan lengkapnya di
    // `Sh9` pada `Sim/Render/Ibl.h`.
    float4 skyIrradianceSh[9];
    // Putaran lingkungan panggang: x kosinus, y sinus, zw cadangan (B3).
    float4 environmentRotation;
    // Kisi probe iradiansi (S1): xyz titik asal dunia, w jarak antar-probe.
    float4 probeOrigin;
    // xyz jumlah probe tiap sumbu, w apa yang dibawa kisinya:
    //   0 tidak ada kisi
    //   1 iradiansi di ruang lingkungan  (putaran diterapkan saat membaca)
    //   2 iradiansi di ruang dunia
    //   3 iradiansi di ruang dunia **beserta visibilitas arah** (S3)
    float4 probeCounts;
};

[[vk::binding(0, 0)]] ConstantBuffer<ShadowParams> shadowParams;

/// Iradiansi E(n) dari SH panggang. **Cerminan `EvaluateIrradiance` di
/// `Ibl.cpp`**, dan keduanya harus tetap sama: yang di sana yang memanggang,
/// yang di sini yang membacanya, dan dua rumus yang berselisih menghasilkan
/// permukaan yang disinari berbeda dari langit yang memanggangnya.
///
/// Mengembalikan E, bukan radiansi — pemakainya masih harus membagi pi untuk
/// mendapat radiansi keluar permukaan Lambert. Kesalahan pi di sini tidak pernah
/// terlihat salah; ia hanya membuat seluruh adegan terlalu terang sebesar faktor
/// tetap, yang paling mudah dikira masalah eksposur.
/// Arah dunia → arah baca lingkungan panggang.
///
/// **Putaran diterapkan di sini, bukan dipanggang ke dalam petanya.** Keputusan
/// 4 di docs/PLAN-IBL.md: memanggangnya membuat setiap geseran slider sebuah
/// panggangan baru, memutar arah cuplikannya membuat slidernya mulus.
///
/// Arahnya diputar dengan **minus** sudutnya, dan itu bukan pilihan: pass langit
/// HDRI menambahkan sudutnya ke bujur (`sky_hdri.frag.slang`), sehingga peta
/// yang terlihat pada arah d adalah peta pada bujur φ(d)+θ. Membaca peta yang
/// dipanggang tanpa putaran pada bujur itu berarti membacanya pada arah
/// Ry(−θ)·d. Tanda yang salah tidak menghasilkan galat apa pun — hanya pantulan
/// yang berputar berlawanan dengan langit yang memantulkannya.
float3 environmentDirection(float3 direction) {
    const float c = shadowParams.environmentRotation.x;
    const float s = shadowParams.environmentRotation.y;
    return float3(direction.x * c - direction.z * s, direction.y,
                  direction.x * s + direction.z * c);
}

float3 skyIrradiance(float3 normal) {
    const float3 n = environmentDirection(normalize(normal));
    // Konvolusi lobe kosinus: Â₀ = π, Â₁ = 2π/3, Â₂ = π/4.
    const float a0 = 3.141593;
    const float a1 = 2.094395;
    const float a2 = 0.785398;

    float3 result = shadowParams.skyIrradianceSh[0].rgb * 0.282095 * a0;

    result += shadowParams.skyIrradianceSh[1].rgb * (0.488603 * n.y) * a1;
    result += shadowParams.skyIrradianceSh[2].rgb * (0.488603 * n.z) * a1;
    result += shadowParams.skyIrradianceSh[3].rgb * (0.488603 * n.x) * a1;

    result += shadowParams.skyIrradianceSh[4].rgb * (1.092548 * n.x * n.y) * a2;
    result += shadowParams.skyIrradianceSh[5].rgb * (1.092548 * n.y * n.z) * a2;
    result += shadowParams.skyIrradianceSh[6].rgb * (0.315392 * (3.0 * n.z * n.z - 1.0)) * a2;
    result += shadowParams.skyIrradianceSh[7].rgb * (1.092548 * n.x * n.z) * a2;
    result += shadowParams.skyIrradianceSh[8].rgb * (0.546274 * (n.x * n.x - n.y * n.y)) * a2;

    // Iradiansi negatif tidak punya arti fisik, dan ia muncul pada lingkungan
    // berkontras tinggi: orde dua tidak bisa mewakili tepi tajam tanpa melewati
    // batas. Dijepit di sini juga, bukan hanya di CPU — warna negatif merambat
    // ke seluruh perhitungan sesudahnya.
    return max(result, float3(0.0));
}

/// Iradiansi E(n) dari sembilan koefisien SH yang diberikan.
///
/// Dipisahkan dari `skyIrradiance` supaya kisi probe dan SH panggang memakai
/// evaluasi yang sama persis — dua salinan rumus ini akan berselisih di tepinya,
/// dan selisih itu muncul sebagai kisi yang tidak pernah cocok dengan tingkat
/// panggang yang seharusnya ia tiru.
float3 evaluateSh9(float3 sh[9], float3 normal) {
    const float3 n = normalize(normal);
    const float a0 = 3.141593;
    const float a1 = 2.094395;
    const float a2 = 0.785398;

    float3 result = sh[0] * 0.282095 * a0;
    result += sh[1] * (0.488603 * n.y) * a1;
    result += sh[2] * (0.488603 * n.z) * a1;
    result += sh[3] * (0.488603 * n.x) * a1;
    result += sh[4] * (1.092548 * n.x * n.y) * a2;
    result += sh[5] * (1.092548 * n.y * n.z) * a2;
    result += sh[6] * (0.315392 * (3.0 * n.z * n.z - 1.0)) * a2;
    result += sh[7] * (1.092548 * n.x * n.z) * a2;
    result += sh[8] * (0.546274 * (n.x * n.x - n.y * n.y)) * a2;
    return max(result, float3(0.0));
}

/// SH sebuah probe, atau nol bila brick yang memuatnya tidak ada.
///
/// `found` yang membedakan "probe gelap" dari "probe tidak ada" — dan
/// perbedaannya bukan kerapian: yang tidak ada harus keluar dari bobot
/// interpolasi, sedangkan yang gelap harus ikut menariknya ke bawah.
void probeAt(uint3 coordinate, out float3 sh[9], out bool found, out uint outSlot) {
    for (int i = 0; i < 9; ++i) {
        sh[i] = float3(0.0);
    }
    found = false;
    outSlot = 0u;

    const uint3 counts = uint3(shadowParams.probeCounts.xyz);
    const uint slot = probeBricks[probeBrickIndex(counts, coordinate)];
    if (slot == kProbeEmptyBrick) {
        return;
    }

    const uint base = probeCoefficientBase(slot, coordinate);
    for (int i = 0; i < 9; ++i) {
        sh[i] = probeSh[base + uint(i)].rgb;
    }
    outSlot = slot;
    found = true;
}

/// Posisi dunia sebuah probe. Cerminan `ProbeVolumeLayout::ProbePosition`.
float3 probePosition(uint3 coordinate) {
    return shadowParams.probeOrigin.xyz + float3(coordinate) * shadowParams.probeOrigin.w;
}

/// Iradiansi dari kisi probe, interpolasi trilinear.
///
/// **Cerminan `SampleProbeVolume` di `ProbeVolume.cpp`**, dan keduanya harus
/// bergerak bersama: yang di sana dipakai uji dan path tracer acuan, yang di
/// sini dipakai menggambar. Dua interpolasi yang berselisih menghasilkan gambar
/// acuan yang tidak bisa menilai apa pun.
///
/// **Dinormalkan terhadap bobot yang benar-benar terpakai.** Begitu S2 membuang
/// brick kosong, sebagian sudut hilang — dan menjumlahkan tujuh dari delapan
/// tanpa menormalkan menggelapkan tepat di dekat brick yang kosong, yaitu di
/// dekat dinding. Cacatnya baru muncul di S2; pencegahnya sudah di sini.
/// Iradiansi dari kisi probe, interpolasi trilinear.
float3 probeIrradiance(float3 worldPosition, float3 normal) {
    const uint3 counts = uint3(shadowParams.probeCounts.xyz);
    uint3 base;
    float3 fraction;
    probeCell(worldPosition, shadowParams.probeOrigin.xyz, shadowParams.probeOrigin.w, counts,
              base, fraction);

    float3 accumulated[9];
    float3 plain[9];
    for (int i = 0; i < 9; ++i) {
        accumulated[i] = float3(0.0);
        plain[i] = float3(0.0);
    }
    float total = 0.0;
    float plainTotal = 0.0;

    for (uint corner = 0u; corner < 8u; ++corner) {
        const float weight = probeCornerWeight(corner, fraction);
        if (weight <= 0.0) {
            continue;
        }
        const uint3 coordinate = probeCorner(base, corner, counts);

        float3 sh[9];
        bool found;
        uint slot;
        probeAt(coordinate, sh, found, slot);
        if (!found) {
            continue;
        }

        // **Visibilitas arah (S3).** Sebuah probe yang berada di balik geometri
        // dari titik ini — atau di dalamnya — tidak boleh menyumbang. Yang di
        // dalam benda pejal dipanggang tepat nol, dan tanpa bobot ini nol itu
        // ikut ke dalam jawabannya: pada `bench.simlevel` jarak 1 m, 246 dari
        // 21.312 probe seperti itu, dan riaknya di layar tumbuh 3,69 kali.
        float visible = 1.0;
        if (shadowParams.probeCounts.w > 2.5) {
            // **Titiknya digeser sepanjang normal sebelum ditanyakan.** Sebuah
            // permukaan berada tepat di batas geometri yang dilihat probe di
            // bawahnya, jadi tanpa geseran ini uji Chebyshev berayun di sekitar
            // ambangnya — dan yang keluar bukan oklusi melainkan struktur baru
            // yang mengikuti kisi. Diukur: tanpanya, kisi 1 m menjadi 12% lebih
            // berriak dengan visibilitas dibanding tanpanya.
            const float3 biased = probeBiasedPosition(worldPosition, normal);
            const float3 toPoint = biased - probePosition(coordinate);
            const float distance = length(toPoint);
            if (distance > 1e-5) {
                const uint texel = probeDepthTexel(toPoint / distance);
                const float2 moments = probeDepth[probeDepthBase(slot, coordinate) + texel];
                visible = probeVisibilityWeight(distance, moments.x, moments.y);
            }
        }
        const float weighted = weight * visible;
        if (weighted <= 0.0) {
            continue;
        }

        for (int i = 0; i < 9; ++i) {
            accumulated[i] += sh[i] * weighted;
            plain[i] += sh[i] * weight;
        }
        total += weighted;
        plainTotal += weight;
    }

    if (plainTotal <= 0.0) {
        return float3(0.0);
    }
    // **Dua jawaban, lalu dicampur menurut berapa banyak yang bertahan.**
    // Menormalkan hanya ke yang bertahan berarti menyatakan seluruh bobotnya
    // milik mereka — dan di koridor yang lebih sempit daripada kisinya itu
    // menghasilkan jawaban dua setengah kali terlalu terang.
    const float confidence = probeConfidence(total, plainTotal);
    for (int i = 0; i < 9; ++i) {
        const float3 weightedAverage = total > 0.0 ? accumulated[i] / total : float3(0.0);
        accumulated[i] = lerp(plain[i] / plainTotal, weightedAverage, confidence);
    }
    // **Putaran lingkungan diterapkan hanya bila isinya memang datang dari
    // lingkungan.** Di S1 tiap probe memuat SH langit apa adanya — dipanggang
    // pada orientasi petanya sendiri — jadi ia harus dibaca lewat arah yang
    // diputar, persis seperti `skyIrradiance`. Tanpa itu, memutar slider HDR
    // menggeser SH panggang tetapi tidak menggeser kisinya, dan keduanya
    // berselisih tanpa satu pun galat.
    //
    // Begitu S2 mengisi probe dengan penelusuran, isinya sudah berada di ruang
    // dunia dan putarannya sudah ikut terpanggang di dalamnya — menerapkannya
    // lagi memutarnya dua kali. Itu sebabnya yang membedakan sebuah angka yang
    // dikirim renderer, bukan sebuah andaian di dalam shader ini.
    const float3 direction =
        shadowParams.probeCounts.w < 1.5 ? environmentDirection(normalize(normal))
                                         : normalize(normal);
    return evaluateSh9(accumulated, direction);
}
[[vk::binding(1, 0)]] Sampler2DArrayShadow shadowMap;

/// Peta lingkungan terprafilter dan LUT DFG-nya (B2).
///
/// **Keduanya selalu terikat, juga sebelum ada panggangan yang selesai.** Yang
/// terikat saat itu cubemap 1x1 hitam dan piramida HiZ; descriptor yang
/// dibiarkan kosong adalah pelanggaran pada setiap draw, bahkan pada draw yang
/// tidak membacanya. Yang menjaga keduanya tidak pernah dipakai sebagai
/// pantulan adalah `viewParams.w` yang nol.
///
/// **Yang membacanya kode material, bukan berkas ini.** Pemetaan kekasaran ke
/// mip tinggal di `openpbr.slang`, dan berkas itu disertakan sesudah yang ini —
/// jadi membungkusnya di sini menuntut salinan kedua pemetaan yang sama, yaitu
/// dua rumus yang suatu saat berselisih.
[[vk::binding(24, 0)]] SamplerCube prefilteredEnv;
[[vk::binding(25, 0)]] Sampler2D dfgLut;

/// Kisi probe iradiansi (S1 di docs/PLAN-STATIC-GI.md).
///
/// **Dua buffer, bukan satu tekstur 3D**, dan itu akibat langsung dari brick:
/// yang dibaca bukan kisi padat melainkan tabel slot lalu isinya, dan sebuah
/// tekstur 3D tidak punya tempat untuk indireksi itu. Ia juga yang membuat S2
/// bisa membuang brick tanpa satu pun perubahan di sini.
///
/// `probeSh` sembilan float4 berurutan per probe — rgb dipakai, w padding, tata
/// letak yang sama dengan `Sh9` di sisi C++. `probeBricks` satu uint per brick:
/// slot brick di dalam `probeSh`, atau 0xFFFFFFFF bila brick itu tidak ada.
[[vk::binding(26, 0)]] StructuredBuffer<float4> probeSh;
[[vk::binding(27, 0)]] StructuredBuffer<uint> probeBricks;

/// Peta kedalaman oktahedral tiap probe (S3): satu pasangan (rata-rata jarak,
/// rata-rata kuadratnya) per texel, `kProbeDepthSize²` texel per probe.
///
/// Selalu terikat, seperti kedua buffer di atas; yang menjaganya tidak terbaca
/// adalah `probeCounts.w` yang kurang dari tiga, bukan bentuk buffernya.
[[vk::binding(28, 0)]] StructuredBuffer<float2> probeDepth;

/// Atlas lightmap: iradiansi per texel untuk permukaan statis (S5).
///
/// Selalu terikat; yang menjaganya tidak terbaca adalah `hasLightmap` yang nol
/// pada instance-nya, bukan bentuk teksturnya.
[[vk::binding(29, 0)]] Sampler2D lightmapAtlas;

/// Iradiansi dari atlas lightmap.
///
/// **Satu angka, bukan sembilan.** Texelnya dipanggang pada normal permukaannya
/// sendiri, jadi tidak ada yang perlu dievaluasi ulang di sini — dan itu pula
/// yang membuatnya tidak bisa dipakai permukaan yang normalnya berubah.
float3 lightmapIrradiance(float2 uv) {
    return max(lightmapAtlas.SampleLevel(uv, 0.0).rgb, float3(0.0));
}

/// Brick yang tidak dialokasikan. Sama dengan `render::kEmptyBrick`.
static const uint kProbeEmptyBrick = 0xFFFFFFFFu;

/// Banyaknya mip peta prefilter. Nol berarti belum ada lingkungan yang
/// dipanggang, dan pemakainya wajib memeriksanya: sebuah pantulan yang datang
/// dari cubemap pengganti adalah pantulan yang tidak berasal dari langit mana
/// pun, yaitu persis konstanta yang B1 buang.
float prefilteredEnvMips() { return shadowParams.viewParams.w; }

/// Cascade untuk sebuah kedalaman pandang, beserta bobot campur ke penerusnya.
///
/// Bentuknya harus sama dengan `ChooseCascade` di sisi C++ — keduanya diuji
/// terhadap kumpulan cascade yang sama, dan rumus yang berselisih menghasilkan
/// pita tempat CPU dan GPU tidak sepakat cascade mana yang berlaku.
int chooseCascade(float viewDepth, out float blendWeight) {
    int count = int(shadowParams.lightDirection.w);
    blendWeight = 0.0;
    for (int i = 0; i < count; ++i) {
        if (viewDepth <= shadowParams.cascadeSplitFar[i]) {
            if (i + 1 < count && viewDepth > shadowParams.cascadeBlendBegin[i]) {
                float span = shadowParams.cascadeSplitFar[i] - shadowParams.cascadeBlendBegin[i];
                blendWeight = span > 1e-6 ? (viewDepth - shadowParams.cascadeBlendBegin[i]) / span
                                          : 0.0;
            }
            return i;
        }
    }
    return count - 1;
}

/// Satu pengambilan bayangan dengan PCF 3x3.
float sampleCascade(int cascade, float3 worldPosition, float3 normal) {
    // **Bias normal, bukan bias depth konstan.** Bias depth konstan adalah angka
    // ajaib yang harus disetel ulang untuk tiap cascade — yang cukup di cascade
    // dekat menghasilkan peter-panning di cascade jauh, dan sebaliknya
    // menghasilkan acne. Menggeser titik sampel sepanjang normal sebanyak satu
    // texel dunia menyesuaikan diri sendiri terhadap resolusi tiap cascade.
    float texel = shadowParams.cascadeTexelSize[cascade];
    float3 offset = normal * (texel * shadowParams.cameraForward.w);
    float4 lightPosition =
        mul(shadowParams.cascadeViewProj[cascade], float4(worldPosition + offset, 1.0));

    float3 projected = lightPosition.xyz / lightPosition.w;
    float2 uv = projected.xy * 0.5 + 0.5;
    // Di luar peta berarti tidak ada yang menghalangi — bukan gelap. Menganggap
    // luar-peta sebagai bayangan membuat seluruh dunia di luar cascade terakhir
    // menjadi hitam.
    if (any(uv < float2(0.0)) || any(uv > float2(1.0)) || projected.z <= 0.0 ||
        projected.z >= 1.0) {
        return 1.0;
    }

    uint width, height, layers, levels;
    shadowMap.GetDimensions(0, width, height, layers, levels);
    float2 texelStep = 1.0 / float2(width, height);
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float2 tap = uv + float2(x, y) * texelStep;
            sum += shadowMap.SampleCmp(float3(tap, float(cascade)), projected.z);
        }
    }
    return sum / 9.0;
}

/// Faktor bayangan 0..1 untuk sebuah titik. 1 berarti tersinari penuh.
float sampleShadow(float3 worldPosition, float3 normal) {
    if (shadowParams.cameraPosition.w < 0.5) {
        return 1.0;
    }
    float viewDepth = dot(worldPosition - shadowParams.cameraPosition.xyz,
                          shadowParams.cameraForward.xyz);
    float blendWeight;
    int cascade = chooseCascade(viewDepth, blendWeight);

    float shadow = sampleCascade(cascade, worldPosition, normal);
    // Pita campur diambil dari kedua cascade, bukan dari salah satunya. Tanpa
    // ini perpindahan cascade adalah garis tajam tempat resolusi bayangan
    // berubah mendadak — dan garis itu bergerak bersama kamera, yang justru
    // membuatnya lebih terlihat daripada bayangan kasarnya sendiri.
    if (blendWeight > 0.0) {
        shadow = lerp(shadow, sampleCascade(cascade + 1, worldPosition, normal), blendWeight);
    }
    return shadow;
}

#endif
