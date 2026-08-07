// Satu segitiga penutup layar, tanpa vertex buffer dan tanpa varying. Yang
// dibutuhkan tahap fragment hanyalah posisi pikselnya, dan itu selalu ada.

[shader("vertex")]
float4 main(uint vertexId : SV_VertexID) : SV_Position {
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(uv * 2.0 - 1.0, 0.0, 1.0);
}
