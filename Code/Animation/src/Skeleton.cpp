#include "Sim/Animation/Skeleton.h"

#include <algorithm>

namespace sim::animation {
namespace {

/// Pembanding untuk pencarian biner pada daftar pemetaan retarget.
struct BySource {
    bool operator()(const std::pair<std::string, std::string>& entry,
                    std::string_view name) const {
        return entry.first < name;
    }
    bool operator()(std::string_view name,
                    const std::pair<std::string, std::string>& entry) const {
        return name < entry.first;
    }
};

}  // namespace

Mat4 BoneTransform::ToMatrix() const {
    Mat4 matrix = glm::mat4_cast(rotation);
    matrix[0] *= scale.x;
    matrix[1] *= scale.y;
    matrix[2] *= scale.z;
    matrix[3] = Vec4(translation, 1.0f);
    return matrix;
}

BoneTransform Concatenate(const BoneTransform& parent, const BoneTransform& child) {
    BoneTransform result;
    result.rotation = parent.rotation * child.rotation;
    result.scale = parent.scale * child.scale;
    result.translation =
        parent.translation + parent.rotation * (parent.scale * child.translation);
    return result;
}

Quat EulerToQuat(const Vec3& radians) {
    return glm::angleAxis(radians.x, kAxisX) * glm::angleAxis(radians.y, kAxisY) *
           glm::angleAxis(radians.z, kAxisZ);
}

Vec3 QuatToEuler(const Quat& rotation) {
    // Diurai dengan urutan yang sama dengan yang dipakai menyusunnya. Memakai
    // `glm::eulerAngles` di sini akan mengembalikan sudut dalam konvensi lain,
    // dan bolak-baliknya tidak lagi menghasilkan rotasi yang sama.
    const Mat3 m = glm::mat3_cast(glm::normalize(rotation));
    // m = Rx(x) * Ry(y) * Rz(z)
    const float sy = std::clamp(m[2][0], -1.0f, 1.0f);
    const float y = std::asin(sy);
    // Di dekat |sy| = 1 sumbu X dan Z berimpit — gimbal lock. Salah satunya
    // dipilih nol, karena membaginya di antara keduanya hanya menghasilkan dua
    // angka yang sama-sama tidak berarti.
    if (std::abs(sy) > 0.99999f) {
        return Vec3(std::atan2(-m[1][2], m[1][1]), y, 0.0f);
    }
    return Vec3(std::atan2(-m[2][1], m[2][2]), y, std::atan2(-m[1][0], m[0][0]));
}

// --- Skeleton -----------------------------------------------------------------

const Bone& Skeleton::Bone(int index) const {
    static const animation::Bone kFallback;
    if (index < 0 || index >= BoneCount()) {
        return kFallback;
    }
    return bones_[static_cast<std::size_t>(index)];
}

Bone& Skeleton::Bone(int index) {
    static animation::Bone fallback;
    if (index < 0 || index >= BoneCount()) {
        fallback = animation::Bone{};
        return fallback;
    }
    // Bind pose bisa diubah lewat referensi ini, jadi cache-nya batal. Menebak
    // "mungkin hanya namanya yang diubah" adalah cara membiarkan matriks bind
    // basi bertahan sampai frame berikutnya.
    Invalidate();
    return bones_[static_cast<std::size_t>(index)];
}

int Skeleton::AddBone(const animation::Bone& bone) {
    if (BoneCount() >= kMaxBones || bone.name.empty()) {
        return -1;
    }
    if (bone.parent >= BoneCount() || bone.parent < -1) {
        return -1;
    }
    if (Find(bone.name) >= 0) {
        // Nama harus unik: track klip menunjuk bone lewat namanya, jadi dua bone
        // senama berarti track yang menunjuk dua tempat sekaligus.
        return -1;
    }
    bones_.push_back(bone);
    Invalidate();
    return BoneCount() - 1;
}

bool Skeleton::RemoveBone(int index) {
    if (index < 0 || index >= BoneCount()) {
        return false;
    }
    std::vector<int> remap(bones_.size(), -1);
    std::vector<animation::Bone> kept;
    kept.reserve(bones_.size());
    for (int i = 0; i < BoneCount(); ++i) {
        if (IsDescendant(i, index)) {
            continue;
        }
        remap[static_cast<std::size_t>(i)] = static_cast<int>(kept.size());
        animation::Bone bone = bones_[static_cast<std::size_t>(i)];
        bone.parent = bone.parent >= 0 ? remap[static_cast<std::size_t>(bone.parent)] : -1;
        kept.push_back(std::move(bone));
    }
    bones_ = std::move(kept);
    Invalidate();
    return true;
}

bool Skeleton::SetBones(const std::vector<animation::Bone>& bones) {
    if (static_cast<int>(bones.size()) > kMaxBones) {
        return false;
    }
    for (std::size_t i = 0; i < bones.size(); ++i) {
        const int parent = bones[i].parent;
        if (parent < -1 || parent >= static_cast<int>(i)) {
            // Induk yang indeksnya tidak lebih kecil melanggar urutan topologis,
            // dan seluruh penghitungan pose bergantung padanya. Ditolak
            // seluruhnya, bukan sebagian: rangka yang separuh dimuat lebih sulit
            // ditemukan sebabnya daripada rangka yang menolak dimuat.
            return false;
        }
        if (bones[i].name.empty()) {
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (bones[j].name == bones[i].name) {
                return false;
            }
        }
    }
    bones_ = bones;
    Invalidate();
    return true;
}

void Skeleton::Clear() {
    bones_.clear();
    Invalidate();
}

int Skeleton::Find(std::string_view name) const {
    for (int i = 0; i < BoneCount(); ++i) {
        if (bones_[static_cast<std::size_t>(i)].name == name) {
            return i;
        }
    }
    return -1;
}

void Skeleton::Children(int index, std::vector<int>& out) const {
    out.clear();
    for (int i = 0; i < BoneCount(); ++i) {
        if (bones_[static_cast<std::size_t>(i)].parent == index) {
            out.push_back(i);
        }
    }
}

bool Skeleton::IsDescendant(int candidate, int ancestor) const {
    if (candidate < 0 || ancestor < 0) {
        return false;
    }
    // Menaik lewat induk, dan itu pasti berhenti: indeks induk selalu lebih
    // kecil, jadi tidak ada lingkaran yang bisa dibentuk.
    while (candidate >= 0) {
        if (candidate == ancestor) {
            return true;
        }
        candidate = bones_[static_cast<std::size_t>(candidate)].parent;
    }
    return false;
}

const std::vector<BoneTransform>& Skeleton::GlobalBind() const {
    if (!cacheValid_) {
        globalBind_.resize(bones_.size());
        inverseBind_.resize(bones_.size());
        for (std::size_t i = 0; i < bones_.size(); ++i) {
            const animation::Bone& bone = bones_[i];
            globalBind_[i] =
                bone.parent >= 0
                    ? Concatenate(globalBind_[static_cast<std::size_t>(bone.parent)], bone.bind)
                    : bone.bind;
            inverseBind_[i] = glm::inverse(globalBind_[i].ToMatrix());
        }
        cacheValid_ = true;
    }
    return globalBind_;
}

const std::vector<Mat4>& Skeleton::InverseBindMatrices() const {
    GlobalBind();
    return inverseBind_;
}

void Skeleton::Invalidate() {
    cacheValid_ = false;
}

// --- RetargetMap --------------------------------------------------------------

void RetargetMap::Set(const std::string& sourceName, const std::string& targetName) {
    if (sourceName.empty()) {
        return;
    }
    const auto at = std::lower_bound(entries_.begin(), entries_.end(),
                                     std::string_view(sourceName), BySource{});
    if (at != entries_.end() && at->first == sourceName) {
        at->second = targetName;
        return;
    }
    entries_.insert(at, {sourceName, targetName});
}

void RetargetMap::Remove(const std::string& sourceName) {
    const auto at = std::lower_bound(entries_.begin(), entries_.end(),
                                     std::string_view(sourceName), BySource{});
    if (at != entries_.end() && at->first == sourceName) {
        entries_.erase(at);
    }
}

void RetargetMap::Clear() {
    entries_.clear();
}

RetargetMap ComposeRetarget(const RetargetMap& sourceToStandard,
                            const RetargetMap& targetToStandard) {
    RetargetMap composed;
    for (const auto& [sourceBone, standard] : sourceToStandard.Entries()) {
        for (const auto& [targetBone, targetStandard] : targetToStandard.Entries()) {
            if (targetStandard == standard) {
                composed.Set(sourceBone, targetBone);
                break;
            }
        }
    }
    return composed;
}

std::string_view RetargetMap::Resolve(std::string_view sourceName) const {
    const auto at = std::lower_bound(entries_.begin(), entries_.end(), sourceName, BySource{});
    if (at != entries_.end() && at->first == sourceName) {
        return at->second;
    }
    return sourceName;
}

}  // namespace sim::animation
