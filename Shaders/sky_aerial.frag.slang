// LUT aerial perspective: udara yang berada **di antara** kamera dan permukaan.
//
// Tiga dimensi, dan ketiganya perlu: dua untuk arah pandang di layar, satu untuk
// jarak. Langit sudah punya LUT dua dimensi untuk arah, tapi ia menjawab
// pertanyaan yang berbeda — "apa warna udara sepanjang seluruh atmosfer" — dan
// yang dibutuhkan kabut adalah "apa warna udara sampai sejauh ini saja".
//
// **Satu draw per slice, bukan satu compute dispatch.** Acuannya memakai compute
// shader; engine ini sengaja belum punya satu pun compute pipeline, dan yang
// pertama akan membawa serta storage image, barrier compute, dan seluruh jalur
// yang belum pernah dijalankan sekali pun. Tiga puluh dua draw berukuran 32×32
// memakai jalur yang setiap hari sudah dilalui empat pass langit yang lain.
//
// **Transmitansi disimpan sebagai satu skalar rata-rata, bukan tiga kanal.**
// Itu yang membuat komposit di ujung menjadi blending Vulkan biasa —
// `dst = src + dst * src.a` — alih-alih menuntut dual-source blending atau
// salinan ping-pong seluruh gambar HDR. Yang hilang: pemerahan pada jalur yang
// sangat panjang, karena udara memakan biru lebih cepat daripada merah. Pada
// jarak beberapa kilometer di dekat permukaan, ketiga kanal transmitansi masih
// berada dalam satu persen satu sama lain; yang menyalakannya di seluruh planet
// akan melihat selisihnya. Acuan Bruneton-Hillaire membuat pilihan yang sama.

#include "sky_common.slang"

struct Push {
    float4x4 invViewProj;
    /// xyz: posisi kamera dalam meter, w: ketinggian kamera dalam km.
    float4 camera;
    /// xyz: arah dari permukaan ke matahari, w: pengali kerapatan aerosol.
    float4 sun;
    /// x: indeks slice, y: banyaknya slice, z: jarak maksimum LUT (km),
    /// w: sisi LUT dalam texel.
    float4 params;
    /// xy: ukuran LUT transmitansi, zw: ukuran LUT multiscattering.
    float4 lutSizes;
};

[[vk::push_constant]] ConstantBuffer<Push> pc;
[[vk::binding(0, 0)]] Sampler2D uTransmittance;
[[vk::binding(1, 0)]] Sampler2D uMultiscatter;

float3 multiscatteringAt(float3 position, float sunCosZenith) {
    float2 uv = float2(sunCosZenith * 0.5 + 0.5,
                       (length(position) - kBottomRadius) / (kTopRadius - kBottomRadius));
    uv = float2(unitToSubUv(saturate(uv.x), pc.lutSizes.z),
                unitToSubUv(saturate(uv.y), pc.lutSizes.w));
    return uMultiscatter.SampleLevel(uv, 0.0).rgb;
}

[shader("fragment")]
float4 main(float4 position : SV_Position) : SV_Target {
    float2 uv = position.xy / pc.params.w;

    // Arah pandang, sama persis dengan `sky_draw`. w = 1 pada bidang dekat:
    // reversed-Z.
    float4 nearPoint = mul(pc.invViewProj, float4(uv * 2.0 - 1.0, 1.0, 1.0));
    float4 farPoint = mul(pc.invViewProj, float4(uv * 2.0 - 1.0, 0.0, 1.0));
    float3 direction = normalize(farPoint.xyz / farPoint.w - nearPoint.xyz / nearPoint.w);

    // Kerangka planet: +Z ke atas. Penukaran sumbunya dilakukan di sini dan
    // hanya di sini, sama dengan pass langit.
    float3 worldDirection = float3(direction.x, -direction.z, direction.y);
    float3 sunDirection = float3(pc.sun.x, -pc.sun.z, pc.sun.y);
    float haze = max(pc.sun.w, 0.0);

    float viewHeight = kBottomRadius + max(pc.camera.w, kRadiusOffset);
    float3 origin = float3(0.0, 0.0, viewHeight);

    // Sebaran kuadratik: kabut yang berarti ada di dekat kamera, dan sebaran
    // seragam menghabiskan hampir seluruh slice-nya pada jarak yang isinya sudah
    // nyaris tak berubah.
    float unit = (pc.params.x + 0.5) / pc.params.y;
    float maxDistance = pc.params.z * unit * unit;

    // Berhenti di permukaan planet. Sinar yang menembusnya mengintegrasikan
    // udara yang berada di dalam tanah — kerapatan eksponensial yang meledak,
    // dan yang terlihat adalah kabut menyilaukan di bawah horizon, bukan galat.
    float toPlanet = raySphereNearest(origin, worldDirection, kBottomRadius);
    float rayLength = toPlanet > 0.0 ? min(maxDistance, toPlanet) : maxDistance;
    if (rayLength <= 0.0) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    float cosTheta = dot(worldDirection, sunDirection);
    float rayleighPhaseValue = rayleighPhase(cosTheta);
    float miePhaseValue = miePhase(cosTheta, kMiePhaseG);

    // Cuplikan bertambah dengan jarak. Slice terdekat membentang beberapa meter
    // dan sudah tepat dengan empat cuplikan; slice terjauh membentang kilometer.
    int samples = int(clamp((pc.params.x + 1.0) * 2.0, 4.0, 32.0));

    float3 accumulatedLight = float3(0.0);
    float3 accumulatedTransmittance = float3(1.0);
    float step = rayLength / float(samples);

    for (int i = 0; i < samples; ++i) {
        float3 samplePosition = origin + worldDirection * ((float(i) + 0.5) * step);
        float3 up = normalize(samplePosition);
        float height = length(samplePosition) - kBottomRadius;

        Scattering scattering = sampleScattering(height);
        float3 extinction = sampleExtinction(height);
        // Haze adalah aerosol, jadi ia mengalikan suku Mie — yang menghamburkan
        // ke depan dan nyaris tak berwarna. Mengalikan Rayleigh justru akan
        // membuat "kabut" yang membirukan udara tanpa mengaburkannya, yaitu
        // kebalikan dari apa yang diminta orang saat meminta kabut.
        float mieDensity = exp(-height / kMieScaleHeight);
        scattering.mie = float3(kMieScattering * mieDensity * haze);
        extinction += kMieExtinction * mieDensity * (haze - 1.0);

        float sunCosZenith = dot(sunDirection, up);
        float2 transParams = float2(length(samplePosition), sunCosZenith);
        float3 toSun = uTransmittance.SampleLevel(transmittanceParamsToUv(transParams), 0.0).rgb;

        float toGround = raySphereNearest(samplePosition + up * kRadiusOffset, sunDirection,
                                          kBottomRadius);
        float lit = toGround < 0.0 ? 1.0 : 0.0;

        float3 phaseScattering = scattering.mie * miePhaseValue +
                                 scattering.rayleigh * rayleighPhaseValue;
        float3 multiscattered = multiscatteringAt(samplePosition, sunCosZenith) *
                                (scattering.rayleigh + scattering.mie);

        float3 inscatter = lit * toSun * phaseScattering + multiscattered;
        float3 stepTransmittance = exp(-extinction * step);
        // Integrasi analitik di dalam langkahnya, sama dengan pass langit:
        // perkalian sederhana meninggalkan pita-pita yang terlihat pada jumlah
        // langkah sekecil ini.
        float3 integrated = (inscatter - inscatter * stepTransmittance) /
                            max(extinction, float3(1e-9));

        accumulatedLight += accumulatedTransmittance * integrated;
        accumulatedTransmittance *= stepTransmittance;
    }

    float averageTransmittance = dot(accumulatedTransmittance, float3(1.0 / 3.0));
    return float4(accumulatedLight, averageTransmittance);
}
