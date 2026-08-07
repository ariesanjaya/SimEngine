// Varying kubus, dipakai bersama tahap vertex, forward, dan prepass.
//
// **Satu deklarasi, bukan tiga.** Tiga salinan struktur varying adalah tiga
// tempat yang bisa berselisih nomor lokasinya — dan selisih itu muncul sebagai
// atribut yang terbaca sebagai atribut lain, bukan sebagai galat.

#ifndef BOX_VARYINGS_SLANG
#define BOX_VARYINGS_SLANG

struct BoxVarying {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 normal;
    [[vk::location(1)]] float4 color;
    [[vk::location(2)]] float3 worldPosition;
    // `nointerpolation`: bitmask tidak punya arti kalau diinterpolasi
    // antar-vertex.
    [[vk::location(3)]] nointerpolation uint flags;
};

#endif
