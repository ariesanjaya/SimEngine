// Menerapkan LUT aerial perspective ke adegan yang sudah tergambar.
//
// **Blending, bukan salinan ping-pong.** Yang harus terjadi pada tiap piksel
// adalah `warna = warna * transmitansi + hamburan` — dan itu persis persamaan
// blending Vulkan `dst = src * ONE + dst * SRC_ALPHA`, asalkan transmitansinya
// satu skalar yang ditaruh di kanal alfa. Bentuk mana pun yang membaca gambar
// HDR lewat descriptor sambil menulisinya adalah perilaku yang tidak
// terdefinisi, dan menghindarinya menuntut gambar HDR kedua seukuran penuh
// beserta satu salinan setiap frame.
//
// **Piksel langit dilewati.** Depth reversed-Z bernilai nol di bidang jauh, dan
// di sana yang tergambar adalah langit — yang sudah mengintegrasikan seluruh
// atmosfer sampai ke puncaknya. Menambahkan kabut di atasnya berarti menghitung
// udara yang sama dua kali: langit menjadi lebih terang dan lebih pucat justru
// saat kabutnya dinaikkan, yang terlihat sebagai langit yang salah alih-alih
// sebagai kabut yang salah.

struct Push {
    float4x4 invViewProj;
    /// xyz: posisi kamera dalam meter, w: jarak maksimum LUT dalam km.
    float4 camera;
    /// x: pengali radiansi langit, sisanya cadangan.
    float4 params;
};

[[vk::push_constant]] ConstantBuffer<Push> pc;
[[vk::binding(0, 0)]] Sampler3D uAerial;
[[vk::binding(1, 0)]] Sampler2D uDepth;

[shader("fragment")]
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float depth = uDepth.SampleLevel(uv, 0.0).r;
    // Nol berarti bidang jauh pada reversed-Z: tidak ada permukaan di sini.
    if (depth <= 0.0) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    float4 world = mul(pc.invViewProj, float4(uv * 2.0 - 1.0, depth, 1.0));
    float3 surface = world.xyz / world.w;
    // Engine bersatuan meter, atmosfer bersatuan kilometer. Mencampurnya tidak
    // menghasilkan galat apa pun, hanya kabut yang seribu kali terlalu pekat
    // atau seribu kali terlalu tipis.
    float distanceKm = length(surface - pc.camera.xyz) * 0.001;

    // Akar, karena sebaran slice-nya kuadratik. Harus membalik pemetaan di
    // `sky_aerial.frag` dengan tepat: yang meleset menghasilkan kabut yang
    // pekatnya benar pada jarak yang salah.
    float w = sqrt(saturate(distanceKm / max(pc.camera.w, 1e-6)));
    float4 aerial = uAerial.SampleLevel(float3(uv, w), 0.0);

    return float4(aerial.rgb * pc.params.x, saturate(aerial.a));
}
