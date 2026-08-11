// Awan volumetrik: raymarch melalui cangkang bola antara dua ketinggian.
//
// **Cangkang bola, bukan kotak.** Kotak sejajar sumbu punya tepi, dan tepi itu
// jatuh di dalam pandangan sebagai garis lurus tempat awan berhenti mendadak —
// tepat di dekat horizon, yaitu tempat lapisan awan paling banyak terlihat.
// Cangkang bola berakhir di horizon karena geometrinya memang begitu.
//
// **Kompositnya blending yang sama dengan aerial perspective:**
// `dst = src + dst * src.a`, dengan transmitansi awan di kanal alfa. Awan berada
// di depan langit dan di belakang geometri, dan keduanya sudah ada di gambar HDR
// itu — membacanya lewat descriptor sambil menulisinya adalah perilaku yang
// tidak terdefinisi.
//
// Derau 3D-nya dibangkitkan di CPU (`Code/Render/src/CloudNoise.cpp`) dan
// diunggah sebagai tekstur volume. Sifat yang menentukan benar-tidaknya — bahwa
// derau itu **menyambung di tepinya** — karena itu punya uji, bukan hanya punya
// tampilan.

#include "sky_common.slang"

struct Push {
    float4x4 invViewProj;
    /// xyz: posisi kamera dalam meter, w: ketinggian kamera dalam km.
    float4 camera;
    /// xyz: arah dari permukaan ke matahari, w: pengali radiansi langit.
    float4 sun;
    /// xyz: radiansi matahari, w: geseran angin dalam km.
    float4 sunRadiance;
    /// x: alas lapisan (km), y: puncak lapisan (km), z: cakupan 0..1,
    /// w: kerapatan.
    float4 layer;
    /// x: sisi ubin derau (km), y: pengali skala rincian, z: kekuatan kikisan,
    /// w: banyak cuplikan sepanjang pandangan.
    float4 shaping;
    /// x: banyak cuplikan ke arah matahari, y: serapan ke matahari,
    /// z: serapan menembus awan, w: ambang kegelapan.
    float4 lighting;
    /// xy: ukuran LUT transmitansi, zw: cadangan.
    float4 lutSizes;
};

[[vk::push_constant]] ConstantBuffer<Push> pc;
[[vk::binding(0, 0)]] Sampler3D uShape;
[[vk::binding(1, 0)]] Sampler3D uDetail;
[[vk::binding(2, 0)]] Sampler2D uTransmittance;
[[vk::binding(3, 0)]] Sampler2D uDepth;

/// Kedua akar perpotongan sinar dengan bola berpusat di titik asal. Mengembalikan
/// (−1, −1) bila tidak memotong.
///
/// **Kedua akar, bukan yang terdekat saja.** Kamera hampir selalu berada di
/// bawah lapisan awan, dan yang dibutuhkan di sana justru akar **jauh** dari
/// bola alas — titik tempat sinar keluar menembusnya. Fungsi yang hanya
/// mengembalikan akar terdekat menjawab pertanyaan yang berbeda, dan jawabannya
/// negatif persis di kasus yang paling sering terjadi.
float2 raySphereBoth(float3 origin, float3 direction, float radius) {
    float b = dot(direction, origin);
    float c = dot(origin, origin) - radius * radius;
    float discriminant = b * b - c;
    if (discriminant < 0.0) { return float2(-1.0, -1.0); }
    float root = sqrt(discriminant);
    return float2(-b - root, -b + root);
}

/// Gradien ketinggian di dalam lapisan. **Harus sama persis dengan
/// `CloudHeightGradient` di `CloudNoise.cpp`**, yang punya ujinya: nol di kedua
/// ujung, puncaknya di bawah tengah. Lapisan yang dipotong rata di alas
/// memperlihatkan dasarnya sebagai bidang datar sempurna sampai horizon, dan
/// tidak ada yang lebih cepat memberi tahu mata bahwa langitnya palsu.
float heightGradient(float heightKm, float bottomKm, float topKm) {
    float thickness = topKm - bottomKm;
    if (thickness <= 0.0) { return 0.0; }
    float fraction = (heightKm - bottomKm) / thickness;
    if (fraction <= 0.0 || fraction >= 1.0) { return 0.0; }
    float bottomFade = saturate(fraction / 0.15);
    float topFade = saturate((1.0 - fraction) / 0.55);
    return bottomFade * topFade * topFade;
}

float sampleDensity(float3 position) {
    float heightKm = length(position) - kBottomRadius;
    float gradient = heightGradient(heightKm, pc.layer.x, pc.layer.y);
    if (gradient <= 0.0) { return 0.0; }

    // Ubin mendatar dari x/y kerangka planet, tegak dari ketinggian. Memakai z
    // kerangka planet apa adanya berarti mengubinkan sebuah koordinat yang
    // besarnya 6360 — dan yang hilang bukan ketelitian melainkan seluruh sumbu
    // tegaknya.
    float3 tiled = float3(position.x + pc.sunRadiance.w, position.y, heightKm) / pc.shaping.x;

    float4 shape = uShape.SampleLevel(tiled, 0.0);
    // Bobot tetap: satu oktaf dasar dan tiga oktaf yang makin rapat dan makin
    // lemah. Jumlahnya dinormalkan supaya cakupan berarti hal yang sama berapa
    // pun bobotnya.
    const float4 kShapeWeights = float4(0.55, 0.27, 0.12, 0.06);
    float shapeFbm = dot(shape, kShapeWeights) / dot(kShapeWeights, float4(1.0));

    // Cakupan mengurangi, bukan mengalikan. Perkalian menipiskan seluruh awan
    // secara merata; pengurangan **memakan tepinya**, dan itulah cara awan
    // benar-benar berkurang saat langit cerah.
    float base = shapeFbm * gradient - (1.0 - pc.layer.z);
    if (base <= 0.0) { return 0.0; }

    float4 detail = uDetail.SampleLevel(tiled * pc.shaping.y, 0.0);
    const float4 kDetailWeights = float4(0.62, 0.26, 0.12, 0.0);
    float detailFbm = dot(detail, kDetailWeights) / dot(kDetailWeights, float4(1.0));

    // Kikisan yang paling kuat di tepi, bukan merata: bagian dalam awan tetap
    // padat sementara tepinya berjumbai. Kikisan merata menghasilkan awan yang
    // berlubang-lubang di tengah, yang tampak seperti busa.
    float edge = 1.0 - base;
    float erosion = edge * edge * edge;
    float density = base - (1.0 - detailFbm) * erosion * pc.shaping.z;
    return max(density, 0.0) * pc.layer.w;
}

/// Transmitansi menuju matahari dari sebuah titik di dalam awan.
float lightMarch(float3 position, float3 sunDirection) {
    int samples = int(max(pc.lighting.x, 1.0));
    // Sinar cahaya hanya perlu menempuh lapisannya, bukan seluruh atmosfer.
    float2 top = raySphereBoth(position, sunDirection, kBottomRadius + pc.layer.y);
    float span = max(top.y, 0.0);
    span = min(span, 8.0);
    if (span <= 0.0) { return 1.0; }

    float total = 0.0;
    float step = span / float(samples);
    for (int i = 0; i < samples; ++i) {
        // Langkah kuadratik: yang menentukan bayangan sendiri sebuah awan adalah
        // beberapa ratus meter pertama menuju matahari.
        float t = float(i) + 0.5;
        t = (t * t) / (float(samples) * float(samples)) * span;
        total += sampleDensity(position + sunDirection * t) * step;
    }
    float transmittance = exp(-total * pc.lighting.y);
    // Lantai kegelapan: awan yang benar-benar hitam di bagian dalamnya adalah
    // awan tanpa hamburan berganda, dan hamburan berganda justru yang membuat
    // kumulus tampak seperti kapas alih-alih seperti batu.
    return pc.lighting.w + transmittance * (1.0 - pc.lighting.w);
}

float henyeyGreenstein(float cosTheta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * kPi * pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5));
}

[shader("fragment")]
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float4 nearPoint = mul(pc.invViewProj, float4(uv * 2.0 - 1.0, 1.0, 1.0));
    float4 farPoint = mul(pc.invViewProj, float4(uv * 2.0 - 1.0, 0.0, 1.0));
    float3 direction = normalize(farPoint.xyz / farPoint.w - nearPoint.xyz / nearPoint.w);

    float3 worldDirection = float3(direction.x, -direction.z, direction.y);
    float3 sunDirection = normalize(float3(pc.sun.x, -pc.sun.z, pc.sun.y));

    float viewHeight = kBottomRadius + max(pc.camera.w, kRadiusOffset);
    float3 origin = float3(0.0, 0.0, viewHeight);

    float bottomRadius = kBottomRadius + pc.layer.x;
    float topRadius = kBottomRadius + pc.layer.y;

    float2 bottom = raySphereBoth(origin, worldDirection, bottomRadius);
    float2 top = raySphereBoth(origin, worldDirection, topRadius);
    float ground = raySphereNearest(origin, worldDirection, kBottomRadius);

    float start = 0.0;
    float end = 0.0;
    if (viewHeight < bottomRadius) {
        // Di bawah lapisan — keadaan yang hampir selalu berlaku. Sinar yang
        // menunduk sampai menyentuh tanah tidak pernah mencapai awan.
        if (ground >= 0.0) { return float4(0.0, 0.0, 0.0, 1.0); }
        start = bottom.y;
        end = top.y;
    } else if (viewHeight < topRadius) {
        start = 0.0;
        end = bottom.x > 0.0 ? bottom.x : top.y;
    } else {
        if (top.x < 0.0) { return float4(0.0, 0.0, 0.0, 1.0); }
        start = top.x;
        end = bottom.x > 0.0 ? bottom.x : top.y;
    }
    if (end <= start) { return float4(0.0, 0.0, 0.0, 1.0); }

    // Geometri memotong awan. Depth reversed-Z: nol berarti tidak ada permukaan.
    float depth = uDepth.SampleLevel(uv, 0.0).r;
    if (depth > 0.0) {
        float4 world = mul(pc.invViewProj, float4(uv * 2.0 - 1.0, depth, 1.0));
        float surfaceKm = length(world.xyz / world.w - pc.camera.xyz) * 0.001;
        if (surfaceKm <= start) { return float4(0.0, 0.0, 0.0, 1.0); }
        end = min(end, surfaceKm);
    }

    int samples = int(max(pc.shaping.w, 1.0));
    float span = end - start;
    float step = span / float(samples);

    // Geseran acak per piksel sepanjang satu langkah. **Tanpa ini, awan menjadi
    // pita-pita**: setiap piksel mencuplik pada jarak yang sama persis, jadi
    // batas antar-cuplikan berbaris rapi di seluruh layar dan terlihat sebagai
    // cangkang berlapis. Deraunya menukar pita dengan bintik, dan bintik jauh
    // lebih mudah diabaikan mata.
    float jitter = frac(sin(dot(uv, float2(12.9898, 78.233))) * 43758.5453);
    float travelled = start + jitter * step;

    float cosTheta = dot(worldDirection, sunDirection);
    // Dua lobus: satu sangat ke depan yang memberi silver lining saat memandang
    // ke arah matahari, satu hampir isotropik yang mengisi sisanya. Satu lobus
    // memaksa memilih salah satunya, dan keduanya terlihat.
    float phase = lerp(henyeyGreenstein(cosTheta, 0.8), henyeyGreenstein(cosTheta, -0.3), 0.4) +
                  0.05;

    float3 energy = float3(0.0);
    float transmittance = 1.0;

    for (int i = 0; i < samples; ++i) {
        if (transmittance < 0.01) { break; }
        float3 samplePosition = origin + worldDirection * travelled;
        travelled += step;

        float density = sampleDensity(samplePosition);
        if (density <= 0.0) { continue; }

        float sunTransmittance = lightMarch(samplePosition, sunDirection);

        // Transmitansi atmosfer di ketinggian itu menuju matahari: awan saat
        // matahari terbenam harus jingga karena cahaya yang sampai kepadanya
        // memang sudah jingga. Awan yang tetap putih saat langitnya merah adalah
        // awan yang menyala sendiri.
        float3 up = normalize(samplePosition);
        float2 transParams = float2(length(samplePosition), dot(sunDirection, up));
        float3 sunlight =
            uTransmittance.SampleLevel(transmittanceParamsToUv(transParams), 0.0).rgb;

        float extinction = density * pc.lighting.z;
        float stepTransmittance = exp(-extinction * step);
        // Integrasi analitik di dalam langkahnya, alasan yang sama dengan pass
        // langit: perkalian sederhana meninggalkan pita pada jumlah langkah
        // sekecil ini.
        float3 scattered = sunlight * sunTransmittance * phase * density;
        float3 integrated = (scattered - scattered * stepTransmittance) / max(extinction, 1e-6);

        energy += integrated * transmittance;
        transmittance *= stepTransmittance;
    }

    return float4(energy * pc.sunRadiance.rgb * pc.sun.w, saturate(transmittance));
}
