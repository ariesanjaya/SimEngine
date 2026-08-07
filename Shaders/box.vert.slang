// Kubus satuan yang di-instance. Geometri kubusnya tetap; yang berubah per
// instance hanya transform dan warnanya, jadi vertex buffer-nya dibuat sekali
// saat start dan tidak pernah disentuh lagi.

#include "box_varyings.slang"
#include "instance_common.slang"

struct BoxPush {
    float4x4 viewProj;
};

[[vk::push_constant]] ConstantBuffer<BoxPush> push;

[shader("vertex")]
BoxVarying main(BoxVertex vertex) {
    float4x4 model = instanceModel(vertex);
    float4 world = mul(model, float4(vertex.position, 1.0));

    BoxVarying output;
    // Normal ditransformasikan dengan matriks model apa adanya, bukan dengan
    // inverse-transpose. Sah di sini karena instance kotak hanya diputar dan
    // diskalakan seragam; skala tak seragam akan memiringkan normalnya, dan itu
    // baru diperbaiki di E8.4 ketika mesh sungguhan masuk beserta matriks
    // normalnya sendiri.
    output.normal = normalize(mul((float3x3)model, vertex.normal));
    output.color = vertex.color;
    output.worldPosition = world.xyz;
    output.flags = vertex.flags;
    output.position = mul(push.viewProj, world);
    return output;
}
