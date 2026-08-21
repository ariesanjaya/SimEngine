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
    /// xyz voxel dunia yang bersesuaian dengan `texelMin`, w banyaknya grid
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

/// Cermin dari `BakedSceneField::GpuGrid`. Satu medan jarak hasil bake per
/// instance mesh yang punya satu.
struct Grid {
    float4x4 inverse;
    /// x skala terkecil, y anisotropi, z pita grid (ruang lokal), w slot tekstur
    float4 scaleBandSlot;
    /// xyz kotak batas dunia terkecil, w sisi voxel grid
    float4 boundsMinVoxel;
    float4 boundsMax;
    /// xyz pusat voxel (0,0,0) di ruang lokal
    float4 origin;
    /// xyz ukuran grid dalam voxel
    float4 size;
};

/// Sama dengan `SdfClipmapResource::kMaxGrids`.
static const int kMaxGrids = 16;

[[vk::push_constant]] ConstantBuffer<Push> pc;
[format("r8")]
[[vk::binding(0, 0)]] RWTexture3D<float> uCascade;
[[vk::binding(0, 1)]] StructuredBuffer<Entry> entries;
[[vk::binding(1, 1)]] StructuredBuffer<Grid> grids;
/// **`Texture3D` tanpa sampler, dibaca lewat `Load`.** Penyaringan perangkat
/// keras tidak sama persis dengan tujuh `lerp` di `TrilinearSample`, dan
/// kriteria selesai G4 menuntut kedua jalur menghasilkan byte yang sama — bukan
/// byte yang mirip. Delapan pengambilan texel lebih mahal daripada satu
/// cuplikan, dan pass ini hanya berjalan saat kamera menggeser sebuah kaskade.
[[vk::binding(0, 2)]] Texture3D<float> gridTextures[kMaxGrids];

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

/// Salinan `SdfGrid::At`: di luar grid, yang bisa dikatakan hanyalah "setidaknya
/// sejauh pita".
float gridAt(int slot, int3 texel, int3 size, float band) {
    if (any(texel < int3(0)) || any(texel >= size)) {
        return band;
    }
    return gridTextures[NonUniformResourceIndex(slot)].Load(int4(texel, 0));
}

/// Salinan `sim::detail::TrilinearSample`, dengan urutan operasi yang sama
/// persis — tujuh `lerp` yang ditulis sebagai `a + (b - a) * t`, bukan sebagai
/// `lerp` intrinsik, karena keduanya boleh dikompilasi menjadi FMA yang berbeda.
float gridSample(Grid grid, float3 local) {
    int slot = int(grid.scaleBandSlot.w);
    float band = grid.scaleBandSlot.z;
    int3 size = int3(grid.size.xyz);
    float3 position = (local - grid.origin.xyz) / grid.boundsMinVoxel.w;
    int3 base = int3(floor(position));
    float3 f = position - float3(base);

    float c000 = gridAt(slot, base + int3(0, 0, 0), size, band);
    float c100 = gridAt(slot, base + int3(1, 0, 0), size, band);
    float c010 = gridAt(slot, base + int3(0, 1, 0), size, band);
    float c110 = gridAt(slot, base + int3(1, 1, 0), size, band);
    float c001 = gridAt(slot, base + int3(0, 0, 1), size, band);
    float c101 = gridAt(slot, base + int3(1, 0, 1), size, band);
    float c011 = gridAt(slot, base + int3(0, 1, 1), size, band);
    float c111 = gridAt(slot, base + int3(1, 1, 1), size, band);

    float x00 = c000 + (c100 - c000) * f.x;
    float x10 = c010 + (c110 - c010) * f.x;
    float x01 = c001 + (c101 - c001) * f.x;
    float x11 = c011 + (c111 - c011) * f.x;
    float y0v = x00 + (x10 - x00) * f.y;
    float y1v = x01 + (x11 - x01) * f.y;
    return y0v + (y1v - y0v) * f.z;
}

/// Salinan `GridLowerBound`. Dua batas bawah digabung dengan `max`; catatan
/// lengkapnya ada di sisi CPU.
float gridLowerBound(Grid grid, float3 world) {
    float3 local = mul(grid.inverse, float4(world, 1.0)).xyz;
    float sampled = gridSample(grid, local) * grid.scaleBandSlot.x;
    if (sampled < 0.0) {
        return sampled;
    }
    float toBox = pointAabbDistance(world, grid.boundsMinVoxel.xyz, grid.boundsMax.xyz);
    return max(sampled, toBox);
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

    // Lalu mesh yang sudah dibake. Urutannya sama dengan sisi CPU — kotak lebih
    // dulu, grid disisipkan dengan `min` — dan urutan itu berarti: `min` bukan
    // operasi yang bisa ditukar tanpa akibat pada float, dan dua jalur yang
    // menjumlahkan hal yang sama dengan urutan berbeda menghasilkan byte yang
    // berbeda pada voxel yang jaraknya nyaris sama dari dua benda.
    for (int g = 0; g < pc.worldMin.w; ++g) {
        Grid grid = grids[g];
        // Pembuangan yang sama persis dengan jalur kotak, dan dengan alasan yang
        // sama: batas bawah jarak dunia dikali anisotropi.
        if (pointAabbDistance(world, grid.boundsMinVoxel.xyz, grid.boundsMax.xyz) *
                grid.scaleBandSlot.y >= band) {
            continue;
        }
        nearest = min(nearest, gridLowerBound(grid, world));
    }

    // Penyandiannya dijabarkan di sini, sama seperti di sisi CPU: pita
    // ±BandRadius dipetakan ke 0..255, dan pembulatannya ke bawah setelah
    // ditambah setengah — yaitu `static_cast<uint8_t>(encoded + 0.5f)`.
    float encoded = clamp(nearest * pc.params.z + 127.5, 0.0, 255.0);
    float quantised = floor(encoded + 0.5);
    uCascade[pc.texelMin.xyz + local] = quantised / 255.0;
}
