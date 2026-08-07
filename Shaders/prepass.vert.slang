// Depth prepass, tahap vertex.
//
// **Tahap vertex sendiri, bukan milik pass forward.** Yang dibutuhkan prepass
// hanyalah posisi klip dan normal; menyalurkan warna dan bendera lewat sini
// berarti tahap vertex mengeluarkan varying yang tidak ada yang membaca — dan
// varying tanpa pembaca adalah peringatan validasi di setiap pembuatan
// pipeline, bukan sekadar pekerjaan yang terbuang.

#include "instance_common.slang"

struct PrepassPush {
    float4x4 viewProj;
};

[[vk::push_constant]] ConstantBuffer<PrepassPush> push;

struct PrepassVarying {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 normal;
};

[shader("vertex")]
PrepassVarying main(BoxVertex vertex) {
    float4x4 model = instanceModel(vertex);
    float4 world = mul(model, float4(vertex.position, 1.0));

    PrepassVarying output;
    // Normal ditransformasikan dengan matriks model apa adanya, sama dengan
    // pass forward — sah selama instance kotak hanya diputar dan diskalakan
    // seragam.
    output.normal = normalize(mul((float3x3)model, vertex.normal));
    output.position = mul(push.viewProj, world);
    return output;
}
