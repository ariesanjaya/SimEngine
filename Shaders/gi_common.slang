// Rumus screen probe yang dipakai bersama beberapa pass.
//
// **Seluruh isinya harus sama persis dengan `ScreenProbe.cpp`.** Yang di CPU
// adalah acuan yang diuji — termasuk uji tungku yang menuntut radiance seragam
// menghasilkan iradiansi tepat π kali radiance-nya — dan yang di sini yang
// benar-benar dipakai menggambar. Dua rumus yang ditulis terpisah berselisih
// tepat di kasus yang paling sulit dilihat.

#ifndef GI_COMMON_SLANG
#define GI_COMMON_SLANG

float giSignNotZero(float value) { return value >= 0.0 ? 1.0 : -1.0; }

/// Arah satuan -> kotak [0,1]².
float2 octEncode(float3 direction) {
    float sum = abs(direction.x) + abs(direction.y) + abs(direction.z);
    if (sum < 1e-20) {
        return float2(0.5);
    }
    float3 n = direction / sum;
    float2 p = n.xy;
    if (n.z < 0.0) {
        // Setengah bola bawah dilipat ke luar kotak dalamnya, memenuhi keempat
        // sudutnya — itu yang membuat seluruh bola muat tanpa daerah kosong.
        p = float2((1.0 - abs(n.y)) * giSignNotZero(n.x),
                   (1.0 - abs(n.x)) * giSignNotZero(n.y));
    }
    return p * 0.5 + 0.5;
}

/// Kotak [0,1]² -> arah satuan.
float3 octDecode(float2 uv) {
    float2 f = uv * 2.0 - 1.0;
    float3 n = float3(f, 1.0 - abs(f.x) - abs(f.y));
    float fold = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -fold : fold;
    n.y += n.y >= 0.0 ? -fold : fold;
    return normalize(n);
}

/// Titik ke-`index` dari urutan Hammersley 2D berukuran `count`.
float2 giHammersley(uint index, uint count) {
    uint bits = index;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float2(float(index) / float(max(count, 1u)), float(bits) * 2.3283064365386963e-10);
}

/// Pergeseran tetap sebuah probe, di [0,1)². Rotasi Cranley-Patterson.
float2 giProbeOffset(uint probeIndex) {
    uint hash = probeIndex * 0x9E3779B9u;
    hash ^= hash >> 15u;
    hash *= 0x85EBCA6Bu;
    hash ^= hash >> 13u;
    return float2(float(hash & 0xFFFFu) / 65536.0, float((hash >> 16u) & 0xFFFFu) / 65536.0);
}

/// Arah ray ke-`ray` sebuah probe pada frame ke-`frame`.
///
/// Satu ray per sel oktahedral, jadi keenam belasnya menutupi seluruh bola tiap
/// frame; yang berjitter hanya letaknya di dalam selnya.
float3 giProbeRayDirection(uint ray, uint frame, uint probeIndex, uint raysPerAxis,
                           uint accumulationFrames) {
    uint axis = max(raysPerAxis, 1u);
    uint index = ray % (axis * axis);
    float2 cell = float2(float(index % axis), float(index / axis));
    float2 jitter = giHammersley(frame, accumulationFrames) + giProbeOffset(probeIndex);
    return octDecode((cell + frac(jitter)) / float(axis));
}

/// Empat basis SH pertama pada sebuah arah.
float4 giShBasis(float3 direction) {
    return float4(0.282095, 0.488603 * direction.y, 0.488603 * direction.z,
                  0.488603 * direction.x);
}

/// Faktor konvolusi cosinus per orde: π untuk orde nol, 2π/3 untuk orde satu.
static const float4 kGiCosineConvolution =
    float4(3.14159265, 2.09439510, 2.09439510, 2.09439510);

/// Bobot sebuah probe untuk sebuah piksel.
///
/// **Jaraknya diukur tegak lurus bidang piksel, bukan sebagai jarak lurus.**
/// Dua titik di lantai yang sama bisa berjarak beberapa meter dan tetap
/// merupakan permukaan yang sama; dua titik yang berjarak sepuluh sentimeter
/// tapi terpisah dinding bukan. Memakai jarak lurus membuat cahaya merembes
/// menembus sudut ruangan.
///
/// Cermin `ProbeWeight` di `ScreenProbe.cpp`.
float giProbeWeight(float3 probePosition, float3 probeNormal, float3 pixelPosition,
                    float3 pixelNormal, float planeDistance, float normalCosine) {
    float cosine = dot(probeNormal, pixelNormal);
    if (cosine < normalCosine) {
        return 0.0;
    }
    float plane = abs(dot(probePosition - pixelPosition, pixelNormal));
    if (plane > planeDistance) {
        return 0.0;
    }
    // Melandai, bukan memotong tegak. Bobot yang melompat dari satu ke nol
    // membuat batas tempat sebuah probe mulai diabaikan terlihat sebagai garis.
    float planeWeight = 1.0 - plane / max(planeDistance, 1e-6);
    float normalWeight = (cosine - normalCosine) / max(1.0 - normalCosine, 1e-6);
    return planeWeight * normalWeight;
}

/// Iradiansi pada sebuah normal dari SH orde satu, satu float4 per kanal.
float3 giEvaluateSh(float4 shR, float4 shG, float4 shB, float3 normal) {
    float4 basis = giShBasis(normal) * kGiCosineConvolution;
    return float3(dot(shR, basis), dot(shG, basis), dot(shB, basis));
}

#endif
