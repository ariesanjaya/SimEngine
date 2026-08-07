// Debug view SDF, bagian vertex: satu segitiga penutup layar dan sinar kamera
// per piksel. Pola yang sama dengan grid.vert.slang — tidak ada vertex buffer,
// dan sinar dihitung dari matriks yang dibalik.

#include "sdf_debug_common.slang"

[shader("vertex")]
SdfDebugVarying main(uint vertexId : SV_VertexID) {
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    float2 ndc = uv * 2.0 - 1.0;
    float nearDepth = pc.cameraPos.w;

    SdfDebugVarying output;
    output.nearPoint = sdfDebugUnproject(ndc, nearDepth);
    output.farPoint = sdfDebugUnproject(ndc, 1.0 - nearDepth);
    output.position = float4(ndc, 0.0, 1.0);
    return output;
}
