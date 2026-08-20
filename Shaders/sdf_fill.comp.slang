// Komposit satu kotak texel kaskade SDF, satu voxel per thread (G4).
//
// **Acuannya `BoxSceneField::Row` dan `SdfVolume::WriteBoxRows` di
// `Code/Render/src/SdfVolume.cpp`, dan keduanya harus sepakat byte demi byte.**
// Yang di CPU punya test tanpa GPU; yang di sini salinannya. Perbandingannya
// tidak bisa dilakukan lewat gambar — satu-satunya pembaca kaskade ini adalah
// penelusuran GI, dan GI tidak deterministik — jadi yang dibandingkan adalah
// isi voxelnya lewat readback. Itu uji yang lebih ketat, dan ia menuntut kedua
// sisi menghitung angka yang sama persis, bukan angka yang mirip.
//
// **Pembuangan di sini per voxel, di CPU per baris. Hasilnya tetap sama.**
// Sebuah entri yang lolos uji baris tetapi gagal uji voxel pasti berjarak lebih
// jauh daripada pita — dan nilai di luar pita menjenuh ke 255 pada kedua jalur.
// Yang berbeda hanyalah kapan pekerjaannya dibuang, bukan hasilnya.

struct Push {
    /// xyz texel tujuan terkecil di dalam tekstur, w banyaknya entri
    int4 texelMin;
    /// xyz voxel dunia yang bersesuaian dengan `texelMin`
    int4 worldMin;
    /// xyz ukuran kotak, voxel
    int4 extent;
    /// x ukuran voxel (meter), y jari-jari pita, z skala penyandian
    float4 params;
};

/// Cermin dari `BoxSceneField::Entry`. Satu kotak per instance mesh: kubus
/// satuan yang dibawa ke dunia oleh matriks model, jadi jaraknya dihitung di
/// ruang lokal lalu dikalikan skala terkecilnya.
struct Entry {
    float4x4 inverse;
    /// x skala terkecil di antara tiga sumbu, y min(skala)/maks(skala)
    float4 scaleAnisotropy;
    /// xyz kotak batas dunia terkecil
    float4 boundsMin;
    /// xyz kotak batas dunia terbesar
    float4 boundsMax;
};

[[vk::push_constant]] ConstantBuffer<Push> pc;
[format("r8")]
[[vk::binding(0, 0)]] RWTexture3D<float> uCascade;
[[vk::binding(0, 1)]] StructuredBuffer<Entry> entries;

/// Salinan `UnitBoxDistance`.
float unitBoxDistance(float3 point) {
    float3 q = abs(point) - float3(0.5);
    // Suku pertama benar di luar kotak, yang kedua di dalam. Memakai salah
    // satunya saja menghasilkan jarak yang salah tepat di sisi yang lain.
    return length(max(q, float3(0.0))) + min(max(q.x, max(q.y, q.z)), 0.0);
}

/// Salinan `AabbDistance`, dengan kotak pertama menciut menjadi satu titik.
float pointAabbDistance(float3 point, float3 boxMin, float3 boxMax) {
    float3 gap = max(max(boxMin - point, point - boxMax), float3(0.0));
    return length(gap);
}

// **Satu dimensi, bukan tiga.** Kotak yang harus diisi hampir selalu lempeng
// tipis — satu gerakan kamera satu voxel menghasilkan lempeng setebal satu
// voxel, dan pembagian toroidalnya memecahnya menjadi bentuk seperti 1×56×9.
// Grup kerja 3D atas bentuk seperti itu membuang sebagian besar thread-nya di
// sumbu yang tebalnya satu; indeks datar yang dibongkar sendiri tidak pernah
// membuang lebih dari satu grup.
[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    int total = pc.extent.x * pc.extent.y * pc.extent.z;
    int flat = int(thread.x);
    if (flat >= total) {
        return;
    }
    int3 local;
    local.x = flat % pc.extent.x;
    local.y = (flat / pc.extent.x) % pc.extent.y;
    local.z = flat / (pc.extent.x * pc.extent.y);

    // Pusat voxel, bukan pojoknya. Sampel di pojok menggeser seluruh medan
    // setengah voxel — cukup untuk membuat permukaan tampak bergeser terhadap
    // geometri yang menghasilkannya.
    float3 world = (float3(pc.worldMin.xyz + local) + float3(0.5)) * pc.params.x;

    float band = pc.params.y;
    float nearest = 3.402823466e+38;  // FLT_MAX, sama dengan sisi CPU
    for (int i = 0; i < pc.texelMin.w; ++i) {
        Entry entry = entries[i];
        // Nilai yang akan disimpan tidak pernah lebih kecil daripada jarak
        // dunia dikali anisotropi; kalau batas bawah itu saja sudah di luar
        // pita, voxel ini akan dijepit ke nilai jenuh — jadi entri ini tidak
        // punya apa pun untuk disumbangkan.
        if (pointAabbDistance(world, entry.boundsMin.xyz, entry.boundsMax.xyz) *
                entry.scaleAnisotropy.y >= band) {
            continue;
        }
        float3 point = mul(entry.inverse, float4(world, 1.0)).xyz;
        nearest = min(nearest, unitBoxDistance(point) * entry.scaleAnisotropy.x);
    }

    // Penyandiannya dijabarkan di sini, sama seperti di sisi CPU: pita
    // ±BandRadius dipetakan ke 0..255, dan pembulatannya ke bawah setelah
    // ditambah setengah — yaitu `static_cast<uint8_t>(encoded + 0.5f)`.
    float encoded = clamp(nearest * pc.params.z + 127.5, 0.0, 255.0);
    float quantised = floor(encoded + 0.5);
    uCascade[pc.texelMin.xyz + local] = quantised / 255.0;
}
