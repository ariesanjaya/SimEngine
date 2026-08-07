// Pass bayangan: kubus yang di-instance, digambar dari sudut pandang cahaya.
//
// Hanya posisi yang dibaca. Normal, warna, dan seluruh atribut lain tetap
// dideklarasikan supaya tata letak vertex buffer-nya sama persis dengan yang
// dipakai pass forward — satu buffer instance melayani keduanya, dan tata letak
// yang berbeda berarti dua salinan data yang sama yang harus dijaga sinkron.
//
// Tidak ada tahap fragment. Pass ini hanya menulis depth, jadi pipeline-nya
// dibuat tanpa fragment shader sama sekali — jalur depth-only yang lebih cepat,
// dan yang membuat validation layer tidak mengeluh soal shader yang menulis ke
// lampiran warna yang tidak dipasang.

#include "instance_common.slang"

struct ShadowPush {
    float4x4 lightViewProj;
};

[[vk::push_constant]] ConstantBuffer<ShadowPush> push;

[shader("vertex")]
float4 main(BoxVertex vertex) : SV_Position {
    float4x4 model = instanceModel(vertex);
    return mul(push.lightViewProj, mul(model, float4(vertex.position, 1.0)));
}
