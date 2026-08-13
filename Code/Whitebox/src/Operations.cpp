#include "Sim/Whitebox/Operations.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace sim::whitebox {
namespace {

uint32_t Index(VertexHandle h) { return static_cast<uint32_t>(h); }
uint32_t Index(FaceHandle h) { return static_cast<uint32_t>(h); }
uint32_t Index(PolygonHandle h) { return static_cast<uint32_t>(h); }

/// Seluruh face sebuah mesh sebagai daftar simpul — bentuk yang bisa disunting
/// tanpa menyentuh pointer.
std::vector<std::vector<VertexHandle>> ExtractFaces(const HalfEdgeMesh& mesh) {
    std::vector<std::vector<VertexHandle>> faces;
    faces.reserve(mesh.FaceCount());
    for (uint32_t f = 0; f < mesh.FaceCount(); ++f) {
        faces.push_back(mesh.FaceVertices(static_cast<FaceHandle>(f)));
    }
    return faces;
}

/// Membangun ulang mesh dari posisi simpul dan daftar face.
///
/// **Urutan face dipertahankan**, dan itu yang membuat poligon serta seleksi
/// bertahan melewati operasi: face ke-i sebelum tetap face ke-i sesudah.
bool Rebuild(HalfEdgeMesh& mesh, const std::vector<Vec3>& positions,
             const std::vector<std::vector<VertexHandle>>& faces) {
    HalfEdgeMesh built;
    for (const Vec3& position : positions) {
        built.AddVertex(position);
    }
    for (const std::vector<VertexHandle>& loop : faces) {
        if (built.AddFace(loop) == FaceHandle::Invalid) {
            return false;
        }
    }
    built.FinalizeBoundaries();
    mesh = std::move(built);
    return true;
}

/// Menyusun ulang pengelompokan poligon dari pengelompokan lama.
///
/// Face lama mempertahankan nomornya, jadi pengelompokannya bisa dipulihkan
/// dengan menyembunyikan kembali setiap rusuk yang kedua sisinya dahulu satu
/// poligon. Face baru — dinding hasil ekstrusi — dimulai sendiri-sendiri.
void RegroupPolygons(const HalfEdgeMesh& mesh, PolygonSet& polygons,
                     const std::vector<uint32_t>& oldFacePolygon) {
    polygons.Reset(mesh);
    for (uint32_t e = 0; e < mesh.EdgeCount(); ++e) {
        const auto [a, b] = mesh.EdgeFaces(static_cast<EdgeHandle>(e));
        if (!IsValid(a) || !IsValid(b)) {
            continue;
        }
        const uint32_t indexA = Index(a);
        const uint32_t indexB = Index(b);
        if (indexA >= oldFacePolygon.size() || indexB >= oldFacePolygon.size()) {
            continue;  // salah satunya face baru: biarkan berdiri sendiri
        }
        if (oldFacePolygon[indexA] == oldFacePolygon[indexB]) {
            // Toleransi lebar: pengelompokan lama sudah pernah dinyatakan sah,
            // dan ekstrusi tidak memiringkan sisi terhadap dirinya sendiri.
            polygons.HideEdge(mesh, static_cast<EdgeHandle>(e), 180.0f);
        }
    }
}

/// Cuplikan pengelompokan sekarang, per face.
std::vector<uint32_t> SnapshotPolygons(const HalfEdgeMesh& mesh, const PolygonSet& polygons) {
    std::vector<uint32_t> result(mesh.FaceCount(), 0);
    for (uint32_t f = 0; f < mesh.FaceCount(); ++f) {
        result[f] = Index(polygons.FacePolygon(static_cast<FaceHandle>(f)));
    }
    return result;
}

}  // namespace

EditResult ExtrudePolygon(HalfEdgeMesh& mesh, PolygonSet& polygons, PolygonHandle polygon,
                          float distance) {
    EditResult result;

    const std::vector<FaceHandle> members = polygons.PolygonFaces(polygon);
    if (members.empty()) {
        result.error = "poligon itu tidak ada";
        return result;
    }

    // **Jarak nol tidak mengubah apa pun.** Menumbuhkan dinding berluas nol
    // menghasilkan face yang normalnya tidak tertentu, dan itu tidak terlihat
    // sekarang — ia muncul jauh kemudian sebagai bercak gelap di pencahayaan.
    if (std::abs(distance) < 1e-6f) {
        result.ok = true;
        result.polygon = polygon;
        return result;
    }

    const Vec3 normal = polygons.PolygonNormal(mesh, polygon);
    if (glm::length(normal) < 0.5f) {
        result.error = "poligon itu tidak punya normal yang tertentu";
        return result;
    }
    const Vec3 offset = normal * distance;

    std::vector<uint8_t> inPolygon(mesh.FaceCount(), 0);
    for (const FaceHandle face : members) {
        inPolygon[Index(face)] = 1;
    }

    const std::vector<uint32_t> oldFacePolygon = SnapshotPolygons(mesh, polygons);
    std::vector<std::vector<VertexHandle>> faces = ExtractFaces(mesh);

    std::vector<Vec3> positions;
    positions.reserve(mesh.VertexCount());
    for (uint32_t v = 0; v < mesh.VertexCount(); ++v) {
        positions.push_back(mesh.GetVertex(static_cast<VertexHandle>(v)).position);
    }

    // Tiap simpul yang dipakai poligon mendapat kembaran yang digeser. Yang di
    // luar poligon tetap di tempatnya — itulah yang membuat dindingnya tumbuh
    // alih-alih seluruh benda bergeser.
    std::unordered_map<uint32_t, VertexHandle> duplicate;
    for (const FaceHandle face : members) {
        for (const VertexHandle vertex : faces[Index(face)]) {
            if (duplicate.count(Index(vertex)) != 0) {
                continue;
            }
            positions.push_back(positions[Index(vertex)] + offset);
            duplicate.emplace(Index(vertex),
                              static_cast<VertexHandle>(
                                  static_cast<uint32_t>(positions.size() - 1)));
        }
    }

    // Rusuk pinggir poligon: yang face seberangnya berada di luar poligon.
    // Di sanalah dinding tumbuh; rusuk di dalam poligon tidak menumbuhkan apa
    // pun, karena kedua sisinya sama-sama ikut terangkat.
    std::vector<std::pair<VertexHandle, VertexHandle>> border;
    for (const FaceHandle face : members) {
        for (const HalfEdgeHandle halfEdge : mesh.FaceHalfEdges(face)) {
            const HalfEdge& edge = mesh.GetHalfEdge(halfEdge);
            const FaceHandle opposite = mesh.GetHalfEdge(edge.twin).face;
            const bool outside = !IsValid(opposite) || !inPolygon[Index(opposite)];
            if (!outside) {
                continue;
            }
            const VertexHandle from = edge.origin;
            const VertexHandle to = mesh.GetHalfEdge(edge.next).origin;
            border.emplace_back(from, to);
        }
    }
    if (border.empty()) {
        result.error = "poligon itu tidak punya pinggir";
        return result;
    }

    // Poligonnya sendiri kini memakai kembaran: ia yang terangkat.
    for (const FaceHandle face : members) {
        for (VertexHandle& vertex : faces[Index(face)]) {
            vertex = duplicate.at(Index(vertex));
        }
    }

    // Dinding, satu quad per rusuk pinggir. Urutan simpulnya mengikuti arah
    // rusuk pinggir, jadi normalnya menghadap keluar tanpa perlu diperiksa
    // terhadap apa pun — arah itu sudah dijamin oleh arah half-edge asalnya.
    for (const auto& [from, to] : border) {
        faces.push_back({from, to, duplicate.at(Index(to)), duplicate.at(Index(from))});
    }

    HalfEdgeMesh rebuilt;
    if (!Rebuild(rebuilt, positions, faces)) {
        result.error = "mesh hasil ekstrusi tidak bisa dibangun";
        return result;
    }

    mesh = std::move(rebuilt);
    RegroupPolygons(mesh, polygons, oldFacePolygon);

    result.ok = true;
    // Nomor face poligonnya tidak berubah, jadi perwakilannya pun sama.
    result.polygon = polygons.FacePolygon(members.front());
    return result;
}

EditResult TranslatePolygon(HalfEdgeMesh& mesh, PolygonSet& polygons, PolygonHandle polygon,
                            const Vec3& displacement) {
    EditResult result;

    const std::vector<FaceHandle> members = polygons.PolygonFaces(polygon);
    if (members.empty()) {
        result.error = "poligon itu tidak ada";
        return result;
    }

    // Menggeser simpul tidak mengubah topologi sama sekali, jadi tidak ada yang
    // perlu dibangun ulang — dan poligon beserta seleksinya tidak tersentuh.
    std::vector<Vec3> positions;
    positions.reserve(mesh.VertexCount());
    for (uint32_t v = 0; v < mesh.VertexCount(); ++v) {
        positions.push_back(mesh.GetVertex(static_cast<VertexHandle>(v)).position);
    }

    std::vector<uint8_t> moved(mesh.VertexCount(), 0);
    for (const FaceHandle face : members) {
        for (const VertexHandle vertex : mesh.FaceVertices(face)) {
            if (!moved[Index(vertex)]) {
                moved[Index(vertex)] = 1;
                positions[Index(vertex)] += displacement;
            }
        }
    }

    const std::vector<uint32_t> oldFacePolygon = SnapshotPolygons(mesh, polygons);
    const std::vector<std::vector<VertexHandle>> faces = ExtractFaces(mesh);

    HalfEdgeMesh rebuilt;
    if (!Rebuild(rebuilt, positions, faces)) {
        result.error = "mesh hasil geser tidak bisa dibangun";
        return result;
    }
    mesh = std::move(rebuilt);
    RegroupPolygons(mesh, polygons, oldFacePolygon);

    result.ok = true;
    result.polygon = polygons.FacePolygon(members.front());
    return result;
}

}  // namespace sim::whitebox
