// Skybox dari peta lingkungan HDR equirectangular.
//
// **Sumber langit kedua, bukan pengganti.** Atmosfer Bruneton menghitung
// langitnya dari fisika, dan itu yang membuat matahari terbenam benar dan awan
// volumetrik punya udara untuk ditinggali. Yang dibayar untuk itu adalah empat
// pass LUT ditambah raymarch awan. Sebuah HDRI membayar **satu cuplikan
// tekstur** dan sudah berisi awan yang difoto sungguhan — yang tidak bisa
// dilakukannya hanyalah berubah terhadap waktu.
//
// Pemetaan arah→uv-nya harus sama persis dengan `DirectionToEquirectUv` di
// `Code/Render/src/Ibl.cpp`, yang punya ujinya. Dua konvensi yang berbeda di dua
// tempat tidak menghasilkan galat apa pun — hanya langit yang berputar terhadap
// matahari yang menyinari adegan, yang terlihat sebagai bayangan yang arahnya
// aneh.

static const float kPi = 3.14159265358979323846;

struct Push {
    float4x4 invViewProj;
    /// x: pengali radiansi, y: putaran mendatar dalam radian, zw: cadangan.
    float4 params;
};

[[vk::push_constant]] ConstantBuffer<Push> pc;
[[vk::binding(0, 0)]] Sampler2D uEnvironment;

[shader("fragment")]
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    // Arah pandang, sama persis dengan pass langit atmosferik. w = 1 pada bidang
    // dekat: reversed-Z.
    float4 nearPoint = mul(pc.invViewProj, float4(uv * 2.0 - 1.0, 1.0, 1.0));
    float4 farPoint = mul(pc.invViewProj, float4(uv * 2.0 - 1.0, 0.0, 1.0));
    float3 direction = normalize(farPoint.xyz / farPoint.w - nearPoint.xyz / nearPoint.w);

    // atan2(x, −z), sama dengan acuan CPU-nya: pada u = 0,5 arahnya −Z.
    float longitude = atan2(direction.x, -direction.z) + pc.params.y;
    float latitude = asin(clamp(direction.y, -1.0, 1.0));

    // **Putarannya masuk sebelum pembungkusan, bukan sesudah.** Menggeser uv
    // yang sudah jadi menuntut pembungkusan tangan di kanal U; menggesernya di
    // sudut membuat sampler yang REPEAT mengurusnya sendiri, dan tidak ada satu
    // pun tempat yang bisa lupa membungkus.
    float2 environmentUv = float2(longitude / (2.0 * kPi) + 0.5, 0.5 - latitude / kPi);

    float3 radiance = uEnvironment.SampleLevel(environmentUv, 0.0).rgb;
    return float4(radiance * pc.params.x, 1.0);
}
