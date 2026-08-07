// Piramida HiZ, dideklarasikan sekali.
//
// **Berkas sendiri, bukan di `gi_common.slang`.** Penyaring probe memakai set
// descriptor-nya sendiri dengan empat binding, dan menyisipkan binding 10 ke
// dalam berkas yang ikut disertakannya akan membuat tata letak yang dideklarasi
// shader berbeda dari yang dibuat C++ — ketidakcocokan yang hanya dilaporkan
// validation layer.

#ifndef GI_DEPTH_SLANG
#define GI_DEPTH_SLANG

/// Tingkat nol adalah salinan penuh depth buffer viewport.
[[vk::binding(10, 0)]] Sampler2D hiZ;

#endif
