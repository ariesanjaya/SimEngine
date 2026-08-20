// Larik tekstur bindless — set 2, binding 1 dan 2.
//
// **Nomor binding-nya sama persis dengan jalur mundur**, dan itu disengaja:
// yang berbeda antara kedua jalur hanyalah larik atau bukan larik. Set 2 tetap
// wilayah material; binding 0 di sana milik blok parameter, dan pada jalur
// bindless ia sendiri sebuah larik — ditulis kompiler graph, bukan di sini,
// karena tipe bloknya berbeda tiap material.
//
// Harus sama dengan `CreateBindlessDescriptors` di VulkanRenderer.cpp.

#ifndef BINDLESS_COMMON_SLANG
#define BINDLESS_COMMON_SLANG

// Tanpa ukuran: kapasitas sesungguhnya ditetapkan descriptor set layout, yang
// menurunkannya dari batas perangkat. Menuliskan angkanya di sini berarti sebuah
// angka yang harus sepakat dengan sisi C++ tanpa ada yang memeriksanya — dan
// larik yang lebih pendek daripada layout-nya tidak menghasilkan galat, hanya
// tekstur yang terbaca sebagai tekstur lain.
[[vk::binding(1, 2)]] Texture2D<float4> gBindlessTextures[];
[[vk::binding(2, 2)]] SamplerState gBindlessSamplers[];

/// Menyampel slot bindless `slot`.
///
/// `NonUniformResourceIndex` dipakai walaupun indeksnya *hari ini* selalu
/// seragam per draw — ia datang dari push constant atau dari blok parameter
/// material. Ia menjadi tidak seragam pada hari G6 mendarat: indirect draw
/// menggabungkan banyak ruas ke dalam satu draw, dan indeksnya lalu datang dari
/// data per-instance. Menandainya sekarang tidak berbiaya apa pun pada indeks
/// yang seragam, sementara melupakannya nanti menghasilkan tekstur milik benda
/// lain pada sebagian lane — tanpa satu pun galat.
float4 sampleBindless(uint slot, float2 uv) {
    const uint index = NonUniformResourceIndex(slot);
    return gBindlessTextures[index].Sample(gBindlessSamplers[index], uv);
}

#endif
