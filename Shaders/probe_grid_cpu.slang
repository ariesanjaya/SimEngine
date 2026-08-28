// Pembungkus `probe_grid.slang` supaya pengalamatannya bisa dijalankan di CPU.
//
// **Tidak ada satu baris aritmatika pun di berkas ini**, dengan alasan yang
// sama persis seperti `openpbr_cpu.slang`: yang diuji harus berkas yang
// benar-benar dipakai GPU, bukan tiruannya.
//
// Bentuknya entry point compute karena itulah bentuk yang bisa dipancarkan
// slangc ke CPU; ia tidak pernah dijalankan di GPU.

#include "probe_grid.slang"

struct ProbeAddressQuery {
    float3 position;
    float3 origin;
    float spacing;
    uint3 counts;
    /// Slot brick yang diandaikan, supaya offset koefisiennya ikut terperiksa.
    uint slot;
};

/// Query kedua: pemetaan oktahedral dan uji Chebyshev-nya (S3).
struct ProbeVisibilityQuery {
    float3 direction;
    float distance;
    float mean;
    float meanSquare;
};

/// **Nama medannya sengaja tidak bertabrakan dengan `ProbeAddressResult`.**
/// slangc membedakan nama yang sama dengan akhiran angka saat memancarkan C++,
/// jadi `weight` di sini keluar sebagai `weight_1` sementara yang di sana
/// `weight_0` — dan sisi C++ berhenti bisa menyebut medannya tanpa tahu urutan
/// penamaan generatornya. Catatan yang sama ada di `openpbr_cpu.slang`.
struct ProbeVisibilityResult {
    float2 octEncoded;
    float3 octDecoded;
    uint depthTexel;
    float visibility;
    uint depthOffset;
    float3 biased;
    float bias;
};

struct ProbeAddressResult {
    uint3 base;
    float3 fraction;
    /// Delapan sudut: indeks brick, offset koefisien, dan bobotnya.
    uint brickIndex[8];
    uint coefficientBase[8];
    float weight[8];
};

StructuredBuffer<ProbeAddressQuery> gQueries;
RWStructuredBuffer<ProbeAddressResult> gResults;
StructuredBuffer<ProbeVisibilityQuery> gVisibility;
RWStructuredBuffer<ProbeVisibilityResult> gVisibilityResults;

[shader("compute")]
[numthreads(1, 1, 1)]
void addressQueries(uint3 tid : SV_DispatchThreadID) {
    const ProbeAddressQuery q = gQueries[tid.x];

    ProbeAddressResult r;
    probeCell(q.position, q.origin, q.spacing, q.counts, r.base, r.fraction);
    for (uint corner = 0; corner < 8; ++corner) {
        const uint3 coordinate = probeCorner(r.base, corner, q.counts);
        r.brickIndex[corner] = probeBrickIndex(q.counts, coordinate);
        r.coefficientBase[corner] = probeCoefficientBase(q.slot, coordinate);
        r.weight[corner] = probeCornerWeight(corner, r.fraction);
    }
    gResults[tid.x] = r;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void visibilityQueries(uint3 tid : SV_DispatchThreadID) {
    const ProbeVisibilityQuery q = gVisibility[tid.x];

    ProbeVisibilityResult r;
    r.octEncoded = probeOctEncode(q.direction);
    r.octDecoded = probeOctDecode(r.octEncoded);
    r.depthTexel = probeDepthTexel(q.direction);
    r.visibility = probeVisibilityWeight(q.distance, q.mean, q.meanSquare);
    r.depthOffset = probeDepthBase(3u, uint3(5u, 2u, 7u));
    r.biased = probeBiasedPosition(float3(1.0, 2.0, 3.0), q.direction);
    r.bias = kProbeVisibilityBias;
    gVisibilityResults[tid.x] = r;
}
