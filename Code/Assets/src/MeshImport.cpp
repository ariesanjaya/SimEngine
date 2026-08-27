#include "Sim/Assets/MeshData.h"

#include "MaxMaterial.h"
#include "Sim/Assets/MaterialXImport.h"
#include "Sim/Core/Log.h"

#include <fbxsdk.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

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

/// **Ditranspos, bukan disalin kolom demi kolom.** FBX SDK mengalikan vektor
/// baris dari kiri (`v' = v · M`), jadi translasinya ada di baris ke-3; glm
/// mengalikan vektor kolom dari kanan (`v' = M · v`) dan menaruh translasi di
/// kolom ke-3. Menyalinnya apa adanya menghasilkan matriks transpos — rotasi
/// yang terbalik arah dan translasi yang tercecer ke baris skala.
Mat4 ToMat4(const FbxAMatrix& m) {
    Mat4 out(1.0f);
    for (int i = 0; i < 4; ++i) {
        const FbxVector4 row = m.GetRow(i);
        out[i] = Vec4(static_cast<float>(row[0]), static_cast<float>(row[1]),
                      static_cast<float>(row[2]), static_cast<float>(row[3]));
    }
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

Vec3 ToVec3(const FbxVector4& value) {
    return Vec3(static_cast<float>(value[0]), static_cast<float>(value[1]),
                static_cast<float>(value[2]));
}

/// Nilai sebuah elemen layer bervektor pada satu sudut poligon.
///
/// **Mode pemetaan dan mode rujukan dibaca keduanya, dan itu syarat, bukan
/// kelengkapan.** FBX menyimpan atribut per titik kontrol atau per sudut
/// poligon, langsung atau lewat indeks, dan keempat kombinasinya lazim di
/// berkas sungguhan. Yang hanya membaca satu kombinasi mendapat angka milik
/// sudut lain — bingkai tangent yang tertukar, dan bukan satu pun galat.
template <typename Element>
bool ReadLayerVector(const Element* element, int controlPoint, int polygonVertex,
                     FbxVector4& out) {
    if (element == nullptr) {
        return false;
    }
    const FbxLayerElement::EMappingMode mapping = element->GetMappingMode();
    int index = 0;
    if (mapping == FbxGeometryElement::eByControlPoint) {
        index = controlPoint;
    } else if (mapping == FbxGeometryElement::eByPolygonVertex) {
        index = polygonVertex;
    } else {
        return false;
    }
    if (element->GetReferenceMode() != FbxGeometryElement::eDirect) {
        if (index < 0 || index >= element->GetIndexArray().GetCount()) {
            return false;
        }
        index = element->GetIndexArray().GetAt(index);
    }
    if (index < 0 || index >= element->GetDirectArray().GetCount()) {
        return false;
    }
    out = element->GetDirectArray().GetAt(index);
    return true;
}

Vec3 ToVec3(const FbxDouble3& value) {
    return Vec3(static_cast<float>(value[0]), static_cast<float>(value[1]),
                static_cast<float>(value[2]));
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

void MeshData::ComputeTangents() {
    if (vertices.empty()) {
        return;
    }
    // Dua akumulator per vertex: arah U dan arah V. Yang kedua hanya dipakai
    // untuk menentukan tandanya, lalu dibuang.
    std::vector<Vec3> alongU(vertices.size(), Vec3(0.0f));
    std::vector<Vec3> alongV(vertices.size(), Vec3(0.0f));

    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t a = indices[i];
        const uint32_t b = indices[i + 1];
        const uint32_t c = indices[i + 2];
        if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size()) {
            continue;
        }
        const Vec3 edge1 = vertices[b].position - vertices[a].position;
        const Vec3 edge2 = vertices[c].position - vertices[a].position;
        const Vec2 duv1 = vertices[b].uv - vertices[a].uv;
        const Vec2 duv2 = vertices[c].uv - vertices[a].uv;

        // Determinan nol berarti segitiganya tidak punya luas di ruang UV —
        // tiga titik yang dipetakan ke satu garis, atau ke satu titik. Tidak ada
        // arah U yang bisa diturunkan dari sana, dan membaginya menghasilkan
        // inf yang menular ke seluruh vertex yang berbagi sudut itu.
        const float determinant = duv1.x * duv2.y - duv2.x * duv1.y;
        if (std::abs(determinant) < 1e-12f) {
            continue;
        }
        const float inverse = 1.0f / determinant;
        const Vec3 u = (edge1 * duv2.y - edge2 * duv1.y) * inverse;
        const Vec3 v = (edge2 * duv1.x - edge1 * duv2.x) * inverse;
        for (const uint32_t corner : {a, b, c}) {
            alongU[corner] += u;
            alongV[corner] += v;
        }
    }

    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const Vec3 normal = vertices[i].normal;
        Vec3 tangent = alongU[i];
        // **Diortogonalkan terhadap normalnya (Gram-Schmidt).** Rata-rata
        // beberapa segitiga hampir tidak pernah tegak lurus normal yang juga
        // dirata-rata, dan bingkai yang miring memiringkan setiap arah yang
        // dibaca dari peta normal.
        tangent -= normal * glm::dot(normal, tangent);
        if (glm::dot(tangent, tangent) < 1e-16f) {
            // Tidak ada UV, atau seluruh segitiganya merosot. Sumbu mana pun
            // yang tegak lurus normal sama sahnya — yang penting bingkainya
            // tidak merosot, karena bingkai nol menghitamkan seluruh vertex.
            const Vec3 axis = std::abs(normal.x) < 0.9f ? Vec3(1.0f, 0.0f, 0.0f)
                                                        : Vec3(0.0f, 1.0f, 0.0f);
            tangent = glm::cross(axis, normal);
        }
        tangent = glm::normalize(tangent);
        // UV yang bercermin membalik arah tangan. Tanpa tandanya, peta normal di
        // sisi yang dicerminkan membelokkan cahaya ke arah yang berlawanan.
        const float handedness =
            glm::dot(glm::cross(normal, tangent), alongV[i]) < 0.0f ? -1.0f : 1.0f;
        vertices[i].tangent = Vec4(tangent, handedness);
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

std::string SafeAssetFileName(std::string_view name) {
    // Pemisah gaya Windows disamakan lebih dulu: `std::filesystem` di Linux
    // membaca `..\\rahasia` sebagai satu nama berkas utuh, sehingga
    // `filename()` mengembalikannya bulat-bulat — dan penyaring yang hanya
    // memeriksa `/` melewatkannya.
    std::string normalized(name);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');

    std::string leaf = std::filesystem::path(normalized).filename().string();
    if (leaf == "." || leaf == "..") {
        return {};
    }
    // Sesudah `filename()` seharusnya tidak ada pemisah yang tersisa; diperiksa
    // lagi karena yang dijaga di sini adalah penulisan berkas, dan pemeriksaan
    // kedua jauh lebih murah daripada kesalahan yang dijaganya.
    if (leaf.find('/') != std::string::npos || leaf.find('\\') != std::string::npos) {
        return {};
    }
    return leaf;
}

Mat4 SkinMatrix(const SkinInfluence& influence, std::span<const Mat4> palette) {
    // **Matriksnya yang dijumlahkan, bukan titik hasilnya.** Keduanya sama
    // secara matematis — transform linear atas jumlah berbobot — tapi yang ini
    // satu perkalian matriks-vektor untuk posisi dan satu lagi untuk normal,
    // bukan empat masing-masing. Shader melakukannya persis begini.
    Mat4 skin(0.0f);
    for (int i = 0; i < kMaxInfluences; ++i) {
        const std::size_t bone = influence.bones[static_cast<std::size_t>(i)];
        if (bone >= palette.size()) {
            skin += Mat4(1.0f) * influence.weights[static_cast<std::size_t>(i)];
            continue;
        }
        skin += palette[bone] * influence.weights[static_cast<std::size_t>(i)];
    }
    return skin;
}

Vec3 SkinPoint(const SkinInfluence& influence, std::span<const Mat4> palette, const Vec3& point) {
    return Vec3(SkinMatrix(influence, palette) * Vec4(point, 1.0f));
}

Vec3 SkinDirection(const SkinInfluence& influence, std::span<const Mat4> palette,
                   const Vec3& direction) {
    return Mat3(SkinMatrix(influence, palette)) * direction;
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

void ProjectOpenPbrToFlat(const OpenPbrMaterial& source, MeshMaterial& target) {
    target.baseColor = source.baseColor * source.baseWeight;
    target.metalness = source.baseMetalness;
    target.roughness = source.specularRoughness;
    target.emissive = source.emissive;
    target.opacity = source.opacity;

    target.baseColorTexture = source.baseColorTexture.path;
    target.roughnessTexture = source.specularRoughnessTexture.path;
    target.metalnessTexture = source.baseMetalnessTexture.path;
    target.emissiveTexture = source.emissiveTexture.path;
    target.normalTexture = source.normalTexture.path;
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

/// Panggung FBX beserta manajer yang memilikinya.
///
/// **Satu destruktor untuk semuanya.** `FbxManager::Destroy` membongkar setiap
/// objek yang dibuat di bawahnya — importer, scene, dan seluruh node di
/// dalamnya. Membebaskannya satu per satu berarti ada `Destroy` yang bisa
/// terlewat pada jalur galat, dan FBX SDK tidak mengeluh; ia hanya menahan
/// memorinya sampai proses berakhir.
class FbxSceneHandle {
public:
    FbxSceneHandle() = default;
    ~FbxSceneHandle() {
        if (manager_ != nullptr) {
            manager_->Destroy();
        }
    }
    FbxSceneHandle(const FbxSceneHandle&) = delete;
    FbxSceneHandle& operator=(const FbxSceneHandle&) = delete;

    bool Open(const std::filesystem::path& path, bool readAnimation, std::string& error);

    FbxManager* Manager() const { return manager_; }
    FbxScene* Scene() const { return scene_; }
    /// Pengali dari satuan panggung ke meter.
    double UnitScale() const { return unitScale_; }

private:
    FbxManager* manager_ = nullptr;
    FbxScene* scene_ = nullptr;
    double unitScale_ = 1.0;
};

bool FbxSceneHandle::Open(const std::filesystem::path& path, bool readAnimation,
                          std::string& error) {
    manager_ = FbxManager::Create();
    if (manager_ == nullptr) {
        error = "cannot create the FBX SDK manager";
        return false;
    }
    FbxIOSettings* io = FbxIOSettings::Create(manager_, IOSROOT);
    // Kurva animasi hanya dibaca importir klip. Rig Mixamo membawa dua take
    // sepanjang ratusan frame, dan menguraikannya untuk mesh yang tidak memakai
    // satu pun kurvanya adalah pekerjaan yang seluruhnya terbuang.
    io->SetBoolProp(IMP_FBX_ANIMATION, readAnimation);
    // **Media tertanam TIDAK dibongkar ke disk.** Bawaannya menyala, dan yang
    // menyala menulis sebuah folder `<nama>.fbm/` di sebelah berkas FBX-nya —
    // di dalam folder aset milik orang lain, sebagai efek samping dari sekadar
    // membaca. Ia juga menulis ulang jalur relatif tiap tekstur supaya menunjuk
    // ke sana, sehingga yang tercatat di aset bukan lagi jalur yang ada di dalam
    // berkasnya. Mesin ini memuat teksturnya sendiri lewat jalur itu.
    io->SetBoolProp(IMP_FBX_EXTRACT_EMBEDDED_DATA, false);
    manager_->SetIOSettings(io);

    FbxImporter* importer = FbxImporter::Create(manager_, "");
    if (importer == nullptr) {
        error = "cannot create the FBX importer";
        return false;
    }
    if (!importer->Initialize(path.string().c_str(), -1, manager_->GetIOSettings())) {
        error = importer->GetStatus().GetErrorString();
        return false;
    }
    scene_ = FbxScene::Create(manager_, "sim");
    if (scene_ == nullptr || !importer->Import(scene_)) {
        error = importer->GetStatus().GetErrorString();
        scene_ = nullptr;
        return false;
    }

    // **Sumbunya dikonversi dalam-dalam; satuannya tidak dikonversi sama
    // sekali.** Keduanya sengaja ditangani berbeda.
    //
    // Sumbu: `DeepConvertScene` menulis ulang transform seluruh hierarkinya,
    // bukan hanya menyisipkan satu rotasi di node root. Yang menyisipkan di root
    // tetap benar secara global, tapi transform LOKAL tiap bone lalu masih dalam
    // konvensi berkasnya — dan transform lokal itulah yang dibaca klip.
    //
    // Satuan: dibiarkan, lalu dikalikan sendiri di `NodeWorld`. `ConvertScene`
    // milik SDK menaruh faktor satuannya sebagai skala pada node root, sehingga
    // bone teratas mewarisi skala 0,01 dan seluruh keturunannya bertranslasi
    // dalam sentimeter. Rangkanya tetap benar **secara global** — jadi uji yang
    // membandingkan posisi bone dengan kotak batas mesh tidak menangkapnya —
    // sementara klip yang mengambil transform lokal membawa translasi meter
    // berskala satu, dan bone yang tidak dianimasikan klip itu tetap memakai
    // bind pose sentimeternya. Yang terlihat: tulang-tulang terlempar seratus
    // kali terlalu jauh, tanpa satu pun galat.
    const FbxAxisSystem target(FbxAxisSystem::OpenGL);
    if (scene_->GetGlobalSettings().GetAxisSystem() != target) {
        target.DeepConvertScene(scene_);
    }

    // **Berkas yang tidak menyebut satuannya dibaca sebagai meter, bukan sebagai
    // sentimeter.** OBJ tidak punya tempat untuk menyimpan satuan sama sekali,
    // dan panggung yang tidak menyebutnya dilaporkan SDK sebagai sentimeter —
    // bawaannya, bukan sesuatu yang dibaca dari berkasnya. Mengalikannya seperti
    // satuan sungguhan mengecilkan setiap OBJ seratus kali; yang terlihat bukan
    // galat melainkan model yang seakan hilang di kejauhan.
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    unitScale_ =
        extension == ".obj"
            ? 1.0
            : scene_->GetGlobalSettings().GetSystemUnit().GetConversionFactorTo(FbxSystemUnit::m);
    return true;
}

/// Transform global sebuah node, dalam meter dan ruang mesin.
///
/// **Satuannya dikonjugasikan, bukan sekadar dikalikan.** Mengubah satuan adalah
/// pergantian basis `C = s·I`, jadi matriks yang benar adalah `C·M·C⁻¹` dan
/// bukan `C·M`. Bedanya justru pada yang penting: konjugasi meninggalkan basis
/// rotasi/skala apa adanya dan hanya menskalakan translasinya, sehingga skala
/// lokal bone tetap 1,0 alih-alih menjadi 0,01 dan menumpuk tiap tingkat sampai
/// rangkanya runtuh. Yang mengalikan dari kiri saja menskalakan keduanya.
Mat4 NodeWorld(FbxNode* node, double unitScale) {
    Mat4 world = ToMat4(node->EvaluateGlobalTransform());
    world[3] = Vec4(Vec3(world[3]) * static_cast<float>(unitScale), 1.0f);
    return world;
}

/// Node scene per tingkat kedalaman, induk selalu mendahului anaknya.
///
/// **Ditelusuri sendiri, bukan lewat `FbxScene::GetNode`.** Yang dikembalikan
/// scene adalah urutan objek di dalam berkasnya, dan tidak ada yang menjamin
/// induk mendahului anaknya di sana. `SkeletonData::IsTopological` menuntut
/// jaminan itu, dan yang membacanya — `Pose::ComputeGlobal` — menghitung
/// transform global satu lintasan maju tanpa rekursi.
///
/// **Melebar, bukan menurun, dan itu bukan pilihan bebas.** Keduanya menaruh
/// induk sebelum anaknya, tapi keduanya menghasilkan NOMOR bone yang berbeda —
/// dan nomor itu yang ditunjuk `SkinInfluence` di dalam buffer skin, yang
/// tersimpan di aset mesh yang sudah diimpor. Urutan melebar adalah urutan yang
/// dipakai pembaca sebelumnya; menggantinya berarti setiap aset yang sudah ada
/// menunjuk tulang yang salah, dan yang terlihat adalah kulit terpelintir tanpa
/// satu pun galat.
std::vector<FbxNode*> SceneNodes(FbxScene& scene) {
    std::vector<FbxNode*> nodes;
    FbxNode* root = scene.GetRootNode();
    if (root == nullptr) {
        return nodes;
    }
    // Root-nya sendiri tidak ikut: ia node sintetis milik SDK, bukan sesuatu
    // yang ada di dalam berkasnya.
    for (int i = 0; i < root->GetChildCount(); ++i) {
        nodes.push_back(root->GetChild(i));
    }
    // Vektornya sekaligus antrean: yang sudah dikunjungi tetap di depan, yang
    // baru ditambahkan di belakang.
    for (std::size_t head = 0; head < nodes.size(); ++head) {
        FbxNode* node = nodes[head];
        for (int i = 0; i < node->GetChildCount(); ++i) {
            nodes.push_back(node->GetChild(i));
        }
    }
    return nodes;
}

/// Membaca bone dari sebuah scene FBX menjadi `SkeletonData`.
void ReadSkeleton(FbxScene& scene, double unitScale, SkeletonData& skeleton,
                  std::unordered_map<const FbxNode*, int>& boneIndex) {
    /// Bind pose dunia tiap bone, sejajar dengan `skeleton.bones`. Dipakai
    /// menurunkan transform lokal, lalu dibuang.
    std::vector<Mat4> worldBind;
    for (FbxNode* node : SceneNodes(scene)) {
        if (node->GetSkeleton() == nullptr) {
            continue;
        }
        SkeletonBone bone;
        bone.name = node->GetName() != nullptr ? node->GetName() : "";
        const auto parent =
            node->GetParent() != nullptr ? boneIndex.find(node->GetParent()) : boneIndex.end();
        bone.parent = parent != boneIndex.end() ? parent->second : -1;

        // **Diturunkan dari transform global, bukan dari transform lokal.**
        // Keduanya sepakat untuk rantai yang seluruhnya bone, tapi yang global
        // juga membuat bone yang induknya BUKAN bone — node bantu rigger di
        // tengah rantai — tetap mendarat di tempat yang benar, karena transform
        // node bantu itu ikut terbawa ke dalamnya.
        const Mat4 bindWorld = NodeWorld(node, unitScale);
        Mat4 bindLocal = bindWorld;
        if (bone.parent >= 0) {
            bindLocal = glm::inverse(worldBind[static_cast<std::size_t>(bone.parent)]) * bindWorld;
        }
        Decompose(bindLocal, bone.translation, bone.rotation, bone.scale);

        boneIndex.emplace(node, static_cast<int>(skeleton.bones.size()));
        worldBind.push_back(bindWorld);
        skeleton.bones.push_back(std::move(bone));
    }
}

SkeletonData LoadSkeleton(const std::filesystem::path& path, std::string& error) {
    SkeletonData skeleton;
    error.clear();
    std::error_code exists;
    if (path.empty() || !std::filesystem::exists(path, exists)) {
        error = "file not found";
        return skeleton;
    }

    // **Dibuka dengan cara yang sama persis dengan `LoadMesh`.** Itu wajib:
    // indeks bone di buffer skin GPU menunjuk ke urutan yang dihasilkan
    // `LoadMesh`, jadi rangka yang dimuat dengan konvensi berbeda menghasilkan
    // pose yang benar untuk rangka yang salah — kulit yang terpelintir, tanpa
    // satu pun galat.
    FbxSceneHandle handle;
    if (!handle.Open(path, /*readAnimation=*/false, error)) {
        return skeleton;
    }
    std::unordered_map<const FbxNode*, int> boneIndex;
    ReadSkeleton(*handle.Scene(), handle.UnitScale(), skeleton, boneIndex);
    if (!skeleton.IsValid()) {
        error = "no bone in this file";
    }
    return skeleton;
}

/// Lihat catatan di `MeshData.h`.
///
/// **Diurutkan, bukan sekadar dicatat batasnya.** Segitiga bermaterial sama
/// tidak dijamin berdampingan di dalam berkasnya — sebuah node bisa
/// menyelang-nyeling dua material muka demi muka — dan ruas yang dicatat apa
/// adanya lalu menjadi ratusan panggilan gambar untuk dua material. Urutan di
/// dalam satu material dipertahankan (`stable`), karena urutan segitiga
/// menentukan hasil pada permukaan tembus pandang.
void GroupByMaterial(MeshData& mesh, const std::vector<int>& triangleMaterial) {
    const std::size_t triangles = mesh.indices.size() / 3;
    mesh.parts.clear();
    if (triangles == 0) {
        return;
    }
    // Berkas tanpa material sama sekali tetap mendapat satu ruas. Jalur gambar
    // karena itu tidak perlu membedakan "punya ruas" dan "tidak".
    if (triangleMaterial.size() != triangles || mesh.materials.empty()) {
        mesh.parts.push_back(SubMesh{0, static_cast<uint32_t>(mesh.indices.size()), -1});
        return;
    }

    std::vector<std::size_t> order(triangles);
    for (std::size_t i = 0; i < triangles; ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return triangleMaterial[a] < triangleMaterial[b];
    });

    std::vector<uint32_t> sorted;
    sorted.reserve(mesh.indices.size());
    for (const std::size_t triangle : order) {
        sorted.push_back(mesh.indices[triangle * 3]);
        sorted.push_back(mesh.indices[triangle * 3 + 1]);
        sorted.push_back(mesh.indices[triangle * 3 + 2]);
    }
    mesh.indices = std::move(sorted);

    for (std::size_t i = 0; i < triangles;) {
        const int material = triangleMaterial[order[i]];
        std::size_t end = i;
        while (end < triangles && triangleMaterial[order[end]] == material) {
            ++end;
        }
        mesh.parts.push_back(SubMesh{static_cast<uint32_t>(i * 3),
                                     static_cast<uint32_t>((end - i) * 3), material});
        i = end;
    }
}

/// Jalur tekstur relatif yang bisa dipakai di mesin lain.
///
/// FBX menyimpan dua jalur: absolut milik mesin yang mengekspornya — yang hampir
/// tidak pernah ada di mesin yang membukanya — dan relatif terhadap berkas FBX
/// itu sendiri. Yang dipakai yang relatif, dengan pemisah dinormalkan ke `/`
/// karena berkas dari Windows menulis `..\\checkerA.tga` dan `std::filesystem`
/// di Linux membaca seluruhnya sebagai satu nama berkas.
std::string TexturePath(const FbxProperty& property) {
    if (!property.IsValid()) {
        return {};
    }
    // Tekstur berlapis — campuran beberapa berkas dalam satu saluran — hanya
    // diambil lapis pertamanya. Mesin ini punya satu slot tekstur per saluran,
    // dan mencampurnya di importir berarti memanggang hasil campuran itu ke
    // sebuah berkas baru yang tidak diminta siapa pun.
    const FbxFileTexture* texture = property.GetSrcObject<FbxFileTexture>(0);
    if (texture == nullptr) {
        return {};
    }
    std::string relative =
        texture->GetRelativeFileName() != nullptr ? texture->GetRelativeFileName() : "";
    if (relative.empty() && texture->GetFileName() != nullptr) {
        // Tidak ada jalur relatif: yang tersisa hanya nama berkasnya, diambil
        // dari ujung jalur absolutnya. Menebak foldernya lebih buruk daripada
        // menyerahkan nama saja — yang mencarinya tahu di mana ia menaruh
        // teksturnya, kode ini tidak.
        relative = std::filesystem::path(texture->GetFileName()).filename().string();
    }
    std::replace(relative.begin(), relative.end(), '\\', '/');
    return relative;
}

/// Nama set UV yang diminta tekstur sebuah saluran material.
///
/// **Sebuah mesh FBX boleh membawa beberapa set UV, dan yang menentukan mana
/// yang dipakai adalah teksturnya, bukan urutannya.** Set kedua lazimnya UV
/// lightmap — pemetaan yang tidak tumpang tindih, dan yang kalau dipakai
/// menggambar tekstur biasa menghasilkan permukaan yang teksturnya teregang ke
/// petak-petak kecil. Sponza membawa dua set di tiap mesh-nya.
std::string TextureUvSet(const FbxProperty& property) {
    if (!property.IsValid()) {
        return {};
    }
    const FbxFileTexture* texture = property.GetSrcObject<FbxFileTexture>(0);
    if (texture == nullptr) {
        return {};
    }
    const FbxString name = texture->UVSet.Get();
    return name.Buffer() != nullptr ? std::string(name.Buffer()) : std::string{};
}

/// Nilai skalar sebuah properti material, atau `fallback` bila tidak ada.
float PropertyScalar(const FbxSurfaceMaterial& source, const char* name, float fallback) {
    const FbxProperty property = source.FindProperty(name);
    return property.IsValid() ? static_cast<float>(property.Get<FbxDouble>()) : fallback;
}

Vec3 PropertyColor(const FbxSurfaceMaterial& source, const char* name, const Vec3& fallback) {
    const FbxProperty property = source.FindProperty(name);
    return property.IsValid() ? ToVec3(property.Get<FbxDouble3>()) : fallback;
}

/// Menerjemahkan sebuah material FBX menjadi bentuk datar kita.
///
/// **Diturunkan dari Lambert/Phong, bukan dibaca dari medan PBR.** FBX SDK
/// menyajikan material apa adanya seperti tersimpan di berkasnya, dan yang
/// tersimpan di berkas Mixamo maupun shaderBall adalah Phong — tidak ada satu
/// pun medan `base_color` atau `roughness` di sana untuk dibaca. Penurunannya
/// karena itu dikerjakan di sini:
///
/// - `roughness` dari `ShininessExponent`, lewat `1 − sqrt(kilap / 100)`.
///   Material tanpa kilap sama sekali — setiap Lambert — jatuh ke 0,5, karena
///   "tidak disebutkan" bukan berarti "cermin" maupun "sepenuhnya kasar".
/// - `metalness` selalu nol. Phong tidak punya konsepnya, dan menurunkannya dari
///   `ReflectionFactor` membuat setiap permukaan yang sekadar memantul menjadi
///   logam — rig Mixamo menyetel 0,5 di sana untuk sendi yang bukan logam.
/// - `opacity` dari properti `Opacity` bila ada. **Bukan dari
///   `TransparencyFactor`**: berkas Maya menulis 1,0 di sana sambil menaruh
///   hitam di `TransparentColor`, yang berarti tembus pandang nol — membacanya
///   sebagai `1 − faktor` menjadikan setiap material sepenuhnya tembus, dan
///   yang terlihat adalah model yang lenyap.
MeshMaterial ReadMaterial(const FbxSurfaceMaterial& source) {
    MeshMaterial material;
    material.name = source.GetName() != nullptr ? source.GetName() : "";
    material.baseColor = PropertyColor(source, FbxSurfaceMaterial::sDiffuse, Vec3(0.8f));
    material.emissive = PropertyColor(source, FbxSurfaceMaterial::sEmissive, Vec3(0.0f));
    material.metalness = 0.0f;

    const FbxProperty shininess = source.FindProperty(FbxSurfaceMaterial::sShininess);
    material.roughness =
        shininess.IsValid()
            ? std::clamp(1.0f - std::sqrt(std::max(static_cast<float>(shininess.Get<FbxDouble>()),
                                                   0.0f) /
                                          100.0f),
                         0.0f, 1.0f)
            : 0.5f;

    const FbxProperty opacity = source.FindProperty("Opacity");
    if (opacity.IsValid()) {
        material.opacity =
            std::clamp(static_cast<float>(opacity.Get<FbxDouble>()), 0.0f, 1.0f);
    } else {
        // Tanpa `Opacity`, yang tersisa adalah warna tembusnya dikali faktornya.
        const Vec3 transparent =
            PropertyColor(source, FbxSurfaceMaterial::sTransparentColor, Vec3(0.0f));
        const float factor =
            PropertyScalar(source, FbxSurfaceMaterial::sTransparencyFactor, 0.0f);
        const float amount = (transparent.x + transparent.y + transparent.z) / 3.0f * factor;
        material.opacity = std::clamp(1.0f - amount, 0.0f, 1.0f);
    }

    material.baseColorTexture = TexturePath(source.FindProperty(FbxSurfaceMaterial::sDiffuse));
    material.normalTexture = TexturePath(source.FindProperty(FbxSurfaceMaterial::sNormalMap));
    if (material.normalTexture.empty()) {
        // Sebagian eksportir menaruh peta normalnya di slot bump. Keduanya
        // bukan hal yang sama, tapi berkas yang mengisi slot bump dengan peta
        // normal jauh lebih banyak daripada yang benar-benar memakai bump.
        material.normalTexture = TexturePath(source.FindProperty(FbxSurfaceMaterial::sBump));
    }
    material.roughnessTexture = TexturePath(source.FindProperty(FbxSurfaceMaterial::sShininess));
    material.metalnessTexture =
        TexturePath(source.FindProperty(FbxSurfaceMaterial::sReflectionFactor));
    material.emissiveTexture = TexturePath(source.FindProperty(FbxSurfaceMaterial::sEmissive));
    return material;
}

std::vector<std::string> DescribeFbxMaterials(const std::filesystem::path& path,
                                              std::string& error) {
    std::vector<std::string> lines;
    error.clear();

    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".fbx") {
        error = "only FBX carries the custom property blocks this dumps";
        return lines;
    }

    FbxSceneHandle handle;
    if (!handle.Open(path, /*readAnimation=*/false, error)) {
        return lines;
    }
    FbxScene& scene = *handle.Scene();

    // Material panggung, bukan material node: satu material yang dipakai
    // beberapa node hanya perlu dicetak sekali, dan yang tidak dipakai node mana
    // pun tetap menarik — ia kerap justru material yang dicari orang.
    const int count = scene.GetSrcObjectCount<FbxSurfaceMaterial>();
    for (int i = 0; i < count; ++i) {
        const FbxSurfaceMaterial* material = scene.GetSrcObject<FbxSurfaceMaterial>(i);
        if (material == nullptr) {
            continue;
        }
        lines.push_back("=== " + std::string(material->GetName() != nullptr ? material->GetName()
                                                                           : "(tanpa nama)") +
                        " [" + std::string(material->ShadingModel.Get().Buffer()) + "] ===");
        for (std::string& line : DescribeMaterialProperties(*material)) {
            lines.push_back("  " + line);
        }
        std::vector<std::string> documents;
        CollectMaterialXPaths(*material, documents);
        for (const std::string& document : documents) {
            lines.push_back("  -> dokumen MaterialX: " + document);
        }
    }
    if (lines.empty()) {
        error = "no materials in this file";
    }
    return lines;
}

MeshData LoadMesh(const std::filesystem::path& path, std::string& error) {
    MeshData mesh;
    error.clear();
    std::error_code exists;
    if (path.empty() || !std::filesystem::exists(path, exists)) {
        error = "file not found";
        return mesh;
    }

    // Dipilih menurut ekstensi, bukan dengan mengendus isinya. FBX SDK dan cgltf
    // sama-sama menolak berkas yang bukan miliknya, jadi mencoba keduanya
    // berurutan akan berhasil — tapi pesan galat yang sampai ke pengguna lalu
    // datang dari pembaca yang salah, dan "not a readable glTF file" untuk
    // sebuah FBX yang rusak menyesatkan sepenuhnya.
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".gltf" || extension == ".glb") {
        return LoadGltfMesh(path, error);
    }
    if (extension == ".usd" || extension == ".usda" || extension == ".usdc" ||
        extension == ".usdz") {
        return LoadUsdMesh(path, error);
    }

    FbxSceneHandle handle;
    if (!handle.Open(path, /*readAnimation=*/false, error)) {
        return mesh;
    }
    FbxScene& scene = *handle.Scene();
    const auto unitScale = static_cast<float>(handle.UnitScale());

    // **Ditriangulasi oleh SDK-nya, bukan dengan kipas sendiri.** Kipas
    // sederhana benar hanya untuk segi banyak cembung; muka cekung — yang ada di
    // hampir setiap model sungguhan — terurai menjadi segitiga yang saling
    // menimpa. Ini mengganti atribut mesh tiap node, jadi ia harus mendahului
    // setiap pointer FbxMesh yang disimpan.
    FbxGeometryConverter converter(handle.Manager());
    converter.Triangulate(&scene, /*pReplace=*/true);

    std::unordered_map<const FbxNode*, int> boneIndex;
    ReadSkeleton(scene, handle.UnitScale(), mesh.skeleton, boneIndex);

    // --- geometri ---------------------------------------------------------
    std::vector<MeshVertex> soup;
    std::vector<SkinInfluence> influenceSoup;
    bool anySkin = false;
    // **Sekali sudut tanpa tangent, seluruh mesh dihitung ulang.** Mencampur
    // bingkai dari berkas dengan bingkai hasil hitungan menghasilkan jahitan di
    // tempat keduanya bertemu — persis cacat yang hendak dihindari dengan
    // memakai tangent berkasnya.
    bool everyCornerHasTangent = true;
    /// Material tiap segitiga di dalam `soup`, sejajar dengan soup/3.
    std::vector<int> triangleMaterial;
    /// Material scene → indeks di `mesh.materials`, supaya material yang dipakai
    /// dua node tidak tercatat dua kali.
    std::unordered_map<const FbxSurfaceMaterial*, int> materialIndex;
    /// Dokumen `.mtlx` yang disebut material di dalam berkas ini, kalau ada.
    /// Dikumpulkan sambil jalan dan dipakai sekali di ujung — mencarinya per
    /// material berarti membuka dan mengurai dokumen yang sama berkali-kali.
    std::vector<std::string> materialXPaths;

    /// Satu pengaruh bone atas satu titik kendali, sebelum dipotong jadi empat.
    struct ControlWeight {
        int bone = -1;
        float weight = 0.0f;
    };
    std::vector<std::vector<ControlWeight>> controlWeights;

    for (FbxNode* node : SceneNodes(scene)) {
        FbxMesh* source = node->GetMesh();
        if (source == nullptr) {
            continue;
        }

        // **Offset geometri ikut, dan ia bukan bagian dari transform node.** FBX
        // menyimpan pivot khusus-atribut yang tidak diwarisi anak-anaknya;
        // `EvaluateGlobalTransform` tidak memuatnya. Node yang memakainya —
        // lazim pada berkas dari 3ds Max — menaruh geometrinya bergeser dari
        // tempat node-nya berada, dan yang melewatkannya menggeser bagian itu
        // saja sementara sisanya benar.
        FbxAMatrix geometry;
        geometry.SetT(node->GetGeometricTranslation(FbxNode::eSourcePivot));
        geometry.SetR(node->GetGeometricRotation(FbxNode::eSourcePivot));
        geometry.SetS(node->GetGeometricScaling(FbxNode::eSourcePivot));
        // Transform geometri→dunia sudah memuat seluruh rantai induknya. Memakai
        // transform lokal saja akan menumpuk setiap bagian di titik asal, yang
        // terlihat sebagai mesh yang "hancur" alih-alih sebagai transform yang
        // terlupa.
        const Mat4 toWorld = ToMat4(node->EvaluateGlobalTransform() * geometry);
        const Mat3 normalMatrix = glm::transpose(glm::inverse(Mat3(toWorld)));
        // Transform yang mencerminkan membalik arah tangan; tanda di berkasnya
        // menyatakan tangan di ruang objek, bukan di ruang dunia.
        const float mirror = glm::determinant(Mat3(toWorld)) < 0.0f ? -1.0f : 1.0f;

        // **Tangent milik berkasnya dipakai kalau ada** — aturan yang sama yang
        // sudah dipatuhi importir glTF. Peta normal dipanggang terhadap bingkai
        // tangent tertentu, dan yang dihitung ulang dari UV belum tentu bingkai
        // yang sama; jahitannya lalu terlihat sebagai garis yang pencahayaannya
        // patah.
        const FbxGeometryElementTangent* tangentElement =
            source->GetElementTangentCount() > 0 ? source->GetElementTangent(0) : nullptr;
        const FbxGeometryElementBinormal* binormalElement =
            source->GetElementBinormalCount() > 0 ? source->GetElementBinormal(0) : nullptr;

        // Normal yang tidak ada dibangkitkan, bukan dibiarkan nol: mesh tanpa
        // normal digambar hitam pekat, dan itu terbaca sebagai kesalahan shader.
        if (source->GetElementNormalCount() == 0) {
            source->GenerateNormals();
        }

        // **`GetStringAt`, bukan `operator[]`.** Yang terakhir mengembalikan
        // `FbxString&`, dan menampungnya sebagai `const char*` lewat konversi
        // implisit menghasilkan pointer ke sementara yang sudah mati di akhir
        // pernyataan itu juga. Namanya lalu tidak cocok dengan set UV mana pun,
        // `GetPolygonVertexUV` menjawab "tidak ada", dan seluruh mesh kehilangan
        // UV-nya tanpa satu pun galat — hanya tekstur yang tidak muncul.
        FbxStringList uvSetNames;
        source->GetUVSetNames(uvSetNames);
        const char* uvSet = uvSetNames.GetCount() > 0 ? uvSetNames.GetStringAt(0) : nullptr;

        // Skin deformer pertama saja. Mesh dengan lebih dari satu adalah mesh
        // yang diulit dua rangka sekaligus, dan tidak ada arti tunggal untuk
        // "bone nomor tiga" di sana.
        FbxSkin* skin =
            source->GetDeformerCount(FbxDeformer::eSkin) > 0
                ? static_cast<FbxSkin*>(source->GetDeformer(0, FbxDeformer::eSkin))
                : nullptr;
        const int controlPointCount = source->GetControlPointsCount();
        const FbxVector4* controlPoints = source->GetControlPoints();
        if (skin != nullptr) {
            controlWeights.assign(static_cast<std::size_t>(std::max(controlPointCount, 0)), {});
            for (int c = 0; c < skin->GetClusterCount(); ++c) {
                const FbxCluster* cluster = skin->GetCluster(c);
                if (cluster == nullptr) {
                    continue;
                }
                const auto found = cluster->GetLink() != nullptr
                                       ? boneIndex.find(cluster->GetLink())
                                       : boneIndex.end();
                if (found == boneIndex.end()) {
                    continue;
                }
                const int* points = cluster->GetControlPointIndices();
                const double* weights = cluster->GetControlPointWeights();
                for (int w = 0; w < cluster->GetControlPointIndicesCount(); ++w) {
                    const int point = points[w];
                    if (point < 0 || static_cast<std::size_t>(point) >= controlWeights.size() ||
                        weights[w] <= 0.0) {
                        continue;
                    }
                    controlWeights[static_cast<std::size_t>(point)].push_back(
                        ControlWeight{found->second, static_cast<float>(weights[w])});
                }
            }
            // **Empat teratas, bukan empat pertama.** FBX menyimpan pengaruhnya
            // per cluster tanpa urutan apa pun, jadi memotong di empat tanpa
            // mengurutkan lebih dulu bisa membuang justru bone yang paling
            // menentukan — dan yang terlihat adalah permukaan yang mengikuti
            // tulang yang salah, bukan galat.
            for (std::vector<ControlWeight>& list : controlWeights) {
                std::sort(list.begin(), list.end(),
                          [](const ControlWeight& a, const ControlWeight& b) {
                              return a.weight > b.weight;
                          });
            }
            anySkin = true;
        }

        // Material node ini, dicatat sekali. Elemen material mengindeks daftar
        // ini, bukan daftar material scene.
        std::vector<int> nodeMaterial(static_cast<std::size_t>(node->GetMaterialCount()), -1);
        /// Set UV yang diminta tiap slot material node ini. Kosong berarti
        /// "tidak menyebut", dan yang tidak menyebut memakai set pertama.
        std::vector<std::string> nodeUvSet(static_cast<std::size_t>(node->GetMaterialCount()));
        for (int m = 0; m < node->GetMaterialCount(); ++m) {
            const FbxSurfaceMaterial* sourceMaterial = node->GetMaterial(m);
            if (sourceMaterial == nullptr) {
                continue;
            }
            nodeUvSet[static_cast<std::size_t>(m)] =
                TextureUvSet(sourceMaterial->FindProperty(FbxSurfaceMaterial::sDiffuse));
            const auto found = materialIndex.find(sourceMaterial);
            if (found != materialIndex.end()) {
                nodeMaterial[static_cast<std::size_t>(m)] = found->second;
                continue;
            }
            const auto index = static_cast<int>(mesh.materials.size());
            MeshMaterial converted = ReadMaterial(*sourceMaterial);
            // **Blok Max dibaca di atas hasil Phong, bukan menggantikannya.**
            // Jalur teksturnya sudah terisi dari slot FBX baku, dan slot itu
            // yang dipakai bila blok Max tidak menyebut petanya sendiri —
            // urutan ini yang membuat cadangan itu ada.
            if (ReadMaxMaterial(*sourceMaterial, converted) && converted.openPbr) {
                ProjectOpenPbrToFlat(*converted.openPbr, converted);
            }
            CollectMaterialXPaths(*sourceMaterial, materialXPaths);
            mesh.materials.push_back(std::move(converted));
            materialIndex.emplace(sourceMaterial, index);
            nodeMaterial[static_cast<std::size_t>(m)] = index;
        }
        const FbxGeometryElementMaterial* materialElement = source->GetElementMaterial();

        for (int polygon = 0; polygon < source->GetPolygonCount(); ++polygon) {
            // Sudah ditriangulasi di atas; apa pun yang bukan segitiga di sini
            // adalah muka rusak, dan memakainya sebagian menghasilkan segitiga
            // yang menghubungkan titik yang tidak bertetangga.
            if (source->GetPolygonSize(polygon) != 3) {
                continue;
            }
            int corners[3] = {source->GetPolygonVertex(polygon, 0),
                              source->GetPolygonVertex(polygon, 1),
                              source->GetPolygonVertex(polygon, 2)};
            if (corners[0] < 0 || corners[1] < 0 || corners[2] < 0 ||
                corners[0] >= controlPointCount || corners[1] >= controlPointCount ||
                corners[2] >= controlPointCount) {
                continue;
            }

            int faceMaterial = -1;
            int faceSlot = -1;
            if (materialElement != nullptr) {
                const FbxLayerElement::EMappingMode mapping = materialElement->GetMappingMode();
                int slot = -1;
                if (mapping == FbxGeometryElement::eByPolygon &&
                    polygon < materialElement->GetIndexArray().GetCount()) {
                    slot = materialElement->GetIndexArray().GetAt(polygon);
                } else if (mapping == FbxGeometryElement::eAllSame &&
                           materialElement->GetIndexArray().GetCount() > 0) {
                    slot = materialElement->GetIndexArray().GetAt(0);
                }
                if (slot >= 0 && static_cast<std::size_t>(slot) < nodeMaterial.size()) {
                    faceMaterial = nodeMaterial[static_cast<std::size_t>(slot)];
                    faceSlot = slot;
                }
            }
            if (faceSlot < 0 && nodeMaterial.size() == 1) {
                // **Node bermaterial tunggal memakainya, dan syaratnya bukan
                // "tidak ada elemen material" melainkan "elemennya tidak
                // menjawab".** Keduanya terjadi di berkas sungguhan: yang tidak
                // menyebut material per muka sama sekali, dan yang membawa
                // elemen bermode `eNone` — elemen yang ada tapi tidak memetakan
                // apa pun, dan yang lazim ditinggalkan pengekspor di sebelah
                // elemen yang benar.
                //
                // Tanpa jalur ini seluruh muka bermaterial −1: bukan galat,
                // hanya permukaan yang digambar jalur mundur dan karena itu
                // tidak bisa dibedakan dari permukaan yang memang tidak
                // bermaterial.
                faceMaterial = nodeMaterial[0];
                faceSlot = 0;
            }
            triangleMaterial.push_back(faceMaterial);

            // **Set UV yang diminta material muka ini, kalau ia menyebut satu
            // yang benar-benar ada di mesh ini.** Nama yang disebut tapi tidak
            // ada bukan alasan untuk kehilangan UV: yang tercatat di sebuah
            // material bisa saja set milik mesh lain, dan mesh tanpa UV
            // digambar tanpa tekstur sama sekali.
            const char* polygonUvSet = uvSet;
            if (faceSlot >= 0 && static_cast<std::size_t>(faceSlot) < nodeUvSet.size() &&
                !nodeUvSet[static_cast<std::size_t>(faceSlot)].empty()) {
                const std::string& wanted = nodeUvSet[static_cast<std::size_t>(faceSlot)];
                for (int i = 0; i < uvSetNames.GetCount(); ++i) {
                    if (wanted == uvSetNames.GetStringAt(i)) {
                        polygonUvSet = uvSetNames.GetStringAt(i);
                        break;
                    }
                }
            }

            for (int corner = 0; corner < 3; ++corner) {
                const int point = corners[corner];
                MeshVertex vertex;
                vertex.position =
                    Vec3(toWorld * Vec4(ToVec3(controlPoints[point]), 1.0f)) * unitScale;

                FbxVector4 normal;
                if (source->GetPolygonVertexNormal(polygon, corner, normal)) {
                    const Vec3 transformed = normalMatrix * ToVec3(normal);
                    const float length = glm::length(transformed);
                    vertex.normal =
                        length > 1e-8f ? transformed / length : Vec3(0.0f, 1.0f, 0.0f);
                }
                if (polygonUvSet != nullptr) {
                    FbxVector2 uv;
                    bool unmapped = false;
                    if (source->GetPolygonVertexUV(polygon, corner, polygonUvSet, uv, unmapped) &&
                        !unmapped) {
                        // **V dibalik: FBX menaruh asalnya di kiri bawah, mesin
                        // ini di kiri atas** — aturan yang sama yang sudah
                        // dipatuhi importir USD, dan yang berlaku juga untuk
                        // OBJ karena ia dibaca SDK yang sama.
                        //
                        // Tanpa ini tidak ada satu pun galat: UV-nya tetap di
                        // dalam jangkauan, teksturnya tetap terpasang, dan yang
                        // berbeda hanya *baris mana* yang terbaca. Terukur atas
                        // Sponza — berkas FBX dan glTF-nya model yang sama —
                        // 99,41% titik hanya cocok setelah `1−v`, dan 0,01%
                        // cocok apa adanya.
                        vertex.uv = Vec2(static_cast<float>(uv[0]),
                                         1.0f - static_cast<float>(uv[1]));
                    }
                }

                bool cornerHasTangent = false;
                FbxVector4 tangent;
                if (ReadLayerVector(tangentElement, point,
                                    source->GetPolygonVertexIndex(polygon) + corner, tangent)) {
                    // Tangent vektor singgung: ia ikut matriks modelnya, bukan
                    // invers-transposnya seperti normal.
                    const Vec3 direction = Mat3(toWorld) * ToVec3(tangent);
                    const float length = glm::length(direction);
                    if (length > 1e-8f) {
                        const Vec3 unit = direction / length;
                        float handedness = tangent[3] < 0.0 ? -1.0f : 1.0f;
                        handedness *= mirror;
                        FbxVector4 binormal;
                        if (ReadLayerVector(binormalElement, point,
                                            source->GetPolygonVertexIndex(polygon) + corner,
                                            binormal)) {
                            // **Bila binormalnya ada, tangannya dihitung dari
                            // ia, di ruang dunia.** Keduanya sudah ditransform,
                            // jadi pencerminan sudah ikut terhitung dan `mirror`
                            // tidak boleh dipakai dua kali. Komponen keempat
                            // tangent hanya jalur mundur: sebagian pengekspor
                            // menaruh tangannya di sana, sebagian meninggalkannya
                            // 1,0 apa pun keadaannya.
                            handedness = glm::dot(glm::cross(vertex.normal, unit),
                                                  Mat3(toWorld) * ToVec3(binormal)) < 0.0f
                                             ? -1.0f
                                             : 1.0f;
                        }
                        // **Dan dibalik, karena V-nya dibalik.** Bitangent
                        // adalah `dP/dv`; mengganti `v` dengan `1 − v` membalik
                        // arahnya, jadi tangan yang tertulis di berkasnya
                        // berlaku untuk UV yang belum dibalik. Tanpa langkah ini
                        // peta normal di seluruh berkas ber-tangent tampak
                        // cekung di tempat yang seharusnya cembung.
                        vertex.tangent = Vec4(unit, -handedness);
                        cornerHasTangent = true;
                    }
                }
                everyCornerHasTangent = everyCornerHasTangent && cornerHasTangent;

                soup.push_back(vertex);

                SkinInfluence influence;
                if (skin != nullptr && static_cast<std::size_t>(point) < controlWeights.size()) {
                    const std::vector<ControlWeight>& list =
                        controlWeights[static_cast<std::size_t>(point)];
                    const std::size_t take = std::min<std::size_t>(list.size(), kMaxInfluences);
                    for (std::size_t w = 0; w < take; ++w) {
                        influence.bones[w] = static_cast<uint16_t>(list[w].bone);
                        influence.weights[w] = list[w].weight;
                    }
                }
                influence.Normalize();
                influenceSoup.push_back(influence);
            }
        }
    }

    if (soup.empty()) {
        error = "no mesh geometry in file";
        return mesh;
    }
    // Rangka dan material disalin lebih dulu: `BuildIndexedMesh` mengembalikan
    // mesh baru.
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
    if (!everyCornerHasTangent) {
        mesh.ComputeTangents();
    }

    // **Dokumen `.mtlx` dibaca terakhir, dan karena itu ia yang menang.**
    // Blok `3dsMax|Parameters` di atas adalah daftar angka; dokumen MaterialX
    // adalah bentuk yang sama yang dipakai Max, Arnold, dan USD, dan ia bisa
    // menyatakan tekstur yang mengemudikan sebuah input. Keduanya boleh ada
    // sekaligus di satu berkas, dan saat itu yang lebih lengkap yang berlaku.
    ApplyMaterialX(mesh, path, materialXPaths);
    return mesh;
}

}  // namespace sim::assets
