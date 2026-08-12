// Impor mesh glTF / GLB.
//
// **Jauh lebih sedikit jebakan daripada FBX**, dan itu sifat formatnya: glTF
// menetapkan tangan-kanan, Y ke atas, dan satuan meter di dalam spesifikasinya.
// Tidak ada konvensi yang harus ditebak, jadi seluruh kelas kesalahan yang dua
// kali menjatuhkan importir FBX — sumbu miring dan satuan seratus kali — tidak
// punya tempat untuk muncul di sini.
//
// Materialnya juga per primitive, bukan per muka, jadi ia memetakan satu-satu ke
// `SubMesh` tanpa perlu menyusun ulang indeks seperti yang dituntut FBX.
//
// **Tidak menulis apa pun ke disk.** Tekstur GLB tertanam di dalam berkasnya,
// dan yang tercatat di sini hanyalah nama logisnya; yang mengeluarkannya menjadi
// berkas adalah langkah tersendiri yang dipanggil dengan sengaja. `LoadMesh`
// dipanggil perender di jalur daftar gambar, dan fungsi yang menulis berkas
// sebagai efek samping menggambar adalah fungsi yang menulis berkas pada waktu
// yang tidak bisa ditebak siapa pun.

#include "Sim/Assets/MeshData.h"

#include "Sim/Core/Log.h"

#include <cgltf.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace sim::assets {
namespace {

Mat4 ToMat4(const cgltf_float m[16]) {
    Mat4 out(1.0f);
    std::memcpy(&out[0][0], m, sizeof(float) * 16);
    return out;
}

/// Nama logis sebuah tekstur, untuk dicatat di `MeshMaterial`.
///
/// Berkas `.gltf` menunjuk berkas gambar lewat URI, dan itu yang dipakai apa
/// adanya. GLB menanam gambarnya di dalam berkas sehingga tidak punya URI sama
/// sekali — di sana namanya diturunkan dari nama gambar di dalam berkasnya, atau
/// dari nomor urutnya kalau ia pun tidak bernama. Yang penting nama itu **tetap
/// sama setiap kali berkasnya dibaca**, karena ia yang akan menjadi nama berkas
/// saat gambarnya dikeluarkan.
std::string TextureName(const cgltf_texture_view& view, std::size_t slot) {
    if (view.texture == nullptr || view.texture->image == nullptr) {
        return {};
    }
    const cgltf_image& image = *view.texture->image;
    if (image.uri != nullptr && image.uri[0] != '\0') {
        // URI data: bukan jalur berkas melainkan isi gambarnya sendiri.
        if (std::strncmp(image.uri, "data:", 5) != 0) {
            std::string uri = image.uri;
            std::replace(uri.begin(), uri.end(), '\\', '/');
            return uri;
        }
    }
    if (image.name != nullptr && image.name[0] != '\0') {
        return std::string(image.name) + ".png";
    }
    return "embedded_" + std::to_string(slot) + ".png";
}

MeshMaterial ReadMaterial(const cgltf_material& source) {
    MeshMaterial material;
    material.name = source.name != nullptr ? source.name : "";

    if (source.has_pbr_metallic_roughness) {
        const cgltf_pbr_metallic_roughness& pbr = source.pbr_metallic_roughness;
        material.baseColor = Vec3(pbr.base_color_factor[0], pbr.base_color_factor[1],
                                  pbr.base_color_factor[2]);
        material.opacity = pbr.base_color_factor[3];
        material.metalness = pbr.metallic_factor;
        material.roughness = pbr.roughness_factor;
        material.baseColorTexture = TextureName(pbr.base_color_texture, 0);
        // Satu tekstur membawa metalness dan roughness sekaligus — biru dan
        // hijau. Dicatat di kedua medan supaya pemakainya tidak perlu tahu
        // bahwa keduanya kebetulan berbagi berkas.
        material.roughnessTexture = TextureName(pbr.metallic_roughness_texture, 1);
        material.metalnessTexture = material.roughnessTexture;
    }
    material.normalTexture = TextureName(source.normal_texture, 2);
    material.emissive = Vec3(source.emissive_factor[0], source.emissive_factor[1],
                             source.emissive_factor[2]);
    material.emissiveTexture = TextureName(source.emissive_texture, 3);
    if (material.name.empty()) {
        material.name = "Material";
    }
    return material;
}

/// Membaca sebuah atribut vektor menjadi float, apa pun tipe penyimpanannya.
/// cgltf yang mengurus normalisasi integer dan stride-nya.
bool ReadFloats(const cgltf_accessor* accessor, std::vector<float>& out, cgltf_size components) {
    if (accessor == nullptr) {
        return false;
    }
    out.assign(accessor->count * components, 0.0f);
    return cgltf_accessor_unpack_floats(accessor, out.data(), out.size()) != 0;
}

}  // namespace

MeshData LoadGltfMesh(const std::filesystem::path& path, std::string& error) {
    MeshData mesh;
    error.clear();

    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path.string().c_str(), &data) != cgltf_result_success) {
        error = "not a readable glTF file";
        return mesh;
    }
    // Buffer dimuat terpisah dari penguraian: `.gltf` menunjuk `.bin` di
    // sebelahnya, sedangkan `.glb` membawanya di dalam berkasnya sendiri.
    if (cgltf_load_buffers(&options, data, path.string().c_str()) != cgltf_result_success) {
        cgltf_free(data);
        error = "cannot read the buffers this file points at";
        return mesh;
    }

    std::vector<MeshVertex> soup;
    std::vector<SkinInfluence> influenceSoup;
    std::unordered_map<const cgltf_material*, int> materialIndex;
    std::unordered_map<const cgltf_node*, int> boneIndex;
    bool anySkin = false;

    // --- rangka -------------------------------------------------------------
    //
    // Sendi diambil dari skin pertama yang ditemukan. Urutannya mengikuti daftar
    // `joints` milik glTF, dan itulah urutan yang dirujuk atribut JOINTS_0 —
    // menyusunnya ulang berarti bobot menunjuk tulang yang salah.
    const cgltf_skin* skin = data->skins_count > 0 ? &data->skins[0] : nullptr;
    if (skin != nullptr) {
        std::vector<float> inverseBind;
        const bool hasInverseBind =
            ReadFloats(skin->inverse_bind_matrices, inverseBind, 16) &&
            inverseBind.size() >= skin->joints_count * 16;
        for (cgltf_size i = 0; i < skin->joints_count; ++i) {
            const cgltf_node* joint = skin->joints[i];
            SkeletonBone bone;
            bone.name = joint != nullptr && joint->name != nullptr ? joint->name
                                                                   : "joint" + std::to_string(i);
            bone.parent = -1;
            boneIndex.emplace(joint, static_cast<int>(i));
            mesh.skeleton.bones.push_back(std::move(bone));
            (void)hasInverseBind;
        }
        // Induk diisi di lintasan kedua: daftar `joints` tidak dijamin terurut
        // induk-sebelum-anak, sementara indeks induk baru bisa dicari setelah
        // seluruhnya tercatat.
        for (cgltf_size i = 0; i < skin->joints_count; ++i) {
            const cgltf_node* joint = skin->joints[i];
            if (joint == nullptr || joint->parent == nullptr) {
                continue;
            }
            const auto parent = boneIndex.find(joint->parent);
            if (parent != boneIndex.end()) {
                mesh.skeleton.bones[i].parent = parent->second;
            }
        }
        // Bind pose lokal diturunkan dari transform dunia tiap sendi, sama
        // seperti importir FBX — dan dengan alasan yang sama: transform lokal
        // node glTF belum tentu relatif terhadap sendi induknya, karena rantai
        // node di antara keduanya boleh berisi node yang bukan sendi.
        std::vector<Mat4> world(skin->joints_count, Mat4(1.0f));
        for (cgltf_size i = 0; i < skin->joints_count; ++i) {
            cgltf_float matrix[16];
            cgltf_node_transform_world(skin->joints[i], matrix);
            world[i] = ToMat4(matrix);
        }
        for (cgltf_size i = 0; i < skin->joints_count; ++i) {
            const int parent = mesh.skeleton.bones[i].parent;
            const Mat4 local =
                parent >= 0 ? glm::inverse(world[static_cast<std::size_t>(parent)]) * world[i]
                            : world[i];
            SkeletonBone& bone = mesh.skeleton.bones[i];
            bone.translation = Vec3(local[3]);
            bone.scale = Vec3(glm::length(Vec3(local[0])), glm::length(Vec3(local[1])),
                              glm::length(Vec3(local[2])));
            Mat3 rotation(local);
            for (int axis = 0; axis < 3; ++axis) {
                const float length = bone.scale[axis];
                if (length > 1e-8f) {
                    rotation[axis] /= length;
                }
            }
            bone.rotation = glm::normalize(glm::quat_cast(rotation));
        }
        // Urutan topologis dituntut `animation::Skeleton`. glTF tidak
        // menjaminnya, jadi rangka yang melanggar dibuang seluruhnya alih-alih
        // diserahkan setengah benar — pose yang dihitung satu lintasan maju di
        // atas urutan yang salah menghasilkan anggota badan yang tertinggal satu
        // tingkat, bukan galat.
        if (!mesh.skeleton.IsTopological()) {
            SIM_WARN("Assets", "{}: joint order is not topological; the skeleton is dropped",
                     path.string());
            mesh.skeleton.bones.clear();
            boneIndex.clear();
        }
    }

    // --- geometri -----------------------------------------------------------
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<float> joints;
    std::vector<float> weights;
    std::vector<int> triangleMaterial;

    for (cgltf_size nodeIndex = 0; nodeIndex < data->nodes_count; ++nodeIndex) {
        const cgltf_node& node = data->nodes[nodeIndex];
        if (node.mesh == nullptr) {
            continue;
        }
        cgltf_float worldMatrix[16];
        cgltf_node_transform_world(&node, worldMatrix);
        const Mat4 toWorld = ToMat4(worldMatrix);
        const Mat3 normalMatrix(glm::transpose(glm::inverse(Mat3(toWorld))));

        for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) {
            const cgltf_primitive& primitive = node.mesh->primitives[p];
            // Hanya segitiga. Titik dan garis tidak punya arti untuk mesh yang
            // digambar jalur ini, dan mengubahnya menjadi segitiga akan
            // menghasilkan geometri yang tidak ada di berkasnya.
            if (primitive.type != cgltf_primitive_type_triangles) {
                continue;
            }

            positions.clear();
            normals.clear();
            uvs.clear();
            joints.clear();
            weights.clear();
            for (cgltf_size a = 0; a < primitive.attributes_count; ++a) {
                const cgltf_attribute& attribute = primitive.attributes[a];
                switch (attribute.type) {
                    case cgltf_attribute_type_position:
                        ReadFloats(attribute.data, positions, 3);
                        break;
                    case cgltf_attribute_type_normal:
                        ReadFloats(attribute.data, normals, 3);
                        break;
                    case cgltf_attribute_type_texcoord:
                        if (attribute.index == 0) {
                            ReadFloats(attribute.data, uvs, 2);
                        }
                        break;
                    case cgltf_attribute_type_joints:
                        if (attribute.index == 0) {
                            ReadFloats(attribute.data, joints, 4);
                        }
                        break;
                    case cgltf_attribute_type_weights:
                        if (attribute.index == 0) {
                            ReadFloats(attribute.data, weights, 4);
                        }
                        break;
                    default:
                        break;
                }
            }
            if (positions.empty()) {
                continue;
            }
            const std::size_t vertexCount = positions.size() / 3;

            int material = -1;
            if (primitive.material != nullptr) {
                const auto found = materialIndex.find(primitive.material);
                if (found != materialIndex.end()) {
                    material = found->second;
                } else {
                    material = static_cast<int>(mesh.materials.size());
                    mesh.materials.push_back(ReadMaterial(*primitive.material));
                    materialIndex.emplace(primitive.material, material);
                }
            }

            const bool skinned = !mesh.skeleton.bones.empty() && joints.size() == vertexCount * 4 &&
                                 weights.size() == vertexCount * 4;
            anySkin = anySkin || skinned;

            const auto emit = [&](std::size_t index) {
                MeshVertex vertex;
                vertex.position =
                    Vec3(toWorld * Vec4(positions[index * 3], positions[index * 3 + 1],
                                        positions[index * 3 + 2], 1.0f));
                if (normals.size() >= (index + 1) * 3) {
                    const Vec3 transformed =
                        normalMatrix * Vec3(normals[index * 3], normals[index * 3 + 1],
                                            normals[index * 3 + 2]);
                    const float length = glm::length(transformed);
                    vertex.normal = length > 1e-8f ? transformed / length : Vec3(0.0f, 1.0f, 0.0f);
                }
                if (uvs.size() >= (index + 1) * 2) {
                    vertex.uv = Vec2(uvs[index * 2], uvs[index * 2 + 1]);
                }
                soup.push_back(vertex);

                SkinInfluence influence;
                if (skinned) {
                    for (int i = 0; i < kMaxInfluences; ++i) {
                        const auto joint = static_cast<std::size_t>(joints[index * 4 + i]);
                        if (joint < mesh.skeleton.bones.size()) {
                            influence.bones[static_cast<std::size_t>(i)] =
                                static_cast<uint16_t>(joint);
                            influence.weights[static_cast<std::size_t>(i)] =
                                weights[index * 4 + i];
                        }
                    }
                }
                influence.Normalize();
                influenceSoup.push_back(influence);
            };

            const std::size_t before = soup.size();
            if (primitive.indices != nullptr) {
                for (cgltf_size i = 0; i + 2 < primitive.indices->count; i += 3) {
                    emit(cgltf_accessor_read_index(primitive.indices, i));
                    emit(cgltf_accessor_read_index(primitive.indices, i + 1));
                    emit(cgltf_accessor_read_index(primitive.indices, i + 2));
                }
            } else {
                for (std::size_t i = 0; i + 2 < vertexCount; i += 3) {
                    emit(i);
                    emit(i + 1);
                    emit(i + 2);
                }
            }
            triangleMaterial.insert(triangleMaterial.end(), (soup.size() - before) / 3, material);
        }
    }
    cgltf_free(data);

    if (soup.empty()) {
        error = "no triangle geometry in file";
        return mesh;
    }

    SkeletonData skeleton = std::move(mesh.skeleton);
    std::vector<MeshMaterial> materials = std::move(mesh.materials);
    mesh = BuildIndexedMesh(soup, anySkin ? influenceSoup : std::vector<SkinInfluence>{});
    mesh.skeleton = std::move(skeleton);
    mesh.materials = std::move(materials);
    if (!mesh.IsValid()) {
        error = "mesh has no triangles";
        return mesh;
    }
    GroupByMaterial(mesh, triangleMaterial);
    return mesh;
}

}  // namespace sim::assets
