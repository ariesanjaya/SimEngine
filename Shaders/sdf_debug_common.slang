// Push constant dan varying pass debug SDF, dipakai kedua tahapnya.

#ifndef SDF_DEBUG_COMMON_SLANG
#define SDF_DEBUG_COMMON_SLANG

struct SdfDebugPush {
    float4x4 invViewProj;
    float4 cameraPos;  // xyz posisi kamera, w depth bidang dekat
    float4 params;     // x jenis debug view, y jangkauan trace maksimum
};

[[vk::push_constant]] ConstantBuffer<SdfDebugPush> pc;

struct SdfDebugVarying {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 nearPoint;
    [[vk::location(1)]] float3 farPoint;
};

float3 sdfDebugUnproject(float2 ndc, float depth) {
    float4 point = mul(pc.invViewProj, float4(ndc, depth, 1.0));
    return point.xyz / point.w;
}

#endif
