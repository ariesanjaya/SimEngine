#include "Sim/Whitebox/PolygonOutline.h"

#include <glm/geometric.hpp>

namespace sim::whitebox {

PolygonOutline BuildPolygonOutline(const WhiteboxMesh& box, PolygonHandle polygon) {
    PolygonOutline outline;
    if (!IsValid(polygon)) {
        return outline;
    }

    const HalfEdgeMesh& mesh = box.Mesh();
    const PolygonSet& polygons = box.Polygons();
    const std::vector<FaceHandle> faces = polygons.PolygonFaces(polygon);
    if (faces.empty()) {
        return outline;
    }

    Vec3 weighted{0.0f};
    for (const FaceHandle face : faces) {
        const std::vector<VertexHandle> corners = mesh.FaceVertices(face);
        if (corners.size() < 3) {
            continue;
        }

        // Kipas dari simpul pertama. Face di sebuah whitebox selalu cembung —
        // yang bisa cekung adalah kelompoknya, dan kelompok tidak disegitigakan
        // di sini melainkan dibiarkan sebagai kumpulan face.
        const Vec3 origin = mesh.GetVertex(corners[0]).position;
        for (std::size_t i = 1; i + 1 < corners.size(); ++i) {
            const Vec3 a = mesh.GetVertex(corners[i]).position;
            const Vec3 b = mesh.GetVertex(corners[i + 1]).position;
            outline.triangles.push_back({origin, a, b});

            const float area = 0.5f * glm::length(glm::cross(a - origin, b - origin));
            weighted += (origin + a + b) / 3.0f * area;
            outline.area += area;
        }

        // Rusuk batas: yang seberangnya bukan poligon yang sama. Rusuk di batas
        // mesh ikut terhitung batas — seberangnya bukan poligon mana pun.
        for (const HalfEdgeHandle halfEdge : mesh.FaceHalfEdges(face)) {
            const EdgeHandle edge = mesh.HalfEdgeEdge(halfEdge);
            const auto [left, right] = mesh.EdgeFaces(edge);
            const FaceHandle other = left == face ? right : left;
            if (IsValid(other) && polygons.FacePolygon(other) == polygon) {
                continue;
            }
            const HalfEdge& he = mesh.GetHalfEdge(halfEdge);
            const VertexHandle from = he.origin;
            const VertexHandle to = mesh.GetHalfEdge(he.next).origin;
            outline.edges.emplace_back(mesh.GetVertex(from).position,
                                       mesh.GetVertex(to).position);
        }
    }

    if (outline.area > 0.0f) {
        outline.centroid = weighted / outline.area;
    } else if (!outline.triangles.empty()) {
        // Sisi berluas nol tetap perlu titik: gizmo yang berdiri di titik asal
        // dunia jauh lebih membingungkan daripada gizmo yang berdiri di sisi
        // yang rusak.
        outline.centroid = outline.triangles.front()[0];
    }
    outline.normal = polygons.PolygonNormal(mesh, polygon);
    return outline;
}

}  // namespace sim::whitebox
