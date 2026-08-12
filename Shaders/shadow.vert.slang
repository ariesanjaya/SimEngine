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

// **Set 0, bukan set 1 seperti pass forward.** Pass ini sedang menulis peta
// bayangan yang ada di dalam set 0 milik forward, jadi ia tidak boleh mengikat
// set itu sama sekali — layout-nya karena itu hanya berisi palet kulit, dan
// palet itu jatuh ke nomor nol.
#define SKIN_SET 0

#include "skin_common.slang"

struct ShadowPush {
    float4x4 lightViewProj;
    // Tidak dibaca di sini; lihat catatan di prepass.vert.
    float4 partColor;
};

[[vk::push_constant]] ConstantBuffer<ShadowPush> push;

[shader("vertex")]
float4 main(BoxVertex vertex) : SV_Position {
    // Kulit ikut di sini, dan itu wajib bukan tambahan: bayangan yang tetap
    // berdiri di bind pose sementara karakternya bergerak adalah bayangan yang
    // lepas dari pemiliknya — cacat yang jauh lebih mencolok daripada tidak ada
    // bayangan sama sekali.
    float4x4 model = instanceModel(vertex);
    return mul(push.lightViewProj, mul(model, float4(skinPosition(vertex), 1.0)));
}
