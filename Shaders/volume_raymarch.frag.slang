// Raymarch volume `.vdb` — asap, api, dan awan yang datang sebagai berkas.
//
// **Acuan kebenarannya ada di CPU**, di `Code/Volume/src/VolumeRaymarch.cpp`,
// dan rumus di sini harus sama persis dengannya. Bukan demi kerapian: acuan
// yang ditulis dari pemahaman yang berbeda tidak bisa dipakai membuktikan apa
// pun, dan sifat yang paling mudah hilang di sini — hasil yang tidak bergantung
// pada besar langkah — justru yang paling mahal ketika hilang.
//
// Volumenya adalah tekstur 3D bilangan bulat ternormalisasi; nilai aslinya
// `texel * scale + bias`. Normalisasinya memakai rentang grid itu sendiri
// karena grid suhu berkisar ratusan sementara asap tipis mungkin tidak pernah
// melewati 0,05 — menjepit keduanya ke 0..1 membuat yang pertama putih rata dan
// yang kedua hilang. Lihat `Sim::Render::EncodeVolume`.

struct Push {
    float4x4 invViewProj;
    /// xyz: posisi kamera di ruang dunia. w: panjang langkah, meter.
    float4 cameraStep;
    /// xyz: sudut minimum kotak volume di ruang dunia. w: koefisien kepunahan.
    float4 boundsMinExtinction;
    /// xyz: sudut maksimum kotak volume. w: transmitansi di mana penjejakan
    /// berhenti.
    float4 boundsMaxCutoff;
    /// xyz: albedo hamburan. w: banyaknya langkah maksimum.
    float4 albedoSteps;
    /// xyz: cahaya yang datang, sudah termasuk fase dan bayangannya. w: skala
    /// dekode texel.
    float4 lightScale;
    /// x: bias dekode texel. yzw: cadangan.
    float4 decode;
};

[[vk::push_constant]] ConstantBuffer<Push> pc;
[[vk::binding(0, 0)]] Sampler3D uVolume;

/// Slab method, sama dengan `IntersectBox` di acuan CPU-nya.
///
/// Pembagian dengan nol disengaja: IEEE menghasilkan ±inf, dan min/max di
/// bawah menanganinya dengan benar untuk ray yang sejajar sebuah sisi.
bool intersectBox(float3 origin, float3 direction, float3 boxMin, float3 boxMax,
                  out float outNear, out float outFar) {
    float3 inverse = 1.0 / direction;
    float3 a = (boxMin - origin) * inverse;
    float3 b = (boxMax - origin) * inverse;
    float3 low = min(a, b);
    float3 high = max(a, b);

    outNear = max(max(low.x, low.y), low.z);
    outFar = min(min(high.x, high.y), high.z);
    return outFar >= max(outNear, 0.0);
}

float sampleDensity(float3 world) {
    float3 size = pc.boundsMaxCutoff.xyz - pc.boundsMinExtinction.xyz;
    float3 uvw = (world - pc.boundsMinExtinction.xyz) / max(size, float3(1e-6));
    // Sampler-nya dijepit di tepi, jadi di luar kotak nilainya texel tepi —
    // dan yang membuat itu tidak bocor keluar adalah `intersectBox` di atas,
    // yang menghentikan penjejakan di dinding kotaknya.
    float texel = uVolume.Sample(uvw).r;
    return texel * pc.lightScale.w + pc.decode.x;
}

[shader("fragment")]
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    // Arah pandang, sama persis dengan pass langit. w = 1 pada bidang dekat:
    // reversed-Z.
    float4 nearPoint = mul(pc.invViewProj, float4(uv * 2.0 - 1.0, 1.0, 1.0));
    float4 farPoint = mul(pc.invViewProj, float4(uv * 2.0 - 1.0, 0.0, 1.0));
    float3 direction = normalize(farPoint.xyz / farPoint.w - nearPoint.xyz / nearPoint.w);
    float3 origin = pc.cameraStep.xyz;

    float near = 0.0;
    float far = 0.0;
    if (!intersectBox(origin, direction, pc.boundsMinExtinction.xyz, pc.boundsMaxCutoff.xyz,
                      near, far)) {
        // Kebanyakan ray memang tidak menyentuh volumenya, dan menolaknya di
        // sini — sebelum langkah pertama — yang membuat volume murah.
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    near = max(near, 0.0);

    float step = pc.cameraStep.w;
    float extinctionScale = pc.boundsMinExtinction.w;
    float cutoff = pc.boundsMaxCutoff.w;
    int maxSteps = int(pc.albedoSteps.w);

    float3 scattered = float3(0.0);
    float transmittance = 1.0;

    for (int i = 0; i < maxSteps; ++i) {
        // Disampel di **tengah** langkah. Sampel di tepi menggeser hasilnya
        // setengah langkah, dan pergeseran itu tumbuh bersama besar langkah —
        // sehingga menaikkan kualitas memindahkan asapnya, bukan hanya
        // menghaluskannya.
        float distance = near + (float(i) + 0.5) * step;
        if (distance >= far) { break; }

        float density = sampleDensity(origin + direction * distance);
        if (density <= 0.0) { continue; }

        float extinction = density * extinctionScale;
        float stepTransmittance = exp(-extinction * step);

        // **Integrasi analitik di dalam langkah**, bukan `radiance * step`.
        // Bentuk naif membuat hasilnya bergantung pada besar langkah, sehingga
        // setiap preset kualitas harus dikalibrasi ulang sendiri-sendiri.
        float3 radiance = pc.lightScale.xyz * pc.albedoSteps.xyz * density;
        float3 integrated = (radiance - radiance * stepTransmittance) / max(extinction, 1e-6);

        scattered += integrated * transmittance;
        transmittance *= stepTransmittance;

        if (transmittance < cutoff) { break; }
    }

    // Alfa membawa transmitansinya: pemanggil mengomposit dengan
    // `dst * alpha + src`, jadi yang tersimpan adalah berapa bagian latar yang
    // masih terlihat menembusnya.
    return float4(scattered, transmittance);
}
