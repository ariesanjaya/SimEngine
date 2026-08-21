// Push constant yang dipakai bersama forward, prepass, dan bayangan.
//
// **Satu deklarasi, bukan empat.** Ketiga pass memang sudah wajib memakai tata
// letak yang sama — pipeline layout-nya harus *compatible* supaya set 0..1 yang
// sudah terikat tidak ikut terlepas saat pipeline material diikat di tengah
// gelung ruas — dan sampai G5 kesepakatan itu dijaga oleh tiga salinan struct
// beserta komentar yang saling menunjuk. Salinan keempat, di tahap fragment,
// adalah yang membuatnya tidak lagi layak: sejak bindless, yang membaca push
// constant bukan cuma tahap vertex.
//
// Harus sama persis dengan `BoxPush` di VulkanRenderer.cpp.

#ifndef BOX_PUSH_SLANG
#define BOX_PUSH_SLANG

struct BoxPush {
    /// `viewProj` untuk forward dan prepass, `lightViewProj` untuk pass
    /// bayangan. Satu medan, karena yang dilakukan keduanya sama: memindahkan
    /// ruang dunia ke ruang klip milik pengamatnya masing-masing.
    float4x4 viewProj;
};

[[vk::push_constant]] ConstantBuffer<BoxPush> push;

#endif
