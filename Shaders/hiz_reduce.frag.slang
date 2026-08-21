// Satu tingkat piramida HiZ.
//
// **Rumusnya harus sama dengan `HiZPyramid::Build` di sisi C++.** Yang di CPU
// adalah acuan yang diuji; yang di sini yang benar-benar dipakai menelusuri.

struct Push {
    int2 sourceSize;
    // Bukan nol pada tingkat nol: isinya disalin apa adanya dari depth buffer.
    //
    // Tingkat nol beresolusi penuh, bukan setengah. Menyalinnya memang satu pass
    // tambahan, tapi tanpa itu tingkat terhalus penelusuran harus membaca depth
    // buffer sementara tingkat lainnya membaca piramida — dua sumber dengan dua
    // aturan pengalamatan di dalam satu lingkaran, dan selisih di antara
    // keduanya muncul sebagai perpotongan yang meleset satu piksel.
    int copyOnly;
    /// Bukan nol: ringkas dengan **minimum**, yaitu permukaan terjauh.
    ///
    /// **Dua piramida dari satu shader, dan keduanya menjawab pertanyaan yang
    /// berlawanan.** Penelusuran sinar bertanya "seberapa dekat permukaan
    /// terdekat di sel ini" — sebuah sinar boleh melompati sel hanya kalau ia
    /// masih di depan yang terdekat. Occlusion culling bertanya "seberapa jauh
    /// permukaan **terjauh**" — sebuah benda tertutup hanya kalau seluruh
    /// petaknya sudah terisi sesuatu yang lebih dekat, dan yang menentukan
    /// adalah texel yang paling jauh di antaranya.
    int reduceMin;
};

[[vk::push_constant]] ConstantBuffer<Push> pc;
[[vk::binding(0, 0)]] Sampler2D uSource;

[shader("fragment")]
float main(float4 position : SV_Position) : SV_Target {
    int2 destination = int2(position.xy);
    int2 last = pc.sourceSize - int2(1);

    if (pc.copyOnly != 0) {
        return uSource.Load(int3(min(destination, last), 0)).r;
    }

    // **Reversed-Z: yang terbesar adalah yang paling dekat.** Piramida
    // penelusuran mengambil yang terbesar; piramida occlusion mengambil yang
    // terkecil. Lihat catatan di `Push::reduceMin`.
    //
    // Ukuran tingkat ini mengikuti aturan mip Vulkan: dibulatkan ke bawah.
    // Karena itu texel **terakhir** sebuah baris merangkum sisa barisnya — tiga
    // texel sumber, bukan dua, saat ukuran sumbernya ganjil. Tanpa itu, baris
    // dan kolom terakhir hilang sama sekali, dan yang hilang itu tepi layar.
    int2 destinationSize = max(int2(1), pc.sourceSize / 2);
    int2 base = destination * 2;
    int2 high = select(destination + int2(1) == destinationSize, last, base + int2(1));

    // Nilai awal yang netral untuk masing-masing: nol adalah yang terjauh pada
    // reversed-Z, satu yang terdekat.
    float result = pc.reduceMin != 0 ? 1.0 : 0.0;
    for (int y = base.y; y <= high.y; ++y) {
        for (int x = base.x; x <= high.x; ++x) {
            const float value = uSource.Load(int3(x, y, 0)).r;
            result = pc.reduceMin != 0 ? min(result, value) : max(result, value);
        }
    }
    return result;
}
