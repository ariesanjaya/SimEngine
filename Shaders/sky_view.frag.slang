// LUT sky-view: warna langit ke segala arah dari posisi kamera sekarang.
//
// Dibangun tiap frame — isinya bergantung pada matahari dan ketinggian kamera —
// tapi hanya 192×108 texel, jadi harganya sepersekian yang dibutuhkan untuk
// menelusuri setiap piksel layar.

#include "sky_common.slang"

struct Push {
    float2 dimensions;
    float2 multiscatterDimensions;
    /// xyz: arah dari permukaan ke matahari, w: ketinggian kamera dalam km.
    float4 sun;
};

[[vk::push_constant]] ConstantBuffer<Push> pc;
[[vk::binding(0, 0)]] Sampler2D uTransmittance;
[[vk::binding(1, 0)]] Sampler2D uMultiscatter;

static const int kSamples = 30;

float3 multiscatteringAt(float3 position, float sunCosZenith) {
    float2 uv = float2(sunCosZenith * 0.5 + 0.5,
                       (length(position) - kBottomRadius) / (kTopRadius - kBottomRadius));
    uv = float2(unitToSubUv(saturate(uv.x), pc.multiscatterDimensions.x),
                unitToSubUv(saturate(uv.y), pc.multiscatterDimensions.y));
    return uMultiscatter.SampleLevel(uv, 0.0).rgb;
}

float3 integrate(float3 origin, float3 direction, float3 sunDirection) {
    float toPlanet = raySphereNearest(origin, direction, kBottomRadius);
    float toTop = raySphereNearest(origin, direction, kTopRadius);
    // Bukan `length`: menutupi fungsi bawaan bernama sama.
    float rayLength;
    if (toPlanet < 0.0 && toTop < 0.0) { return float3(0.0); }
    else if (toPlanet < 0.0) { rayLength = toTop; }
    else if (toTop < 0.0) { rayLength = toPlanet; }
    else { rayLength = min(toPlanet, toTop); }

    float cosTheta = dot(direction, sunDirection);
    float rayleighPhaseValue = rayleighPhase(cosTheta);
    float miePhaseValue = miePhase(cosTheta, kMiePhaseG);

    float3 accumulatedLight = float3(0.0);
    float3 accumulatedTransmittance = float3(1.0);

    for (int i = 0; i < kSamples; ++i) {
        // **Langkah kuadratik, bukan seragam.** Hampir seluruh hamburan terjadi
        // di beberapa kilometer pertama; langkah seragam menghabiskan sebagian
        // besar cuplikannya di udara tipis di kejauhan dan menyisakan dua-tiga
        // cuplikan untuk bagian yang menentukan warnanya.
        float t0 = float(i) / float(kSamples);
        float t1 = float(i + 1) / float(kSamples);
        t0 *= t0;
        t1 *= t1;
        t0 = t0 * rayLength;
        t1 = min(t1, 1.0) * rayLength;
        float shift = t0 + (t1 - t0) * 0.3;
        float step = t1 - t0;

        float3 position = origin + direction * shift;
        float3 up = normalize(position);
        float height = length(position) - kBottomRadius;

        Scattering scattering = sampleScattering(height);
        float3 extinction = sampleExtinction(height);
        float sunCosZenith = dot(sunDirection, up);

        float2 transParams = float2(length(position), sunCosZenith);
        float3 toSun = uTransmittance.SampleLevel(transmittanceParamsToUv(transParams), 0.0).rgb;

        float toGround = raySphereNearest(position + up * kRadiusOffset, sunDirection,
                                          kBottomRadius);
        float lit = toGround < 0.0 ? 1.0 : 0.0;

        float3 phaseScattering = scattering.mie * miePhaseValue +
                                 scattering.rayleigh * rayleighPhaseValue;
        float3 multiscattered = multiscatteringAt(position, sunCosZenith) *
                                (scattering.rayleigh + scattering.mie);

        float3 inscatter = lit * toSun * phaseScattering + multiscattered;
        float3 stepTransmittance = exp(-extinction * step);
        // Integrasi analitik di dalam langkahnya, bukan perkalian sederhana:
        // dengan tiga puluh langkah saja, selisih keduanya terlihat sebagai
        // pita-pita di langit.
        float3 integrated = (inscatter - inscatter * stepTransmittance) /
                            max(extinction, float3(1e-9));

        accumulatedLight += accumulatedTransmittance * integrated;
        accumulatedTransmittance *= stepTransmittance;
    }
    return accumulatedLight;
}

[shader("fragment")]
float4 main(float4 position : SV_Position) : SV_Target {
    float viewHeight = kBottomRadius + max(pc.sun.w, kRadiusOffset);
    float3 origin = float3(0.0, 0.0, viewHeight);

    float2 uv = position.xy / pc.dimensions;
    float2 params = uvToSkyViewParams(uv, pc.dimensions, viewHeight);

    // Matahari diletakkan di bidang xz kerangka lokal LUT ini; sumbu x layar
    // mengukur sudut terhadapnya. Itulah yang membuat LUT dua dimensi cukup
    // untuk langit tiga dimensi.
    float sunCosZenith = dot(normalize(origin), pc.sun.xyz);
    float3 localSun = normalize(float3(safeSqrt(saturate(1.0 - sunCosZenith * sunCosZenith)),
                                       0.0, sunCosZenith));

    float3 direction = float3(cos(params.y) * sin(params.x),
                              sin(params.y) * sin(params.x),
                              cos(params.x));

    return float4(integrate(origin, direction, localSun), 1.0);
}
