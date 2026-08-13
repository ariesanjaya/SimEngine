#include "Sim/Whitebox/Picking.h"

#include <cmath>

namespace sim::whitebox {
namespace {

/// Möller–Trumbore, sisi depan maupun belakang.
///
/// **Sisi belakang ikut diterima**, dan itu disengaja: perancang kerap bekerja
/// dari dalam ruangan yang baru dibuatnya, dan sisi yang tidak bisa diklik dari
/// dalam berarti dinding yang tidak bisa dipindahkan tanpa memutar kamera
/// keluar.
bool RayTriangle(const Vec3& origin, const Vec3& direction, const Vec3& a, const Vec3& b,
                 const Vec3& c, float& outDistance) {
    constexpr float kEpsilon = 1e-8f;
    const Vec3 edge1 = b - a;
    const Vec3 edge2 = c - a;
    const Vec3 pvec = glm::cross(direction, edge2);
    const float determinant = glm::dot(edge1, pvec);
    if (std::abs(determinant) < kEpsilon) {
        return false;  // sinar sejajar bidang segitiga
    }
    const float inverse = 1.0f / determinant;
    const Vec3 tvec = origin - a;
    const float u = glm::dot(tvec, pvec) * inverse;
    if (u < 0.0f || u > 1.0f) {
        return false;
    }
    const Vec3 qvec = glm::cross(tvec, edge1);
    const float v = glm::dot(direction, qvec) * inverse;
    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }
    const float distance = glm::dot(edge2, qvec) * inverse;
    if (distance < kEpsilon) {
        return false;  // di belakang titik asal sinar
    }
    outDistance = distance;
    return true;
}

}  // namespace

PolygonHit PickPolygon(const WhiteboxMesh& box, const Vec3& origin, const Vec3& direction,
                       float maxDistance) {
    PolygonHit result;

    const float length = glm::length(direction);
    if (!(length > 0.0f) || !(maxDistance > 0.0f)) {
        return result;
    }
    const Vec3 ray = direction / length;

    const HalfEdgeMesh& mesh = box.Mesh();
    float nearest = maxDistance;

    for (uint32_t f = 0; f < mesh.FaceCount(); ++f) {
        const FaceHandle face = static_cast<FaceHandle>(f);
        const std::vector<VertexHandle> loop = mesh.FaceVertices(face);
        if (loop.size() < 3) {
            continue;
        }
        // Kipas segitiga, urutan yang sama dengan yang dipakai `BuildMeshData` —
        // supaya yang ditunjuk kursor adalah persis yang tergambar. Dua
        // penyegitigaan berbeda untuk satu sisi berarti klik yang meleset di
        // dekat diagonalnya.
        const Vec3& first = mesh.GetVertex(loop[0]).position;
        for (std::size_t i = 1; i + 1 < loop.size(); ++i) {
            const Vec3& second = mesh.GetVertex(loop[i]).position;
            const Vec3& third = mesh.GetVertex(loop[i + 1]).position;
            float distance = 0.0f;
            if (!RayTriangle(origin, ray, first, second, third, distance)) {
                continue;
            }
            if (distance >= nearest) {
                continue;
            }
            nearest = distance;
            result.hit = true;
            result.face = face;
            result.distance = distance;
            result.position = origin + ray * distance;
        }
    }

    if (result.hit) {
        // **Poligonnya, bukan face-nya.** Pengguna menunjuk sebuah sisi; bahwa
        // sisi itu tersusun dari dua segitiga adalah urusan mesin.
        result.polygon = box.Polygons().FacePolygon(result.face);
    }
    return result;
}

}  // namespace sim::whitebox
