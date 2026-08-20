// Culling di GPU yang menghasilkan perintah gambar, satu thread per permukaan (G6).
//
// **Acuannya `Frustum::Intersects` di `Code/Render/src/Frustum.cpp`, dan
// keduanya harus sepakat.** Yang di CPU punya test tanpa GPU; yang di sini
// adalah salinannya, bukan penafsirannya. Aturan yang sama dengan
// `cluster_assign.comp`, dan alasannya sama: dua implementasi yang harus sepakat
// adalah cara termurah menemukan yang mana yang salah.
//
// **Yang dihasilkan bukan daftar melainkan perintah gambar.** Sebuah permukaan
// yang tersaring tidak dihapus dari mana pun; ia mendapat perintahnya sendiri
// dengan `instanceCount` nol. Itu yang membuat keluaran pass ini **tidak
// bergantung pada urutan thread**: setiap perintah ditulis di slot tetapnya,
// tanpa satu pun atomic. Memadatkannya akan menghemat perintah yang dilewati
// GPU — dan menukarnya dengan urutan gambar yang berubah tiap frame, yaitu
// perbandingan gambar yang tidak bisa dipakai lagi.
//
// Dua fase, dan keduanya menjalankan shader yang sama:
//
//   Fase 0, sebelum depth prepass — frustum saja. Keluarannya `commands`.
//   Fase 1, sesudah prepass dan piramida occlusion — frustum **dan** occlusion.
//            Keluarannya `visibleCommands`, dipakai pass forward.
//
// Prepass karena itu tetap menggambar seluruh isi frustum, dan pass forward
// hanya menggambar yang benar-benar terlihat. Itu bukan skema dua fase klasik —
// yang itu memakai himpunan terlihat frame lalu untuk memurahkan prepass-nya
// juga — melainkan yang bisa dilakukan renderer yang **sudah** punya prepass
// penuh: piramidanya dibangun dari depth frame ini sendiri, jadi hasilnya tepat
// dan tidak ada benda yang bisa berkedip masuk satu frame terlambat.

struct Params {
    /// Enam bidang frustum, sudah dinormalkan. Urutannya urutan `Frustum`.
    float4 planes[6];
    /// Dipakai memproyeksikan kotak ke layar saat menguji occlusion.
    float4x4 viewProj;
    /// x: banyaknya permukaan. y: banyaknya tingkat piramida. zw: ukuran
    /// viewport, yaitu ukuran tingkat nol.
    uint4 counts;
};

/// Kotak dunia sebuah permukaan, sudah dalam bentuk pusat + setengah-lebar.
///
/// **Bukan min/max.** `Frustum::Intersects` menghitung keduanya dari min/max
/// setiap kali dipanggil; menghitungnya sekali di CPU membuat kedua sisi memakai
/// angka yang sama persis, bukan dua hasil pembagian yang seharusnya sama.
struct Bounds {
    float4 centre;
    float4 extent;
};

/// Rentang indeks ruas mesh yang digambar permukaan ini.
struct Surface {
    uint indexCount;
    uint firstIndex;
};

/// Harus sama persis dengan `VkDrawIndexedIndirectCommand`.
struct DrawCommand {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    /// Dibaca tahap vertex sebagai `SV_StartInstanceLocation`, dan itulah nomor
    /// permukaannya. Menuntut fitur `drawIndirectFirstInstance`.
    uint firstInstance;
};

struct Push {
    /// 0: frustum saja. 1: frustum dan occlusion.
    uint phase;
};

[[vk::push_constant]] ConstantBuffer<Push> pc;

[[vk::binding(0, 0)]] ConstantBuffer<Params> params;
[[vk::binding(1, 0)]] StructuredBuffer<Bounds> bounds;
[[vk::binding(2, 0)]] StructuredBuffer<Surface> surfaces;
[[vk::binding(3, 0)]] RWStructuredBuffer<DrawCommand> commands;
/// Piramida depth yang meringkas dengan **minimum**, yaitu permukaan terjauh.
/// Bukan piramida penelusuran GI — itu meringkas dengan maksimum. Lihat
/// `DepthReduce` di DepthPyramid.h.
[[vk::binding(4, 0)]] Sampler2D occlusionPyramid;
[[vk::binding(5, 0)]] RWStructuredBuffer<DrawCommand> visibleCommands;

/// Ukuran sepetak yang sah pada sebuah tingkat, dengan pembagian dibulatkan ke
/// bawah — aturan mip Vulkan, sama dengan `hizLevelSize` di gi_trace.slang.
int2 pyramidLevelSize(int level) {
    int2 size = int2(params.counts.zw);
    for (int i = 0; i < level; ++i) {
        size = max(int2(1), size / 2);
    }
    return size;
}

float pyramidLoad(int level, float2 uv) {
    const int2 size = pyramidLevelSize(level);
    const int2 texel = clamp(int2(floor(uv * float2(size))), int2(0), size - int2(1));
    return occlusionPyramid.Load(int3(texel, level)).r;
}

/// True bila kotak ini benar-benar tertutup oleh yang sudah tergambar.
///
/// **Salah dalam arah yang aman.** Setiap keraguan — kotak yang menyentuh bidang
/// dekat, petak yang keluar layar, piramida yang belum ada — dijawab "tidak
/// tertutup". Benda yang sesekali digambar padahal tidak terlihat hanya
/// membuang waktu; benda yang sesekali hilang adalah cacat gambar.
bool occluded(Bounds box) {
    const float3 lo = box.centre.xyz - box.extent.xyz;
    const float3 hi = box.centre.xyz + box.extent.xyz;

    float2 ndcMin = float2(1e30);
    float2 ndcMax = float2(-1e30);
    // Reversed-Z: yang terbesar adalah yang paling dekat.
    float nearest = 0.0;
    for (int corner = 0; corner < 8; ++corner) {
        const float3 point = float3((corner & 1) != 0 ? hi.x : lo.x,
                                    (corner & 2) != 0 ? hi.y : lo.y,
                                    (corner & 4) != 0 ? hi.z : lo.z);
        const float4 clip = mul(params.viewProj, float4(point, 1.0));
        // Sudut di belakang atau tepat pada bidang kamera: pembagiannya tidak
        // punya arti, dan kotak yang menyentuh bidang dekat adalah kotak yang
        // menyelimuti kamera. Keduanya dijawab "terlihat".
        if (clip.w <= 1e-6) {
            return false;
        }
        const float3 ndc = clip.xyz / clip.w;
        ndcMin = min(ndcMin, ndc.xy);
        ndcMax = max(ndcMax, ndc.xy);
        nearest = max(nearest, ndc.z);
    }

    // Petak layar dalam UV. Y tidak dibalik di sini: proyeksinya sudah membalik
    // Y (`PerspectiveReversedZ` menulis `-f` di `[1][1]`), jadi arah NDC-nya
    // sudah searah dengan baris framebuffer — dan piramidanya salinan
    // framebuffer itu.
    const float2 uvMin = saturate(ndcMin * 0.5 + 0.5);
    const float2 uvMax = saturate(ndcMax * 0.5 + 0.5);

    // Tingkat yang satu texel-nya sudah menutupi seluruh petak, supaya empat
    // sampel di bawah cukup. Dibulatkan **ke atas**: tingkat yang terlalu halus
    // membuat empat sampel melewatkan bagian tengah petak, dan yang terlewat
    // adalah occluder yang seharusnya menyelamatkannya.
    const float2 sizeTexels = (uvMax - uvMin) * float2(params.counts.zw);
    const float widest = max(max(sizeTexels.x, sizeTexels.y), 1.0);
    const int level = clamp(int(ceil(log2(widest))), 0, int(params.counts.y) - 1);

    // Tiap texel piramida sudah berisi **minimum** sepetaknya; minimum dari
    // keempatnya adalah permukaan terjauh di seluruh petak ini. Kotak tertutup
    // hanya kalau sudut terdekatnya masih lebih jauh daripada itu.
    const int2 size = pyramidLevelSize(level);
    const int2 t0 = clamp(int2(floor(uvMin * float2(size))), int2(0), size - int2(1));
    const int2 t1 = clamp(int2(floor(uvMax * float2(size))), int2(0), size - int2(1));
    float farthest = 1.0;
    for (int ty = t0.y; ty <= t1.y; ++ty) {
        for (int tx = t0.x; tx <= t1.x; ++tx) {
            farthest = min(farthest, occlusionPyramid.Load(int3(tx, ty, level)).r);
        }
    }
    return farthest > nearest;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id: SV_DispatchThreadID) {
    const uint index = id.x;
    if (index >= params.counts.x) {
        return;
    }

    const Bounds box = bounds[index];
    bool visible = true;
    [unroll]
    for (int plane = 0; plane < 6; ++plane) {
        const float4 p = params.planes[plane];
        // Jarak pusat ke bidang, dikurangi jangkauan kotak ke arah bidang itu —
        // uji "sudut positif" yang ditulis tanpa cabang, sama persis dengan yang
        // di CPU.
        const float distance = dot(p.xyz, box.centre.xyz) + p.w;
        const float reach = dot(abs(p.xyz), box.extent.xyz);
        if (distance + reach < 0.0) {
            visible = false;
        }
    }
    if (visible && pc.phase != 0u) {
        visible = !occluded(box);
    }

    const Surface surface = surfaces[index];
    DrawCommand command;
    command.indexCount = surface.indexCount;
    command.instanceCount = visible ? 1u : 0u;
    command.firstIndex = surface.firstIndex;
    command.vertexOffset = 0;
    command.firstInstance = index;
    if (pc.phase != 0u) {
        visibleCommands[index] = command;
    } else {
        commands[index] = command;
    }
}
