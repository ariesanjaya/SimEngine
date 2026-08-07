struct VertexOut {
    float4 position : SV_Position;
    [[vk::location(0)]] float4 color;
};

[shader("fragment")]
float4 main(VertexOut input) : SV_Target {
    return input.color;
}
