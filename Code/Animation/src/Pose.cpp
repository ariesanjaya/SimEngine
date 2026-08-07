#include "Sim/Animation/Pose.h"

#include <algorithm>

namespace sim::animation {
namespace {

/// Nlerp yang dipendekkan.
///
/// Tanda `b` dibalik kalau hasil kalinya negatif. Tanpa itu, dua kuaternion yang
/// mewakili rotasi berdekatan tapi bertanda berlawanan akan dicampur lewat jalan
/// memutar 360° — terlihat sebagai karakter yang berputar penuh di tengah
/// transisi yang seharusnya beberapa derajat.
Quat NlerpShortest(const Quat& a, const Quat& b, float t) {
    const float sign = glm::dot(a, b) < 0.0f ? -1.0f : 1.0f;
    Quat result;
    result.w = a.w + (b.w * sign - a.w) * t;
    result.x = a.x + (b.x * sign - a.x) * t;
    result.y = a.y + (b.y * sign - a.y) * t;
    result.z = a.z + (b.z * sign - a.z) * t;
    return glm::normalize(result);
}

BoneTransform Mix(const BoneTransform& a, const BoneTransform& b, float t) {
    BoneTransform result;
    result.translation = a.translation + (b.translation - a.translation) * t;
    result.scale = a.scale + (b.scale - a.scale) * t;
    result.rotation = NlerpShortest(a.rotation, b.rotation, t);
    return result;
}

}  // namespace

Pose::Pose(const Skeleton& skeleton) {
    Reset(skeleton);
}

void Pose::Reset(const Skeleton& skeleton) {
    local_.resize(static_cast<std::size_t>(skeleton.BoneCount()));
    for (int i = 0; i < skeleton.BoneCount(); ++i) {
        local_[static_cast<std::size_t>(i)] = skeleton.Bone(i).bind;
    }
}

void Pose::Resize(int boneCount) {
    // `resize`, bukan `assign`: yang sudah ada harus bertahan. Lihat catatan di
    // header — `Blend` menulis ke pose yang kerap salah satu masukannya sendiri.
    local_.resize(static_cast<std::size_t>(std::max(boneCount, 0)));
}

const BoneTransform& Pose::Local(int index) const {
    static const BoneTransform kFallback;
    if (index < 0 || index >= BoneCount()) {
        return kFallback;
    }
    return local_[static_cast<std::size_t>(index)];
}

BoneTransform& Pose::Local(int index) {
    static BoneTransform fallback;
    if (index < 0 || index >= BoneCount()) {
        fallback = BoneTransform{};
        return fallback;
    }
    return local_[static_cast<std::size_t>(index)];
}

std::vector<Vec3>& Pose::EulerScratch(std::size_t boneCount) {
    eulerScratch_.assign(boneCount, Vec3(0.0f));
    return eulerScratch_;
}

std::vector<uint8_t>& Pose::TouchedScratch(std::size_t boneCount) {
    touchedScratch_.assign(boneCount, 0u);
    return touchedScratch_;
}

void Pose::ComputeGlobal(const Skeleton& skeleton, std::vector<BoneTransform>& out) const {
    const int count = std::min(BoneCount(), skeleton.BoneCount());
    out.resize(static_cast<std::size_t>(count));
    // Satu lintasan maju, tanpa rekursi: urutan topologis rangka menjamin
    // transform induk sudah selesai saat anaknya dihitung.
    for (int i = 0; i < count; ++i) {
        const int parent = skeleton.Bone(i).parent;
        out[static_cast<std::size_t>(i)] =
            parent >= 0 && parent < i
                ? Concatenate(out[static_cast<std::size_t>(parent)],
                              local_[static_cast<std::size_t>(i)])
                : local_[static_cast<std::size_t>(i)];
    }
}

void Pose::ComputeSkinning(const Skeleton& skeleton, std::vector<Mat4>& out) const {
    std::vector<BoneTransform> global;
    ComputeGlobal(skeleton, global);
    const std::vector<Mat4>& inverseBind = skeleton.InverseBindMatrices();
    out.resize(global.size());
    for (std::size_t i = 0; i < global.size(); ++i) {
        out[i] = global[i].ToMatrix() * inverseBind[i];
    }
}

// --- BoneMask -----------------------------------------------------------------

void BoneMask::Reset(int boneCount, float weight) {
    weights_.assign(static_cast<std::size_t>(std::max(boneCount, 0)),
                    std::clamp(weight, 0.0f, 1.0f));
}

float BoneMask::Weight(int bone) const {
    if (weights_.empty()) {
        return 1.0f;
    }
    if (bone < 0 || bone >= BoneCount()) {
        return 0.0f;
    }
    return weights_[static_cast<std::size_t>(bone)];
}

void BoneMask::SetWeight(int bone, float weight) {
    if (bone < 0 || bone >= BoneCount()) {
        return;
    }
    weights_[static_cast<std::size_t>(bone)] = std::clamp(weight, 0.0f, 1.0f);
}

void BoneMask::SetChainWeight(const Skeleton& skeleton, int root, float weight) {
    if (weights_.empty()) {
        Reset(skeleton.BoneCount(), 0.0f);
    }
    for (int i = 0; i < skeleton.BoneCount() && i < BoneCount(); ++i) {
        if (skeleton.IsDescendant(i, root)) {
            weights_[static_cast<std::size_t>(i)] = std::clamp(weight, 0.0f, 1.0f);
        }
    }
}

// --- pencampuran --------------------------------------------------------------

void Blend(const Pose& a, const Pose& b, float weight, Pose& out) {
    static const BoneMask kNoMask;
    Blend(a, b, weight, kNoMask, out);
}

void Blend(const Pose& a, const Pose& b, float weight, const BoneMask& mask, Pose& out) {
    const int count = std::min(a.BoneCount(), b.BoneCount());
    out.Resize(count);
    const float base = std::clamp(weight, 0.0f, 1.0f);
    for (int i = 0; i < count; ++i) {
        const float t = base * mask.Weight(i);
        // Dua ujungnya dijalan-pintas, bukan demi kecepatan semata: pada t = 0
        // hasilnya harus `a` **byte demi byte**, karena layer bermask menyentuh
        // sebagian besar bone dengan bobot nol dan nlerp yang dinormalkan ulang
        // akan menggeser bit terakhirnya pada tiap frame.
        if (t <= 0.0f) {
            out.Local(i) = a.Local(i);
        } else if (t >= 1.0f) {
            out.Local(i) = b.Local(i);
        } else {
            out.Local(i) = Mix(a.Local(i), b.Local(i), t);
        }
    }
}

void BlendAdditive(const Pose& base, const Pose& additive, const Pose& reference, float weight,
                   const BoneMask& mask, Pose& out) {
    const int count = std::min({base.BoneCount(), additive.BoneCount(), reference.BoneCount()});
    out.Resize(count);
    const float amount = std::clamp(weight, 0.0f, 1.0f);
    for (int i = 0; i < count; ++i) {
        const float t = amount * mask.Weight(i);
        const BoneTransform& b = base.Local(i);
        if (t <= 0.0f) {
            out.Local(i) = b;
            continue;
        }
        // Selisih diambil terhadap pose acuan, bukan terhadap bind pose. Klip
        // aditif dibuat sebagai "beda dari sebuah pose tertentu" — memakai bind
        // pose sebagai acuan hanya benar kalau klipnya kebetulan dibuat begitu,
        // dan kalau tidak, seluruh anggota badan bergeser.
        const BoneTransform& a = additive.Local(i);
        const BoneTransform& r = reference.Local(i);
        BoneTransform delta;
        delta.translation = a.translation - r.translation;
        delta.rotation = a.rotation * glm::inverse(r.rotation);
        delta.scale = Vec3(r.scale.x != 0.0f ? a.scale.x / r.scale.x : 1.0f,
                           r.scale.y != 0.0f ? a.scale.y / r.scale.y : 1.0f,
                           r.scale.z != 0.0f ? a.scale.z / r.scale.z : 1.0f);

        BoneTransform result;
        result.translation = b.translation + delta.translation * t;
        result.rotation = glm::normalize(
            NlerpShortest(Quat(1.0f, 0.0f, 0.0f, 0.0f), delta.rotation, t) * b.rotation);
        result.scale = b.scale * (Vec3(1.0f) + (delta.scale - Vec3(1.0f)) * t);
        out.Local(i) = result;
    }
}

}  // namespace sim::animation
