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
