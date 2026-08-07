// Satu tingkat rantai penurunan log-luminansi: rata-rata 2×2 tepat.
//
// Sumbernya selalu berukuran pangkat dua karena petak awalnya begitu, jadi
// tidak ada kasus ganjil sama sekali — itulah gunanya petak awal berukuran
// tetap. Penjepitan di bawah tetap ada sebagai penjaga, bukan sebagai jalur
// yang dipakai.

struct Push {
    int2 sourceSize;
};

[[vk::push_constant]] ConstantBuffer<Push> pc;
[[vk::binding(0, 0)]] Sampler2D uSource;

[shader("fragment")]
float main(float4 position : SV_Position) : SV_Target {
    int2 base = int2(position.xy) * 2;
    int2 last = pc.sourceSize - int2(1);
    float sum = uSource.Load(int3(min(base, last), 0)).r +
                uSource.Load(int3(min(base + int2(1, 0), last), 0)).r +
                uSource.Load(int3(min(base + int2(0, 1), last), 0)).r +
                uSource.Load(int3(min(base + int2(1, 1), last), 0)).r;
    return sum * 0.25;
}
