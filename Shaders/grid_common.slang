// Push constant grid, dipakai kedua tahapnya.
//
// **Satu deklarasi, bukan dua.** Blok push constant yang ditulis terpisah di
// vertex dan fragment adalah dua blok yang bisa berselisih — dan selisihnya
// muncul sebagai grid yang memudar di jarak yang salah, bukan sebagai galat.

#ifndef GRID_COMMON_SLANG
#define GRID_COMMON_SLANG

struct GridPush {
    float4x4 invViewProj;
    float4 cameraPos;   // xyz = posisi kamera
    float4 params;      // x = ukuran petak, y = jarak pudar, z = tebal sumbu,
                        // w = nilai depth bidang dekat (0 biasa, 1 reversed-Z)
};

[[vk::push_constant]] ConstantBuffer<GridPush> pc;

float3 gridUnproject(float2 ndc, float depth) {
    float4 point = mul(pc.invViewProj, float4(ndc, depth, 1.0));
    return point.xyz / point.w;
}

#endif
