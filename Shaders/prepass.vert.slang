// Depth prepass, tahap vertex.
//
// **Tahap vertex sendiri, bukan milik pass forward.** Yang dibutuhkan prepass
// hanyalah posisi klip dan normal; menyalurkan warna dan bendera lewat sini
// berarti tahap vertex mengeluarkan varying yang tidak ada yang membaca — dan
// varying tanpa pembaca adalah peringatan validasi di setiap pembuatan
// pipeline, bukan sekadar pekerjaan yang terbuang.

// Set 1 untuk palet kulit, sama dengan pass forward: keduanya memakai pipeline
// layout yang sama, dan set 0 di sana milik keadaan bayangan per-frame.
#define SKIN_SET 1

#include "box_push.slang"
#include "skin_common.slang"

struct PrepassVarying {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 normal;
};

[shader("vertex")]
PrepassVarying main(BoxVertex vertex, uint instanceId : SV_InstanceID) {
    float3 localPosition;
    float3 localNormal;
    applySkin(vertex, localPosition, localNormal);

    float4x4 model = instanceModel(push.instanceBase + instanceId);
    float4 world = mul(model, float4(localPosition, 1.0));

    PrepassVarying output;
    // Normal ditransformasikan dengan matriks model apa adanya, sama dengan
    // pass forward — sah selama instance kotak hanya diputar dan diskalakan
    // seragam.
    output.normal = normalize(mul((float3x3)model, localNormal));
    output.position = mul(push.viewProj, world);
    return output;
}
