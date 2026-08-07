// Menggambar langit ke target HDR, di belakang segalanya.
//
// Membaca LUT sky-view alih-alih menelusuri ulang: penelusuran per piksel
// mengulang pekerjaan yang sama delapan ratus ribu kali untuk sesuatu yang
// berubah mulus di seluruh layar.

#include "sky_common.slang"

struct Push {
    float4x4 invViewProj;
    /// xyz: posisi kamera dalam meter, w: ketinggian kamera dalam km.
    float4 camera;
    /// xyz: arah dari permukaan ke matahari, w: tidak dipakai.
    float4 sun;
    /// xy: ukuran LUT sky-view, zw: radiansi matahari sebagai pengali
    float4 params;
    /// xyz: radiansi matahari, w: kosinus jari-jari sudutnya
    float4 sunRadiance;
};

[[vk::push_constant]] ConstantBuffer<Push> pc;
[[vk::binding(0, 0)]] Sampler2D uSkyView;
[[vk::binding(1, 0)]] Sampler2D uTransmittance;

[shader("fragment")]
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    // Arah pandang dari matriks terbalik. w = 1 pada bidang dekat: reversed-Z.
    float4 nearPoint = mul(pc.invViewProj, float4(uv * 2.0 - 1.0, 1.0, 1.0));
    float4 farPoint = mul(pc.invViewProj, float4(uv * 2.0 - 1.0, 0.0, 1.0));
    float3 direction = normalize(farPoint.xyz / farPoint.w - nearPoint.xyz / nearPoint.w);

    // Kerangka planet: +Z ke atas dari pusat planet. Sumbu atas adegan adalah
    // +Y, jadi keduanya ditukar di sini dan hanya di sini.
    float viewHeight = kBottomRadius + max(pc.camera.w, kRadiusOffset);
    float3 up = float3(0.0, 0.0, 1.0);
    float3 worldDirection = float3(direction.x, -direction.z, direction.y);
    float3 sunDirection = float3(pc.sun.x, -pc.sun.z, pc.sun.y);

    float viewZenithCos = dot(worldDirection, up);
    float viewZenithAngle = acos(clamp(viewZenithCos, -1.0, 1.0));

    // Sudut terhadap matahari diukur pada bidang mendatar, bukan di ruang penuh:
    // itulah sumbu kedua LUT-nya.
    float3 sideDirection = normalize(worldDirection - up * viewZenithCos);
    float3 sunSide = normalize(sunDirection - up * dot(sunDirection, up));
    float lightViewAngle = acos(clamp(dot(sideDirection, sunSide), -1.0, 1.0));

    float3 origin = float3(0.0, 0.0, viewHeight);
    bool intersectsGround = raySphereNearest(origin, worldDirection, kBottomRadius) >= 0.0;
    float2 lutUv = skyViewParamsToUv(float2(viewZenithAngle, lightViewAngle), pc.params.xy,
                                     viewHeight, intersectsGround);
    float3 sky = uSkyView.SampleLevel(lutUv, 0.0).rgb;

    // Cakram matahari, ditambahkan hanya di luar planet. **Digambar di sini,
    // bukan sebagai objek:** ia berada di jarak tak hingga, dan objek di jarak
    // tak hingga adalah objek yang setiap sistem culling, bayangan, dan depth
    // harus punya kekecualian untuknya.
    if (!intersectsGround) {
        float cosToSun = dot(worldDirection, sunDirection);
        if (cosToSun > pc.sunRadiance.w) {
            float2 transParams = float2(viewHeight, dot(sunDirection, up));
            float3 toSun =
                uTransmittance.SampleLevel(transmittanceParamsToUv(transParams), 0.0).rgb;
            // Tepi lembut selebar seperempat cakramnya, supaya matahari tidak
            // menjadi lingkaran bergerigi selebar beberapa piksel.
            float edge = smoothstep(pc.sunRadiance.w, lerp(pc.sunRadiance.w, 1.0, 0.25),
                                    cosToSun);
            sky += pc.sunRadiance.rgb * toSun * edge;
        }
    }

    return float4(sky * pc.params.z, 1.0);
}
