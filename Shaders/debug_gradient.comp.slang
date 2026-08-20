// Pass compute paling sepele yang masih membuktikan sesuatu (G3).
//
// **Bukan sebuah efek melainkan sebuah pemeriksaan.** Yang dibuktikan gambar
// ini bukan "gradiennya bagus" melainkan bahwa seluruh jalur compute hidup:
// akhiran `.comp` sampai ke slangc, SPIR-V-nya menjadi `VkPipeline` lewat cache
// yang sama dengan pipeline grafis, storage image-nya berpindah layout lewat
// barrier yang disimpulkan frame graph, dan hasilnya sampai ke layar. Kalau
// salah satu dari itu putus, yang terlihat adalah layar hitam — bukan gambar
// yang benar dengan diam-diam satu langkah dilewati.
//
// **Garis di batas grup kerja ada karena kesalahan yang paling mahal di sini
// tidak terlihat di tengah gambar.** Jumlah grup dibulatkan ke atas, jadi
// dispatch terakhir selalu melampaui petaknya; pembulatan yang salah atau uji
// batas yang salah menyisakan pita hitam di tepi kanan dan bawah saja. Garis
// yang jaraknya persis selebar satu grup membuat lebar itu bisa dihitung dengan
// mata, dan pita yang hilang bisa dilihat tanpa membandingkan dengan apa pun.

struct Push {
    /// Petak yang harus diisi, piksel. Lebih kecil daripada image-nya: target
    /// dialokasikan lebih besar daripada viewport supaya menyeret pemisah dock
    /// tidak mengalokasi ulang tiap frame.
    uint2 size;
};

[[vk::push_constant]] ConstantBuffer<Push> pc;

// **Formatnya disebut, bukan dibiarkan `Unknown`.** Tanpa atribut ini Slang
// memancarkan `StorageImageWriteWithoutFormat`, dan kapabilitas itu menuntut
// fitur perangkat `shaderStorageImageWriteWithoutFormat` yang tidak diminta
// siapa pun di sini — kegagalannya muncul sebagai shader yang ditolak driver,
// jauh dari sebabnya.
[format("rgba8")]
[[vk::binding(0, 0)]] RWTexture2D<float4> uOut;

// Harus sama dengan `ComputeGradient::kGroupSize` di sisi C++: yang menghitung
// banyaknya grup adalah pemanggil, dan ia menghitungnya dari angka itu.
[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    if (thread.x >= pc.size.x || thread.y >= pc.size.y) {
        return;
    }
    const float2 uv = (float2(thread.xy) + 0.5) / float2(pc.size);
    float3 color = float3(uv, 0.35);
    if (thread.x % 8 == 0 || thread.y % 8 == 0) {
        color *= 0.55;
    }
    uOut[thread.xy] = float4(color, 1.0);
}
