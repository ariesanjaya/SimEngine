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

// --- visibilitas arah (S3) ---------------------------------------------------
//
// **Tiap probe membawa jaraknya ke geometri, bukan hanya iradiansinya.** Tanpa
// itu tidak ada cara membedakan probe yang berada *di depan* dinding dari probe
// yang berada *di dalamnya* — dan yang kedua dipanggang tepat nol lalu bocor ke
// permukaan di dekatnya. Itu bukan dugaan: pada `bench.simlevel`, 246 dari
// 21.312 probe jatuh di dalam benda pejal pada jarak 1 m, dan riaknya di layar
// tumbuh 3,69 kali dibandingkan kisi yang tidak punya satu pun.

/// Sisi peta kedalaman oktahedral tiap probe.
///
/// **Delapan, dan itu bukan angka bulat yang kebetulan.** Yang disimpan bukan
/// bentuk geometri melainkan rata-rata jarak beserta kuadratnya, dan uji
/// Chebyshev yang membacanya hanya menuntut arah kasar: peta yang lebih halus
/// menyimpan ketelitian yang tidak pernah terbaca. 8² = 64 texel per probe, dua
/// float masing-masing — lima kali SH-nya, dan itu harga yang memang dibayar
/// untuk berhenti membocorkan cahaya menembus dinding.
static const uint kProbeDepthSize = 8;

/// Arah unit → koordinat oktahedral di [0,1]².
///
/// **Oktahedral, bukan kubus.** Enam muka berarti enam tepi yang harus dijepit
/// terpisah dan enam kali padding; oktahedral memetakan seluruh bola ke satu
/// petak persegi dengan kerapatan yang hampir merata. Yang dibayar cuma
/// aritmatika lipatan di bawah ini.
float2 probeOctEncode(float3 direction) {
    const float3 d = direction / (abs(direction.x) + abs(direction.y) + abs(direction.z));
    float2 uv = d.xz;
    if (d.y < 0.0) {
        // Belahan bawah dilipat keluar. Tandanya diambil dari uv itu sendiri,
        // dan `sign` pada nol menjawab nol — yang membuat texel di sumbu jatuh
        // ke tempat yang salah. Karena itu nol diperlakukan sebagai positif.
        const float2 s = float2(uv.x >= 0.0 ? 1.0 : -1.0, uv.y >= 0.0 ? 1.0 : -1.0);
        uv = (float2(1.0, 1.0) - abs(float2(d.z, d.x))) * s;
    }
    return uv * 0.5 + 0.5;
}

/// Koordinat oktahedral → arah unit. Kebalikan persis dari yang di atas; dipakai
/// pemanggang untuk tahu arah mana yang diwakili tiap texel.
float3 probeOctDecode(float2 uv) {
    const float2 f = uv * 2.0 - 1.0;
    float3 d = float3(f.x, 1.0 - abs(f.x) - abs(f.y), f.y);
    const float t = max(-d.y, 0.0);
    d.x += d.x >= 0.0 ? -t : t;
    d.z += d.z >= 0.0 ? -t : t;
    return normalize(d);
}

/// Indeks texel peta kedalaman untuk sebuah arah, di dalam peta satu probe.
uint probeDepthTexel(float3 direction) {
    const float2 uv = probeOctEncode(direction);
    const uint2 texel = min(uint2(uv * float(kProbeDepthSize)), uint2(kProbeDepthSize - 1));
    return texel.y * kProbeDepthSize + texel.x;
}

/// Indeks texel **pertama** peta kedalaman sebuah probe. Satu texel membawa
/// pasangan (rata-rata jarak, rata-rata kuadratnya).
uint probeDepthBase(uint slot, uint3 coordinate) {
    const uint3 inside = coordinate % kProbeBrickSize;
    const uint local = (inside.z * kProbeBrickSize + inside.y) * kProbeBrickSize + inside.x;
    const uint probe = slot * (kProbeBrickSize * kProbeBrickSize * kProbeBrickSize) + local;
    return probe * (kProbeDepthSize * kProbeDepthSize);
}

/// Bobot visibilitas sebuah probe untuk titik yang dinaunginya — uji Chebyshev.
///
/// `distance` jarak dari probe ke titik teduh; `mean` dan `meanSquare` dibaca
/// dari peta kedalaman probe itu pada arah yang sama.
///
/// **Mengembalikan satu ketika titiknya lebih dekat daripada geometri terdekat
/// pada arah itu**, dan turun ketika ia berada di baliknya. Sebuah probe di
/// dalam benda pejal punya `mean` mendekati nol ke segala arah, jadi setiap
/// titik di luar benda itu berada jauh di baliknya — dan bobotnya jatuh ke nol
/// tanpa satu pun cabang khusus tentang "di dalam".
float probeVisibilityWeight(float distance, float mean, float meanSquare) {
    if (distance <= mean) {
        return 1.0;
    }
    // Varians dijaga tidak nol: permukaan datar yang seluruh sampelnya menjawab
    // jarak yang sama menghasilkan varians nol, dan pembagian di sana melompat
    // dari satu ke nol pada satu texel — yang terlihat sebagai tepi keras yang
    // mengikuti kisi, bukan sebagai oklusi.
    const float variance = max(meanSquare - mean * mean, 1e-4);
    const float delta = distance - mean;
    const float chebyshev = variance / (variance + delta * delta);
    // Dipangkatkan tiga supaya peralihannya tidak terlalu landai; angka yang
    // sama dipakai DDGI, dan yang menggantikannya harus datang dengan gambar.
    return chebyshev * chebyshev * chebyshev;
}
