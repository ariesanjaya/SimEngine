// Pembangkit UV lightmap — S4 di docs/PLAN-STATIC-GI.md.
//
// **Dependensi opsional, dengan aturan yang sama seperti MaterialX dan
// OpenUSD:** berkas ini tetap ikut dibangun tanpa xatlas dan menolak dengan
// pesan yang menyebut sakelarnya. Yang membangun tanpanya kehilangan
// pembangkitan UV lightmap, bukan seluruh mesin.

#include "Sim/Assets/LightmapUv.h"

#include "Sim/Core/Log.h"

#include <algorithm>
#include <vector>

#if SIM_WITH_XATLAS
#include <xatlas.h>
#endif

namespace sim::assets {

#if SIM_WITH_XATLAS

bool HasLightmapUnwrapper() { return true; }

LightmapUnwrapResult GenerateLightmapUv(MeshData& mesh) {
    LightmapUnwrapResult result;
    if (!mesh.IsValid()) {
        result.error = "mesh has no triangles to unwrap";
        return result;
    }

    xatlas::Atlas* atlas = xatlas::Create();

    xatlas::MeshDecl declaration;
    declaration.vertexCount = static_cast<uint32_t>(mesh.vertices.size());
    declaration.vertexPositionData = &mesh.vertices[0].position;
    declaration.vertexPositionStride = sizeof(MeshVertex);
    // **Normalnya ikut diserahkan.** Tanpanya xatlas memutuskan batas chart dari
    // geometri saja, dan sebuah tepi tajam yang mulus di posisi tetapi patah di
    // normal berakhir di dalam satu chart — lalu cahaya di kedua sisinya
    // dirata-rata melintasi lipatan yang seharusnya memisahkannya.
    declaration.vertexNormalData = &mesh.vertices[0].normal;
    declaration.vertexNormalStride = sizeof(MeshVertex);
    declaration.indexCount = static_cast<uint32_t>(mesh.indices.size());
    declaration.indexData = mesh.indices.data();
    declaration.indexFormat = xatlas::IndexFormat::UInt32;

    if (const xatlas::AddMeshError error = xatlas::AddMesh(atlas, declaration);
        error != xatlas::AddMeshError::Success) {
        result.error = std::string("xatlas rejected the mesh: ") +
                       xatlas::StringForEnum(error);
        xatlas::Destroy(atlas);
        return result;
    }

    xatlas::ChartOptions chartOptions;
    xatlas::PackOptions packOptions;
    // **Padding satu texel, dan itu bukan kerapian.** Dua chart yang bersentuhan
    // tanpa jarak saling meminjam texel lewat interpolasi bilinear, dan yang
    // terlihat adalah garis cahaya asing di sepanjang jahitannya.
    packOptions.padding = 1;
    // Resolusi nol berarti xatlas memilihnya sendiri dari luas chart-nya. Yang
    // memilih resolusi lightmap sungguhan adalah S5, dari kerapatan texel yang
    // disetel pengarang — di sini yang dibutuhkan hanya tata letak yang sah.
    packOptions.resolution = 0;
    packOptions.bruteForce = false;

    xatlas::Generate(atlas, chartOptions, packOptions);
    if (atlas->meshCount == 0 || atlas->width == 0 || atlas->height == 0) {
        result.error = "xatlas produced an empty atlas";
        xatlas::Destroy(atlas);
        return result;
    }

    const xatlas::Mesh& output = atlas->meshes[0];
    const float width = static_cast<float>(atlas->width);
    const float height = static_cast<float>(atlas->height);

    // **Vertexnya disusun ulang, bukan ditambahi atribut.** Sebuah vertex yang
    // dipakai dua chart dipecah xatlas — ia tidak bisa membawa dua UV lightmap
    // sekaligus — jadi yang keluar adalah daftar vertex yang lain dengan
    // `xref` menunjuk asalnya.
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> sources;
    vertices.reserve(output.vertexCount);
    sources.reserve(output.vertexCount);
    for (uint32_t i = 0; i < output.vertexCount; ++i) {
        const xatlas::Vertex& produced = output.vertexArray[i];
        MeshVertex vertex = mesh.vertices[produced.xref];
        vertex.lightmapUv = Vec2(produced.uv[0] / width, produced.uv[1] / height);
        vertices.push_back(vertex);
        sources.push_back(produced.xref);
    }

    std::vector<uint32_t> indices(output.indexArray, output.indexArray + output.indexCount);

    // **Ruas material dipetakan ulang lewat urutan segitiga**, dan itu langkah
    // yang paling mudah dilewatkan: xatlas mempertahankan urutan segitiganya,
    // jadi segitiga ke-n keluar tetap ke-n — tetapi indeksnya menunjuk vertex
    // yang lain. Ruas yang tidak ikut dipetakan menghasilkan mesh yang benar
    // bentuknya dan salah materialnya.
    if (output.indexCount != mesh.indices.size()) {
        result.error = "xatlas changed the triangle count, which submesh ranges assume it will not";
        xatlas::Destroy(atlas);
        return result;
    }

    result.chartCount = output.chartCount;
    result.vertexCount = output.vertexCount;
    result.utilisation = atlas->utilization != nullptr ? atlas->utilization[0] : 0.0f;
    result.ok = true;

    mesh.vertices = std::move(vertices);
    mesh.indices = std::move(indices);
    mesh.lightmapVertexSource = std::move(sources);
    mesh.hasLightmapUv = true;
    // Pengaruh skin ikut vertexnya. Mesh statis tidak punya, dan mesh ber-skin
    // bukan calon lightmap — tetapi membiarkannya sejajar dengan daftar vertex
    // yang lama adalah larik yang panjangnya tidak lagi cocok, dan itu kerusakan
    // memori pada pemakai berikutnya alih-alih galat di sini.
    if (!mesh.influences.empty()) {
        std::vector<SkinInfluence> influences;
        influences.reserve(output.vertexCount);
        for (uint32_t i = 0; i < output.vertexCount; ++i) {
            influences.push_back(mesh.influences[output.vertexArray[i].xref]);
        }
        mesh.influences = std::move(influences);
    }

    xatlas::Destroy(atlas);
    return result;
}

#else

bool HasLightmapUnwrapper() { return false; }

LightmapUnwrapResult GenerateLightmapUv(MeshData& mesh) {
    LightmapUnwrapResult result;
    result.error =
        "pembangkit UV lightmap tidak ikut dibangun (SIM_WITH_XATLAS=OFF), jadi mesh ini "
        "tidak bisa dipanggangi lightmap";
    (void)mesh;
    return result;
}

#endif  // SIM_WITH_XATLAS

}  // namespace sim::assets
