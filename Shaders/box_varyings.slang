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
/// fragment.** `box.frag` sempat menuliskan bit pertama sendiri.
///
/// Bit kedua — "warna instance mengalahkan warna ruas" — hilang di G6: warna
/// ruas sekarang diselesaikan CPU dan sampai ke sini sebagai satu warna, jadi
/// tidak ada lagi dua warna yang harus dipilih di shader.
static const uint kFlagReceiveShadows = 1u;

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
    /// `x` slot material, `y` slot tekstur — keduanya diteruskan apa adanya
    /// dari atribut instance.
    ///
    /// `nointerpolation` karena keduanya nomor, bukan besaran: nomor yang
    /// diinterpolasi antar-vertex menunjuk material yang tidak ada.
    ///
    /// **Satu lokasi berdua, bukan satu lokasi masing-masing.** Yang membacanya
    /// berbeda-beda: shader material mengambil `x`, `box_bindless.frag`
    /// mengambil `y`, dan tidak ada satu pun yang mengambil keduanya. Slang
    /// membuang varying yang tidak dibaca dari antarmuka tahap fragment, jadi
    /// dua lokasi terpisah berarti setiap pipeline menyisakan satu keluaran
    /// tanpa pembaca — yaitu peringatan validasi di setiap pembuatan pipeline.
    [[vk::location(6)]] nointerpolation uint2 slots;
};

#endif
