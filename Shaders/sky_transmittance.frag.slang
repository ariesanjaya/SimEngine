// LUT transmitansi: berapa banyak cahaya yang selamat menempuh atmosfer dari
// sebuah ketinggian ke arah tertentu, sampai keluar.
//
// Dibangun sekali saat parameternya berubah, bukan tiap frame: isinya tidak
// bergantung pada matahari maupun kamera.

#include "sky_common.slang"

struct Push {
    float2 dimensions;
};

[[vk::push_constant]] ConstantBuffer<Push> pc;

[shader("fragment")]
float4 main(float4 position : SV_Position) : SV_Target {
    float2 uv = position.xy / pc.dimensions;
    float2 params = uvToTransmittanceParams(uv);

    float3 origin = float3(0.0, 0.0, params.x);
    float3 direction = float3(0.0, safeSqrt(1.0 - params.y * params.y), params.y);

    // Bukan `length`: itu nama fungsi bawaan, dan menutupinya membuat setiap
    // pemakaian `length(...)` di bawahnya gagal dengan "no call operation
    // found for type" — pesan yang tidak menyebut sebabnya sama sekali.
    float rayLength = raySphereNearest(origin, direction, kTopRadius);
    if (rayLength <= 0.0) {
        return float4(1.0, 1.0, 1.0, 1.0);
    }
    // Empat ratus langkah. LUT ini dibangun sekali, jadi ketelitiannya murah —
    // dan galatnya masuk ke setiap pembacaan berikutnya, termasuk ke LUT
    // multiscattering dan sky-view yang membacanya ratusan kali per texel.
    const int kSamples = 400;
    float step = rayLength / float(kSamples);
    float3 depth = float3(0.0);
    for (int i = 0; i < kSamples; ++i) {
        // Titik tengah tiap langkah: memakai tepinya membiaskan seluruh integral
        // ke satu arah, dan biasnya tidak pernah hilang seberapa pun langkah
        // ditambah.
        float3 point = origin + direction * ((float(i) + 0.5) * step);
        depth += sampleExtinction(length(point) - kBottomRadius) * step;
    }
    return float4(exp(-depth), 1.0);
}
