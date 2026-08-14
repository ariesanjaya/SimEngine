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
    // Skin: empat bone dan bobotnya, per vertex. Lihat `skin_common.slang` —
    // yang dideklarasikan di sini hanyalah atributnya, karena `BoxVertex` yang
    // menjadi masukan setiap tahap vertex.
    //
    // **Ada juga saat mesh-nya tidak ber-skin.** Slang mengunci daftar antarmuka
    // entry point sebelum konstanta spesialisasi dinilai, jadi `kSkinned` yang
    // mati tidak membuang keduanya; pipeline yang tidak menyediakannya adalah
    // galat validasi di setiap draw. Jalur tak-ber-skin memasangnya lewat
    // binding ber-stride nol.
    [[vk::location(8)]] uint4 boneIndices;
    [[vk::location(9)]] float4 boneWeights;
    // Awal palet matriks kulit instance ini, per instance. **Per instance dan
    // bukan push constant**: satu ruas draw memuat seluruh instance yang memakai
    // mesh yang sama, dan dua karakter bermesh sama tetap berpose sendiri-sendiri.
    [[vk::location(10)]] uint skinBase;
    // Warna per **vertex**, bukan per instance seperti `color` di atas.
    //
    // Putih adalah nilai satuannya: mesh yang tidak mengisinya tergambar persis
    // seperti sebelum atribut ini ada. Yang memakainya hari ini terrain, yang
    // memadukan warna layernya menurut bobot cat dan memanggangnya ke simpul.
    [[vk::location(11)]] float4 vertexColor;
    // UV. Sudah terunggah ke GPU sejak mesh sungguhan masuk — buffer vertexnya
    // di-`memcpy` dari `MeshVertex` apa adanya — dan yang belum ada selama ini
    // hanya deklarasi ini.
    [[vk::location(12)]] float2 uv;
};

float4x4 instanceModel(BoxVertex vertex) {
    return transpose(float4x4(vertex.row0, vertex.row1, vertex.row2, vertex.row3));
}

#endif
