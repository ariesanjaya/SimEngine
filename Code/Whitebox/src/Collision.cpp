#include "Sim/Whitebox/Collision.h"

#include <glm/geometric.hpp>

namespace sim::whitebox {

CollisionShape BuildCollisionShape(const WhiteboxMesh& box) {
    const HalfEdgeMesh& mesh = box.Mesh();

    CollisionShape shape;
    shape.points.reserve(mesh.VertexCount());
    for (std::size_t i = 0; i < mesh.VertexCount(); ++i) {
        shape.points.push_back(mesh.GetVertex(static_cast<VertexHandle>(i)).position);
    }

    // Simpulnya dipakai apa adanya, tanpa dipetakan ulang. Simpul whitebox
    // sudah dipakai bersama seluruh face yang menyentuhnya — itu arti struktur
    // half-edge — jadi tidak ada yang perlu digabung, dan menggabungkan ulang
    // hanya membuka peluang menggabungkan yang seharusnya terpisah.
    for (std::size_t i = 0; i < mesh.FaceCount(); ++i) {
        const std::vector<VertexHandle> corners =
            mesh.FaceVertices(static_cast<FaceHandle>(i));
        if (corners.size() < 3) {
            continue;
        }
        for (std::size_t c = 1; c + 1 < corners.size(); ++c) {
            shape.indices.push_back(static_cast<uint32_t>(corners[0]));
            shape.indices.push_back(static_cast<uint32_t>(corners[c]));
            shape.indices.push_back(static_cast<uint32_t>(corners[c + 1]));
        }
    }

    shape.convex = IsConvex(box);
    return shape;
}

bool IsConvex(const WhiteboxMesh& box, float toleranceMeters) {
    const HalfEdgeMesh& mesh = box.Mesh();
    if (mesh.FaceCount() == 0 || mesh.VertexCount() == 0) {
        return true;
    }

    // Setiap simpul di belakang setiap bidang face. Kuadratik, dan itu bukan
    // kelalaian: blockout berukuran puluhan sampai ratusan sisi, dan pemeriksaan
    // ini berjalan sekali saat Play ditekan — bukan tiap frame. Algoritma yang
    // lebih pintar akan lebih sulit dipercaya untuk keuntungan yang tidak
    // terukur.
    for (std::size_t f = 0; f < mesh.FaceCount(); ++f) {
        const FaceHandle face = static_cast<FaceHandle>(f);
        const std::vector<VertexHandle> corners = mesh.FaceVertices(face);
        if (corners.size() < 3) {
            continue;
        }
        const Vec3 normal = mesh.FaceNormal(face);
        // Face berluas nol tidak punya bidang untuk dijadikan acuan. Melewatinya
        // lebih benar daripada memakai normal yang arahnya hasil pembagian
        // dengan nol.
        if (glm::length(normal) < 1e-6f) {
            continue;
        }
        const Vec3 origin = mesh.GetVertex(corners[0]).position;

        for (std::size_t v = 0; v < mesh.VertexCount(); ++v) {
            const Vec3 point = mesh.GetVertex(static_cast<VertexHandle>(v)).position;
            if (glm::dot(point - origin, normal) > toleranceMeters) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace sim::whitebox
