#include "Sim/Whitebox/Picking.h"

#include "Sim/Core/Intersect.h"

namespace sim::whitebox {

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
            // `nearest` diserahkan sebagai batas atas, bukan diperiksa
            // sesudahnya: segitiga yang lebih jauh dari yang sudah ditemukan
            // ditolak sebelum barycentric-nya dihitung.
            const TriangleHit hit = RayTriangle(origin, ray, first, second, third, nearest);
            if (!hit) {
                continue;
            }
            nearest = hit.distance;
            result.hit = true;
            result.face = face;
            result.distance = hit.distance;
            result.position = origin + ray * hit.distance;
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
