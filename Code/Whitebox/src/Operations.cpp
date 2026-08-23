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

// --- sub-objek: simpul dan rusuk (W7.1) --------------------------------------

namespace {

std::vector<Vec3> SnapshotPositions(const HalfEdgeMesh& mesh) {
    std::vector<Vec3> positions;
    positions.reserve(mesh.VertexCount());
    for (uint32_t v = 0; v < mesh.VertexCount(); ++v) {
        positions.push_back(mesh.GetVertex(static_cast<VertexHandle>(v)).position);
    }
    return positions;
}

/// Membangun ulang mesh dari posisi baru, dengan topologi yang tidak berubah.
///
/// Dipisah karena ketiga operasi simpul melakukannya persis sama, dan menyalin
/// urutan "snapshot poligon, ekstrak face, rebuild, regroup" ke tiga tempat
/// adalah tiga tempat yang salah satunya suatu saat lupa me-regroup.
bool ApplyPositions(HalfEdgeMesh& mesh, PolygonSet& polygons,
                    const std::vector<Vec3>& positions, EditResult& result) {
    const std::vector<uint32_t> oldFacePolygon = SnapshotPolygons(mesh, polygons);
    const std::vector<std::vector<VertexHandle>> faces = ExtractFaces(mesh);

    HalfEdgeMesh rebuilt;
    if (!Rebuild(rebuilt, positions, faces)) {
        result.error = "mesh hasil suntingan simpul tidak bisa dibangun";
        return false;
    }
    mesh = std::move(rebuilt);
    RegroupPolygons(mesh, polygons, oldFacePolygon);
    return true;
}

/// Kedua simpul ujung sebuah rusuk.
std::pair<VertexHandle, VertexHandle> EdgeVertices(const HalfEdgeMesh& mesh, EdgeHandle edge) {
    const auto [first, second] = mesh.EdgeHalfEdges(edge);
    if (!IsValid(first) || !IsValid(second)) {
        return {VertexHandle::Invalid, VertexHandle::Invalid};
    }
    return {mesh.GetHalfEdge(first).origin, mesh.GetHalfEdge(second).origin};
}

}  // namespace

EditResult TranslateVertices(HalfEdgeMesh& mesh, PolygonSet& polygons,
                             std::span<const VertexHandle> vertices, const Vec3& displacement) {
    EditResult result;
    if (vertices.empty()) {
        result.error = "tidak ada simpul yang dipilih";
        return result;
    }

    std::vector<Vec3> positions = SnapshotPositions(mesh);

    // Bendera, bukan menggeser langsung: handle yang disebut dua kali di dalam
    // `vertices` akan menggeser simpulnya dua kali, dan seleksi yang datang dari
    // kotak seleksi memang bisa memuat kembar.
    std::vector<uint8_t> moved(mesh.VertexCount(), 0);
    for (const VertexHandle vertex : vertices) {
        const uint32_t index = Index(vertex);
        if (index >= positions.size() || moved[index]) {
            continue;
        }
        moved[index] = 1;
        positions[index] += displacement;
    }

    if (!ApplyPositions(mesh, polygons, positions, result)) {
        return result;
    }
    result.ok = true;
    return result;
}

EditResult AlignVertices(HalfEdgeMesh& mesh, PolygonSet& polygons,
                         std::span<const VertexHandle> vertices, int axis, AlignMode mode) {
    EditResult result;
    if (axis < 0 || axis > 2) {
        result.error = "sumbu harus 0, 1, atau 2";
        return result;
    }

    std::vector<uint32_t> unique;
    std::vector<uint8_t> seen(mesh.VertexCount(), 0);
    for (const VertexHandle vertex : vertices) {
        const uint32_t index = Index(vertex);
        if (index >= mesh.VertexCount() || seen[index]) {
            continue;
        }
        seen[index] = 1;
        unique.push_back(index);
    }
    // Meratakan satu simpul terhadap dirinya sendiri tidak mengubah apa pun, dan
    // entri undo untuk operasi yang tidak mengubah apa pun membuat Ctrl+Z terasa
    // rusak — dua kali tekan untuk membatalkan satu perubahan.
    if (unique.size() < 2) {
        result.error = "perataan menuntut sekurangnya dua simpul";
        return result;
    }

    std::vector<Vec3> positions = SnapshotPositions(mesh);

    float target = positions[unique.front()][axis];
    switch (mode) {
        case AlignMode::Mean: {
            float sum = 0.0f;
            for (const uint32_t index : unique) {
                sum += positions[index][axis];
            }
            target = sum / static_cast<float>(unique.size());
            break;
        }
        case AlignMode::Minimum:
            for (const uint32_t index : unique) {
                target = std::min(target, positions[index][axis]);
            }
            break;
        case AlignMode::Maximum:
            for (const uint32_t index : unique) {
                target = std::max(target, positions[index][axis]);
            }
            break;
    }

    for (const uint32_t index : unique) {
        positions[index][axis] = target;
    }

    if (!ApplyPositions(mesh, polygons, positions, result)) {
        return result;
    }
    result.ok = true;
    return result;
}

EditResult SlideVertexAlongEdge(HalfEdgeMesh& mesh, PolygonSet& polygons, VertexHandle vertex,
                                EdgeHandle edge, float t) {
    EditResult result;
    const uint32_t index = Index(vertex);
    if (index >= mesh.VertexCount()) {
        result.error = "simpul itu tidak ada";
        return result;
    }
    if (static_cast<uint32_t>(edge) >= mesh.EdgeCount()) {
        result.error = "rusuk itu tidak ada";
        return result;
    }

    const auto [a, b] = EdgeVertices(mesh, edge);
    VertexHandle anchor = VertexHandle::Invalid;
    if (a == vertex) {
        anchor = b;
    } else if (b == vertex) {
        anchor = a;
    } else {
        result.error = "rusuk itu tidak menyentuh simpul yang dipilih";
        return result;
    }

    std::vector<Vec3> positions = SnapshotPositions(mesh);
    const Vec3 fixed = positions[Index(anchor)];
    const Vec3 moving = positions[index];

    // Dijepit, bukan dibiarkan melewati ujungnya: simpul yang melewati tetangga
    // di seberangnya membalik urutan face yang memuatnya, dan yang dihasilkan
    // bukan bentuk yang aneh melainkan mesh yang tidak sah.
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    positions[index] = fixed + (moving - fixed) * clamped;

    if (!ApplyPositions(mesh, polygons, positions, result)) {
        return result;
    }
    result.ok = true;
    return result;
}

EditResult SplitEdges(HalfEdgeMesh& mesh, PolygonSet& polygons,
                      std::span<const EdgeHandle> edges) {
    EditResult result;
    if (edges.empty()) {
        result.error = "tidak ada rusuk yang dipilih";
        return result;
    }

    std::vector<Vec3> positions = SnapshotPositions(mesh);

    // Kunci pasangan simpul terurut, bukan nomor rusuk: yang menelusuri loop
    // face di bawah melihat pasangan simpul, dan nomor rusuk tidak muncul di
    // sana sama sekali.
    std::unordered_map<uint64_t, uint32_t> inserted;
    const auto key = [](uint32_t x, uint32_t y) {
        const uint32_t low = std::min(x, y);
        const uint32_t high = std::max(x, y);
        return (static_cast<uint64_t>(low) << 32) | high;
    };

    for (const EdgeHandle edge : edges) {
        if (static_cast<uint32_t>(edge) >= mesh.EdgeCount()) {
            result.error = "rusuk itu tidak ada";
            return result;
        }
        const auto [a, b] = EdgeVertices(mesh, edge);
        if (!IsValid(a) || !IsValid(b)) {
            result.error = "rusuk itu tidak punya kedua ujungnya";
            return result;
        }
        const uint64_t pair = key(Index(a), Index(b));
        if (inserted.count(pair) != 0) {
            continue;  // rusuk yang sama disebut dua kali
        }
        inserted[pair] = static_cast<uint32_t>(positions.size());
        positions.push_back((positions[Index(a)] + positions[Index(b)]) * 0.5f);
    }

    const std::vector<uint32_t> oldFacePolygon = SnapshotPolygons(mesh, polygons);
    std::vector<std::vector<VertexHandle>> faces = ExtractFaces(mesh);

    // **Face dibangun ulang dengan menyisipkan, bukan dengan menyulam.** Setiap
    // loop ditelusuri sekali; di setiap pasangan berurutan yang rusuknya dipecah,
    // simpul barunya masuk di antara keduanya. Face yang dua rusuknya dipecah
    // karena itu naik derajatnya dua, tanpa perlakuan khusus.
    for (std::vector<VertexHandle>& loop : faces) {
        if (loop.size() < 3) {
            continue;
        }
        std::vector<VertexHandle> rebuilt;
        rebuilt.reserve(loop.size() * 2);
        for (std::size_t i = 0; i < loop.size(); ++i) {
            const VertexHandle current = loop[i];
            const VertexHandle following = loop[(i + 1) % loop.size()];
            rebuilt.push_back(current);
            const auto found = inserted.find(key(Index(current), Index(following)));
            if (found != inserted.end()) {
                rebuilt.push_back(static_cast<VertexHandle>(found->second));
            }
        }
        loop = std::move(rebuilt);
    }

    HalfEdgeMesh built;
    if (!Rebuild(built, positions, faces)) {
        result.error = "mesh hasil pemecahan rusuk tidak bisa dibangun";
        return result;
    }
    mesh = std::move(built);
    RegroupPolygons(mesh, polygons, oldFacePolygon);

    result.ok = true;
    return result;
}

EditResult ConnectEdges(HalfEdgeMesh& mesh, PolygonSet& polygons,
                        std::span<const EdgeHandle> edges) {
    EditResult result;
    if (edges.empty()) {
        result.error = "tidak ada rusuk yang dipilih";
        return result;
    }

    std::vector<Vec3> positions = SnapshotPositions(mesh);

    const auto key = [](uint32_t x, uint32_t y) {
        const uint32_t low = std::min(x, y);
        const uint32_t high = std::max(x, y);
        return (static_cast<uint64_t>(low) << 32) | high;
    };

    std::unordered_map<uint64_t, uint32_t> midpoint;
    for (const EdgeHandle edge : edges) {
        if (static_cast<uint32_t>(edge) >= mesh.EdgeCount()) {
            result.error = "rusuk itu tidak ada";
            return result;
        }
        const auto [a, b] = EdgeVertices(mesh, edge);
        if (!IsValid(a) || !IsValid(b)) {
            result.error = "rusuk itu tidak punya kedua ujungnya";
            return result;
        }
        const uint64_t pair = key(Index(a), Index(b));
        if (midpoint.count(pair) != 0) {
            continue;
        }
        midpoint[pair] = static_cast<uint32_t>(positions.size());
        positions.push_back((positions[Index(a)] + positions[Index(b)]) * 0.5f);
    }

    const std::vector<uint32_t> oldFacePolygon = SnapshotPolygons(mesh, polygons);
    const std::vector<std::vector<VertexHandle>> faces = ExtractFaces(mesh);

    // Belahan pertama menggantikan face aslinya di tempatnya; belahan kedua
    // menunggu di sini dan ditambahkan di belakang seluruhnya. Menyisipkannya di
    // tengah akan menggeser nomor face sesudahnya — dan `RegroupPolygons`
    // memulihkan pengelompokan justru dari nomor itu.
    std::vector<std::vector<VertexHandle>> rebuiltFaces;
    std::vector<std::vector<VertexHandle>> appended;
    rebuiltFaces.reserve(faces.size());

    for (const std::vector<VertexHandle>& loop : faces) {
        if (loop.size() < 3) {
            rebuiltFaces.push_back(loop);
            continue;
        }

        std::vector<VertexHandle> expanded;
        std::vector<std::size_t> inserted;  // kedudukan tiap titik tengah di `expanded`
        expanded.reserve(loop.size() * 2);
        for (std::size_t i = 0; i < loop.size(); ++i) {
            const VertexHandle current = loop[i];
            const VertexHandle following = loop[(i + 1) % loop.size()];
            expanded.push_back(current);
            const auto found = midpoint.find(key(Index(current), Index(following)));
            if (found != midpoint.end()) {
                inserted.push_back(expanded.size());
                expanded.push_back(static_cast<VertexHandle>(found->second));
            }
        }

        if (inserted.size() > 2) {
            result.error =
                "sebuah sisi menyentuh lebih dari dua rusuk terpilih; hubungkan sepasang "
                "dahulu, lalu ulangi";
            return result;
        }
        if (inserted.size() < 2) {
            rebuiltFaces.push_back(std::move(expanded));
            continue;
        }

        // Membelah gelang di kedua titik tengahnya. Keduanya masuk ke **dua**
        // belahan sekaligus — di sanalah rusuk barunya lahir, sebagai satu ruas
        // yang dilalui dua face dengan arah berlawanan.
        const std::size_t first = inserted[0];
        const std::size_t second = inserted[1];

        std::vector<VertexHandle> front;
        for (std::size_t i = first; i <= second; ++i) {
            front.push_back(expanded[i]);
        }
        std::vector<VertexHandle> back;
        for (std::size_t i = second; i < expanded.size(); ++i) {
            back.push_back(expanded[i]);
        }
        for (std::size_t i = 0; i <= first; ++i) {
            back.push_back(expanded[i]);
        }

        if (front.size() < 3 || back.size() < 3) {
            result.error = "pembelahan menghasilkan sisi berderajat kurang dari tiga";
            return result;
        }

        rebuiltFaces.push_back(std::move(front));
        appended.push_back(std::move(back));
    }

    for (std::vector<VertexHandle>& loop : appended) {
        rebuiltFaces.push_back(std::move(loop));
    }

    HalfEdgeMesh built;
    if (!Rebuild(built, positions, rebuiltFaces)) {
        result.error = "mesh hasil penyisipan rusuk tidak bisa dibangun";
        return result;
    }
    mesh = std::move(built);
    RegroupPolygons(mesh, polygons, oldFacePolygon);

    result.ok = true;
    return result;
}

}  // namespace sim::whitebox
