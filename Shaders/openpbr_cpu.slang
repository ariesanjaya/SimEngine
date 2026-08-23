// Pembungkus `openpbr.slang` supaya model shading-nya bisa dijalankan di CPU.
//
// **Tidak ada satu baris matematika pun di berkas ini, dan itu seluruh
// gunanya.** R4 menuntut acuan yang menilai OpenPBR, dan acuan itu butuh
// kembaran CPU dari model shading-nya. Menuliskan kembarannya dengan tangan —
// 920 baris, termasuk sheen LTC Zeltner, EON beserta albedo terarahnya, F82,
// dan kompensasi energi GGX — berarti dua implementasi yang harus dijaga tetap
// sama selamanya. Yang meleset di salah satunya bukan acuan melainkan renderer
// kedua yang kebetulan mirip, dan selisihnya akan terbaca sebagai galat GI
// padahal asalnya BSDF.
//
// `slangc -target cpp` memancarkan C++ dari sumber yang sama yang dijalankan
// GPU. Drift karena itu bukan sesuatu yang diuji, melainkan sesuatu yang tidak
// bisa terjadi.
//
// Bentuknya entry point compute karena itulah bentuk yang bisa dipancarkan
// slangc ke CPU; ia tidak pernah dijalankan di GPU.

#include "openpbr.slang"

/// Satu permukaan pada satu titik, dengan satu arah pandang dan satu arah
/// cahaya. Bingkai diserahkan utuh alih-alih diturunkan di sini: uji kesamaan
/// harus bisa menaruh sumbu di tempat yang dipilihnya sendiri, termasuk yang
/// tidak akan pernah dihasilkan mesh sungguhan.
/// **Nama medannya sengaja tidak sama dengan `ShadingFrame`.** slangc
/// membedakan nama yang bertabrakan dengan akhiran angka saat memancarkan C++,
/// jadi sebuah medan bernama `normal` keluar sebagai `normal_1` sementara yang
/// tidak bertabrakan keluar sebagai `_0`. Nama yang berdiri sendiri membuat
/// akhirannya seragam, dan sisi C++ berhenti bergantung pada urutan
/// penamaan generatornya.
struct OpenPbrQuery {
    float3 shadingNormal;
    float3 shadingTangent;
    float3 shadingBitangent;
    float3 viewDirection;

    float3 coatNormal;
    float3 coatTangent;
    float3 coatBitangent;

    /// Menunjuk **dari permukaan ke cahaya**, sama seperti `evaluateOpenPBR`.
    float3 light;
    float3 radiance;

    /// Untuk jalur lingkungan. Diserahkan sudah jadi, dengan alasan yang sama
    /// seperti di `evaluateOpenPBR_IBL`.
    float3 irradiance;
    float3 prefilteredBase;
    float3 prefilteredCoat;
    float2 dfg;

    OpenPBRSurface surface;
};

StructuredBuffer<OpenPbrQuery> gQueries;
RWStructuredBuffer<float3> gDirect;
RWStructuredBuffer<float3> gAmbient;

ShadingFrame frameOf(OpenPbrQuery q) {
    ShadingFrame frame;
    frame.normal = q.shadingNormal;
    frame.tangent = q.shadingTangent;
    frame.bitangent = q.shadingBitangent;
    frame.view = q.viewDirection;
    return frame;
}

ShadingFrame coatFrameOf(OpenPbrQuery q) {
    ShadingFrame frame;
    frame.normal = q.coatNormal;
    frame.tangent = q.coatTangent;
    frame.bitangent = q.coatBitangent;
    frame.view = q.viewDirection;
    return frame;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void evaluateQueries(uint3 tid : SV_DispatchThreadID) {
    const OpenPbrQuery q = gQueries[tid.x];
    const ShadingFrame frame = frameOf(q);
    const ShadingFrame coat = coatFrameOf(q);

    gDirect[tid.x] = evaluateOpenPBR(q.surface, frame, coat, q.light, q.radiance);
    gAmbient[tid.x] = evaluateOpenPBR_IBL(q.surface, frame, coat, q.irradiance,
                                          q.prefilteredBase, q.prefilteredCoat, q.dfg);
}
