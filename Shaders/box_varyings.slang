// Varying kubus, dipakai bersama tahap vertex, forward, dan prepass.
//
// **Satu deklarasi, bukan tiga.** Tiga salinan struktur varying adalah tiga
// tempat yang bisa berselisih nomor lokasinya — dan selisih itu muncul sebagai
// atribut yang terbaca sebagai atribut lain, bukan sebagai galat.

#ifndef BOX_VARYINGS_SLANG
#define BOX_VARYINGS_SLANG

/// Bit `BoxVertex::flags` / `BoxVarying::flags`. Harus sama dengan `kInstance*`
/// di VulkanRenderer.cpp.
///
/// **Di sini karena inilah berkas yang dipakai bersama tahap vertex dan
/// fragment.** `box.frag` sempat menuliskan bit pertama sendiri; bit kedua
/// ditambahkan bersamanya supaya yang ketiga tidak menjadi tiga salinan yang
/// harus sepakat.
static const uint kFlagReceiveShadows = 1u;
static const uint kFlagForceInstanceColor = 2u;

struct BoxVarying {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 normal;
    [[vk::location(1)]] float4 color;
    [[vk::location(2)]] float3 worldPosition;
    [[vk::location(4)]] float2 uv;
    // `nointerpolation`: bitmask tidak punya arti kalau diinterpolasi
    // antar-vertex.
    [[vk::location(3)]] nointerpolation uint flags;
    /// Tangent ruang dunia, `w` arah bitangent.
    ///
    /// Dibawa untuk normal map. `box.frag` tidak membacanya — ia tidak punya
    /// model shading yang memakainya — sementara shader material hasil kompilasi
    /// graph justru itu yang pertama diambilnya.
    [[vk::location(5)]] float4 worldTangent;
};

#endif
