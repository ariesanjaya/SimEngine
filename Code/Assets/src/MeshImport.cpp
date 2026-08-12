#include "Sim/Assets/MeshData.h"

#include "Sim/Core/Log.h"

#include <ufbx.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace sim::assets {
namespace {

/// Kunci hash untuk vertex yang dibandingkan bit-per-bit.
///
/// **Pengaruh skin ikut menjadi bagian kuncinya.** Dua titik yang sama persis
/// tapi berbeda bobot skin adalah dua vertex yang berbeda; menyatukannya berarti
/// separuh permukaan mengikuti bone yang salah — yang terlihat sebagai kulit
/// yang tertarik, bukan sebagai galat.
struct VertexKey {
    MeshVertex vertex;
    SkinInfluence influence;

    friend bool operator==(const VertexKey& a, const VertexKey& b) {
        return std::memcmp(&a.vertex, &b.vertex, sizeof(MeshVertex)) == 0 &&
               std::memcmp(&a.influence, &b.influence, sizeof(SkinInfluence)) == 0;
    }
};

struct VertexHash {
    std::size_t operator()(const VertexKey& key) const {
        // FNV-1a atas byte mentahnya. Membandingkan dan meng-hash byte yang sama
        // membuat keduanya tidak mungkin tidak sepakat — hash yang dihitung dari
        // sebagian medan sementara pembandingnya melihat seluruhnya adalah bug
        // yang muncul sebagai vertex kembar yang lolos, bukan sebagai galat.
        std::size_t hash = 1469598103934665603ull;
        const auto mix = [&hash](const void* data, std::size_t bytes) {
            const auto* raw = static_cast<const unsigned char*>(data);
            for (std::size_t i = 0; i < bytes; ++i) {
                hash ^= raw[i];
                hash *= 1099511628211ull;
            }
        };
        mix(&key.vertex, sizeof(MeshVertex));
        mix(&key.influence, sizeof(SkinInfluence));
        return hash;
    }
};

Mat4 ToMat4(const ufbx_matrix& m) {
    // ufbx menyimpan tiga kolom rotasi/skala ditambah satu kolom translasi;
    // glm kolom-mayor, jadi kolomnya dipetakan satu-satu.
    Mat4 out(1.0f);
    for (int column = 0; column < 3; ++column) {
        out[column] = Vec4(static_cast<float>(m.cols[column].x), static_cast<float>(m.cols[column].y),
                           static_cast<float>(m.cols[column].z), 0.0f);
    }
    out[3] = Vec4(static_cast<float>(m.cols[3].x), static_cast<float>(m.cols[3].y),
                  static_cast<float>(m.cols[3].z), 1.0f);
    return out;
}

/// Menguraikan matriks menjadi translasi, rotasi, dan skala.
///
/// **Diuraikan, bukan disimpan sebagai matriks.** Matriks tidak bisa di-blend:
/// menginterpolasi dua matriks rotasi secara komponen menghasilkan matriks yang
/// bukan rotasi lagi — ia menyusut dan menceng di tengah transisi. Seluruh guna
/// sistem animasi ada pada pencampuran.
void Decompose(const Mat4& matrix, Vec3& translation, Quat& rotation, Vec3& scale) {
    translation = Vec3(matrix[3]);
    Vec3 columns[3]{Vec3(matrix[0]), Vec3(matrix[1]), Vec3(matrix[2])};
    scale = Vec3(glm::length(columns[0]), glm::length(columns[1]), glm::length(columns[2]));
    for (int i = 0; i < 3; ++i) {
        if (scale[i] > 1e-8f) {
            columns[i] /= scale[i];
        } else {
            columns[i] = Vec3(i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f);
            scale[i] = 1.0f;
        }
    }
    Mat3 basis(columns[0], columns[1], columns[2]);
    // Basis kidal — mis. dari skala negatif — membuat `quat_cast` menghasilkan
    // kuaternion yang bukan rotasi. Membalik seluruhnya menjaga rotasinya sah
    // dan memindahkan tanda negatifnya ke skala, tempat ia tidak merusak apa pun.
    if (glm::determinant(basis) < 0.0f) {
        basis = -basis;
        scale = -scale;
    }
    rotation = glm::normalize(glm::quat_cast(basis));
}

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

void SkinInfluence::Normalize() {
    // Yang terlemah dibuang lebih dulu bila ada lebih dari empat — pemanggil
    // sudah menyisakan empat teratas — lalu sisanya dinormalkan. **Bobot yang
    // tidak berjumlah satu menyusutkan vertexnya ke arah titik asal**, dan yang
    // terlihat adalah permukaan yang berkerut di tempat-tempat tertentu, bukan
    // galat.
    float sum = WeightSum();
    if (sum <= 1e-6f) {
        // Vertex tanpa pengaruh sama sekali mengikuti bone pertama sepenuhnya.
        // Nol di seluruh bobot berarti vertex itu runtuh ke titik asal.
        bones = {};
        weights = {1.0f, 0.0f, 0.0f, 0.0f};
        return;
    }
    for (float& weight : weights) {
        weight /= sum;
    }
}

int SkeletonData::Find(std::string_view name) const {
    for (std::size_t i = 0; i < bones.size(); ++i) {
        if (bones[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool SkeletonData::IsTopological() const {
    for (std::size_t i = 0; i < bones.size(); ++i) {
        if (bones[i].parent >= static_cast<int>(i)) {
            return false;
        }
    }
    return true;
}

MeshData BuildIndexedMesh(const std::vector<MeshVertex>& triangleSoup,
                          const std::vector<SkinInfluence>& influenceSoup) {
    MeshData mesh;
    if (triangleSoup.empty() || triangleSoup.size() % 3 != 0) {
        return mesh;
    }
    const bool skinned = influenceSoup.size() == triangleSoup.size();
    std::unordered_map<VertexKey, uint32_t, VertexHash> lookup;
    lookup.reserve(triangleSoup.size());
    mesh.indices.reserve(triangleSoup.size());

    for (std::size_t i = 0; i < triangleSoup.size(); ++i) {
        VertexKey key{triangleSoup[i], SkinInfluence{}};
        if (skinned) {
            key.influence = influenceSoup[i];
        }
        const auto found = lookup.find(key);
        if (found != lookup.end()) {
            mesh.indices.push_back(found->second);
            continue;
        }
        const auto index = static_cast<uint32_t>(mesh.vertices.size());
        lookup.emplace(key, index);
        mesh.vertices.push_back(key.vertex);
        if (skinned) {
            mesh.influences.push_back(key.influence);
        }
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

    // --- rangka -----------------------------------------------------------
    //
    // `scene->nodes` terurut depth-first dengan induk selalu mendahului
    // anaknya, jadi satu lintasan sudah menghasilkan urutan topologis yang
    // dituntut `animation::Skeleton` — dan yang membuat transform global bisa
    // dihitung satu lintasan maju tanpa rekursi.
    std::unordered_map<const ufbx_node*, int> boneIndex;
    /// Bind pose dunia tiap bone, sejajar dengan `skeleton.bones`. Dipakai
    /// menurunkan transform lokal, lalu dibuang.
    std::vector<Mat4> worldBind;
    for (std::size_t nodeIndex = 0; nodeIndex < scene->nodes.count; ++nodeIndex) {
        const ufbx_node* node = scene->nodes.data[nodeIndex];
        if (node == nullptr || node->is_root || node->bone == nullptr) {
            continue;
        }
        SkeletonBone bone;
        bone.name = node->name.data != nullptr ? node->name.data : "";
        const auto parent = node->parent != nullptr ? boneIndex.find(node->parent) : boneIndex.end();
        bone.parent = parent != boneIndex.end() ? parent->second : -1;

        // **Diturunkan dari `node_to_world`, bukan dari `local_transform`.**
        // Konversi satuan ufbx dipanggang ke transform node root, jadi transform
        // lokal anak-anaknya tetap dalam satuan asli berkasnya. Terukur pada
        // rig Mixamo: mesh-nya benar setinggi 1,80 m sementara translasi
        // bone-nya masih 99,79 — sentimeter. Kulit yang diulit rangka seratus
        // kali terlalu besar tidak menghasilkan satu pun galat; ia menghasilkan
        // karakter yang lenyap.
        const Mat4 bindWorld = ToMat4(node->node_to_world);
        Mat4 bindLocal = bindWorld;
        if (bone.parent >= 0) {
            bindLocal = glm::inverse(worldBind[static_cast<std::size_t>(bone.parent)]) * bindWorld;
        }
        Decompose(bindLocal, bone.translation, bone.rotation, bone.scale);

        boneIndex.emplace(node, static_cast<int>(mesh.skeleton.bones.size()));
        worldBind.push_back(bindWorld);
        mesh.skeleton.bones.push_back(std::move(bone));
    }

    // --- geometri ---------------------------------------------------------
    std::vector<MeshVertex> soup;
    std::vector<SkinInfluence> influenceSoup;
    std::vector<uint32_t> triangleIndices;
    bool anySkin = false;

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

        // Skin deformer pertama saja. Mesh dengan lebih dari satu adalah mesh
        // yang diulit dua rangka sekaligus, dan tidak ada arti tunggal untuk
        // "bone nomor tiga" di sana.
        const ufbx_skin_deformer* skin =
            source->skin_deformers.count > 0 ? source->skin_deformers.data[0] : nullptr;
        std::vector<int> clusterBone;
        if (skin != nullptr) {
            clusterBone.resize(skin->clusters.count, -1);
            for (std::size_t i = 0; i < skin->clusters.count; ++i) {
                const ufbx_node* boneNode = skin->clusters.data[i]->bone_node;
                const auto found = boneNode != nullptr ? boneIndex.find(boneNode) : boneIndex.end();
                if (found != boneIndex.end()) {
                    clusterBone[i] = found->second;
                }
            }
            anySkin = true;
        }

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

                SkinInfluence influence;
                if (skin != nullptr && index < source->vertex_indices.count) {
                    const uint32_t vertexIndex = source->vertex_indices.data[index];
                    if (vertexIndex < skin->vertices.count) {
                        const ufbx_skin_vertex& entry = skin->vertices.data[vertexIndex];
                        // **Empat teratas, bukan empat pertama.** ufbx sudah
                        // mengurutkan bobotnya menurun, jadi memotong di empat
                        // membuang yang paling lemah — memotong tanpa urutan itu
                        // bisa membuang justru bone yang paling menentukan.
                        const uint32_t take =
                            std::min<uint32_t>(entry.num_weights, kMaxInfluences);
                        int written = 0;
                        for (uint32_t w = 0; w < take; ++w) {
                            const ufbx_skin_weight& weight =
                                skin->weights.data[entry.weight_begin + w];
                            if (weight.cluster_index >= clusterBone.size()) {
                                continue;
                            }
                            const int bone = clusterBone[weight.cluster_index];
                            if (bone < 0) {
                                continue;
                            }
                            influence.bones[written] = static_cast<uint16_t>(bone);
                            influence.weights[written] = static_cast<float>(weight.weight);
                            ++written;
                        }
                    }
                }
                influence.Normalize();
                influenceSoup.push_back(influence);
            }
        }
    }

    ufbx_free_scene(scene);

    if (soup.empty()) {
        error = "no mesh geometry in file";
        return mesh;
    }
    // Rangka disalin lebih dulu: `BuildIndexedMesh` mengembalikan mesh baru.
    SkeletonData skeleton = std::move(mesh.skeleton);
    mesh = BuildIndexedMesh(soup, anySkin ? influenceSoup : std::vector<SkinInfluence>{});
    mesh.skeleton = std::move(skeleton);
    if (!mesh.IsValid()) {
        error = "mesh has no triangles";
    }
    return mesh;
}

}  // namespace sim::assets
