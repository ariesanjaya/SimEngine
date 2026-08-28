// Pengalamatan kisi probe iradiansi — S2 di docs/PLAN-STATIC-GI.md.
//
// **Tidak ada satu pun pembacaan buffer di berkas ini, dan itu seluruh
// gunanya.** Yang tinggal di sini hanya aritmatika: brick mana yang memuat
// sebuah probe, di mana probe itu di dalam brick-nya, di offset berapa
// koefisiennya, dan sel mana beserta bobot trilinear mana yang berlaku untuk
// sebuah titik. Itu bagian yang salahnya paling mahal dan paling sunyi — indeks
// yang meleset satu brick tetap mengembalikan angka yang masuk akal, dan yang
// terlihat cuma cahaya tak-langsung yang bergeser beberapa meter.
//
// **Berkas ini juga dikompilasi ke C++ lewat `slangc -target cpp`**
// (`probe_grid_cpu.slang`), lalu diadu dengan sisi C++ yang menulis artefaknya.
// Polanya sama persis dengan `openpbr_cpu.slang`, dan alasannya sama: dua
// implementasi dari satu pengalamatan akan berselisih, dan selisihnya akan
// terbaca sebagai transport yang salah alih-alih sebagai indeks yang salah.

/// Probe per sumbu di dalam satu brick. Harus sama dengan
/// `ProbeVolumeLayout::kBrickSize`.
static const uint kProbeBrickSize = 4;

/// Jumlah brick tiap sumbu untuk kisi sebesar `counts`.
uint3 probeBrickCounts(uint3 counts) {
    return (counts + (kProbeBrickSize - 1)) / kProbeBrickSize;
}

/// Indeks linear brick yang memuat sebuah probe, urutan x-cepat.
uint probeBrickIndex(uint3 counts, uint3 coordinate) {
    const uint3 bricks = probeBrickCounts(counts);
    const uint3 brick = coordinate / kProbeBrickSize;
    return (brick.z * bricks.y + brick.y) * bricks.x + brick.x;
}

/// Indeks koefisien **pertama** sebuah probe di dalam larik SH, bila brick-nya
/// menempati `slot`.
///
/// Sembilan koefisien berurutan per probe; satu brick menempati 4³ probe
/// berurutan, urutan x-cepat di dalamnya.
uint probeCoefficientBase(uint slot, uint3 coordinate) {
    const uint3 inside = coordinate % kProbeBrickSize;
    const uint local = (inside.z * kProbeBrickSize + inside.y) * kProbeBrickSize + inside.x;
    return (slot * (kProbeBrickSize * kProbeBrickSize * kProbeBrickSize) + local) * 9;
}

/// Sel kisi yang memuat sebuah titik dunia, beserta pecahannya.
///
/// Titik di luar kisi dijepit ke tepinya, bukan dijawab nol: permukaan yang
/// duduk tepat di batas adegan akan mengambil sampel di luar, dan nol di tepi
/// terbaca sebagai garis gelap yang mengikuti dindingnya.
void probeCell(float3 position, float3 origin, float spacing, uint3 counts,
               out uint3 base, out float3 fraction) {
    const float3 grid = (position - origin) / max(spacing, 1e-4);
    const float3 last = float3(counts) - float3(1.0);
    const float3 clamped = clamp(grid, float3(0.0), last);
    const float3 floored = floor(clamped);
    base = uint3(floored);
    fraction = clamped - floored;
}

/// Koordinat probe sebuah sudut sel, dijepit ke dalam kisi.
uint3 probeCorner(uint3 base, uint corner, uint3 counts) {
    const uint3 offset = uint3(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
    return min(base + offset, counts - uint3(1u));
}

/// Bobot trilinear sebuah sudut sel.
float probeCornerWeight(uint corner, float3 fraction) {
    const float3 offset = float3(float(corner & 1u), float((corner >> 1u) & 1u),
                                 float((corner >> 2u) & 1u));
    const float3 blend = lerp(float3(1.0) - fraction, fraction, offset);
    return blend.x * blend.y * blend.z;
}
