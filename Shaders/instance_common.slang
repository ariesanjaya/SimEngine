// Atribut per-instance kubus, dipakai pass forward, prepass, dan bayangan.
//
// **Transform instance ada di storage buffer, bukan di atribut vertex.** Ia
// pindah ke sini supaya pipeline material — yang memang membacanya dari buffer —
// bisa menggantikan `box.frag` tanpa mengubah cara instance dikumpulkan. Yang
// ikut hilang: 64 byte per instance di aliran atribut, dan empat lokasi atribut
// yang harus disediakan setiap pipeline yang tidak memakainya.
//
// **Lokasi 2..5 sengaja dibiarkan kosong.** Menomori ulang atribut sesudahnya
// akan menggeser normal, warna, dan uv ke lokasi yang berbeda dari yang tertulis
// di sisi C++ — dan yang terjadi bukan galat validasi melainkan mesh yang
// membaca uv sebagai warna.
//
// **Matriksnya diunggah apa adanya, tanpa transpose di sisi mana pun.** Slang
// mendekorasi `float4x4` di dalam StructuredBuffer dengan `RowMajor` sementara
// `glm::mat4` menyimpan kolom demi kolom, dan kedua kata itu terbaca
// bertentangan. Yang menyelesaikannya bukan penalaran melainkan bukti yang sudah
// ada: `push.viewProj` diunggah sebagai `glm::mat4` mentah, mendapat dekorasi
// yang sama persis, dan `mul` pada keduanya menjadi `OpVectorTimesMatrix` yang
// sama — dan jalur itu sudah menggambar dengan benar sejak E8.1.

#ifndef INSTANCE_COMMON_SLANG
#define INSTANCE_COMMON_SLANG

struct BoxVertex {
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 normal;
    // Lokasi 2..5 dulu memuat kolom matriks model; lihat catatan di atas.
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

#ifndef INSTANCE_SET
#error "INSTANCE_SET harus didefinisikan sebelum meng-include instance_common.slang"
#endif

/// Transform dunia tiap instance, sejajar dengan aliran atribut instance.
[[vk::binding(1, INSTANCE_SET)]] StructuredBuffer<float4x4> instanceTransforms;

/// Transform instance ke-`index`.
///
/// **Indeksnya bukan `SV_InstanceID` apa adanya.** slangc menerjemahkan
/// `SV_InstanceID` menjadi `gl_InstanceIndex - gl_BaseInstance`, jadi ia mulai
/// dari nol pada setiap draw — sementara atribut instance di sebelahnya diambil
/// menurut `gl_InstanceIndex`, yaitu **termasuk** `firstInstance`. Pemanggil
/// menambahkan basisnya lewat push constant supaya keduanya menunjuk instance
/// yang sama; tanpa itu, setiap ruas sesudah yang pertama memakai transform
/// milik ruas pertama.
float4x4 instanceModel(uint index) {
    return instanceTransforms[index];
}

#endif
