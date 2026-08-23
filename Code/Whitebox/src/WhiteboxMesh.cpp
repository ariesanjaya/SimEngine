#include "Sim/Whitebox/WhiteboxMesh.h"

#include <algorithm>
#include <map>

namespace sim::whitebox {
namespace {

uint32_t Index(PolygonHandle h) { return static_cast<uint32_t>(h); }

/// Basis planar untuk memproyeksikan UV sebuah sisi.
///
/// **Cukup untuk melihat skala tekstur benar saat merancang**, dan tidak lebih.
/// UV yang digarap adalah pekerjaan DCC; whitebox hanya perlu menunjukkan bahwa
/// ubinnya sebesar apa.
void PlanarBasis(const Vec3& normal, Vec3& outU, Vec3& outV) {
    // Sumbu bantu yang paling tidak sejajar dengan normalnya, supaya hasil kali
    // silangnya tidak mendekati nol.
    const Vec3 helper = std::abs(normal.y) < 0.9f ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(1.0f, 0.0f, 0.0f);
    outU = glm::normalize(glm::cross(helper, normal));
    outV = glm::cross(normal, outU);
}

}  // namespace

WhiteboxMesh WhiteboxMesh::MakeCube() {
    WhiteboxMesh box;
    box.mesh_ = MakeUnitCube();
    box.polygons_.Reset(box.mesh_);
    box.polygonMaterial_.assign(box.mesh_.FaceCount(), kNoMaterial);
    return box;
}

WhiteboxMesh WhiteboxMesh::FromMesh(HalfEdgeMesh mesh) {
    WhiteboxMesh out;
    out.mesh_ = std::move(mesh);
    out.polygons_.Reset(out.mesh_);
    out.polygonMaterial_.assign(out.mesh_.FaceCount(), kNoMaterial);
    return out;
}

WhiteboxMesh WhiteboxMesh::MakeRamp() { return FromMesh(MakeUnitRamp()); }

WhiteboxMesh WhiteboxMesh::MakeStairs(uint32_t steps) { return FromMesh(MakeUnitStairs(steps)); }

WhiteboxMesh WhiteboxMesh::MakeCylinder(uint32_t sides) {
    return FromMesh(MakeUnitCylinder(sides));
}

WhiteboxMesh WhiteboxMesh::MakeCone(uint32_t sides) { return FromMesh(MakeUnitCone(sides)); }

WhiteboxMesh WhiteboxMesh::MakeTriangulatedCube() {
    WhiteboxMesh box;
    box.mesh_ = MakeUnitCubeTriangulated();
    box.polygons_.Reset(box.mesh_);
    box.polygonMaterial_.assign(box.mesh_.FaceCount(), kNoMaterial);
    return box;
}

namespace {

/// Cuplikan face → poligon sebelum sebuah operasi, supaya materialnya bisa
/// diikutkan pindah sesudahnya.
std::vector<uint32_t> Snapshot(const HalfEdgeMesh& mesh, const PolygonSet& polygons) {
    std::vector<uint32_t> result(mesh.FaceCount(), 0);
    for (uint32_t f = 0; f < mesh.FaceCount(); ++f) {
        result[f] = static_cast<uint32_t>(polygons.FacePolygon(static_cast<FaceHandle>(f)));
    }
    return result;
}

}  // namespace

EditResult WhiteboxMesh::Extrude(PolygonHandle polygon, float distance) {
    const std::vector<uint32_t> before = Snapshot(mesh_, polygons_);
    EditResult result = ExtrudePolygon(mesh_, polygons_, polygon, distance);
    if (result.ok) {
        RemapMaterials(before);
    }
    return result;
}

EditResult WhiteboxMesh::Translate(PolygonHandle polygon, const Vec3& displacement) {
    const std::vector<uint32_t> before = Snapshot(mesh_, polygons_);
    EditResult result = TranslatePolygon(mesh_, polygons_, polygon, displacement);
    if (result.ok) {
        RemapMaterials(before);
    }
    return result;
}

EditResult WhiteboxMesh::TranslateVertices(std::span<const VertexHandle> vertices,
                                           const Vec3& displacement) {
    const std::vector<uint32_t> before = Snapshot(mesh_, polygons_);
    EditResult result = whitebox::TranslateVertices(mesh_, polygons_, vertices, displacement);
    if (result.ok) {
        RemapMaterials(before);
    }
    return result;
}

EditResult WhiteboxMesh::AlignVertices(std::span<const VertexHandle> vertices, int axis,
                                       AlignMode mode) {
    const std::vector<uint32_t> before = Snapshot(mesh_, polygons_);
    EditResult result = whitebox::AlignVertices(mesh_, polygons_, vertices, axis, mode);
    if (result.ok) {
        RemapMaterials(before);
    }
    return result;
}

EditResult WhiteboxMesh::SlideVertexAlongEdge(VertexHandle vertex, EdgeHandle edge, float t) {
    const std::vector<uint32_t> before = Snapshot(mesh_, polygons_);
    EditResult result = whitebox::SlideVertexAlongEdge(mesh_, polygons_, vertex, edge, t);
    if (result.ok) {
        RemapMaterials(before);
    }
    return result;
}

EditResult WhiteboxMesh::SplitEdges(std::span<const EdgeHandle> edges) {
    const std::vector<uint32_t> before = Snapshot(mesh_, polygons_);
    EditResult result = whitebox::SplitEdges(mesh_, polygons_, edges);
    if (result.ok) {
        RemapMaterials(before);
    }
    return result;
}

EditResult WhiteboxMesh::ConnectEdges(std::span<const EdgeHandle> edges) {
    const std::vector<uint32_t> before = Snapshot(mesh_, polygons_);
    EditResult result = whitebox::ConnectEdges(mesh_, polygons_, edges);
    if (result.ok) {
        RemapMaterials(before);
    }
    return result;
}

bool WhiteboxMesh::HideEdge(EdgeHandle edge, float toleranceDegrees) {
    const std::vector<uint32_t> before = Snapshot(mesh_, polygons_);
    if (!polygons_.HideEdge(mesh_, edge, toleranceDegrees)) {
        return false;
    }
    RemapMaterials(before);
    return true;
}

bool WhiteboxMesh::RestoreEdge(EdgeHandle edge) {
    const std::vector<uint32_t> before = Snapshot(mesh_, polygons_);
    if (!polygons_.RestoreEdge(mesh_, edge)) {
        return false;
    }
    RemapMaterials(before);
    return true;
}

std::size_t WhiteboxMesh::MergeCoplanar(float toleranceDegrees) {
    const std::vector<uint32_t> before = Snapshot(mesh_, polygons_);
    const std::size_t merged = polygons_.MergeCoplanar(mesh_, toleranceDegrees);
    if (merged > 0) {
        RemapMaterials(before);
    }
    return merged;
}

bool WhiteboxMesh::Build(WhiteboxMesh& out, const WhiteboxData& data, std::string& error) {
    HalfEdgeMesh mesh;
    for (const Vec3& position : data.positions) {
        mesh.AddVertex(position);
    }
    for (const std::vector<uint32_t>& face : data.faces) {
        std::vector<VertexHandle> loop;
        loop.reserve(face.size());
        for (const uint32_t index : face) {
            if (index >= data.positions.size()) {
                error = "sebuah face menunjuk simpul yang tidak ada";
                return false;
            }
            loop.push_back(static_cast<VertexHandle>(index));
        }
        if (mesh.AddFace(loop) == FaceHandle::Invalid) {
            error = "sebuah face ditolak: simpulnya berulang, terlalu sedikit, atau "
                    "geometrinya non-manifold";
            return false;
        }
    }
    mesh.FinalizeBoundaries();

    const MeshCheck topology = mesh.CheckInvariants();
    if (!topology.ok) {
        error = "topologi hasil muat tidak sah: " + topology.error;
        return false;
    }

    WhiteboxMesh box;
    box.mesh_ = std::move(mesh);
    box.polygons_.Reset(box.mesh_);
    box.polygonMaterial_.assign(box.mesh_.FaceCount(), kNoMaterial);

    // Rusuk tersembunyi dicari lewat pasangan simpulnya. Toleransi dibuka lebar
    // karena berkasnya sudah menyatakan pengelompokan yang dahulu sah — menolak
    // di sini berarti berkas yang tersimpan benar tidak bisa dimuat kembali.
    for (const auto& [a, b] : data.hiddenEdges) {
        bool found = false;
        for (uint32_t e = 0; e < box.mesh_.EdgeCount() && !found; ++e) {
            const auto [first, second] = box.mesh_.EdgeHalfEdges(static_cast<EdgeHandle>(e));
            if (!IsValid(first) || !IsValid(second)) {
                continue;
            }
            const uint32_t from = static_cast<uint32_t>(box.mesh_.GetHalfEdge(first).origin);
            const uint32_t to = static_cast<uint32_t>(box.mesh_.GetHalfEdge(second).origin);
            if ((from == a && to == b) || (from == b && to == a)) {
                box.polygons_.HideEdge(box.mesh_, static_cast<EdgeHandle>(e), 180.0f);
                found = true;
            }
        }
        if (!found) {
            error = "sebuah rusuk tersembunyi menunjuk pasangan simpul yang tidak bertetangga";
            return false;
        }
    }

    // Material dibaca dari face perwakilan tiap poligon. Menyimpannya per face
    // membuat bentuk berkasnya tidak perlu tahu apa itu poligon sama sekali.
    for (const PolygonHandle polygon : box.polygons_.Polygons()) {
        const uint32_t representative = Index(polygon);
        if (representative < data.faceMaterials.size()) {
            box.polygonMaterial_[representative] = data.faceMaterials[representative];
        }
    }

    const MeshCheck check = box.CheckInvariants();
    if (!check.ok) {
        error = check.error;
        return false;
    }

    out = std::move(box);
    return true;
}

WhiteboxData WhiteboxMesh::ToData() const {
    WhiteboxData data;
    data.positions.reserve(mesh_.VertexCount());
    for (uint32_t v = 0; v < mesh_.VertexCount(); ++v) {
        data.positions.push_back(mesh_.GetVertex(static_cast<VertexHandle>(v)).position);
    }
    data.faces.reserve(mesh_.FaceCount());
    data.faceMaterials.reserve(mesh_.FaceCount());
    for (uint32_t f = 0; f < mesh_.FaceCount(); ++f) {
        const FaceHandle face = static_cast<FaceHandle>(f);
        std::vector<uint32_t> loop;
        for (const VertexHandle vertex : mesh_.FaceVertices(face)) {
            loop.push_back(static_cast<uint32_t>(vertex));
        }
        data.faces.push_back(std::move(loop));
        data.faceMaterials.push_back(PolygonMaterial(polygons_.FacePolygon(face)));
    }

    for (uint32_t e = 0; e < mesh_.EdgeCount(); ++e) {
        const EdgeHandle edge = static_cast<EdgeHandle>(e);
        if (!polygons_.IsEdgeHidden(edge)) {
            continue;
        }
        const auto [first, second] = mesh_.EdgeHalfEdges(edge);
        const uint32_t a = static_cast<uint32_t>(mesh_.GetHalfEdge(first).origin);
        const uint32_t b = static_cast<uint32_t>(mesh_.GetHalfEdge(second).origin);
        data.hiddenEdges.emplace_back(std::min(a, b), std::max(a, b));
    }
    // Diurutkan supaya keluarannya tetap: berkas yang isinya bergeser tanpa ada
    // yang menyuntingnya menghasilkan diff palsu, dan diff palsu membuat yang
    // sungguhan tidak terbaca.
    std::sort(data.hiddenEdges.begin(), data.hiddenEdges.end());
    return data;
}

int WhiteboxMesh::PolygonMaterial(PolygonHandle polygon) const {
    const uint32_t index = Index(polygon);
    return index < polygonMaterial_.size() ? polygonMaterial_[index] : kNoMaterial;
}

bool WhiteboxMesh::SetPolygonMaterial(PolygonHandle polygon, int material) {
    const uint32_t index = Index(polygon);
    if (index >= polygonMaterial_.size() || polygons_.PolygonFaces(polygon).empty()) {
        return false;
    }
    polygonMaterial_[index] = material;
    return true;
}

std::size_t WhiteboxMesh::UsedMaterialCount() const {
    std::vector<int> slots;
    for (const PolygonHandle polygon : polygons_.Polygons()) {
        slots.push_back(PolygonMaterial(polygon));
    }
    std::sort(slots.begin(), slots.end());
    slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
    return slots.size();
}

assets::MeshData WhiteboxMesh::BuildMeshData() const {
    assets::MeshData data;

    // Sisi dikelompokkan menurut slot materialnya. `std::map` dipakai karena
    // urutannya harus tetap: ruas yang berpindah-pindah urutan membuat daftar
    // material di `MeshRendererComponent` — yang diindeks urutan ruas — menunjuk
    // sisi yang berbeda setiap kali meshnya dibangun ulang.
    std::map<int, std::vector<PolygonHandle>> byMaterial;
    for (const PolygonHandle polygon : polygons_.Polygons()) {
        byMaterial[PolygonMaterial(polygon)].push_back(polygon);
    }

    for (const auto& [material, group] : byMaterial) {
        assets::SubMesh part;
        part.firstIndex = static_cast<uint32_t>(data.indices.size());
        part.material = material;

        for (const PolygonHandle polygon : group) {
            const Vec3 normal = polygons_.PolygonNormal(mesh_, polygon);
            Vec3 axisU(1.0f, 0.0f, 0.0f);
            Vec3 axisV(0.0f, 0.0f, 1.0f);
            PlanarBasis(normal, axisU, axisV);

            for (const FaceHandle face : polygons_.PolygonFaces(polygon)) {
                const std::vector<VertexHandle> loop = mesh_.FaceVertices(face);
                if (loop.size() < 3) {
                    continue;
                }
                const uint32_t base = static_cast<uint32_t>(data.vertices.size());

                // **Simpul tidak dibagi antar sisi.** Whitebox digambar rata:
                // dua sisi yang bertemu di sebuah rusuk punya normal berbeda,
                // dan simpul yang dibagi hanya bisa membawa satu di antaranya —
                // hasilnya rusuk yang membulat, yang justru menghapus tampilan
                // blockout.
                for (const VertexHandle vertex : loop) {
                    const Vec3& position = mesh_.GetVertex(vertex).position;
                    assets::MeshVertex out;
                    out.position = position;
                    out.normal = normal;
                    out.uv = Vec2(glm::dot(position, axisU), glm::dot(position, axisV));
                    out.tangent = Vec4(axisU, 1.0f);
                    data.vertices.push_back(out);
                }

                // Kipas segitiga dari simpul pertama. Sah untuk sisi cembung,
                // dan sisi whitebox cembung selama hanya ekstrusi dan geser yang
                // dipakai.
                for (std::size_t i = 1; i + 1 < loop.size(); ++i) {
                    data.indices.push_back(base);
                    data.indices.push_back(base + static_cast<uint32_t>(i));
                    data.indices.push_back(base + static_cast<uint32_t>(i + 1));
                }
            }
        }

        part.indexCount = static_cast<uint32_t>(data.indices.size()) - part.firstIndex;
        // Ruas kosong tidak diterbitkan: ia satu panggilan gambar yang tidak
        // menggambar apa pun, dan satu slot material yang menyesatkan.
        if (part.indexCount > 0) {
            data.parts.push_back(part);
        }
    }

    if (!data.vertices.empty()) {
        data.boundsMin = data.vertices.front().position;
        data.boundsMax = data.vertices.front().position;
        for (const assets::MeshVertex& vertex : data.vertices) {
            data.boundsMin = glm::min(data.boundsMin, vertex.position);
            data.boundsMax = glm::max(data.boundsMax, vertex.position);
        }
    }

    return data;
}

void WhiteboxMesh::RemapMaterials(const std::vector<uint32_t>& oldFacePolygon) {
    // Poligon dikenali face terkecilnya, jadi sesudah topologinya berubah
    // sebuah sisi bisa berpindah nomor. Materialnya ikut dipindahkan, bukan
    // ditinggalkan di nomor lama — sisi yang kehilangan materialnya sesudah
    // didorong adalah kejutan yang menyalahkan operasi dorongnya.
    std::vector<int> updated(mesh_.FaceCount(), kNoMaterial);
    for (const PolygonHandle polygon : polygons_.Polygons()) {
        const std::vector<FaceHandle> faces = polygons_.PolygonFaces(polygon);
        for (const FaceHandle face : faces) {
            const uint32_t index = static_cast<uint32_t>(face);
            if (index >= oldFacePolygon.size()) {
                continue;  // face baru: belum punya material
            }
            const uint32_t oldPolygon = oldFacePolygon[index];
            if (oldPolygon < polygonMaterial_.size() &&
                polygonMaterial_[oldPolygon] != kNoMaterial) {
                updated[Index(polygon)] = polygonMaterial_[oldPolygon];
                break;
            }
        }
    }
    polygonMaterial_ = std::move(updated);
}

MeshCheck WhiteboxMesh::CheckInvariants() const {
    const MeshCheck topology = mesh_.CheckInvariants();
    if (!topology.ok) {
        return topology;
    }
    const MeshCheck grouping = polygons_.CheckInvariants(mesh_);
    if (!grouping.ok) {
        return grouping;
    }
    if (polygonMaterial_.size() != mesh_.FaceCount()) {
        return MeshCheck{false, "daftar material tidak sejajar dengan jumlah face"};
    }
    return MeshCheck{};
}

}  // namespace sim::whitebox
