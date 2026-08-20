// Frustum culling di GPU, satu thread per permukaan (G6).
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

struct Params {
    /// Enam bidang frustum, sudah dinormalkan. Urutannya urutan `Frustum`.
    float4 planes[6];
    /// x: banyaknya permukaan. Sisanya belum dipakai.
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

[[vk::binding(0, 0)]] ConstantBuffer<Params> params;
[[vk::binding(1, 0)]] StructuredBuffer<Bounds> bounds;
[[vk::binding(2, 0)]] StructuredBuffer<Surface> surfaces;
[[vk::binding(3, 0)]] RWStructuredBuffer<DrawCommand> commands;

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

    const Surface surface = surfaces[index];
    DrawCommand command;
    command.indexCount = surface.indexCount;
    command.instanceCount = visible ? 1u : 0u;
    command.firstIndex = surface.firstIndex;
    command.vertexOffset = 0;
    command.firstInstance = index;
    commands[index] = command;
}
