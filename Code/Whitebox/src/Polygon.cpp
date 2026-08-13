#include "Sim/Whitebox/Polygon.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace sim::whitebox {
namespace {

uint32_t Index(FaceHandle h) { return static_cast<uint32_t>(h); }
uint32_t Index(EdgeHandle h) { return static_cast<uint32_t>(h); }
uint32_t Index(PolygonHandle h) { return static_cast<uint32_t>(h); }

/// Luas sebuah face, dipakai membobot normal poligon.
float FaceArea(const HalfEdgeMesh& mesh, FaceHandle face) {
    const std::vector<VertexHandle> loop = mesh.FaceVertices(face);
    if (loop.size() < 3) {
        return 0.0f;
    }
    // Setengah panjang jumlah hasil kali silang — berlaku untuk poligon
    // sebidang berapa pun sisinya, bukan hanya segitiga.
    Vec3 sum(0.0f);
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const Vec3& a = mesh.GetVertex(loop[i]).position;
        const Vec3& b = mesh.GetVertex(loop[(i + 1) % loop.size()]).position;
        sum += glm::cross(a, b);
    }
    return 0.5f * glm::length(sum);
}

}  // namespace

void PolygonSet::Reset(const HalfEdgeMesh& mesh) {
    facePolygon_.assign(mesh.FaceCount(), 0);
    polygonFaces_.assign(mesh.FaceCount(), {});
    for (uint32_t f = 0; f < mesh.FaceCount(); ++f) {
        facePolygon_[f] = f;
        polygonFaces_[f] = {static_cast<FaceHandle>(f)};
    }
    hiddenEdge_.assign(mesh.EdgeCount(), false);
}

std::size_t PolygonSet::PolygonCount() const {
    std::size_t count = 0;
    for (const std::vector<FaceHandle>& faces : polygonFaces_) {
        count += faces.empty() ? 0 : 1;
    }
    return count;
}

PolygonHandle PolygonSet::FacePolygon(FaceHandle face) const {
    const uint32_t index = Index(face);
    return index < facePolygon_.size() ? static_cast<PolygonHandle>(facePolygon_[index])
                                       : PolygonHandle::Invalid;
}

std::vector<FaceHandle> PolygonSet::PolygonFaces(PolygonHandle polygon) const {
    const uint32_t index = Index(polygon);
    return index < polygonFaces_.size() ? polygonFaces_[index] : std::vector<FaceHandle>{};
}

std::vector<PolygonHandle> PolygonSet::Polygons() const {
    std::vector<PolygonHandle> result;
    for (uint32_t i = 0; i < polygonFaces_.size(); ++i) {
        if (!polygonFaces_[i].empty()) {
            result.push_back(static_cast<PolygonHandle>(i));
        }
    }
    return result;
}

bool PolygonSet::IsEdgeHidden(EdgeHandle edge) const {
    const uint32_t index = Index(edge);
    return index < hiddenEdge_.size() && hiddenEdge_[index];
}

std::size_t PolygonSet::HiddenEdgeCount() const {
    return static_cast<std::size_t>(std::count(hiddenEdge_.begin(), hiddenEdge_.end(), true));
}

bool PolygonSet::HideEdge(const HalfEdgeMesh& mesh, EdgeHandle edge, float toleranceDegrees) {
    const uint32_t index = Index(edge);
    if (index >= hiddenEdge_.size() || hiddenEdge_[index]) {
        return false;
    }

    const auto [faceA, faceB] = mesh.EdgeFaces(edge);
    // Rusuk di batas mesh hanya punya satu sisi: tidak ada yang bisa digabung.
    if (!IsValid(faceA) || !IsValid(faceB)) {
        return false;
    }

    const uint32_t polyA = facePolygon_[Index(faceA)];
    const uint32_t polyB = facePolygon_[Index(faceB)];
    if (polyA == polyB) {
        // Sudah satu poligon lewat jalan lain. Menyembunyikan rusuk ini tetap
        // sah secara tampilan, tetapi ia tidak menggabungkan apa pun — dan
        // mengembalikan true untuk itu akan berbohong kepada pemanggil.
        return false;
    }

    // **Sebidang, atau tidak digabung.** Menggabungkan dua bidang yang menyudut
    // menghasilkan "sisi" yang tidak punya satu normal, dan seluruh gunanya
    // poligon adalah bahwa ia punya.
    const Vec3 normalA = mesh.FaceNormal(faceA);
    const Vec3 normalB = mesh.FaceNormal(faceB);
    const float cosine = glm::clamp(glm::dot(normalA, normalB), -1.0f, 1.0f);
    const float degrees = std::acos(cosine) * 180.0f / 3.14159265f;
    if (degrees > toleranceDegrees) {
        return false;
    }

    hiddenEdge_[index] = true;

    // **Yang bernomor kecil menjadi perwakilan**, bukan yang anggotanya lebih
    // banyak. Perwakilan yang dipilih berdasarkan ukuran akan berubah-ubah
    // mengikuti urutan penggabungan; perwakilan terkecil selalu sama berapa pun
    // urutannya, dan itulah yang membuat gabung-lalu-pulihkan mengembalikan
    // nomor yang sama persis.
    const uint32_t keep = std::min(polyA, polyB);
    const uint32_t drop = std::max(polyA, polyB);
    for (const FaceHandle face : polygonFaces_[drop]) {
        facePolygon_[Index(face)] = keep;
        polygonFaces_[keep].push_back(face);
    }
    polygonFaces_[drop].clear();
    std::sort(polygonFaces_[keep].begin(), polygonFaces_[keep].end());
    return true;
}

bool PolygonSet::RestoreEdge(const HalfEdgeMesh& mesh, EdgeHandle edge) {
    const uint32_t index = Index(edge);
    if (index >= hiddenEdge_.size() || !hiddenEdge_[index]) {
        return false;
    }
    hiddenEdge_[index] = false;

    const auto [faceA, faceB] = mesh.EdgeFaces(edge);
    if (!IsValid(faceA)) {
        return true;
    }
    Resplit(mesh, static_cast<PolygonHandle>(facePolygon_[Index(faceA)]));
    return true;
}

void PolygonSet::Resplit(const HalfEdgeMesh& mesh, PolygonHandle polygon) {
    const uint32_t index = Index(polygon);
    if (index >= polygonFaces_.size() || polygonFaces_[index].empty()) {
        return;
    }

    // Komponen terhubung dihitung ulang dari nol, menelusuri hanya rusuk yang
    // masih tersembunyi. **Bukan tambal-sulam**: sebuah poligon bisa terbelah
    // menjadi dua, tetap satu, atau — bila rusuknya bukan satu-satunya
    // penghubung — tidak berubah sama sekali, dan menebak yang mana lebih mahal
    // daripada menghitungnya.
    const std::vector<FaceHandle> members = polygonFaces_[index];
    std::vector<uint8_t> visited(mesh.FaceCount(), 0);
    // Seluruh slot lama dikosongkan lebih dulu: perwakilan yang baru dihitung
    // dari komponennya, dan slot yang tidak terpilih harus tidak meninggalkan
    // daftar basi.
    for (const FaceHandle face : members) {
        polygonFaces_[Index(face)].clear();
    }

    for (const FaceHandle seed : members) {
        if (visited[Index(seed)]) {
            continue;
        }
        std::vector<FaceHandle> component;
        std::vector<FaceHandle> stack{seed};
        visited[Index(seed)] = 1;
        while (!stack.empty()) {
            const FaceHandle face = stack.back();
            stack.pop_back();
            component.push_back(face);
            for (const HalfEdgeHandle halfEdge : mesh.FaceHalfEdges(face)) {
                const EdgeHandle shared = mesh.HalfEdgeEdge(halfEdge);
                if (!IsValid(shared) || !hiddenEdge_[Index(shared)]) {
                    continue;
                }
                const auto [a, b] = mesh.EdgeFaces(shared);
                const FaceHandle other = a == face ? b : a;
                if (IsValid(other) && !visited[Index(other)]) {
                    visited[Index(other)] = 1;
                    stack.push_back(other);
                }
            }
        }

        // Perwakilannya face terkecil di dalam komponen itu — dihitung, bukan
        // dialokasikan. Karena itu memulihkan rusuk mengembalikan nomor poligon
        // yang sama persis dengan sebelum ia disembunyikan, berapa kali pun
        // gabung-pulihkan diulang.
        std::sort(component.begin(), component.end());
        const uint32_t target = Index(component.front());
        polygonFaces_[target] = component;
        for (const FaceHandle face : component) {
            facePolygon_[Index(face)] = target;
        }
    }
}

std::size_t PolygonSet::MergeCoplanar(const HalfEdgeMesh& mesh, float toleranceDegrees) {
    std::size_t merged = 0;
    for (uint32_t e = 0; e < mesh.EdgeCount(); ++e) {
        if (HideEdge(mesh, static_cast<EdgeHandle>(e), toleranceDegrees)) {
            ++merged;
        }
    }
    return merged;
}

Vec3 PolygonSet::PolygonNormal(const HalfEdgeMesh& mesh, PolygonHandle polygon) const {
    Vec3 sum(0.0f);
    for (const FaceHandle face : PolygonFaces(polygon)) {
        sum += mesh.FaceNormal(face) * FaceArea(mesh, face);
    }
    const float length = glm::length(sum);
    return length > 1e-12f ? sum / length : Vec3(0.0f);
}

MeshCheck PolygonSet::CheckInvariants(const HalfEdgeMesh& mesh) const {
    const auto fail = [](std::string message) { return MeshCheck{false, std::move(message)}; };

    if (facePolygon_.size() != mesh.FaceCount()) {
        return fail("peta face ke poligon tidak sejajar dengan mesh");
    }
    if (hiddenEdge_.size() != mesh.EdgeCount()) {
        return fail("daftar rusuk tersembunyi tidak sejajar dengan mesh");
    }

    // 1. Tiap face berada di dalam daftar poligon yang diakuinya.
    for (uint32_t f = 0; f < facePolygon_.size(); ++f) {
        const uint32_t polygon = facePolygon_[f];
        if (polygon >= polygonFaces_.size()) {
            return fail("face " + std::to_string(f) + " menunjuk poligon di luar jangkauan");
        }
        const std::vector<FaceHandle>& members = polygonFaces_[polygon];
        if (std::find(members.begin(), members.end(), static_cast<FaceHandle>(f)) ==
            members.end()) {
            return fail("face " + std::to_string(f) + " mengaku milik poligon " +
                        std::to_string(polygon) + " tetapi tidak terdaftar di dalamnya");
        }
    }

    // 2. Dan sebaliknya: tiap face yang terdaftar mengaku memilikinya.
    for (uint32_t p = 0; p < polygonFaces_.size(); ++p) {
        for (const FaceHandle face : polygonFaces_[p]) {
            if (Index(face) >= facePolygon_.size()) {
                return fail("poligon " + std::to_string(p) + " memuat face di luar jangkauan");
            }
            if (facePolygon_[Index(face)] != p) {
                return fail("poligon " + std::to_string(p) + " memuat face " +
                            std::to_string(Index(face)) + " yang mengaku milik poligon lain");
            }
        }
    }

    // 3. Rusuk tersembunyi selalu memisahkan dua face di poligon yang sama.
    //    Kebalikannya tidak dituntut: dua face sepoligon boleh bertetangga lewat
    //    rusuk yang terlihat, selama keduanya terhubung lewat jalan lain.
    for (uint32_t e = 0; e < hiddenEdge_.size(); ++e) {
        if (!hiddenEdge_[e]) {
            continue;
        }
        const auto [a, b] = mesh.EdgeFaces(static_cast<EdgeHandle>(e));
        if (!IsValid(a) || !IsValid(b)) {
            return fail("rusuk " + std::to_string(e) +
                        " disembunyikan padahal ia di batas mesh");
        }
        if (facePolygon_[Index(a)] != facePolygon_[Index(b)]) {
            return fail("rusuk " + std::to_string(e) +
                        " disembunyikan tetapi kedua sisinya di poligon berbeda");
        }
    }

    // 4. Tiap poligon terhubung lewat rusuk tersembunyi. Poligon yang terbelah
    //    menjadi dua kepingan terpisah adalah "sisi" yang separuhnya tidak ikut
    //    bergerak saat didorong — dan itu terlihat sebagai bug ekstrusi, bukan
    //    sebagai bug pengelompokan.
    std::vector<uint8_t> visited(mesh.FaceCount(), 0);
    for (uint32_t p = 0; p < polygonFaces_.size(); ++p) {
        const std::vector<FaceHandle>& members = polygonFaces_[p];
        if (members.size() < 2) {
            continue;
        }
        std::vector<FaceHandle> stack{members.front()};
        visited[Index(members.front())] = 1;
        std::size_t reached = 0;
        while (!stack.empty()) {
            const FaceHandle face = stack.back();
            stack.pop_back();
            ++reached;
            for (const HalfEdgeHandle halfEdge : mesh.FaceHalfEdges(face)) {
                const EdgeHandle shared = mesh.HalfEdgeEdge(halfEdge);
                if (!IsValid(shared) || !hiddenEdge_[Index(shared)]) {
                    continue;
                }
                const auto [a, b] = mesh.EdgeFaces(shared);
                const FaceHandle other = a == face ? b : a;
                if (IsValid(other) && !visited[Index(other)]) {
                    visited[Index(other)] = 1;
                    stack.push_back(other);
                }
            }
        }
        if (reached != members.size()) {
            return fail("poligon " + std::to_string(p) + " punya " +
                        std::to_string(members.size()) + " face tetapi hanya " +
                        std::to_string(reached) + " yang terhubung lewat rusuk tersembunyi");
        }
    }

    return MeshCheck{};
}

}  // namespace sim::whitebox
