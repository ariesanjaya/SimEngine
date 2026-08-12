#include "Sim/Assets/MeshData.h"

#include "Sim/Core/Log.h"

#include <ufbx.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace sim::assets {
namespace {

/// Kunci hash untuk vertex yang dibandingkan bit-per-bit.
struct VertexKey {
    MeshVertex vertex;

    friend bool operator==(const VertexKey& a, const VertexKey& b) {
        return std::memcmp(&a.vertex, &b.vertex, sizeof(MeshVertex)) == 0;
    }
};

struct VertexHash {
    std::size_t operator()(const VertexKey& key) const {
        // FNV-1a atas byte mentahnya. Membandingkan dan meng-hash byte yang sama
        // membuat keduanya tidak mungkin tidak sepakat — hash yang dihitung dari
        // sebagian medan sementara pembandingnya melihat seluruhnya adalah bug
        // yang muncul sebagai vertex kembar yang lolos, bukan sebagai galat.
        const auto* bytes = reinterpret_cast<const unsigned char*>(&key.vertex);
        std::size_t hash = 1469598103934665603ull;
        for (std::size_t i = 0; i < sizeof(MeshVertex); ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
        return hash;
    }
};

Vec3 ToVec3(const ufbx_vec3& value) {
    return Vec3(static_cast<float>(value.x), static_cast<float>(value.y),
                static_cast<float>(value.z));
}

}  // namespace

void MeshData::ComputeBounds() {
    if (vertices.empty()) {
        boundsMin = Vec3(0.0f);
        boundsMax = Vec3(0.0f);
        return;
    }
    boundsMin = vertices.front().position;
    boundsMax = vertices.front().position;
    for (const MeshVertex& vertex : vertices) {
        boundsMin = glm::min(boundsMin, vertex.position);
        boundsMax = glm::max(boundsMax, vertex.position);
    }
}

MeshData BuildIndexedMesh(const std::vector<MeshVertex>& triangleSoup) {
    MeshData mesh;
    if (triangleSoup.empty() || triangleSoup.size() % 3 != 0) {
        return mesh;
    }
    std::unordered_map<VertexKey, uint32_t, VertexHash> lookup;
    lookup.reserve(triangleSoup.size());
    mesh.indices.reserve(triangleSoup.size());

    for (const MeshVertex& vertex : triangleSoup) {
        const VertexKey key{vertex};
        const auto found = lookup.find(key);
        if (found != lookup.end()) {
            mesh.indices.push_back(found->second);
            continue;
        }
        const auto index = static_cast<uint32_t>(mesh.vertices.size());
        lookup.emplace(key, index);
        mesh.vertices.push_back(vertex);
        mesh.indices.push_back(index);
    }
    mesh.ComputeBounds();
    return mesh;
}

MeshData LoadMesh(const std::filesystem::path& path, std::string& error) {
    MeshData mesh;
    error.clear();
    std::error_code exists;
    if (path.empty() || !std::filesystem::exists(path, exists)) {
        error = "file not found";
        return mesh;
    }

    ufbx_load_opts options{};
    // **Sumbu dan satuan dikonversi oleh ufbx, bukan oleh kode di bawahnya.**
    // FBX menyimpan konvensinya sendiri di dalam berkas, dan berkas dari DCC
    // yang berbeda memakai konvensi yang berbeda. Mengoreksinya tangan berarti
    // menebak konvensi sumbernya — dan tebakan yang salah menghasilkan mesh yang
    // terbaring miring atau seribu kali terlalu besar, bukan galat.
    options.target_axes = ufbx_axes_right_handed_y_up;
    options.target_unit_meters = 1.0f;
    options.generate_missing_normals = true;

    ufbx_error loadError;
    ufbx_scene* scene = ufbx_load_file(path.string().c_str(), &options, &loadError);
    if (scene == nullptr) {
        error = loadError.description.data != nullptr ? loadError.description.data
                                                      : "cannot read file";
        return mesh;
    }

    std::vector<MeshVertex> soup;
    std::vector<uint32_t> triangleIndices;

    for (std::size_t nodeIndex = 0; nodeIndex < scene->nodes.count; ++nodeIndex) {
        const ufbx_node* node = scene->nodes.data[nodeIndex];
        if (node == nullptr || node->is_root || node->mesh == nullptr) {
            continue;
        }
        const ufbx_mesh* source = node->mesh;

        // Transform geometri→dunia sudah memuat seluruh rantai induknya. Memakai
        // transform lokal saja akan menumpuk setiap bagian di titik asal, yang
        // terlihat sebagai mesh yang "hancur" alih-alih sebagai transform yang
        // terlupa.
        const ufbx_matrix& toWorld = node->geometry_to_world;
        const ufbx_matrix normalMatrix = ufbx_matrix_for_normals(&toWorld);

        triangleIndices.resize(std::max<std::size_t>(source->max_face_triangles * 3, 3));

        for (std::size_t faceIndex = 0; faceIndex < source->faces.count; ++faceIndex) {
            const ufbx_face face = source->faces.data[faceIndex];
            // Segi banyak dipecah menjadi segitiga oleh ufbx. Memecahnya sendiri
            // dengan kipas sederhana benar hanya untuk segi banyak cembung, dan
            // muka cekung — yang ada di hampir setiap model sungguhan — menjadi
            // segitiga yang saling menimpa.
            const uint32_t triangles = ufbx_triangulate_face(
                triangleIndices.data(), triangleIndices.size(), source, face);

            for (uint32_t corner = 0; corner < triangles * 3; ++corner) {
                const uint32_t index = triangleIndices[corner];
                MeshVertex vertex;
                const ufbx_vec3 position = ufbx_get_vertex_vec3(&source->vertex_position, index);
                vertex.position = ToVec3(ufbx_transform_position(&toWorld, position));
                if (source->vertex_normal.exists) {
                    const ufbx_vec3 normal = ufbx_get_vertex_vec3(&source->vertex_normal, index);
                    const Vec3 transformed =
                        ToVec3(ufbx_transform_direction(&normalMatrix, normal));
                    const float length = glm::length(transformed);
                    vertex.normal =
                        length > 1e-8f ? transformed / length : Vec3(0.0f, 1.0f, 0.0f);
                }
                if (source->vertex_uv.exists) {
                    const ufbx_vec2 uv = ufbx_get_vertex_vec2(&source->vertex_uv, index);
                    vertex.uv = Vec2(static_cast<float>(uv.x), static_cast<float>(uv.y));
                }
                soup.push_back(vertex);
            }
        }
    }

    ufbx_free_scene(scene);

    if (soup.empty()) {
        error = "no mesh geometry in file";
        return mesh;
    }
    mesh = BuildIndexedMesh(soup);
    if (!mesh.IsValid()) {
        error = "mesh has no triangles";
    }
    return mesh;
}

}  // namespace sim::assets
