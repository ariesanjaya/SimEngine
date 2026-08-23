#include "Sim/Whitebox/HalfEdgeMesh.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sim::whitebox {
namespace {

uint32_t Index(VertexHandle h) { return static_cast<uint32_t>(h); }
uint32_t Index(HalfEdgeHandle h) { return static_cast<uint32_t>(h); }
uint32_t Index(FaceHandle h) { return static_cast<uint32_t>(h); }

/// Kunci arah rusuk: dari simpul `a` ke simpul `b`.
///
/// Dipakai mencari pasangan saat face ditambahkan. Arahnya penting — dua face
/// bertetangga menelusuri rusuk yang sama ke arah berlawanan, dan itulah yang
/// membuat keduanya bisa saling menemukan.
uint64_t DirectedKey(VertexHandle a, VertexHandle b) {
    return (static_cast<uint64_t>(Index(a)) << 32) | static_cast<uint64_t>(Index(b));
}

}  // namespace

void HalfEdgeMesh::Clear() {
    vertices_.clear();
    halfEdges_.clear();
    edges_.clear();
    faces_.clear();
    halfEdgeEdge_.clear();
}

VertexHandle HalfEdgeMesh::AddVertex(const Vec3& position) {
    Vertex vertex;
    vertex.position = position;
    vertices_.push_back(vertex);
    return static_cast<VertexHandle>(static_cast<uint32_t>(vertices_.size() - 1));
}

const Vertex& HalfEdgeMesh::GetVertex(VertexHandle h) const { return vertices_[Index(h)]; }
const HalfEdge& HalfEdgeMesh::GetHalfEdge(HalfEdgeHandle h) const { return halfEdges_[Index(h)]; }
const Edge& HalfEdgeMesh::GetEdge(EdgeHandle h) const {
    return edges_[static_cast<uint32_t>(h)];
}
const Face& HalfEdgeMesh::GetFace(FaceHandle h) const { return faces_[Index(h)]; }

FaceHandle HalfEdgeMesh::AddFace(const std::vector<VertexHandle>& vertices) {
    if (vertices.size() < 3) {
        return FaceHandle::Invalid;
    }
    for (const VertexHandle vertex : vertices) {
        if (!IsValid(vertex) || Index(vertex) >= vertices_.size()) {
            return FaceHandle::Invalid;
        }
    }
    // Simpul berulang di dalam satu face menghasilkan rusuk berpanjang nol dan
    // lingkaran yang tidak bisa ditelusuri. Ditolak, bukan diperbaiki diam-diam.
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        for (std::size_t j = i + 1; j < vertices.size(); ++j) {
            if (vertices[i] == vertices[j]) {
                return FaceHandle::Invalid;
            }
        }
    }

    // Peta arah → half-edge, dibangun dari yang sudah ada. Disusun ulang tiap
    // kali karena `AddFace` dipanggil beberapa kali saat membangun, bukan di
    // jalur panas.
    std::unordered_map<uint64_t, HalfEdgeHandle> directed;
    directed.reserve(halfEdges_.size());
    for (uint32_t i = 0; i < halfEdges_.size(); ++i) {
        const HalfEdge& halfEdge = halfEdges_[i];
        if (!IsValid(halfEdge.next)) {
            continue;
        }
        const VertexHandle tip = halfEdges_[Index(halfEdge.next)].origin;
        directed.emplace(DirectedKey(halfEdge.origin, tip),
                         static_cast<HalfEdgeHandle>(i));
    }

    // **Diperiksa seluruhnya sebelum apa pun ditulis.** Face yang ditolak di
    // tengah akan meninggalkan half-edge menggantung, dan mesh yang rusak
    // separuh jauh lebih sulit didiagnosis daripada penolakan yang bersih.
    const std::size_t count = vertices.size();
    for (std::size_t i = 0; i < count; ++i) {
        const VertexHandle from = vertices[i];
        const VertexHandle to = vertices[(i + 1) % count];
        if (directed.count(DirectedKey(from, to)) != 0) {
            // Arah itu sudah dipakai face lain: dua face menghadap arah yang
            // sama pada rusuk yang sama, yang berarti geometri non-manifold.
            return FaceHandle::Invalid;
        }
    }

    const FaceHandle face = static_cast<FaceHandle>(static_cast<uint32_t>(faces_.size()));
    const uint32_t first = static_cast<uint32_t>(halfEdges_.size());

    for (std::size_t i = 0; i < count; ++i) {
        HalfEdge halfEdge;
        halfEdge.origin = vertices[i];
        halfEdge.face = face;
        halfEdge.next =
            static_cast<HalfEdgeHandle>(first + static_cast<uint32_t>((i + 1) % count));
        halfEdges_.push_back(halfEdge);
        halfEdgeEdge_.push_back(EdgeHandle::Invalid);
    }

    for (std::size_t i = 0; i < count; ++i) {
        const VertexHandle from = vertices[i];
        const VertexHandle to = vertices[(i + 1) % count];
        const HalfEdgeHandle self = static_cast<HalfEdgeHandle>(first + static_cast<uint32_t>(i));

        // Pasangan sudah ada bila face tetangga menelusuri rusuk ini ke arah
        // sebaliknya.
        const auto found = directed.find(DirectedKey(to, from));
        if (found != directed.end()) {
            halfEdges_[Index(self)].twin = found->second;
            halfEdges_[Index(found->second)].twin = self;
            // Berbagi rusuk dengan pasangannya: itulah arti berpasangan.
            halfEdgeEdge_[Index(self)] = halfEdgeEdge_[Index(found->second)];
        } else {
            // Rusuk baru. Pasangannya menyusul di `FinalizeBoundaries`.
            Edge edge;
            edge.halfEdge = self;
            edges_.push_back(edge);
            halfEdgeEdge_[Index(self)] =
                static_cast<EdgeHandle>(static_cast<uint32_t>(edges_.size() - 1));
        }

        if (!IsValid(vertices_[Index(from)].outgoing)) {
            vertices_[Index(from)].outgoing = self;
        }
    }

    Face record;
    record.halfEdge = static_cast<HalfEdgeHandle>(first);
    faces_.push_back(record);
    return face;
}

void HalfEdgeMesh::FinalizeBoundaries() {
    // Sebuah half-edge tanpa pasangan berada di tepi mesh. Ia diberi pasangan
    // batas — half-edge tanpa face — sehingga aturan "setiap half-edge punya
    // pasangan" berlaku tanpa kekecualian.
    const uint32_t interior = static_cast<uint32_t>(halfEdges_.size());
    std::vector<HalfEdgeHandle> created;

    for (uint32_t i = 0; i < interior; ++i) {
        if (IsValid(halfEdges_[i].twin)) {
            continue;
        }
        HalfEdge boundary;
        // Pangkalnya adalah ujung half-edge yang dipasanginya.
        boundary.origin = halfEdges_[Index(halfEdges_[i].next)].origin;
        boundary.face = FaceHandle::Invalid;
        boundary.twin = static_cast<HalfEdgeHandle>(i);
        halfEdges_.push_back(boundary);
        halfEdgeEdge_.push_back(halfEdgeEdge_[i]);

        const HalfEdgeHandle handle =
            static_cast<HalfEdgeHandle>(static_cast<uint32_t>(halfEdges_.size() - 1));
        halfEdges_[i].twin = handle;
        created.push_back(handle);
    }

    // Merangkai lingkaran batas. Dari sebuah half-edge batas, yang berikutnya
    // adalah batas yang berpangkal di ujungnya — ditemukan dengan berputar
    // mengelilingi simpul itu sampai bertemu tepi berikutnya.
    for (const HalfEdgeHandle handle : created) {
        const VertexHandle tip = halfEdges_[Index(halfEdges_[Index(handle)].twin)].origin;
        for (const HalfEdgeHandle candidate : created) {
            if (halfEdges_[Index(candidate)].origin == tip) {
                halfEdges_[Index(handle)].next = candidate;
                break;
            }
        }
    }
}

std::vector<HalfEdgeHandle> HalfEdgeMesh::FaceHalfEdges(FaceHandle face) const {
    std::vector<HalfEdgeHandle> result;
    if (!IsValid(face) || Index(face) >= faces_.size()) {
        return result;
    }
    const HalfEdgeHandle start = faces_[Index(face)].halfEdge;
    HalfEdgeHandle current = start;
    // Batas putaran menjaga penelusuran tetap berakhir walau lingkarannya rusak;
    // yang memeriksa kerusakannya adalah `CheckInvariants`, bukan di sini.
    for (std::size_t guard = 0; guard <= halfEdges_.size(); ++guard) {
        result.push_back(current);
        current = halfEdges_[Index(current)].next;
        if (!IsValid(current) || current == start) {
            break;
        }
    }
    return result;
}

std::vector<VertexHandle> HalfEdgeMesh::FaceVertices(FaceHandle face) const {
    std::vector<VertexHandle> result;
    for (const HalfEdgeHandle handle : FaceHalfEdges(face)) {
        result.push_back(halfEdges_[Index(handle)].origin);
    }
    return result;
}

std::vector<HalfEdgeHandle> HalfEdgeMesh::VertexOutgoing(VertexHandle vertex) const {
    std::vector<HalfEdgeHandle> result;
    if (!IsValid(vertex) || Index(vertex) >= vertices_.size()) {
        return result;
    }
    const HalfEdgeHandle start = vertices_[Index(vertex)].outgoing;
    if (!IsValid(start)) {
        return result;
    }
    HalfEdgeHandle current = start;
    for (std::size_t guard = 0; guard <= halfEdges_.size(); ++guard) {
        result.push_back(current);
        // Berputar mengelilingi simpul: pasangan lalu berikutnya membawa ke
        // half-edge keluar berikutnya.
        const HalfEdgeHandle twin = halfEdges_[Index(current)].twin;
        if (!IsValid(twin)) {
            break;
        }
        current = halfEdges_[Index(twin)].next;
        if (!IsValid(current) || current == start) {
            break;
        }
    }
    return result;
}

EdgeHandle HalfEdgeMesh::HalfEdgeEdge(HalfEdgeHandle halfEdge) const {
    const uint32_t index = Index(halfEdge);
    return index < halfEdgeEdge_.size() ? halfEdgeEdge_[index] : EdgeHandle::Invalid;
}

std::pair<HalfEdgeHandle, HalfEdgeHandle> HalfEdgeMesh::EdgeHalfEdges(EdgeHandle edge) const {
    const uint32_t index = static_cast<uint32_t>(edge);
    if (index >= edges_.size()) {
        return {HalfEdgeHandle::Invalid, HalfEdgeHandle::Invalid};
    }
    const HalfEdgeHandle first = edges_[index].halfEdge;
    return {first, halfEdges_[Index(first)].twin};
}

std::pair<FaceHandle, FaceHandle> HalfEdgeMesh::EdgeFaces(EdgeHandle edge) const {
    const auto [a, b] = EdgeHalfEdges(edge);
    if (!IsValid(a) || !IsValid(b)) {
        return {FaceHandle::Invalid, FaceHandle::Invalid};
    }
    return {halfEdges_[Index(a)].face, halfEdges_[Index(b)].face};
}

Vec3 HalfEdgeMesh::FaceNormal(FaceHandle face) const {
    // Newell, bukan hasil kali silang dua rusuk pertama. Sesudah beberapa kali
    // ekstrusi, poligon jarang sebidang sempurna, dan dua rusuk pertama bisa
    // hampir sejajar — keduanya menghasilkan normal yang salah arah atau nol.
    const std::vector<VertexHandle> loop = FaceVertices(face);
    Vec3 normal(0.0f);
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const Vec3& a = vertices_[Index(loop[i])].position;
        const Vec3& b = vertices_[Index(loop[(i + 1) % loop.size()])].position;
        normal.x += (a.y - b.y) * (a.z + b.z);
        normal.y += (a.z - b.z) * (a.x + b.x);
        normal.z += (a.x - b.x) * (a.y + b.y);
    }
    const float length = glm::length(normal);
    return length > 1e-12f ? normal / length : Vec3(0.0f);
}

MeshCheck HalfEdgeMesh::CheckInvariants() const {
    const auto fail = [](std::string message) { return MeshCheck{false, std::move(message)}; };

    // 1. Setiap half-edge punya pasangan, dan pasangan dari pasangannya adalah
    //    dirinya sendiri.
    for (uint32_t i = 0; i < halfEdges_.size(); ++i) {
        const HalfEdge& halfEdge = halfEdges_[i];
        if (!IsValid(halfEdge.twin)) {
            return fail("half-edge " + std::to_string(i) + " tidak punya pasangan");
        }
        if (Index(halfEdge.twin) >= halfEdges_.size()) {
            return fail("half-edge " + std::to_string(i) + " menunjuk pasangan di luar jangkauan");
        }
        if (Index(halfEdges_[Index(halfEdge.twin)].twin) != i) {
            return fail("pasangan half-edge " + std::to_string(i) + " tidak menunjuk kembali");
        }
        if (Index(halfEdge.twin) == i) {
            return fail("half-edge " + std::to_string(i) + " berpasangan dengan dirinya sendiri");
        }
    }

    // 2. Pasangan menghadap arah berlawanan: pangkal yang satu adalah ujung yang
    //    lain. Ini yang membedakan pasangan sejati dari dua half-edge yang
    //    kebetulan saling menunjuk.
    for (uint32_t i = 0; i < halfEdges_.size(); ++i) {
        const HalfEdge& halfEdge = halfEdges_[i];
        if (!IsValid(halfEdge.next)) {
            return fail("half-edge " + std::to_string(i) + " tidak punya penerus");
        }
        const VertexHandle tip = halfEdges_[Index(halfEdge.next)].origin;
        if (halfEdges_[Index(halfEdge.twin)].origin != tip) {
            return fail("pasangan half-edge " + std::to_string(i) +
                        " tidak berpangkal di ujungnya");
        }
    }

    // 3. Setiap face tertutup melingkar, dan seluruh half-edge di dalamnya
    //    menunjuk kembali ke face itu.
    for (uint32_t f = 0; f < faces_.size(); ++f) {
        const FaceHandle face = static_cast<FaceHandle>(f);
        const HalfEdgeHandle start = faces_[f].halfEdge;
        if (!IsValid(start) || Index(start) >= halfEdges_.size()) {
            return fail("face " + std::to_string(f) + " tidak menunjuk half-edge yang sah");
        }
        HalfEdgeHandle current = start;
        std::size_t steps = 0;
        do {
            if (halfEdges_[Index(current)].face != face) {
                return fail("half-edge " + std::to_string(Index(current)) +
                            " berada di lingkaran face " + std::to_string(f) +
                            " tetapi mengaku milik face lain");
            }
            current = halfEdges_[Index(current)].next;
            if (++steps > halfEdges_.size()) {
                return fail("lingkaran face " + std::to_string(f) + " tidak tertutup");
            }
        } while (current != start);
        if (steps < 3) {
            return fail("face " + std::to_string(f) + " hanya punya " + std::to_string(steps) +
                        " sisi");
        }
    }

    // 4. Setiap simpul mencapai seluruh half-edge yang berpangkal padanya
    //    dengan berputar mengelilinginya. Ini yang menangkap simpul yang
    //    "terbelah" — dua kipas terpisah yang kebetulan berbagi satu simpul,
    //    dan yang membuat penelusuran tetangga melewatkan separuh mesh.
    std::vector<std::size_t> reachable(vertices_.size(), 0);
    for (uint32_t v = 0; v < vertices_.size(); ++v) {
        reachable[v] = VertexOutgoing(static_cast<VertexHandle>(v)).size();
    }
    std::vector<std::size_t> actual(vertices_.size(), 0);
    for (const HalfEdge& halfEdge : halfEdges_) {
        if (IsValid(halfEdge.origin) && Index(halfEdge.origin) < vertices_.size()) {
            ++actual[Index(halfEdge.origin)];
        }
    }
    for (uint32_t v = 0; v < vertices_.size(); ++v) {
        if (actual[v] == 0) {
            continue;  // simpul terpisah: sah, hanya belum dipakai face mana pun
        }
        if (reachable[v] != actual[v]) {
            return fail("simpul " + std::to_string(v) + " punya " + std::to_string(actual[v]) +
                        " half-edge keluar tetapi hanya " + std::to_string(reachable[v]) +
                        " yang tercapai dengan berputar");
        }
    }

    // 5a. Pasangan berbagi rusuk yang sama. Tanpa ini, "rusuk" tidak punya arti
    //     tunggal dan penyembunyian rusuk di lapisan poligon akan menggabungkan
    //     sisi yang salah.
    if (halfEdgeEdge_.size() != halfEdges_.size()) {
        return fail("peta half-edge ke rusuk tidak sejajar dengan half-edge");
    }
    for (uint32_t i = 0; i < halfEdges_.size(); ++i) {
        const EdgeHandle edge = halfEdgeEdge_[i];
        if (!IsValid(edge) || static_cast<uint32_t>(edge) >= edges_.size()) {
            return fail("half-edge " + std::to_string(i) + " tidak menunjuk rusuk yang sah");
        }
        if (halfEdgeEdge_[Index(halfEdges_[i].twin)] != edge) {
            return fail("half-edge " + std::to_string(i) +
                        " dan pasangannya menunjuk rusuk yang berbeda");
        }
    }

    // 5. Tiap rusuk menunjuk half-edge yang sah, dan tiap pasangan half-edge
    //    dihitung tepat sekali sebagai rusuk.
    if (edges_.size() * 2 != halfEdges_.size()) {
        return fail("jumlah rusuk " + std::to_string(edges_.size()) + " tidak setengah dari " +
                    std::to_string(halfEdges_.size()) + " half-edge");
    }

    return MeshCheck{};
}

HalfEdgeMesh MakeUnitCube() {
    HalfEdgeMesh mesh;

    // Delapan sudut kubus satuan berpusat di titik asal.
    const VertexHandle v[8] = {
        mesh.AddVertex(Vec3(-0.5f, -0.5f, -0.5f)),  // 0
        mesh.AddVertex(Vec3(0.5f, -0.5f, -0.5f)),   // 1
        mesh.AddVertex(Vec3(0.5f, 0.5f, -0.5f)),    // 2
        mesh.AddVertex(Vec3(-0.5f, 0.5f, -0.5f)),   // 3
        mesh.AddVertex(Vec3(-0.5f, -0.5f, 0.5f)),   // 4
        mesh.AddVertex(Vec3(0.5f, -0.5f, 0.5f)),    // 5
        mesh.AddVertex(Vec3(0.5f, 0.5f, 0.5f)),     // 6
        mesh.AddVertex(Vec3(-0.5f, 0.5f, 0.5f)),    // 7
    };

    // Enam quad, seluruhnya berlawanan arah jarum jam **dilihat dari luar**.
    // Urutannya yang menentukan arah normal, dan satu sisi yang terbalik tidak
    // terlihat sebagai galat melainkan sebagai lubang di dinding.
    mesh.AddFace({v[4], v[5], v[6], v[7]});  // +Z depan
    mesh.AddFace({v[1], v[0], v[3], v[2]});  // -Z belakang
    mesh.AddFace({v[5], v[1], v[2], v[6]});  // +X kanan
    mesh.AddFace({v[0], v[4], v[7], v[3]});  // -X kiri
    mesh.AddFace({v[7], v[6], v[2], v[3]});  // +Y atas
    mesh.AddFace({v[0], v[1], v[5], v[4]});  // -Y bawah

    mesh.FinalizeBoundaries();
    return mesh;
}

HalfEdgeMesh MakeUnitRamp() {
    HalfEdgeMesh mesh;
    // Enam simpul: alas persegi, ditambah satu rusuk di atas sisi belakang.
    const VertexHandle v[6] = {
        mesh.AddVertex(Vec3(-0.5f, -0.5f, -0.5f)),  // 0 alas depan kiri
        mesh.AddVertex(Vec3(0.5f, -0.5f, -0.5f)),   // 1 alas depan kanan
        mesh.AddVertex(Vec3(0.5f, -0.5f, 0.5f)),    // 2 alas belakang kanan
        mesh.AddVertex(Vec3(-0.5f, -0.5f, 0.5f)),   // 3 alas belakang kiri
        mesh.AddVertex(Vec3(-0.5f, 0.5f, 0.5f)),    // 4 puncak kiri
        mesh.AddVertex(Vec3(0.5f, 0.5f, 0.5f)),     // 5 puncak kanan
    };

    mesh.AddFace({v[0], v[1], v[2], v[3]});  // -Y alas
    mesh.AddFace({v[3], v[2], v[5], v[4]});  // +Z dinding belakang
    mesh.AddFace({v[0], v[4], v[5], v[1]});  // bidang miring
    mesh.AddFace({v[0], v[3], v[4]});        // -X sisi kiri
    mesh.AddFace({v[1], v[5], v[2]});        // +X sisi kanan

    mesh.FinalizeBoundaries();
    return mesh;
}

HalfEdgeMesh MakeUnitStairs(uint32_t steps) {
    const uint32_t count = std::clamp(steps, 1u, 64u);
    const auto n = static_cast<float>(count);
    HalfEdgeMesh mesh;

    // Bidang z dan tingkat y, keduanya `count + 1` buah dan berjarak sama.
    const auto planeZ = [n](uint32_t j) { return -0.5f + static_cast<float>(j) / n; };
    const auto levelY = [n](uint32_t k) { return -0.5f + static_cast<float>(k) / n; };

    // Simpul dibuat saat diminta, bukan sebagai kisi penuh: kisi penuh
    // meninggalkan simpul yang tidak dipakai sisi mana pun, dan simpul terpencil
    // merusak V - E + F = 2 tanpa mengubah apa pun yang terlihat.
    std::map<std::pair<uint32_t, uint32_t>, VertexHandle> left;
    std::map<std::pair<uint32_t, uint32_t>, VertexHandle> right;
    const auto at = [&](bool onLeft, uint32_t j, uint32_t k) {
        auto& table = onLeft ? left : right;
        const auto key = std::make_pair(j, k);
        const auto found = table.find(key);
        if (found != table.end()) {
            return found->second;
        }
        const VertexHandle handle =
            mesh.AddVertex(Vec3(onLeft ? -0.5f : 0.5f, levelY(k), planeZ(j)));
        table.emplace(key, handle);
        return handle;
    };

    for (uint32_t i = 0; i < count; ++i) {
        // Sisi kiri dan kanan kolom ini, satu quad per tingkat sampai puncaknya.
        for (uint32_t k = 0; k <= i; ++k) {
            mesh.AddFace({at(true, i, k), at(true, i + 1, k), at(true, i + 1, k + 1),
                          at(true, i, k + 1)});
            mesh.AddFace({at(false, i + 1, k), at(false, i, k), at(false, i, k + 1),
                          at(false, i + 1, k + 1)});
        }
        // Alas, tapak, dan tegaknya.
        mesh.AddFace({at(true, i, 0), at(false, i, 0), at(false, i + 1, 0), at(true, i + 1, 0)});
        mesh.AddFace({at(true, i + 1, i + 1), at(false, i + 1, i + 1), at(false, i, i + 1),
                      at(true, i, i + 1)});
        mesh.AddFace({at(false, i, i), at(true, i, i), at(true, i, i + 1), at(false, i, i + 1)});
    }
    // Dinding belakang, ikut dibagi per tingkat supaya rusuknya bertemu penuh
    // dengan quad sisi kolom terakhir.
    for (uint32_t k = 0; k < count; ++k) {
        mesh.AddFace({at(true, count, k), at(false, count, k), at(false, count, k + 1),
                      at(true, count, k + 1)});
    }

    mesh.FinalizeBoundaries();
    return mesh;
}

namespace {

/// Cincin `sides` simpul berjari-jari 0,5 pada ketinggian `y`, sudut menaik.
std::vector<VertexHandle> AddRing(HalfEdgeMesh& mesh, uint32_t sides, float y) {
    std::vector<VertexHandle> ring;
    ring.reserve(sides);
    for (uint32_t i = 0; i < sides; ++i) {
        const float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(sides);
        ring.push_back(mesh.AddVertex(Vec3(0.5f * std::cos(angle), y, 0.5f * std::sin(angle))));
    }
    return ring;
}

}  // namespace

HalfEdgeMesh MakeUnitCylinder(uint32_t sides) {
    const uint32_t count = std::clamp(sides, 3u, 64u);
    HalfEdgeMesh mesh;
    const std::vector<VertexHandle> bottom = AddRing(mesh, count, -0.5f);
    const std::vector<VertexHandle> top = AddRing(mesh, count, 0.5f);

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t next = (i + 1) % count;
        mesh.AddFace({bottom[i], top[i], top[next], bottom[next]});
    }
    // Alas mengikuti sudut menaik; tutupnya kebalikannya. Segi-n beraturan itu
    // cembung, jadi kipas dari simpul pertamanya sah.
    mesh.AddFace(bottom);
    std::vector<VertexHandle> cap(top.rbegin(), top.rend());
    mesh.AddFace(cap);

    mesh.FinalizeBoundaries();
    return mesh;
}

HalfEdgeMesh MakeUnitCone(uint32_t sides) {
    const uint32_t count = std::clamp(sides, 3u, 64u);
    HalfEdgeMesh mesh;
    const std::vector<VertexHandle> base = AddRing(mesh, count, -0.5f);
    const VertexHandle apex = mesh.AddVertex(Vec3(0.0f, 0.5f, 0.0f));

    for (uint32_t i = 0; i < count; ++i) {
        mesh.AddFace({base[i], apex, base[(i + 1) % count]});
    }
    mesh.AddFace(base);

    mesh.FinalizeBoundaries();
    return mesh;
}

HalfEdgeMesh MakeUnitCubeTriangulated() {
    HalfEdgeMesh mesh;
    const VertexHandle v[8] = {
        mesh.AddVertex(Vec3(-0.5f, -0.5f, -0.5f)), mesh.AddVertex(Vec3(0.5f, -0.5f, -0.5f)),
        mesh.AddVertex(Vec3(0.5f, 0.5f, -0.5f)),   mesh.AddVertex(Vec3(-0.5f, 0.5f, -0.5f)),
        mesh.AddVertex(Vec3(-0.5f, -0.5f, 0.5f)),  mesh.AddVertex(Vec3(0.5f, -0.5f, 0.5f)),
        mesh.AddVertex(Vec3(0.5f, 0.5f, 0.5f)),    mesh.AddVertex(Vec3(-0.5f, 0.5f, 0.5f)),
    };

    // Tiap quad dibelah pada diagonalnya. Urutannya dipertahankan sama dengan
    // `MakeUnitCube` supaya kedua kubus menghadap arah yang sama.
    const VertexHandle quads[6][4] = {
        {v[4], v[5], v[6], v[7]}, {v[1], v[0], v[3], v[2]}, {v[5], v[1], v[2], v[6]},
        {v[0], v[4], v[7], v[3]}, {v[7], v[6], v[2], v[3]}, {v[0], v[1], v[5], v[4]},
    };
    for (const auto& quad : quads) {
        mesh.AddFace({quad[0], quad[1], quad[2]});
        mesh.AddFace({quad[0], quad[2], quad[3]});
    }

    mesh.FinalizeBoundaries();
    return mesh;
}

}  // namespace sim::whitebox
