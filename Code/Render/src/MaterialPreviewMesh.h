#pragma once

#include "Sim/Core/Math.h"
#include "Sim/Render/IMaterialPreview.h"

#include <cstdint>
#include <vector>

namespace sim::render {

/// Vertex yang bentuknya **wajib** sama dengan `MaterialVertex` di modul Slang
/// yang dirakit `AssembleMaterialModule`.
///
/// Urutan dan lokasinya diambil dari `MaterialVertexLocation`, bukan dari
/// hitungan sendiri. Atribut tulang tetap ada meski preview tidak pernah
/// melakukan skinning: daftar antarmuka `OpEntryPoint` sudah terkunci di modul,
/// jadi atribut yang tidak dipasang menghasilkan nilai tak terdefinisi — bukan
/// pesan galat.
struct PreviewVertex {
    Vec3 position{0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    Vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
    Vec2 uv{0.0f};
    Vec4 color{1.0f};
    uint32_t boneIndices[4]{0, 0, 0, 0};
    Vec4 boneWeights{1.0f, 0.0f, 0.0f, 0.0f};
};

struct PreviewMeshData {
    std::vector<PreviewVertex> vertices;
    std::vector<uint32_t> indices;
};

/// Membangun geometri sebuah bentuk preview.
///
/// **Normal dan tangent dihitung analitis, bukan dirata-ratakan dari segitiga.**
/// Ketiga bentuknya punya bentuk tertutup yang persis, dan rata-rata per-vertex
/// akan memasukkan galat kecil yang justru paling terlihat pada material yang
/// mengkilap — yaitu material yang paling sering dilihat orang di preview.
PreviewMeshData BuildPreviewMesh(PreviewShape shape);

}  // namespace sim::render
