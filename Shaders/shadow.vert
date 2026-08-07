#version 450

// Pass bayangan: kubus yang di-instance, digambar dari sudut pandang cahaya.
//
// Hanya posisi yang dibaca. Normal, warna, dan seluruh atribut lain tetap
// dideklarasikan supaya tata letak vertex buffer-nya sama persis dengan yang
// dipakai pass forward — satu buffer instance melayani keduanya, dan tata letak
// yang berbeda berarti dua salinan data yang sama yang harus dijaga sinkron.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inRow0;
layout(location = 3) in vec4 inRow1;
layout(location = 4) in vec4 inRow2;
layout(location = 5) in vec4 inRow3;
layout(location = 6) in vec4 inColor;
layout(location = 7) in uint inFlags;

// Tidak ada tahap fragment. Pass ini hanya menulis depth, jadi pipeline-nya
// dibuat tanpa fragment shader sama sekali — jalur depth-only yang lebih cepat,
// dan yang membuat validation layer tidak mengeluh soal shader yang menulis ke
// lampiran warna yang tidak dipasang.
layout(push_constant) uniform Push {
    mat4 lightViewProj;
} push;

void main() {
    mat4 model = mat4(inRow0, inRow1, inRow2, inRow3);
    gl_Position = push.lightViewProj * model * vec4(inPosition, 1.0);
}
