// Garis ruang-dunia untuk wireframe seleksi, sumbu, dan ikon billboard.
// Semuanya sudah dibentangkan jadi segmen di CPU oleh renderer — termasuk
// billboard, yang butuh posisi kamera untuk menghadap dan ukuran layar untuk
// menjaga besarnya tetap dalam piksel.

struct LinePush {
    float4x4 viewProj;
};

[[vk::push_constant]] ConstantBuffer<LinePush> pc;

struct LineVertex {
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float4 color;
};

struct VertexOut {
    float4 position : SV_Position;
    [[vk::location(0)]] float4 color;
};

[shader("vertex")]
VertexOut main(LineVertex vertex) {
    VertexOut output;
    output.position = mul(pc.viewProj, float4(vertex.position, 1.0));
    output.color = vertex.color;
    return output;
}
