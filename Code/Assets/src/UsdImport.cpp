#include "Sim/Assets/MeshData.h"

#include "Sim/Core/Log.h"

#if SIM_WITH_USD

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdSkel/bindingAPI.h>
#include <pxr/usd/usdSkel/cache.h>
#include <pxr/usd/usdSkel/utils.h>
#include <pxr/usd/usdSkel/root.h>
#include <pxr/usd/usdSkel/skeleton.h>
#include <pxr/usd/usdSkel/topology.h>

#include <algorithm>
#include <unordered_map>

namespace sim::assets {
namespace {

namespace usd = pxr;

/// Ubah satu titik dari ruang panggung USD ke ruang mesin.
///
/// **Dua hal sekaligus, dan urutannya penting.** Panggung USD menyebut satuannya
/// sendiri (`metersPerUnit`) dan sumbu atasnya sendiri (`upAxis`); mesin ini
/// selalu meter dan selalu Y-atas. Menskalakan setelah memutar menghasilkan
/// hasil yang sama, tapi memutar setelah menskalakan membuat sumbu yang salah
/// yang ikut terskala pada panggung non-seragam — dan itu kesalahan yang tidak
/// terlihat sampai ada yang mengimpor aset dari DCC yang lain.
struct StageSpace {
    double metersPerUnit = 1.0;
    bool zUp = false;

    Vec3 Point(const usd::GfVec3d& value) const {
        const auto x = static_cast<float>(value[0] * metersPerUnit);
        const auto y = static_cast<float>(value[1] * metersPerUnit);
        const auto z = static_cast<float>(value[2] * metersPerUnit);
        return zUp ? Vec3(x, z, -y) : Vec3(x, y, z);
    }

    /// Arah tidak ikut diskalakan: normal yang diperpendek oleh satuan panggung
    /// tetap menunjuk ke arah yang sama, tapi tidak lagi sepanjang satu.
    Vec3 Direction(const usd::GfVec3d& value) const {
        const auto x = static_cast<float>(value[0]);
        const auto y = static_cast<float>(value[1]);
        const auto z = static_cast<float>(value[2]);
        return zUp ? Vec3(x, z, -y) : Vec3(x, y, z);
    }
};

Vec3 ToVec3(const usd::GfVec3f& value) {
    return Vec3(value[0], value[1], value[2]);
}

/// Menelusuri satu masukan `UsdPreviewSurface` sampai ke nilainya.
///
/// Masukan bisa berupa nilai tetap atau tersambung ke simpul lain. Yang
/// tersambung ke `UsdUVTexture` menghasilkan jalur berkas; selain itu diabaikan.
/// **Simpul di antaranya tidak diikuti** — jaringan shader USD bisa sedalam apa
/// pun, dan mesin ini hanya punya satu slot tekstur per saluran untuk diisi.
bool ReadTexturePath(const usd::UsdShadeInput& input, std::string& out) {
    // Masukan yang tidak ada bukan kesalahan — material yang tidak memakai peta
    // normal memang tidak punya masukan `normal`. Tapi menanyakan sambungannya
    // menyentuh prim kosong, dan USD melemparkan pengecualian untuk itu, bukan
    // mengembalikan false.
    if (!input) {
        return false;
    }
    usd::UsdShadeConnectableAPI source;
    usd::TfToken sourceName;
    usd::UsdShadeAttributeType sourceType;
    if (!input.GetConnectedSource(&source, &sourceName, &sourceType)) {
        return false;
    }
    const usd::UsdShadeShader shader(source.GetPrim());
    if (!shader) {
        return false;
    }
    usd::TfToken id;
    shader.GetShaderId(&id);
    if (id != usd::TfToken("UsdUVTexture")) {
        return false;
    }
    usd::SdfAssetPath file;
    if (!shader.GetInput(usd::TfToken("file")).Get(&file)) {
        return false;
    }
    // Jalur yang sudah diselesaikan lebih dulu: berkas USD boleh menyebut aset
    // lewat pencarian (`search path`), dan yang tertulis mentah di sana belum
    // tentu ada relatif terhadap berkasnya.
    out = file.GetResolvedPath().empty() ? file.GetAssetPath() : file.GetResolvedPath();
    return !out.empty();
}

template <typename T>
bool ReadInput(const usd::UsdShadeShader& shader, const char* name, T& out) {
    const usd::UsdShadeInput input = shader.GetInput(usd::TfToken(name));
    return input && input.Get(&out);
}

MeshMaterial ReadMaterial(const usd::UsdShadeMaterial& material) {
    MeshMaterial result;
    result.name = material.GetPrim().GetName().GetString();

    usd::UsdShadeShader surface = material.ComputeSurfaceSource();
    if (!surface) {
        return result;
    }

    usd::GfVec3f color;
    if (ReadInput(surface, "diffuseColor", color)) {
        result.baseColor = ToVec3(color);
    }
    usd::GfVec3f emissive;
    if (ReadInput(surface, "emissiveColor", emissive)) {
        result.emissive = ToVec3(emissive);
    }
    float scalar = 0.0f;
    if (ReadInput(surface, "roughness", scalar)) {
        result.roughness = scalar;
    }
    if (ReadInput(surface, "metallic", scalar)) {
        result.metalness = scalar;
    }
    if (ReadInput(surface, "opacity", scalar)) {
        result.opacity = scalar;
    }

    ReadTexturePath(surface.GetInput(usd::TfToken("diffuseColor")), result.baseColorTexture);
    ReadTexturePath(surface.GetInput(usd::TfToken("normal")), result.normalTexture);
    ReadTexturePath(surface.GetInput(usd::TfToken("roughness")), result.roughnessTexture);
    ReadTexturePath(surface.GetInput(usd::TfToken("metallic")), result.metalnessTexture);
    ReadTexturePath(surface.GetInput(usd::TfToken("emissiveColor")), result.emissiveTexture);
    return result;
}

/// Nomor material sebuah material terikat, ditambahkan bila belum ada.
int MaterialIndex(MeshData& mesh, std::unordered_map<std::string, int>& seen,
                  const usd::UsdShadeMaterial& material) {
    if (!material) {
        return -1;
    }
    const std::string path = material.GetPath().GetString();
    const auto found = seen.find(path);
    if (found != seen.end()) {
        return found->second;
    }
    const auto index = static_cast<int>(mesh.materials.size());
    mesh.materials.push_back(ReadMaterial(material));
    seen.emplace(path, index);
    return index;
}

/// Material per muka, dari ikatan mesh-nya dan setiap `GeomSubset` di atasnya.
///
/// **Subset menimpa ikatan mesh, bukan menambahinya.** Muka yang disebut sebuah
/// subset memakai material subset itu; sisanya memakai material mesh. Urutan
/// subset dalam berkas tidak dijamin, jadi muka yang disebut dua subset
/// menghasilkan yang terakhir menang — sama seperti yang dilakukan Hydra.
std::vector<int> FaceMaterials(MeshData& mesh, std::unordered_map<std::string, int>& seen,
                               const usd::UsdGeomMesh& geom, std::size_t faceCount) {
    const usd::UsdShadeMaterial bound =
        usd::UsdShadeMaterialBindingAPI(geom.GetPrim()).ComputeBoundMaterial();
    std::vector<int> faceMaterial(faceCount, MaterialIndex(mesh, seen, bound));

    for (const usd::UsdGeomSubset& subset :
         usd::UsdGeomSubset::GetAllGeomSubsets(usd::UsdGeomImageable(geom.GetPrim()))) {
        usd::TfToken elementType;
        subset.GetElementTypeAttr().Get(&elementType);
        if (elementType != usd::UsdGeomTokens->face) {
            continue;
        }
        const usd::UsdShadeMaterial subsetMaterial =
            usd::UsdShadeMaterialBindingAPI(subset.GetPrim()).ComputeBoundMaterial();
        if (!subsetMaterial) {
            continue;
        }
        const int index = MaterialIndex(mesh, seen, subsetMaterial);
        usd::VtIntArray indices;
        subset.GetIndicesAttr().Get(&indices);
        for (const int face : indices) {
            if (face >= 0 && static_cast<std::size_t>(face) < faceCount) {
                faceMaterial[static_cast<std::size_t>(face)] = index;
            }
        }
    }
    return faceMaterial;
}

/// Rangka dari `UsdSkelSkeleton` pertama yang ditemukan.
///
/// **Nama sendi USD adalah jalur, bukan nama.** `Root/Hip/Knee` adalah satu
/// entri, dan induknya ditentukan oleh awalan jalur itu — bukan oleh urutan di
/// dalam berkas. `UsdSkelTopology` yang menghitungnya, dan yang disimpan ke
/// `SkeletonBone::name` adalah ruas terakhirnya supaya cocok dengan nama sendi
/// yang sama dari FBX maupun glTF.
void ReadSkeleton(const usd::UsdSkelSkeleton& skeleton, const StageSpace& space,
                  SkeletonData& out) {
    usd::VtTokenArray joints;
    if (!skeleton.GetJointsAttr().Get(&joints) || joints.empty()) {
        return;
    }
    const usd::UsdSkelTopology topology(joints);
    std::string reason;
    if (!topology.Validate(&reason)) {
        SIM_WARN("Assets", "USD skeleton topology rejected: {}", reason);
        return;
    }

    usd::VtMatrix4dArray restTransforms;
    skeleton.GetRestTransformsAttr().Get(&restTransforms);

    out.bones.reserve(joints.size());
    for (std::size_t i = 0; i < joints.size(); ++i) {
        SkeletonBone bone;
        const std::string path = joints[i].GetString();
        const std::size_t slash = path.rfind('/');
        bone.name = slash == std::string::npos ? path : path.substr(slash + 1);
        bone.parent = static_cast<int>(topology.GetParent(i));

        if (i < restTransforms.size()) {
            const usd::GfMatrix4d& rest = restTransforms[i];
            bone.translation = space.Point(rest.ExtractTranslation());
            const usd::GfQuatd rotation = rest.ExtractRotationQuat();
            const usd::GfVec3d imaginary = rotation.GetImaginary();
            // Rotasi sendi tidak diubah sumbunya di sini. Yang memutar seluruh
            // panggung sudah dikerjakan pada posisinya; memutar rotasi lokal
            // sekali lagi memutar rangka dua kali.
            bone.rotation = Quat(static_cast<float>(rotation.GetReal()),
                                 static_cast<float>(imaginary[0]),
                                 static_cast<float>(imaginary[1]),
                                 static_cast<float>(imaginary[2]));
        }
        out.bones.push_back(std::move(bone));
    }
}

}  // namespace

MeshData LoadUsdMesh(const std::filesystem::path& path, std::string& error) {
    MeshData mesh;

    const usd::UsdStageRefPtr stage = usd::UsdStage::Open(path.string());
    if (!stage) {
        error = "cannot open the USD stage";
        return mesh;
    }

    StageSpace space;
    space.metersPerUnit = usd::UsdGeomGetStageMetersPerUnit(stage);
    space.zUp = usd::UsdGeomGetStageUpAxis(stage) == usd::UsdGeomTokens->z;

    // Rangka dibaca lebih dulu supaya bobot kulit punya sesuatu untuk ditunjuk.
    for (const usd::UsdPrim& prim : stage->Traverse()) {
        if (prim.IsA<usd::UsdSkelSkeleton>()) {
            ReadSkeleton(usd::UsdSkelSkeleton(prim), space, mesh.skeleton);
            break;
        }
    }

    std::unordered_map<std::string, int> materialIndex;
    std::vector<int> triangleMaterial;
    usd::UsdGeomXformCache xformCache;

    for (const usd::UsdPrim& prim : stage->Traverse()) {
        if (!prim.IsA<usd::UsdGeomMesh>()) {
            continue;
        }
        const usd::UsdGeomMesh geom(prim);

        usd::VtVec3fArray points;
        usd::VtIntArray faceCounts;
        usd::VtIntArray faceIndices;
        geom.GetPointsAttr().Get(&points);
        geom.GetFaceVertexCountsAttr().Get(&faceCounts);
        geom.GetFaceVertexIndicesAttr().Get(&faceIndices);
        if (points.empty() || faceCounts.empty()) {
            continue;
        }

        // Transform prim ikut dipanggang ke titiknya. Mesin ini menggambar satu
        // aset mesh sebagai satu benda; hirarki xform di dalam berkasnya tidak
        // punya tempat untuk hidup setelah diimpor.
        const usd::GfMatrix4d toWorld = xformCache.GetLocalToWorldTransform(prim);

        usd::VtVec3fArray normals;
        geom.GetNormalsAttr().Get(&normals);
        const usd::TfToken normalInterpolation = geom.GetNormalsInterpolation();

        usd::VtVec2fArray uvs;
        const usd::UsdGeomPrimvarsAPI primvars(prim);
        usd::UsdGeomPrimvar st = primvars.GetPrimvar(usd::TfToken("st"));
        if (!st) {
            st = primvars.GetPrimvar(usd::TfToken("UVMap"));
        }
        usd::TfToken uvInterpolation;
        if (st) {
            st.ComputeFlattened(&uvs);
            uvInterpolation = st.GetInterpolation();
        }

        usd::TfToken orientation;
        geom.GetOrientationAttr().Get(&orientation);
        const bool leftHanded = orientation == usd::UsdGeomTokens->leftHanded;

        const std::vector<int> faceMaterial =
            FaceMaterials(mesh, materialIndex, geom, faceCounts.size());

        // --- bobot kulit ---------------------------------------------------
        // Nomor sendi di sini menunjuk ke urutan sendi rangkanya, kecuali kalau
        // mesh-nya menyebut daftar sendinya sendiri — bentuk yang dipakai model
        // yang hanya memakai sebagian rangka. Yang tidak memetakannya ulang
        // memasang bobot pada tulang yang salah, dan hasilnya rig yang tampak
        // benar sampai ada satu tulang yang bergerak.
        const usd::UsdSkelBindingAPI skinBinding(prim);
        usd::VtIntArray skinIndices;
        usd::VtFloatArray skinWeights;
        std::vector<int> jointRemap;
        int perPoint = 0;
        bool constantSkin = false;
        if (!mesh.skeleton.bones.empty()) {
            const usd::UsdGeomPrimvar indicesPrimvar = skinBinding.GetJointIndicesPrimvar();
            const usd::UsdGeomPrimvar weightsPrimvar = skinBinding.GetJointWeightsPrimvar();
            if (indicesPrimvar && weightsPrimvar &&
                indicesPrimvar.ComputeFlattened(&skinIndices) &&
                weightsPrimvar.ComputeFlattened(&skinWeights)) {
                perPoint = std::max(0, indicesPrimvar.GetElementSize());
                constantSkin = indicesPrimvar.GetInterpolation() == usd::UsdGeomTokens->constant;

                usd::VtTokenArray localJoints;
                if (skinBinding.GetJointsAttr().Get(&localJoints)) {
                    jointRemap.reserve(localJoints.size());
                    for (const usd::TfToken& joint : localJoints) {
                        const std::string jointPath = joint.GetString();
                        const std::size_t slash = jointPath.rfind('/');
                        jointRemap.push_back(mesh.skeleton.Find(
                            slash == std::string::npos ? jointPath : jointPath.substr(slash + 1)));
                    }
                }
            }
        }

        // Kipas segitiga dari setiap muka. USD membolehkan muka bersisi berapa
        // pun, dan yang cembung — hampir semua yang keluar dari DCC — terurai
        // benar dengan kipas.
        std::size_t corner = 0;
        for (std::size_t face = 0; face < faceCounts.size(); ++face) {
            const auto sides = static_cast<std::size_t>(faceCounts[face]);
            if (sides < 3 || corner + sides > faceIndices.size()) {
                corner += sides;
                continue;
            }
            for (std::size_t triangle = 0; triangle + 2 < sides; ++triangle) {
                const std::size_t fan[3] = {corner, corner + triangle + 1, corner + triangle + 2};
                for (int step = 0; step < 3; ++step) {
                    // Muka kidal dibalik urutannya, bukan dibalik normalnya:
                    // yang menentukan muka depan saat menggambar adalah urutan
                    // titiknya, dan normal yang tidak sepakat dengannya
                    // menghasilkan pencahayaan yang benar pada sisi yang salah.
                    const std::size_t slot = leftHanded ? fan[2 - step] : fan[step];
                    const auto pointIndex = static_cast<std::size_t>(faceIndices[slot]);
                    if (pointIndex >= points.size()) {
                        continue;
                    }

                    MeshVertex vertex;
                    const usd::GfVec3f& point = points[pointIndex];
                    vertex.position = space.Point(
                        toWorld.Transform(usd::GfVec3d(point[0], point[1], point[2])));

                    if (!normals.empty()) {
                        const std::size_t normalSlot =
                            normalInterpolation == usd::UsdGeomTokens->faceVarying ? slot
                            : normalInterpolation == usd::UsdGeomTokens->uniform   ? face
                                                                                  : pointIndex;
                        if (normalSlot < normals.size()) {
                            const usd::GfVec3f& normal = normals[normalSlot];
                            const usd::GfVec3d world = toWorld.TransformDir(
                                usd::GfVec3d(normal[0], normal[1], normal[2]));
                            vertex.normal = glm::normalize(space.Direction(world));
                        }
                    }

                    if (!uvs.empty()) {
                        const std::size_t uvSlot =
                            uvInterpolation == usd::UsdGeomTokens->faceVarying ? slot : pointIndex;
                        if (uvSlot < uvs.size()) {
                            // V dibalik: USD menaruh asalnya di kiri bawah,
                            // mesin ini di kiri atas.
                            vertex.uv = Vec2(uvs[uvSlot][0], 1.0f - uvs[uvSlot][1]);
                        }
                    }

                    // Setiap titik yang punya rangka mendapat satu entri, juga
                    // yang tidak berbobot: `influences` dibaca sejajar dengan
                    // `vertices`, dan satu mesh tak berkulit di tengah berkas
                    // yang berkulit menggeser seluruh sisanya.
                    if (!mesh.skeleton.bones.empty()) {
                        SkinInfluence influence;
                        if (perPoint > 0) {
                            const std::size_t base =
                                constantSkin ? 0
                                             : pointIndex * static_cast<std::size_t>(perPoint);
                            const int used = std::min<int>(perPoint, kMaxInfluences);
                            for (int slotIndex = 0; slotIndex < used; ++slotIndex) {
                                const std::size_t at = base + static_cast<std::size_t>(slotIndex);
                                if (at >= skinIndices.size() || at >= skinWeights.size()) {
                                    break;
                                }
                                int joint = skinIndices[at];
                                if (!jointRemap.empty()) {
                                    joint = joint >= 0 && static_cast<std::size_t>(joint) <
                                                              jointRemap.size()
                                                ? jointRemap[static_cast<std::size_t>(joint)]
                                                : -1;
                                }
                                if (joint < 0 ||
                                    static_cast<std::size_t>(joint) >= mesh.skeleton.bones.size()) {
                                    continue;
                                }
                                influence.bones[static_cast<std::size_t>(slotIndex)] =
                                    static_cast<uint16_t>(joint);
                                influence.weights[static_cast<std::size_t>(slotIndex)] =
                                    skinWeights[at];
                            }
                        }
                        mesh.influences.push_back(influence);
                    }

                    mesh.indices.push_back(static_cast<uint32_t>(mesh.vertices.size()));
                    mesh.vertices.push_back(vertex);
                }
                triangleMaterial.push_back(faceMaterial[face]);
            }
            corner += sides;
        }
    }

    if (mesh.vertices.empty()) {
        error = "the stage holds no readable mesh";
        return mesh;
    }

    GroupByMaterial(mesh, triangleMaterial);
    mesh.ComputeBounds();
    mesh.ComputeTangents();
    return mesh;
}

}  // namespace sim::assets

#else

namespace sim::assets {

MeshData LoadUsdMesh(const std::filesystem::path& /*path*/, std::string& error) {
    // Disebutkan apa adanya. Berkas USD yang ditolak dengan "format tidak
    // dikenal" mengirim orang memeriksa berkasnya, padahal yang kurang ada di
    // sisi mesin.
    error = "this build of SimEngine has no USD support";
    return MeshData{};
}

}  // namespace sim::assets

#endif
