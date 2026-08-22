#include "Sim/SceneView/SkinnedPreview.h"

#include "Sim/Animation/AnimationIo.h"
#include "Sim/Animation/ClipImport.h"
#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Assets/MeshData.h"
#include "Sim/Core/Log.h"
#include "Sim/Scene/Components.h"

#include <algorithm>
#include <utility>

namespace sim::view {
namespace {

/// Jalur absolut sebuah rujukan aset, atau kosong.
std::filesystem::path ResolveAsset(const assets::AssetDatabase* database, const AssetRef& ref) {
    if (database == nullptr || !ref.IsValid()) {
        return {};
    }
    const assets::AssetRecord* record = database->Find(ref.guid);
    return record != nullptr ? database->AbsolutePath(*record) : std::filesystem::path{};
}

/// `assets::SkeletonData` menjadi `animation::Skeleton`.
///
/// **Penerjemahan ini ada di editor, bukan di salah satu modulnya.** Sisi aset
/// tidak boleh mengenal tipe animasi dan sebaliknya — keduanya modul sejajar —
/// jadi yang menjembatani adalah yang memakai keduanya.
bool ToSkeleton(const assets::SkeletonData& source, animation::Skeleton& out) {
    std::vector<animation::Bone> bones;
    bones.reserve(source.bones.size());
    for (const assets::SkeletonBone& bone : source.bones) {
        animation::Bone converted;
        converted.name = bone.name;
        converted.parent = bone.parent;
        converted.bind.translation = bone.translation;
        converted.bind.rotation = bone.rotation;
        converted.bind.scale = bone.scale;
        bones.push_back(std::move(converted));
    }
    return out.SetBones(bones);
}

}  // namespace

const animation::Skeleton* SkinnedPreview::AcquireSkeleton(const std::filesystem::path& path) {
    const std::string key = path.string();
    if (const auto found = skeletons_.find(key); found != skeletons_.end()) {
        // Rangka kosong berarti "sudah pernah dicoba dan gagal".
        return found->second.BoneCount() > 0 ? &found->second : nullptr;
    }

    std::string error;
    const assets::SkeletonData data = assets::LoadSkeleton(path, error);
    animation::Skeleton skeleton;
    if (!data.IsValid() || !ToSkeleton(data, skeleton)) {
        SIM_WARN("Editor", "cannot read a skeleton from {}: {}", key,
                 error.empty() ? "the bone order is not topological" : error);
        skeletons_.emplace(key, animation::Skeleton{});
        return nullptr;
    }
    SIM_INFO("Editor", "skeleton ready: {} ({} bones)", key, skeleton.BoneCount());
    const auto inserted = skeletons_.emplace(key, std::move(skeleton));
    return &inserted.first->second;
}

const animation::Clip* SkinnedPreview::AcquireClip(const std::filesystem::path& path) {
    const std::string key = path.string();
    if (const auto found = clips_.find(key); found != clips_.end()) {
        const animation::Clip& clip = found->second;
        const bool empty = clip.TrackCount() == 0 && clip.RotationTrackCount() == 0;
        return empty ? nullptr : &clip;
    }

    animation::Clip clip;
    std::string error;
    // **Berkas sumber boleh dirujuk langsung sebagai klip** — FBX, glTF, maupun
    // USD. Memaksanya lewat langkah impor ke `.simanim` lebih dulu berarti
    // sebuah berkas Mixamo tidak bisa dicoba tanpa ritual — dan mencobanya
    // justru hal pertama yang ingin dilakukan orang. Yang memanggangnya menjadi
    // `.simanim` tetap berguna, tapi ia menjadi pilihan, bukan syarat.
    //
    // Yang bukan `.simanim` diserahkan ke `ImportClips`, dan ia yang memilih
    // pembacanya menurut ekstensi.
    std::filesystem::path extension = path.extension();
    std::string lowered = extension.string();
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowered == ".simanim") {
        animation::ClipDocument document;
        const animation::AnimationIoResult result = animation::LoadClip(clip, document, path);
        if (!result.ok) {
            error = result.error;
        }
    } else {
        std::vector<animation::Clip> imported = animation::ImportClips(path, error);
        if (!imported.empty()) {
            // Yang pertama. Berkas bertake banyak menuntut cara memilihnya, dan
            // itu medan baru di komponennya — bukan tebakan diam-diam di sini.
            if (imported.size() > 1) {
                SIM_WARN("Editor", "{} has {} takes; playing '{}'", key, imported.size(),
                         imported.front().name);
            }
            clip = std::move(imported.front());
        }
    }

    if (clip.TrackCount() == 0 && clip.RotationTrackCount() == 0) {
        SIM_WARN("Editor", "cannot read a clip from {}: {}", key,
                 error.empty() ? "it animates nothing" : error);
        clips_.emplace(key, animation::Clip{});
        return nullptr;
    }
    SIM_INFO("Editor", "clip ready: {} ({} rotation tracks, {:.2f} s)", key,
             clip.RotationTrackCount(), clip.duration);
    const auto inserted = clips_.emplace(key, std::move(clip));
    return &inserted.first->second;
}

void SkinnedPreview::Update(scene::World& world, const assets::AssetDatabase* assets,
                            float deltaSeconds) {
    animated_ = 0;
    for (const auto raw : world.Registry().view<scene::AnimatorComponent>()) {
        const auto entity = static_cast<scene::Entity>(raw);
        auto* animator = world.TryGet<scene::AnimatorComponent>(entity);
        const auto* renderer = world.TryGet<scene::MeshRendererComponent>(entity);
        if (animator == nullptr || renderer == nullptr) {
            continue;
        }

        const animation::Skeleton* skeleton = nullptr;
        if (const std::filesystem::path mesh = ResolveAsset(assets, renderer->mesh);
            !mesh.empty()) {
            skeleton = AcquireSkeleton(mesh);
        }
        const animation::Clip* clip = nullptr;
        if (const std::filesystem::path path = ResolveAsset(assets, animator->clip);
            !path.empty()) {
            clip = AcquireClip(path);
        }
        if (skeleton == nullptr || clip == nullptr) {
            states_.erase(static_cast<uint32_t>(entity));
            continue;
        }

        State& state = states_[static_cast<uint32_t>(entity)];
        // Dipasang ulang hanya saat pasangannya berubah. Memasang track ke bone
        // adalah pencarian nama untuk tiap track, dan jawabannya tidak berubah
        // selama klip dan rangkanya tetap.
        if (state.clip != clip || state.skeleton != skeleton) {
            state.clip = clip;
            state.skeleton = skeleton;
            state.binding.Bind(*clip, *skeleton);
            if (!state.binding.Unresolved().empty()) {
                // Disebutkan, tidak didiamkan: animasi yang setengah berjalan
                // tanpa sebab yang terlihat lebih buruk daripada anggota badan
                // yang jelas tidak bergerak.
                SIM_WARN("Editor", "{} track without a bone in this rig, first '{}'",
                         state.binding.Unresolved().size(), state.binding.Unresolved().front());
            }
        }

        if (animator->playing) {
            animator->time += deltaSeconds * animator->speed;
        }
        // Dibungkus di sini, bukan diserahkan ke `SampleClip`: waktu yang tumbuh
        // tanpa batas kehilangan presisi float-nya sesudah beberapa jam, dan
        // yang terlihat adalah animasi yang makin tersendat pada editor yang
        // dibiarkan terbuka.
        if (animator->loop) {
            animator->time = std::fmod(animator->time, clip->duration);
            if (animator->time < 0.0f) {
                animator->time += clip->duration;
            }
        } else {
            animator->time = std::clamp(animator->time, 0.0f, clip->duration);
        }

        animation::SampleClip(*clip, state.binding, *skeleton, animator->time, state.pose);
        state.pose.ComputeSkinning(*skeleton, state.palette);
        ++animated_;
    }
}

std::span<const Mat4> SkinnedPreview::PaletteFor(scene::Entity entity) const {
    const auto found = states_.find(static_cast<uint32_t>(entity));
    if (found == states_.end()) {
        return {};
    }
    return found->second.palette;
}

void SkinnedPreview::Clear() {
    skeletons_.clear();
    clips_.clear();
    states_.clear();
    animated_ = 0;
}

}  // namespace sim::view
