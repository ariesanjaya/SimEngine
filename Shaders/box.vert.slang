// Kubus satuan yang di-instance. Geometri kubusnya tetap; yang berubah per
// instance hanya transform dan warnanya, jadi vertex buffer-nya dibuat sekali
// saat start dan tidak pernah disentuh lagi.

// Set 1 untuk palet kulit: set 0 sudah dipakai keadaan bayangan per-frame.
#define SKIN_SET 1

#include "box_push.slang"
#include "box_varyings.slang"
#include "skin_common.slang"

[shader("vertex")]
BoxVarying main(BoxVertex vertex, uint instanceId : SV_InstanceID,
                uint instanceBase : SV_StartInstanceLocation) {
    // **Kulit lebih dulu, transform instance sesudahnya.** Palet memetakan ruang
    // bind ke ruang lokal mesh, jadi ia berada di dalam transform dunia
    // entity-nya — bukan menggantikannya. Urutan yang terbalik membuat karakter
    // yang dipindahkan di editor tetap berpose di titik asal dunia.
    float3 localPosition;
    float3 localNormal;
    float3 localTangent;
    applySkinTangent(vertex, localPosition, localNormal, localTangent);

    float4x4 model = instanceModel(instanceBase + instanceId);
    float4 world = mul(model, float4(localPosition, 1.0));

    BoxVarying output;
    // Normal ditransformasikan dengan matriks model apa adanya, bukan dengan
    // inverse-transpose. Sah di sini karena instance kotak hanya diputar dan
    // diskalakan seragam; skala tak seragam akan memiringkan normalnya, dan itu
    // baru diperbaiki di E8.4 ketika mesh sungguhan masuk beserta matriks
    // normalnya sendiri.
    output.normal = normalize(mul((float3x3)model, localNormal));
    // **Warna ruas sudah diselesaikan CPU dan tinggal di atribut instance.**
    // Sampai G6 ia datang lewat push constant — warna material ruas menang atas
    // warna instance kecuali saat instance memaksanya, dan yang memaksa hanyalah
    // sorotan seleksi — dan aturan itu tidak berubah; yang berubah hanya di mana
    // ia dinilai. Menilainya di sini berarti dua entity bermesh sama dengan
    // warna ruas berbeda adalah dua panggilan gambar.
    //
    // **Dikalikan dengan warna simpul, bukan menggantikannya.** Warna simpul
    // adalah pemodulasi: mesh yang tidak mengisinya membawa putih, dan putih
    // tidak mengubah apa pun. Alpha-nya ikut supaya sisi yang sengaja tembus
    // pandang tetap tembus pandang.
    output.color = vertex.color * vertex.vertexColor;
    output.slots = uint2(vertex.materialSlot, vertex.textureSlot);
    output.uv = vertex.uv;
    // Petak instance memetakan UV lightmap mesh ke UV atlas. Skala nol berarti
    // instance ini tidak punya petak — objek dinamis, atau objek yang tidak
    // kebagian tempat di atlas.
    output.lightmapUv =
        vertex.lightmapUv * vertex.lightmapScaleOffset.xy + vertex.lightmapScaleOffset.zw;
    output.hasLightmap = vertex.lightmapScaleOffset.x > 0.0 ? 1u : 0u;
    // Arah tangen ikut matriks model, sama dengan normal. `w`-nya arah tangan,
    // yang tidak boleh ikut ditransformasikan — ia sebuah tanda, bukan vektor.
    output.worldTangent = float4(mul((float3x3)model, localTangent), vertex.tangent.w);
    output.worldPosition = world.xyz;
    output.flags = vertex.flags;
    output.position = mul(push.viewProj, world);
    return output;
}
