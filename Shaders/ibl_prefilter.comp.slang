// Prefilter spekular GGX satu mip peta lingkungan, satu texel per thread.
//
// **Acuannya `PrefilterSpecular` di `Code/Render/src/Ibl.cpp`, dan keduanya
// harus menghitung integral yang sama.** Yang di CPU punya test tanpa GPU dan
// tetap menjadi jalur yang dipakai `--bench`; yang di sini salinannya, dipakai
// editor. Dua jalur untuk satu rumus selalu berisiko, dan yang membenarkannya
// di sini selisih biayanya: peta 256² dengan 256 sampel adalah 33 juta cuplikan
// lingkungan, yaitu belasan detik di CPU dan beberapa milidetik di sini.
//
// **Mip 0 tidak lewat sini sama sekali.** Ia mencuplik `IEnvironmentSampler` —
// langit atmosferik yang satu cuplikannya satu ray march, atau sebuah HDR
// 4096×2048 di memori host — dan tidak satu pun dari keduanya ada di GPU saat
// panggangan berjalan. Yang di sini menyaring mip 0 itu, bukan lingkungannya.
//
// **Setiap dispatch membaca mip 0 dan menulis satu mip yang lain**, jadi
// seluruh mip boleh berjalan berturut-turut tanpa satu barrier pun di
// antaranya. Itu bukan optimisasi melainkan konsekuensi: tidak ada mip yang
// menjadi masukan mip berikutnya, persis seperti di sisi CPU.

struct Push {
    /// Sisi muka mip yang ditulis, texel.
    uint extent;
    /// Sampel GGX per texel.
    uint sampleCount;
    /// Kekasaran yang diwakili mip ini — `RoughnessForMip` di sisi CPU yang
    /// menghitungnya, bukan shader ini. Menghitungnya di dua tempat berarti
    /// dua pemetaan mip→kekasaran yang harus berpindah bersama, dan yang
    /// tertinggal muncul sebagai pantulan yang buramnya tidak cocok dengan
    /// slider kekasaran hanya pada sebagian mip.
    float roughness;
    float pad;
};

[[vk::push_constant]] ConstantBuffer<Push> pc;

// **Formatnya disebut, bukan dibiarkan `Unknown`** — alasan yang sama dengan
// `debug_gradient.comp.slang`: tanpa atribut ini Slang memancarkan
// `StorageImageWriteWithoutFormat`, dan kapabilitas itu menuntut fitur
// perangkat yang tidak diminta siapa pun di sini.
//
// `RWTexture2DArray` dan bukan `RWTextureCube`: Vulkan tidak punya storage
// image bertipe kubus. Enam mukanya ditulis sebagai enam lapis, dan urutan
// lapisnya urutan muka Vulkan (+X, −X, +Y, −Y, +Z, −Z) — urutan yang sama yang
// dipetakan `cubeFaceDirection` di bawah.
[format("rgba32f")]
[[vk::binding(0, 0)]] RWTexture2DArray<float4> uMip;

/// Mip 0 saja, sebagai kubus. View-nya sengaja dibatasi satu mip: mip yang
/// sedang ditulis dispatch ini ada di image yang sama, dan view yang mencakup
/// seluruh rantai membuat "dibaca" dan "ditulis" menunjuk subresource yang
/// tidak jelas terpisah.
///
/// `SamplerCube` dan bukan `TextureCube` + `SamplerState`, sama dengan
/// `prefilteredEnv` di `shadow_common.slang`: satu binding, satu descriptor
/// gabungan. Yang memisahkannya di sini saja akan menjadi satu-satunya tempat
/// di mesin ini yang memakai konvensi lain.
[[vk::binding(1, 0)]] SamplerCube gSource;

/// Sama dengan `kMinRoughness` di `Ibl.cpp`.
static const float kMinRoughness = 0.0045;
static const float kTwoPi = 6.28318530717958647692;

/// Salinan `RadicalInverse`: deret Van der Corput basis 2.
float radicalInverse(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

/// Salinan `Hammersley`.
float2 hammersley(uint index, uint count) {
    return float2(float(index) / float(max(count, 1u)), radicalInverse(index));
}

/// Salinan `BuildFrame`. Sumbu bantunya dipilih dari komponen normal yang
/// paling kecil — catatan lengkapnya ada di sisi CPU.
void buildFrame(float3 normal, out float3 tangent, out float3 bitangent) {
    float3 helper = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    tangent = normalize(cross(helper, normal));
    bitangent = cross(normal, tangent);
}

/// Salinan `ImportanceSampleGgx`. `alpha` kekasaran kuadrat, bukan
/// kekasarannya.
float3 importanceSampleGgx(float2 xi, float3 normal, float roughness) {
    float clamped = max(roughness, kMinRoughness);
    float alpha = clamped * clamped;

    float phi = kTwoPi * xi.x;
    float cosTheta = sqrt(max((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y), 0.0));
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));

    float3 local = float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
    float3 tangent;
    float3 bitangent;
    buildFrame(normal, tangent, bitangent);
    return normalize(tangent * local.x + bitangent * local.y + normal * local.z);
}

/// Salinan `CubeFaceDirection`. Konvensinya konvensi Vulkan, jadi arah yang
/// dihasilkannya dan arah yang dipakai `gSource.SampleLevel` menunjuk texel
/// yang sama — dan ketidakcocokan di sini tidak menghasilkan galat apa pun,
/// hanya lingkungan yang isinya benar di muka yang salah.
float3 cubeFaceDirection(uint face, float u, float v) {
    float s = u * 2.0 - 1.0;
    float t = 1.0 - v * 2.0;
    switch (face) {
        case 0: return normalize(float3(1.0, t, -s));
        case 1: return normalize(float3(-1.0, t, s));
        case 2: return normalize(float3(s, 1.0, -t));
        case 3: return normalize(float3(s, -1.0, t));
        case 4: return normalize(float3(s, t, 1.0));
        default: return normalize(float3(-s, t, -1.0));
    }
}

// Harus sama dengan `IblPrefilter::kGroupSize` di sisi C++: yang menghitung
// banyaknya grup adalah pemanggil, dan ia menghitungnya dari angka itu. Sumbu z
// tidak dibulatkan — ia selalu enam, satu grup per muka.
[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    if (thread.x >= pc.extent || thread.y >= pc.extent) {
        return;
    }
    uint face = thread.z;

    // Tengah texel, sama alasannya dengan sisi CPU.
    float u = (float(thread.x) + 0.5) / float(pc.extent);
    float v = (float(thread.y) + 0.5) / float(pc.extent);
    float3 normal = cubeFaceDirection(face, u, v);

    // Cermin sempurna: satu pengambilan. Cabang ini ada di sisi CPU juga, dan
    // tanpanya integralnya menyebar sampel di sekitar arah pantul karena alpha
    // dijepit ke kMinRoughness — pantulan tajam jadi selalu sedikit buram.
    if (pc.roughness <= kMinRoughness) {
        uMip[uint3(thread.xy, face)] =
            float4(gSource.SampleLevel(normal, 0.0).rgb, 1.0);
        return;
    }

    uint count = max(pc.sampleCount, 1u);
    float3 sum = float3(0.0);
    float weight = 0.0;
    for (uint i = 0; i < count; ++i) {
        float3 halfway = importanceSampleGgx(hammersley(i, count), normal, pc.roughness);
        // N = V = R, jadi arah cahaya adalah pantulan normal terhadap halfway.
        float3 light = normalize(halfway * (2.0 * dot(normal, halfway)) - normal);
        float nDotL = dot(normal, light);
        if (nDotL <= 0.0) {
            continue;
        }
        // **`SampleLevel(…, 0.0)`, bukan `Sample`.** Mip yang lain sedang
        // ditulis dispatch lain pada image yang sama, dan pemilihan mip
        // otomatis di dalam gelung yang bercabang tidak punya turunan yang
        // berarti. Yang disaring memang mip 0, sama dengan sisi CPU.
        sum += gSource.SampleLevel(light, 0.0).rgb * nDotL;
        weight += nDotL;
    }

    // Dibobot n·l, bukan dijumlah rata — catatan lengkapnya di sisi CPU.
    float3 radiance = weight > 0.0 ? sum / weight
                                   : gSource.SampleLevel(normal, 0.0).rgb;
    uMip[uint3(thread.xy, face)] = float4(radiance, 1.0);
}
