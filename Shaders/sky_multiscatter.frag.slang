// LUT multiscattering: perkiraan cahaya yang sudah terhambur lebih dari sekali.
//
// **Tanpanya langit siang hari kehilangan sebagian besar kecerahannya**, dan
// yang paling terlihat adalah bayangan di bawah langit terbuka yang menjadi
// hitam pekat alih-alih biru — karena satu-satunya cahaya yang sampai ke sana
// memang cahaya yang sudah terhambur berkali-kali.
//
// Diringkas seperti Hillaire: satu suku yang mengasumsikan hamburan berikutnya
// isotropik dan seragam di sekitar titiknya, lalu dijumlahkan sebagai deret
// geometri.

#include "sky_common.slang"

struct Push {
    float2 dimensions;
    float2 transmittanceDimensions;
};

[[vk::push_constant]] ConstantBuffer<Push> pc;
[[vk::binding(0, 0)]] Sampler2D uTransmittance;

static const int kSphereSamples = 64;
static const int kMarchSamples = 20;
static const float kGoldenRatio = 1.6180339;
static const float kUniformPhase = 1.0 / (4.0 * kPi);

struct MarchResult {
    float3 luminance;
    float3 multiscattering;
};

float3 transmittanceTo(float3 position, float3 sunDirection) {
    float2 params = float2(length(position), dot(sunDirection, normalize(position)));
    return uTransmittance.SampleLevel(transmittanceParamsToUv(params), 0.0).rgb;
}

MarchResult march(float3 origin, float3 direction, float3 sunDirection) {
    MarchResult result;
    result.luminance = float3(0.0);
    result.multiscattering = float3(0.0);

    float toPlanet = raySphereNearest(origin, direction, kBottomRadius);
    float toTop = raySphereNearest(origin, direction, kTopRadius);
    // Bukan `length`: menutupi fungsi bawaan bernama sama.
    float rayLength;
    if (toPlanet < 0.0 && toTop < 0.0) { return result; }
    else if (toPlanet < 0.0) { rayLength = toTop; }
    else if (toTop < 0.0) { rayLength = toPlanet; }
    else { rayLength = min(toPlanet, toTop); }

    float3 accumulatedTransmittance = float3(1.0);
    float previousShift = 0.0;

    for (int i = 0; i < kMarchSamples; ++i) {
        // Dicuplik di sepertiga langkah: untuk fungsi eksponensial itu memberi
        // hasil yang jauh lebih dekat daripada titik tengah.
        float shift = rayLength * (float(i) + 0.3) / float(kMarchSamples);
        float step = shift - previousShift;
        previousShift = shift;

        float3 position = origin + direction * shift;
        float3 up = normalize(position);
        float3 toSun = transmittanceTo(position, sunDirection);

        Scattering scattering = sampleScattering(length(position) - kBottomRadius);
        float3 totalScattering = scattering.rayleigh + scattering.mie;
        float3 extinction = sampleExtinction(length(position) - kBottomRadius);
        float3 stepTransmittance = exp(-extinction * step);

        // Di dalam bayangan planet tidak ada cahaya matahari langsung sama
        // sekali. Melewatkan uji ini menerangi sisi malam planet.
        float toGround = raySphereNearest(position, sunDirection,
                                          kBottomRadius);
        float lit = toGround < 0.0 ? 1.0 : 0.0;

        float3 sunLight = lit * toSun * totalScattering * kUniformPhase;
        float3 multiscatterStep = (totalScattering - totalScattering * stepTransmittance) /
                                  max(extinction, float3(1e-9));
        float3 inscatterStep = (sunLight - sunLight * stepTransmittance) /
                               max(extinction, float3(1e-9));

        result.multiscattering += accumulatedTransmittance * multiscatterStep;
        result.luminance += accumulatedTransmittance * inscatterStep;
        accumulatedTransmittance *= stepTransmittance;
    }
    return result;
}

[shader("fragment")]
float4 main(float4 position : SV_Position) : SV_Target {
    float2 uv = position.xy / pc.dimensions;
    uv = float2(subUvToUnit(uv.x, pc.dimensions.x), subUvToUnit(uv.y, pc.dimensions.y));

    float sunCosZenith = uv.x * 2.0 - 1.0;
    float3 sunDirection = float3(0.0, safeSqrt(saturate(1.0 - sunCosZenith * sunCosZenith)),
                                 sunCosZenith);
    float viewHeight = kBottomRadius +
                       saturate(uv.y + kRadiusOffset) * (kTopRadius - kBottomRadius - kRadiusOffset);
    float3 origin = float3(0.0, 0.0, viewHeight);

    float3 multiscattering = float3(0.0);
    float3 luminance = float3(0.0);
    // Kisi Fibonacci: arah yang tersebar merata di bola tanpa kutub yang
    // kelebihan cuplikan. Kisi lintang-bujur menumpuk cuplikannya di kutub, dan
    // yang terlihat adalah dua bercak terang di langit yang tidak seharusnya ada.
    for (int i = 0; i < kSphereSamples; ++i) {
        float theta = acos(1.0 - 2.0 * (float(i) + 0.5) / float(kSphereSamples));
        float phi = (2.0 * kPi * float(i)) / kGoldenRatio;
        float3 direction = float3(cos(theta) * sin(phi), sin(theta) * sin(phi), cos(phi));
        MarchResult result = march(origin, direction, sunDirection);
        multiscattering += result.multiscattering / float(kSphereSamples);
        luminance += result.luminance / float(kSphereSamples);
    }

    // Deret geometri: setiap peristiwa hamburan menyumbang r kali sebelumnya,
    // jadi jumlah seluruhnya 1/(1−r). Menjumlahkannya tertutup jauh lebih murah
    // daripada menelusuri pantulan demi pantulan, dan selisihnya tidak terlihat.
    float3 r = min(multiscattering, float3(0.999));
    return float4(luminance / (float3(1.0) - r), 1.0);
}
