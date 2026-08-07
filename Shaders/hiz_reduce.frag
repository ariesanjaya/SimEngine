#version 450

// Satu tingkat piramida HiZ.
//
// **Rumusnya harus sama dengan `HiZPyramid::Build` di sisi C++.** Yang di CPU
// adalah acuan yang diuji; yang di sini yang benar-benar dipakai menelusuri.

layout(push_constant) uniform Push {
    ivec2 sourceSize;
    // Bukan nol pada tingkat nol: isinya disalin apa adanya dari depth buffer.
    //
    // Tingkat nol beresolusi penuh, bukan setengah. Menyalinnya memang satu pass
    // tambahan, tapi tanpa itu tingkat terhalus penelusuran harus membaca depth
    // buffer sementara tingkat lainnya membaca piramida — dua sumber dengan dua
    // aturan pengalamatan di dalam satu lingkaran, dan selisih di antara
    // keduanya muncul sebagai perpotongan yang meleset satu piksel.
    int copyOnly;
} pc;

layout(set = 0, binding = 0) uniform sampler2D uSource;

layout(location = 0) out float outDepth;

void main() {
    ivec2 destination = ivec2(gl_FragCoord.xy);
    ivec2 last = pc.sourceSize - ivec2(1);

    if (pc.copyOnly != 0) {
        outDepth = texelFetch(uSource, min(destination, last), 0).r;
        return;
    }

    // **Reversed-Z: yang diambil yang TERBESAR.** Depth terbesar adalah
    // permukaan yang paling dekat ke kamera, dan itulah yang harus diketahui
    // penelusur — sebuah sel boleh dilompati hanya kalau sinarnya masih di depan
    // permukaan terdekat di dalamnya.
    //
    // Ukuran tingkat ini mengikuti aturan mip Vulkan: dibulatkan ke bawah.
    // Karena itu texel **terakhir** sebuah baris merangkum sisa barisnya — tiga
    // texel sumber, bukan dua, saat ukuran sumbernya ganjil. Tanpa itu, baris
    // dan kolom terakhir hilang sama sekali, dan yang hilang itu tepi layar.
    ivec2 destinationSize = max(ivec2(1), pc.sourceSize / 2);
    ivec2 base = destination * 2;
    ivec2 high = mix(base + ivec2(1), last, equal(destination + ivec2(1), destinationSize));

    float closest = 0.0;
    for (int y = base.y; y <= high.y; ++y) {
        for (int x = base.x; x <= high.x; ++x) {
            closest = max(closest, texelFetch(uSource, ivec2(x, y), 0).r);
        }
    }
    outDepth = closest;
}
