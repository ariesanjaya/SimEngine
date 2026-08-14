// Kubus satuan yang di-instance. Geometri kubusnya tetap; yang berubah per
// instance hanya transform dan warnanya, jadi vertex buffer-nya dibuat sekali
// saat start dan tidak pernah disentuh lagi.

// Set 1 untuk palet kulit: set 0 sudah dipakai keadaan bayangan per-frame.
#define SKIN_SET 1

#include "box_varyings.slang"
#include "skin_common.slang"

struct BoxPush {
    float4x4 viewProj;
    // Warna dasar ruas yang sedang digambar; `w` nol berarti ruas ini tidak
    // punya material di berkasnya.
    float4 partColor;
};

[[vk::push_constant]] ConstantBuffer<BoxPush> push;

[shader("vertex")]
BoxVarying main(BoxVertex vertex) {
    // **Kulit lebih dulu, transform instance sesudahnya.** Palet memetakan ruang
    // bind ke ruang lokal mesh, jadi ia berada di dalam transform dunia
    // entity-nya — bukan menggantikannya. Urutan yang terbalik membuat karakter
    // yang dipindahkan di editor tetap berpose di titik asal dunia.
    float3 localPosition;
    float3 localNormal;
    applySkin(vertex, localPosition, localNormal);

    float4x4 model = instanceModel(vertex);
    float4 world = mul(model, float4(localPosition, 1.0));

    BoxVarying output;
    // Normal ditransformasikan dengan matriks model apa adanya, bukan dengan
    // inverse-transpose. Sah di sini karena instance kotak hanya diputar dan
    // diskalakan seragam; skala tak seragam akan memiringkan normalnya, dan itu
    // baru diperbaiki di E8.4 ketika mesh sungguhan masuk beserta matriks
    // normalnya sendiri.
    output.normal = normalize(mul((float3x3)model, localNormal));
    // **Warna material ruas menang atas warna instance, kecuali saat instance
    // memaksanya.** Yang memaksa hanyalah sorotan seleksi: objek terpilih harus
    // berubah warna seluruhnya, dan material berkasnya justru yang paling tidak
    // boleh menutupinya.
    bool forceInstanceColor = (vertex.flags & kFlagForceInstanceColor) != 0u;
    float4 tint = (push.partColor.w > 0.0 && !forceInstanceColor) ? push.partColor
                                                                  : vertex.color;
    // **Dikalikan, bukan menggantikan.** Warna simpul adalah pemodulasi: mesh
    // yang tidak mengisinya membawa putih, dan putih tidak mengubah apa pun.
    // Yang menggantikan akan membuat setiap mesh tanpa warna simpul menjadi
    // hitam pekat pada hari atribut ini ditambahkan.
    //
    // Alpha-nya ikut dikalikan supaya sisi yang sengaja tembus pandang tetap
    // tembus pandang; sorotan seleksi memakai jalur `forceInstanceColor` di
    // atas dan karena itu tidak ikut teredam warna simpul.
    output.color = tint * vertex.vertexColor;
    output.uv = vertex.uv;
    output.worldPosition = world.xyz;
    output.flags = vertex.flags;
    output.position = mul(push.viewProj, world);
    return output;
}
