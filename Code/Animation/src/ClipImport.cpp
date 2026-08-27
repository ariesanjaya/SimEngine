#include "Sim/Animation/ClipImport.h"

#include "ClipImportBackends.h"

#include "Sim/Core/FbxSdkLock.h"
#include "Sim/Core/Log.h"

#include <fbxsdk.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

namespace sim::animation {
namespace {

/// Selisih di bawah ini dianggap "tidak bergerak".
///
/// **Nol persis tidak dipakai, dan itu perubahan dari pembaca sebelumnya.**
/// Kunci di sini bukan nilai yang tersimpan di berkasnya melainkan hasil
/// pencuplikan: FBX SDK menyusun matriks lokal tiap frame dari pivot,
/// pre-rotation, dan kurvanya, lalu diuraikan lagi menjadi T/R/S. Rangkaian itu
/// tidak dijamin menghasilkan bit yang sama persis pada dua frame yang di
/// berkasnya memang identik, dan satu bit selisih pada satu bone sudah cukup
/// untuk menjadikan seluruh kanalnya "bergerak" — lalu terbawa sebagai proporsi
/// rig sumber (lihat `AddChannel`). Satu mikrometer berada jauh di bawah apa pun
/// yang berarti pada rig sebesar manusia, dan jauh di atas derau itu.
using detail::kConstantEpsilon;
using detail::SampledFrame;

Vec3 ToVec3(const FbxVector4& v) {
    return Vec3(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]));
}

Quat ToQuat(const FbxQuaternion& q) {
    // FBX menyimpan x, y, z, w; konstruktor glm menerima w lebih dulu.
    return Quat(static_cast<float>(q[3]), static_cast<float>(q[0]), static_cast<float>(q[1]),
                static_cast<float>(q[2]));
}

/// Panggung FBX beserta manajer yang memilikinya.
///
/// **Dibuka dengan konvensi yang sama persis dengan `assets::LoadMesh`.** Klip
/// membawa transform LOKAL dan rangkanya membawa bind pose; keduanya dirangkai
/// induk-ke-anak oleh `Pose::ComputeGlobal`, jadi satu saja yang memakai
/// konvensi berbeda membuat bone yang tidak dianimasikan klip itu terlempar dari
/// tempatnya. Alasan tiap langkahnya ditulis di `Code/Assets/src/MeshImport.cpp`;
/// yang penting di sini adalah keduanya tidak boleh berbeda.
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

    bool Open(const std::filesystem::path& path, std::string& error) {
        // **Kunci yang sama dengan `assets::LoadMesh`, dan itu syarat.** FBX SDK
        // menyimpan pendaftaran kelas dan properti bawaannya per proses, bukan
        // per manajer, jadi importir klip yang berjalan bersamaan dengan
        // importir mesh mengaduk struktur yang sama. Alasan lengkapnya beserta
        // gejalanya di `Sim/Core/FbxSdkLock.h`.
        lock_ = std::unique_lock<std::mutex>(FbxSdkMutex());

        manager_ = FbxManager::Create();
        if (manager_ == nullptr) {
            error = "cannot create the FBX SDK manager";
            return false;
        }
        FbxIOSettings* io = FbxIOSettings::Create(manager_, IOSROOT);
        // **Membaca berkas tidak boleh menulis apa pun** — sama seperti di
        // `MeshImport.cpp`, dan alasannya sama: bawaan SDK membongkar media
        // tertanam ke folder `<nama>.fbm/` di sebelah berkas sumbernya, yaitu di
        // dalam folder aset milik orang lain. Importir klip tidak menyentuh satu
        // pun tekstur, jadi di sini efeknya murni sampah.
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

        const FbxAxisSystem target(FbxAxisSystem::OpenGL);
        if (scene_->GetGlobalSettings().GetAxisSystem() != target) {
            target.DeepConvertScene(scene_);
        }
        unitScale_ =
            scene_->GetGlobalSettings().GetSystemUnit().GetConversionFactorTo(FbxSystemUnit::m);
        return true;
    }

    FbxScene* Scene() const { return scene_; }
    double UnitScale() const { return unitScale_; }

private:
    /// Pertama, jadi ia yang terakhir dilepas — destruktor menjalankan
    /// `manager_->Destroy()` di badannya sebelum satu pun anggota dihancurkan.
    std::unique_lock<std::mutex> lock_;
    FbxManager* manager_ = nullptr;
    FbxScene* scene_ = nullptr;
    double unitScale_ = 1.0;
};

/// Bone per tingkat kedalaman — urutan yang sama dengan `assets::LoadMesh`.
///
/// Track diikat lewat nama, jadi urutannya tidak menentukan kebenaran di sini;
/// yang disamakan adalah urutan dua importir yang membaca berkas yang sama,
/// supaya keluarannya bisa dibandingkan langsung saat ada yang ditelusuri.
std::vector<FbxNode*> SceneBones(FbxScene& scene) {
    std::vector<FbxNode*> queue;
    std::vector<FbxNode*> bones;
    FbxNode* root = scene.GetRootNode();
    if (root == nullptr) {
        return bones;
    }
    for (int i = 0; i < root->GetChildCount(); ++i) {
        queue.push_back(root->GetChild(i));
    }
    for (std::size_t head = 0; head < queue.size(); ++head) {
        FbxNode* node = queue[head];
        if (node->GetSkeleton() != nullptr) {
            bones.push_back(node);
        }
        for (int i = 0; i < node->GetChildCount(); ++i) {
            queue.push_back(node->GetChild(i));
        }
    }
    return bones;
}

}  // namespace

namespace detail {

std::string JointLeafName(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void AlignHemisphere(std::vector<SampledFrame>& frames) {
    for (std::size_t i = 1; i < frames.size(); ++i) {
        if (glm::dot(frames[i - 1].rotation, frames[i].rotation) < 0.0f) {
            frames[i].rotation = -frames[i].rotation;
        }
    }
}

namespace {

bool IsConstant(const std::vector<float>& values) {
    for (std::size_t i = 1; i < values.size(); ++i) {
        if (std::abs(values[i] - values[0]) > kConstantEpsilon) {
            return false;
        }
    }
    return true;
}

/// Menambahkan sebuah kanal skalar dari deret nilai yang sudah dicuplik.
///
/// **Kanal yang tetap sepanjang klip tidak diimpor sama sekali**, dan itu bukan
/// penghematan tempat melainkan syarat agar klipnya bisa dipakai rig lain.
///
/// Pada animasi rangka, satu-satunya bone yang translasinya benar-benar
/// bergerak lazimnya adalah root/pinggul; translasi bone lain tetap, dan
/// nilainya adalah **panjang tulang rig yang mengekspor klip itu**. Mengimpornya
/// berarti memanggang proporsi rig sumber ke dalam klipnya, dan memasangnya ke
/// rig lain akan meregangkan rig itu menjadi proporsi si sumber.
///
/// Terukur: `Defeated.fbx` dan `Y Bot.fbx` sama-sama rig Mixamo bernama sama,
/// tapi Spine2-nya 0,09322 m lawan 0,13459 m. Dengan kanal tetap ikut diimpor,
/// memasang klip Defeated ke Y Bot memendekkan tulang itu **30,7%**; tanpa
/// kanal tetap, bone yang tidak dianimasikan tinggal di bind pose rig tujuan —
/// yaitu persis arti retargeting yang dijanjikan `RetargetMap`: sudut yang sama,
/// bukan titik yang sama.
///
/// **Interpolasinya linear, bukan bezier.** Bezier adalah bawaan `CurveKey`
/// karena itu yang diinginkan kurva yang ditulis tangan; kunci yang dicuplik
/// adalah cuplikan rapat pada laju tetap, dan bezier bertangen nol di antara
/// dua cuplikan menahan nilainya lalu meloncat — gerak yang tersendat pada klip
/// yang di berkasnya mulus.
bool RotationIsConstant(const std::vector<SampledFrame>& frames) {
    for (std::size_t i = 1; i < frames.size(); ++i) {
        const Quat& a = frames[0].rotation;
        const Quat& b = frames[i].rotation;
        if (std::abs(a.x - b.x) > kConstantEpsilon || std::abs(a.y - b.y) > kConstantEpsilon ||
            std::abs(a.z - b.z) > kConstantEpsilon || std::abs(a.w - b.w) > kConstantEpsilon) {
            return false;
        }
    }
    return true;
}

}  // namespace

void AddScalarChannel(Clip& clip, const std::string& bone, Channel channel,
                      const std::vector<float>& times, const std::vector<float>& values) {
    if (values.empty() || values.size() != times.size() || IsConstant(values)) {
        return;
    }
    const int track = clip.EnsureTrack(bone, channel);
    Curve& curve = clip.TrackAt(track).curve;
    for (std::size_t i = 0; i < values.size(); ++i) {
        CurveKey out;
        out.time = times[i];
        out.value = values[i];
        out.interpolation = Interpolation::Linear;
        curve.AddKey(out);
    }
}

void AddBoneChannels(Clip& clip, const std::string& bone,
                     const std::vector<SampledFrame>& frames) {
    if (frames.empty()) {
        return;
    }
    std::vector<float> times(frames.size());
    std::vector<float> values(frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        times[i] = frames[i].time;
    }

    for (int component = 0; component < 3; ++component) {
        for (std::size_t i = 0; i < frames.size(); ++i) {
            values[i] = frames[i].translation[component];
        }
        AddScalarChannel(clip, bone,
                         static_cast<Channel>(static_cast<int>(Channel::TranslationX) + component),
                         times, values);
    }
    for (int component = 0; component < 3; ++component) {
        for (std::size_t i = 0; i < frames.size(); ++i) {
            values[i] = frames[i].scale[component];
        }
        AddScalarChannel(clip, bone,
                         static_cast<Channel>(static_cast<int>(Channel::ScaleX) + component),
                         times, values);
    }

    // **Rotasi tetap TETAP diimpor, tidak seperti translasi.** Rotasi bind adalah
    // orientasi tulang, bukan panjangnya: sebuah klip yang meluruskan lengan
    // sepanjang durasinya benar-benar menyatakan sesuatu, dan membuangnya akan
    // mengembalikan lengan itu ke pose rig tujuan di tengah animasi yang
    // seharusnya menahannya.
    const int track = clip.EnsureRotationTrack(bone);
    const std::size_t count = RotationIsConstant(frames) ? 1u : frames.size();
    for (std::size_t i = 0; i < count; ++i) {
        clip.RotationTrackAt(track).AddKey(RotationKey{frames[i].time, frames[i].rotation});
    }
}

std::vector<Clip> ImportClipsFromFbxFile(const std::filesystem::path& path,
                                        std::string& error) {
    std::vector<Clip> clips;
    error.clear();
    std::error_code exists;
    if (path.empty() || !std::filesystem::exists(path, exists)) {
        error = "file not found";
        return clips;
    }

    FbxSceneHandle handle;
    if (!handle.Open(path, error)) {
        return clips;
    }
    FbxScene& scene = *handle.Scene();
    const auto unitScale = static_cast<float>(handle.UnitScale());

    const double sceneRate = FbxTime::GetFrameRate(scene.GetGlobalSettings().GetTimeMode());
    const float frameRate = sceneRate > 1.0 ? static_cast<float>(sceneRate) : 30.0f;

    const std::vector<FbxNode*> bones = SceneBones(scene);

    for (int stackIndex = 0; stackIndex < scene.GetSrcObjectCount<FbxAnimStack>(); ++stackIndex) {
        FbxAnimStack* stack = scene.GetSrcObject<FbxAnimStack>(stackIndex);
        if (stack == nullptr) {
            continue;
        }
        FbxAnimLayer* layer = stack->GetMember<FbxAnimLayer>(0);
        if (layer == nullptr) {
            continue;
        }
        // Node dievaluasi terhadap stack yang sedang aktif, bukan terhadap stack
        // yang diminta — jadi yang lupa menyetelnya mencuplik take pertama
        // sebanyak jumlah take yang ada.
        scene.SetCurrentAnimationStack(stack);

        const FbxTimeSpan span = stack->GetLocalTimeSpan();
        // Klip mulai dari nol, bukan dari waktu tempatnya kebetulan berada di
        // timeline berkasnya. Take yang mulai di detik ke-30 akan tampak sebagai
        // klip yang diam selama 30 detik lebih dulu.
        const double begin = span.GetStart().GetSecondDouble();
        const double length = span.GetStop().GetSecondDouble() - begin;
        if (length <= 0.0) {
            continue;
        }

        Clip clip;
        // **Dinamai menurut berkasnya bila hanya ada satu take yang berisi.**
        // Take di berkas Mixamo bernama "mixamo.com", yang tidak memberi tahu
        // apa pun; nama berkasnya justru yang dipilih orang — "Running.fbx".
        // Beberapa take berisi berarti namanya harus membedakan, jadi di sana
        // nama take yang dipakai.
        clip.name = stack->GetName() != nullptr ? stack->GetName() : "";
        clip.frameRate = frameRate;
        clip.duration = std::max(static_cast<float>(length), 1e-6f);
        clip.looping = true;

        const int steps =
            std::max(1, static_cast<int>(std::lround(length * static_cast<double>(frameRate))));

        std::vector<SampledFrame> frames(static_cast<std::size_t>(steps) + 1);
        for (FbxNode* node : bones) {
            if (node->GetName() == nullptr) {
                continue;
            }
            // **Hanya bone yang punya kurva di take ini.** Mencuplik seluruh bone
            // akan memberi track rotasi juga kepada bone yang take ini tidak
            // menyentuhnya sama sekali — dan track rotasi tetap adalah orientasi
            // bind rig SUMBER, yang dipasangkan ke rig lain menimpa bind pose rig
            // tujuan. Itu persis kerusakan yang dijaga `AddChannel` untuk
            // translasi, lewat pintu yang lain.
            //
            // Ia juga yang membuat take kosong terdeteksi: "Take 001" milik
            // berkas Mixamo tidak punya satu pun kurva, jadi ia tidak
            // menghasilkan track apa pun dan dibuang di bawah.
            if (node->LclTranslation.GetCurveNode(layer) == nullptr &&
                node->LclRotation.GetCurveNode(layer) == nullptr &&
                node->LclScaling.GetCurveNode(layer) == nullptr) {
                continue;
            }
            const std::string bone = node->GetName();

            for (std::size_t f = 0; f < frames.size(); ++f) {
                const double at =
                    begin + length * static_cast<double>(f) / static_cast<double>(steps);
                FbxTime when;
                when.SetSecondDouble(at);
                const FbxAMatrix local = node->EvaluateLocalTransform(when);

                SampledFrame& frame = frames[f];
                frame.time = static_cast<float>(at - begin);
                frame.translation = ToVec3(local.GetT()) * unitScale;
                frame.rotation = glm::normalize(ToQuat(local.GetQ()));
                frame.scale = ToVec3(local.GetS());
                // **Belahan kuaternion disamakan dengan frame sebelumnya.** `q`
                // dan `-q` adalah rotasi yang sama, dan penguraian tiap frame
                // berdiri sendiri — jadi tandanya boleh berbalik di tengah klip
                // tanpa ada yang berubah pada rotasinya. Yang membacanya
                // meng-slerp dua kunci berurutan, dan dua kunci yang berseberangan
                // belahan diputar lewat jalan memutar: satu frame yang berputar
                // hampir 360°.
                if (f > 0 && glm::dot(frames[f - 1].rotation, frame.rotation) < 0.0f) {
                    frame.rotation = -frame.rotation;
                }
            }

            AddBoneChannels(clip, bone, frames);
        }

        // Take yang tidak menganimasikan satu bone pun dibuang, tidak
        // dikembalikan sebagai klip kosong. Berkas Mixamo selalu membawa sebuah
        // "Take 001" yang persis begitu, dan klip kosong yang ikut terbawa akan
        // muncul di daftar aset sebagai klip yang bisa dipilih dan tidak
        // melakukan apa-apa.
        if (clip.TrackCount() == 0 && clip.RotationTrackCount() == 0) {
            continue;
        }
        clips.push_back(std::move(clip));
    }

    if (clips.size() == 1) {
        clips.front().name = path.stem().string();
    }
    if (clips.empty()) {
        error = "no animation take in this file animates a bone";
    }
    return clips;
}

}  // namespace detail

std::vector<Clip> ImportClips(const std::filesystem::path& path, std::string& error) {
    error.clear();
    std::error_code exists;
    if (path.empty() || !std::filesystem::exists(path, exists)) {
        error = "file not found";
        return {};
    }

    // Dipilih menurut ekstensi, dengan alasan yang sama seperti `LoadMesh`:
    // mencoba tiap pembaca berurutan berhasil, tapi pesan galat yang sampai ke
    // pengguna lalu datang dari pembaca yang salah.
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (extension == ".gltf" || extension == ".glb") {
        return detail::ImportClipsFromGltfFile(path, error);
    }
    if (extension == ".usd" || extension == ".usda" || extension == ".usdc" ||
        extension == ".usdz") {
        return detail::ImportClipsFromUsdFile(path, error);
    }
    return detail::ImportClipsFromFbxFile(path, error);
}

}  // namespace sim::animation
