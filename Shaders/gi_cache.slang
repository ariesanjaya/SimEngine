// Hash grid radiance cache, sisi shader.
//
// **Rumusnya harus sama dengan `RadianceCache.cpp`.** Yang di CPU adalah acuan
// yang diuji — termasuk uji tungku yang menuntut albedo 1,0 di bawah cahaya
// seragam tidak menggelap setelah berapa pun pantulan.

#ifndef GI_CACHE_SLANG
#define GI_CACHE_SLANG

#include "shadow_common.slang"

/// Satu entri. Empat `uint` plus tiga `float` — 32 byte, sejajar 16.
///
/// **Kuncinya disimpan sebagai sidik jari 32-bit, bukan sebagai sel utuh.**
/// Menyimpan sel utuh menuntut tiga `int` lagi per entri, dan dua kunci berbeda
/// yang sidik jarinya kebetulan sama hanya menghasilkan satu entri yang isinya
/// campuran dua tempat — cacat yang sudah melekat pada cache lossy, bukan cacat
/// baru. Nol dipakai sebagai "kosong", jadi sidik jarinya dipaksa bukan nol.
struct GiCacheEntry {
    uint fingerprint;
    uint samples;
    uint lastFrame;
    uint padding;
    float3 radiance;
    float padding2;
};

[[vk::binding(16, 0)]] RWStructuredBuffer<GiCacheEntry> giCache;

/// x kapasitas (pangkat dua), y ukuran sel terhalus, z jarak LOD,
/// w langkah probing maksimum.
/// Diteruskan lewat `shadowParams.cacheParams`.

uint giCacheMix(uint value) {
    value ^= value >> 16;
    value *= 0x85EBCA6Bu;
    value ^= value >> 13;
    value *= 0xC2B2AE35u;
    value ^= value >> 16;
    return value;
}

/// Sumbu mayor sebuah normal, 0..5 untuk +X, −X, +Y, −Y, +Z, −Z.
uint giMajorAxis(float3 normal) {
    float3 magnitude = abs(normal);
    if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z) {
        return normal.x >= 0.0 ? 0u : 1u;
    }
    if (magnitude.y >= magnitude.z) {
        return normal.y >= 0.0 ? 2u : 3u;
    }
    return normal.z >= 0.0 ? 4u : 5u;
}

struct GiCacheKey {
    int3 cell;
    uint face;
    uint level;
};

GiCacheKey giCacheKey(float3 position, float3 normal, float3 cameraPosition) {
    GiCacheKey key;
    key.face = giMajorAxis(normal);

    float cellSize = shadowParams.cacheParams.y;
    float lodDistance = shadowParams.cacheParams.z;
    float viewDistance = length(position - cameraPosition);

    uint level = 0u;
    float span = lodDistance;
    // Lingkaran berbatas, bukan `log2`: yang di CPU juga lingkaran, dan dua
    // rumus yang setara secara matematis berselisih satu tingkat tepat di
    // batasnya — yang terlihat sebagai pita tempat cache tiba-tiba kehilangan
    // seluruh isinya.
    while (viewDistance > span && level < 15u) {
        span *= 2.0;
        ++level;
    }
    key.level = level;

    float size = cellSize * float(1u << level);
    key.cell = int3(floor(position / size));
    return key;
}

/// Sidik jari sebuah kunci, tidak pernah nol.
uint giCacheFingerprint(GiCacheKey key) {
    uint hash = giCacheMix(uint(key.cell.x) * 0x9E3779B9u);
    hash ^= giCacheMix(uint(key.cell.y) * 0x85EBCA6Bu);
    hash ^= giCacheMix(uint(key.cell.z) * 0xC2B2AE35u);
    hash ^= giCacheMix((key.face << 8) | key.level);
    hash = giCacheMix(hash);
    return hash == 0u ? 1u : hash;
}

uint giCacheSlot(GiCacheKey key) {
    uint capacity = uint(shadowParams.cacheParams.x);
    // Slot dipilih dari sidik jari yang dicampur sekali lagi: memakai sidik
    // jarinya langsung membuat entri yang bertetangga slotnya juga bertetangga
    // sidik jarinya, dan perbandingan sidik jari lalu kehilangan sebagian
    // dayanya membedakan.
    return giCacheMix(giCacheFingerprint(key) ^ 0x27D4EB2Fu) & (capacity - 1u);
}

/// Radiansi sebuah titik permukaan.
///
/// **Mengembalikan false untuk entri yang belum pernah diisi, bukan hitam.**
/// Pelajaran yang sama dengan sinar SDF di M3: nilai yang tidak diketahui dan
/// nilai nol adalah dua hal berbeda.
bool giCacheQuery(float3 position, float3 normal, float3 cameraPosition, out float3 radiance) {
    radiance = float3(0.0);
    uint capacity = uint(shadowParams.cacheParams.x);
    if (capacity == 0u) {
        return false;
    }
    GiCacheKey key = giCacheKey(position, normal, cameraPosition);
    uint fingerprint = giCacheFingerprint(key);
    uint start = giCacheSlot(key);
    uint maxProbe = uint(shadowParams.cacheParams.w);

    for (uint step = 0u; step < maxProbe; ++step) {
        uint slot = (start + step) & (capacity - 1u);
        uint stored = giCache[slot].fingerprint;
        if (stored == 0u) {
            // Slot kosong: penyisipan selalu mengisi slot kosong pertama yang
            // ditemuinya, jadi tidak ada yang bisa bersembunyi di belakangnya.
            return false;
        }
        if (stored == fingerprint) {
            radiance = giCache[slot].radiance;
            return giCache[slot].samples > 0u;
        }
    }
    return false;
}

/// Menambahkan satu sampel radiansi.
///
/// **Slotnya direbut dengan atomik; isinya ditulis biasa.** Merebut slot harus
/// atomik — dua invokasi yang mengklaim slot yang sama secara bersamaan akan
/// menghasilkan dua entri yang saling menimpa kuncinya. Isinya tidak: yang
/// terburuk yang bisa terjadi adalah satu sampel dari probe lain hilang, dan itu
/// perilaku yang memang melekat pada cache lossy. Atomik float menuntut
/// `VK_EXT_shader_atomic_float`, yaitu syarat perangkat baru untuk menghindari
/// kehilangan yang tidak ada yang bisa melihatnya.
void giCacheInsert(float3 position, float3 normal, float3 cameraPosition, float3 radiance,
                   uint frame) {
    uint capacity = uint(shadowParams.cacheParams.x);
    if (capacity == 0u) {
        return;
    }
    GiCacheKey key = giCacheKey(position, normal, cameraPosition);
    uint fingerprint = giCacheFingerprint(key);
    uint start = giCacheSlot(key);
    uint maxProbe = uint(shadowParams.cacheParams.w);
    uint accumulation = uint(shadowParams.cacheDecay.x);
    uint staleFrames = uint(shadowParams.cacheDecay.y);

    for (uint step = 0u; step < maxProbe; ++step) {
        uint slot = (start + step) & (capacity - 1u);
        uint previous;
        InterlockedCompareExchange(giCache[slot].fingerprint, 0u, fingerprint, previous);
        if (previous == 0u || previous == fingerprint) {
            uint samples = previous == 0u ? 0u : giCache[slot].samples;
            uint weight = min(samples + 1u, max(accumulation, 1u));
            float3 history = previous == 0u ? radiance : giCache[slot].radiance;
            giCache[slot].radiance = history + (radiance - history) / float(weight);
            giCache[slot].samples = weight;
            giCache[slot].lastFrame = frame;
            return;
        }
    }

    // Lintasan kedua: merebut yang basi. Dipisah dari yang pertama karena
    // merebut di sana akan membuang entri hidup yang kebetulan ditemui lebih
    // dulu daripada slot kosong di belakangnya.
    for (uint step = 0u; step < maxProbe; ++step) {
        uint slot = (start + step) & (capacity - 1u);
        if (frame - giCache[slot].lastFrame >= staleFrames) {
            uint previous;
            InterlockedExchange(giCache[slot].fingerprint, fingerprint, previous);
            giCache[slot].radiance = radiance;
            giCache[slot].samples = 1u;
            giCache[slot].lastFrame = frame;
            return;
        }
    }
}

#endif
