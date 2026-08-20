// Kubus satuan yang di-instance. Geometri kubusnya tetap; yang berubah per
// instance hanya transform dan warnanya, jadi vertex buffer-nya dibuat sekali
// saat start dan tidak pernah disentuh lagi.

// Set 1 untuk palet kulit: set 0 sudah dipakai keadaan bayangan per-frame.
#define SKIN_SET 1

#include "box_push.slang"
#include "box_varyings.slang"
#include "skin_common.slang"

[shader("vertex")]
BoxVarying main(BoxVertex vertex, uint instanceId : SV_InstanceID) {
    // **Kulit lebih dulu, transform instance sesudahnya.** Palet memetakan ruang
    // bind ke ruang lokal mesh, jadi ia berada di dalam transform dunia
    // entity-nya — bukan menggantikannya. Urutan yang terbalik membuat karakter
    // yang dipindahkan di editor tetap berpose di titik asal dunia.
    float3 localPosition;
    float3 localNormal;
    float3 localTangent;
    applySkinTangent(vertex, localPosition, localNormal, localTangent);

    float4x4 model = instanceModel(push.instanceBase + instanceId);
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
    // Arah tangen ikut matriks model, sama dengan normal. `w`-nya arah tangan,
    // yang tidak boleh ikut ditransformasikan — ia sebuah tanda, bukan vektor.
    output.worldTangent = float4(mul((float3x3)model, localTangent), vertex.tangent.w);
    output.worldPosition = world.xyz;
    output.flags = vertex.flags;
    output.position = mul(push.viewProj, world);
    return output;
}
