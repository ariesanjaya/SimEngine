// Grid tanah prosedural, bagian vertex.
//
// Tidak ada vertex buffer: tiga vertex membentuk segitiga yang menutupi seluruh
// layar, lalu fragment shader menghitung perpotongan sinar kamera dengan bidang
// y = 0. Keuntungannya, grid otomatis tak terbatas dan selalu tajam pada jarak
// berapa pun, tanpa perlu membangun ulang geometri saat kamera bergerak.

#include "grid_common.slang"

struct VertexOut {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 nearPoint;
    [[vk::location(1)]] float3 farPoint;
};

[shader("vertex")]
VertexOut main(uint vertexId : SV_VertexID) {
    // (0,0), (2,0), (0,2) -> NDC (-1,-1), (3,-1), (-1,3)
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    float2 ndc = uv * 2.0 - 1.0;

    // Vulkan memakai rentang depth [0,1]. Bidang dekat ada di 0 pada proyeksi
    // biasa dan di 1 pada reversed-Z — jadi pemanggil yang menyebutkannya, bukan
    // shader yang menebak. Menebaknya berarti grid yang menghilang begitu
    // renderer berganti proyeksi, tanpa satu pun pesan galat.
    float nearDepth = pc.params.w;

    VertexOut output;
    output.nearPoint = gridUnproject(ndc, nearDepth);
    output.farPoint = gridUnproject(ndc, 1.0 - nearDepth);
    output.position = float4(ndc, 0.0, 1.0);
    return output;
}
