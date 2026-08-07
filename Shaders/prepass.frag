#version 450

#include "gi_common.glsl"

// Depth prepass, tahap fragment: menulis normal permukaan terdepan dan tidak
// satu pun hal lain.
//
// **Prepass sekarang punya keluaran, dan itu perubahan yang disengaja.** Ia
// sebelumnya menjalankan shader fragment forward penuh dengan tulis warna
// dimatikan — seluruh pencahayaan dihitung lalu dibuang. Sekarang ia menghitung
// satu normal dan menyimpannya, dan itu justru lebih murah daripada sebelumnya
// sekaligus memberi GI hal yang paling dibutuhkannya: normal per piksel dari
// permukaan yang benar-benar terlihat.
//
// Disandikan oktahedral ke dua kanal, bukan disimpan sebagai tiga. Normal
// satuan hanya punya dua derajat kebebasan, dan dua kanal 16-bit menyimpannya
// jauh lebih teliti daripada tiga kanal 8-bit dengan ukuran yang sama.

layout(location = 0) in vec3 inNormal;

layout(location = 0) out vec2 outNormal;

void main() {
    outNormal = octEncode(normalize(inNormal));
}
