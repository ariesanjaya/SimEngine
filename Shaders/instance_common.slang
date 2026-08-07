// Atribut per-instance kubus, dipakai pass forward, prepass, dan bayangan.
//
// **`row0..row3` sebenarnya KOLOM matriks model.** Namanya warisan dari sisi
// C++, yang menuliskan `model[0..3]` — dan indeks glm memilih kolom, bukan
// baris. GLSL menyusunnya kembali dengan `mat4(a,b,c,d)` yang juga kolom-mayor,
// jadi di sana keduanya cocok tanpa transpose di mana pun.
//
// Slang mengikuti HLSL: `float4x4(a,b,c,d)` menyusunnya sebagai **baris**. Jadi
// di sini transpose-nya harus disebut, dan disebut sekali di satu tempat.
// Melewatkannya tidak menghasilkan galat apa pun — hanya setiap kotak yang
// tergambar di tempat yang salah, dengan orientasi yang salah.

#ifndef INSTANCE_COMMON_SLANG
#define INSTANCE_COMMON_SLANG

struct BoxVertex {
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float4 row0;
    [[vk::location(3)]] float4 row1;
    [[vk::location(4)]] float4 row2;
    [[vk::location(5)]] float4 row3;
    [[vk::location(6)]] float4 color;
    [[vk::location(7)]] uint flags;
};

float4x4 instanceModel(BoxVertex vertex) {
    return transpose(float4x4(vertex.row0, vertex.row1, vertex.row2, vertex.row3));
}

#endif
