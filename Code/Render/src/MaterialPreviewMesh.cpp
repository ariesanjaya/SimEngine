#include "MaterialPreviewMesh.h"

#include <cmath>

namespace sim::render {
namespace {

constexpr int kSphereRings = 48;
constexpr int kSphereSegments = 96;

/// Bola UV. Rings/segments cukup rapat supaya siluetnya tidak bersegi pada
/// ukuran preview yang wajar — bola bersegi membuat orang menyalahkan
/// materialnya untuk cacat yang sebenarnya milik meshnya.
PreviewMeshData BuildSphere() {
    PreviewMeshData mesh;
    mesh.vertices.reserve(static_cast<size_t>(kSphereRings + 1) * (kSphereSegments + 1));

    for (int ring = 0; ring <= kSphereRings; ++ring) {
        const float v = static_cast<float>(ring) / static_cast<float>(kSphereRings);
        const float phi = v * kPi;
        const float sinPhi = std::sin(phi);
        const float cosPhi = std::cos(phi);
        for (int segment = 0; segment <= kSphereSegments; ++segment) {
            const float u = static_cast<float>(segment) / static_cast<float>(kSphereSegments);
            const float theta = u * kTwoPi;
            const float sinTheta = std::sin(theta);
            const float cosTheta = std::cos(theta);

            PreviewVertex vertex;
            vertex.normal = Vec3(sinPhi * cosTheta, cosPhi, sinPhi * sinTheta);
            vertex.position = vertex.normal;
            // Tangent mengikuti arah u, yaitu keliling bola. Itu yang membuat
            // peta normal dan anisotropi berorientasi seperti yang diharapkan
            // penulis material.
            vertex.tangent = Vec4(-sinTheta, 0.0f, cosTheta, 1.0f);
            vertex.uv = Vec2(u, v);
            mesh.vertices.push_back(vertex);
        }
    }

    const int stride = kSphereSegments + 1;
    for (int ring = 0; ring < kSphereRings; ++ring) {
        for (int segment = 0; segment < kSphereSegments; ++segment) {
            const auto a = static_cast<uint32_t>(ring * stride + segment);
            const auto b = static_cast<uint32_t>(a + stride);
            // Urutan lilitan berlawanan jarum jam dilihat dari luar — sama
            // dengan `VK_FRONT_FACE_COUNTER_CLOCKWISE` yang dipakai pipeline.
            mesh.indices.insert(mesh.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    }
    return mesh;
}

PreviewMeshData BuildCube() {
    struct Face {
        Vec3 normal;
        Vec3 tangent;
    };
    // Enam sisi dengan normal dan tangent-nya sendiri: kubus yang vertex-nya
    // dipakai bersama antar-sisi tidak punya normal yang benar di sudut mana pun.
    const Face faces[6]{
        {{0, 0, 1}, {1, 0, 0}},  {{0, 0, -1}, {-1, 0, 0}}, {{1, 0, 0}, {0, 0, -1}},
        {{-1, 0, 0}, {0, 0, 1}}, {{0, 1, 0}, {1, 0, 0}},   {{0, -1, 0}, {1, 0, 0}},
    };

    PreviewMeshData mesh;
    for (const Face& face : faces) {
        const Vec3 bitangent = glm::cross(face.normal, face.tangent);
        const auto base = static_cast<uint32_t>(mesh.vertices.size());
        for (int corner = 0; corner < 4; ++corner) {
            const float x = (corner == 1 || corner == 2) ? 1.0f : -1.0f;
            const float y = (corner >= 2) ? 1.0f : -1.0f;
            PreviewVertex vertex;
            vertex.position = face.normal + face.tangent * x + bitangent * y;
            vertex.normal = face.normal;
            vertex.tangent = Vec4(face.tangent, 1.0f);
            vertex.uv = Vec2(x * 0.5f + 0.5f, y * 0.5f + 0.5f);
            mesh.vertices.push_back(vertex);
        }
        mesh.indices.insert(mesh.indices.end(),
                            {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    return mesh;
}

PreviewMeshData BuildPlane() {
    PreviewMeshData mesh;
    for (int corner = 0; corner < 4; ++corner) {
        const float x = (corner == 1 || corner == 2) ? 1.0f : -1.0f;
        const float z = (corner >= 2) ? 1.0f : -1.0f;
        PreviewVertex vertex;
        vertex.position = Vec3(x, 0.0f, z);
        vertex.normal = Vec3(0.0f, 1.0f, 0.0f);
        vertex.tangent = Vec4(1.0f, 0.0f, 0.0f, 1.0f);
        vertex.uv = Vec2(x * 0.5f + 0.5f, z * 0.5f + 0.5f);
        mesh.vertices.push_back(vertex);
    }
    mesh.indices = {0, 2, 1, 0, 3, 2};
    return mesh;
}

}  // namespace

const char* ToString(PreviewShape shape) {
    switch (shape) {
        case PreviewShape::Sphere:
            return "Sphere";
        case PreviewShape::Cube:
            return "Cube";
        case PreviewShape::Plane:
            return "Plane";
    }
    return "Sphere";
}

PreviewMeshData BuildPreviewMesh(PreviewShape shape) {
    switch (shape) {
        case PreviewShape::Sphere:
            return BuildSphere();
        case PreviewShape::Cube:
            return BuildCube();
        case PreviewShape::Plane:
            return BuildPlane();
    }
    return BuildSphere();
}

}  // namespace sim::render
