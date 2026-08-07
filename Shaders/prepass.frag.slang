// Depth prepass, tahap fragment: menulis normal permukaan terdepan dan tidak
// satu pun hal lain.
//
// **Prepass sebelumnya berjalan tanpa tahap fragment sama sekali**, dan itu
// bentuk yang benar selama tidak ada yang membutuhkan keluarannya. Screen probe
// membutuhkannya: penempatan probe dan bobot interpolasinya sama-sama menuntut
// normal per piksel dari permukaan yang benar-benar terlihat, dan menurunkannya
// dari turunan depth menghasilkan normal yang ngawur tepat di siluet — yaitu
// tempat probe paling sering salah tempat.
//
// Yang hilang karena itu adalah jalur depth-only yang lebih cepat. Yang didapat
// adalah normal tanpa satu pun draw tambahan: geometrinya sudah digambar di
// sini, dan yang bertambah hanya satu penyandian oktahedral per fragmen.
//
// Disandikan oktahedral ke dua kanal, bukan disimpan sebagai tiga. Normal
// satuan hanya punya dua derajat kebebasan, dan dua kanal 16-bit menyimpannya
// jauh lebih teliti daripada tiga kanal 8-bit dengan ukuran yang sama.

#include "gi_common.slang"

struct PrepassVarying {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 normal;
};

[shader("fragment")]
float2 main(PrepassVarying input) : SV_Target {
    return octEncode(normalize(input.normal));
}
