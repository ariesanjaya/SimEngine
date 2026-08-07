#include "Sim/Animation/Clip.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace sim::animation {
namespace {

constexpr std::array<const char*, kChannelCount> kChannelNames{
    "TranslationX", "TranslationY", "TranslationZ", "RotationX", "RotationY",
    "RotationZ",    "ScaleX",       "ScaleY",       "ScaleZ",
};

}  // namespace

float ChannelValue(const BoneTransform& transform, Channel channel) {
    const Vec3 euler = QuatToEuler(transform.rotation);
    switch (channel) {
        case Channel::TranslationX: return transform.translation.x;
        case Channel::TranslationY: return transform.translation.y;
        case Channel::TranslationZ: return transform.translation.z;
        case Channel::RotationX: return euler.x;
        case Channel::RotationY: return euler.y;
        case Channel::RotationZ: return euler.z;
        case Channel::ScaleX: return transform.scale.x;
        case Channel::ScaleY: return transform.scale.y;
        case Channel::ScaleZ: return transform.scale.z;
    }
    return 0.0f;
}

const char* ToString(Channel channel) {
    const auto index = static_cast<std::size_t>(channel);
    return index < kChannelNames.size() ? kChannelNames[index] : kChannelNames[0];
}

Channel ChannelFromString(std::string_view text) {
    for (std::size_t i = 0; i < kChannelNames.size(); ++i) {
        if (text == kChannelNames[i]) {
            return static_cast<Channel>(i);
        }
    }
    return Channel::TranslationX;
}

int ChannelGroup(Channel channel) {
    return static_cast<int>(channel) / 3;
}

// --- track --------------------------------------------------------------------

const Track& Clip::TrackAt(int index) const {
    static const Track kFallback;
    if (index < 0 || index >= TrackCount()) {
        return kFallback;
    }
    return tracks_[static_cast<std::size_t>(index)];
}

Track& Clip::TrackAt(int index) {
    static Track fallback;
    if (index < 0 || index >= TrackCount()) {
        fallback = Track{};
        return fallback;
    }
    return tracks_[static_cast<std::size_t>(index)];
}

int Clip::FindTrack(std::string_view bone, Channel channel) const {
    for (int i = 0; i < TrackCount(); ++i) {
        const Track& track = tracks_[static_cast<std::size_t>(i)];
        if (track.channel == channel && track.bone == bone) {
            return i;
        }
    }
    return -1;
}

int Clip::EnsureTrack(const std::string& bone, Channel channel) {
    const int existing = FindTrack(bone, channel);
    if (existing >= 0) {
        return existing;
    }
    Track track;
    track.bone = bone;
    track.channel = channel;
    tracks_.push_back(std::move(track));
    return TrackCount() - 1;
}

bool Clip::RemoveTrack(int index) {
    if (index < 0 || index >= TrackCount()) {
        return false;
    }
    tracks_.erase(tracks_.begin() + index);
    return true;
}

void Clip::SetTracks(const std::vector<Track>& tracks) {
    tracks_ = tracks;
}

int Clip::RemoveEmptyTracks() {
    const auto tail = std::remove_if(tracks_.begin(), tracks_.end(), [](const Track& track) {
        return track.curve.Keys().empty();
    });
    const int removed = static_cast<int>(std::distance(tail, tracks_.end()));
    tracks_.erase(tail, tracks_.end());
    return removed;
}

// --- event --------------------------------------------------------------------

std::size_t Clip::AddEvent(const Event& event) {
    // Disisipkan menurut waktu, bukan ditempel di belakang: `CollectEvents`
    // mengembalikan event berurutan waktu, dan mengurutkan ulang setiap kali ia
    // dipanggil adalah pengurutan di dalam lingkaran pemutaran.
    const auto at = std::lower_bound(events_.begin(), events_.end(), event.time,
                                     [](const Event& candidate, float time) {
                                         return candidate.time < time;
                                     });
    const auto index = static_cast<std::size_t>(std::distance(events_.begin(), at));
    events_.insert(at, event);
    return index;
}

bool Clip::RemoveEvent(std::size_t index) {
    if (index >= events_.size()) {
        return false;
    }
    events_.erase(events_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

void Clip::SetEvents(const std::vector<Event>& events) {
    events_ = events;
    std::stable_sort(events_.begin(), events_.end(),
                     [](const Event& a, const Event& b) { return a.time < b.time; });
}

void Clip::CollectEvents(float fromTime, float toTime, std::vector<const Event*>& out) const {
    out.clear();
    if (events_.empty()) {
        return;
    }
    const auto gather = [&](float low, float high) {
        for (const Event& event : events_) {
            if (event.time >= low && event.time < high) {
                out.push_back(&event);
            }
        }
    };
    if (toTime >= fromTime) {
        gather(fromTime, toTime);
        return;
    }
    // Lintasan yang membungkus: ekor klip lalu kepalanya. Urutannya sengaja
    // begitu — event di detik terakhir memang terjadi sebelum event di detik
    // pertama lintasan berikutnya.
    gather(fromTime, duration);
    gather(0.0f, toTime);
}

// --- penanda fase -------------------------------------------------------------

std::size_t Clip::AddSyncMarker(const SyncMarker& marker) {
    const auto at = std::lower_bound(sync_.begin(), sync_.end(), marker.time,
                                     [](const SyncMarker& candidate, float time) {
                                         return candidate.time < time;
                                     });
    const auto index = static_cast<std::size_t>(std::distance(sync_.begin(), at));
    sync_.insert(at, marker);
    return index;
}

bool Clip::RemoveSyncMarker(std::size_t index) {
    if (index >= sync_.size()) {
        return false;
    }
    sync_.erase(sync_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

void Clip::SetSyncMarkers(const std::vector<SyncMarker>& markers) {
    sync_ = markers;
    std::stable_sort(sync_.begin(), sync_.end(),
                     [](const SyncMarker& a, const SyncMarker& b) { return a.time < b.time; });
}

float Clip::MatchPhase(float time, const Clip& other) const {
    const float safeDuration = std::max(duration, 1e-6f);
    const float normalized = std::clamp(WrapTime(time) / safeDuration, 0.0f, 1.0f);
    if (sync_.empty() || sync_.size() != other.sync_.size()) {
        // Jatuh kembali ke waktu ternormalisasi. Bukan diam-diam salah: hasilnya
        // sama persis dengan tidak memakai sinkronisasi, yang memang keadaan
        // yang benar ketika salah satu klip belum ditandai.
        return normalized * std::max(other.duration, 1e-6f);
    }

    // Selang antar-penanda diperlakukan melingkar: penanda terakhir menyambung
    // ke penanda pertama lewat ujung klip. Tanpa itu, bagian sesudah penanda
    // terakhir tidak punya pasangan sama sekali — dan pada klip berjalan itu
    // justru bagian yang menyambung ke langkah berikutnya.
    const std::size_t count = sync_.size();
    const float here = WrapTime(time);
    std::size_t index = count - 1;
    for (std::size_t i = 0; i < count; ++i) {
        if (here < sync_[i].time) {
            index = i == 0 ? count - 1 : i - 1;
            break;
        }
    }
    const float start = sync_[index].time;
    const float end = index + 1 < count ? sync_[index + 1].time : sync_[0].time + duration;
    const float span = end - start;
    const float phase = span > 0.0f ? (here >= start ? here - start : here + duration - start) / span
                                    : 0.0f;

    const float otherStart = other.sync_[index].time;
    const float otherEnd = index + 1 < count ? other.sync_[index + 1].time
                                             : other.sync_[0].time + other.duration;
    return other.WrapTime(otherStart + (otherEnd - otherStart) * std::clamp(phase, 0.0f, 1.0f));
}

float Clip::WrapTime(float time) const {
    const float safeDuration = std::max(duration, 1e-6f);
    if (!looping) {
        return std::clamp(time, 0.0f, safeDuration);
    }
    float wrapped = std::fmod(time, safeDuration);
    if (wrapped < 0.0f) {
        wrapped += safeDuration;
    }
    return wrapped;
}

// --- binding ------------------------------------------------------------------

void ClipBinding::Bind(const Clip& clip, const Skeleton& skeleton) {
    static const RetargetMap kIdentity;
    Bind(clip, skeleton, kIdentity);
}

void ClipBinding::Bind(const Clip& clip, const Skeleton& skeleton, const RetargetMap& retarget) {
    boneForTrack_.assign(static_cast<std::size_t>(clip.TrackCount()), -1);
    unresolved_.clear();
    for (int i = 0; i < clip.TrackCount(); ++i) {
        const std::string_view target = retarget.Resolve(clip.TrackAt(i).bone);
        const int bone = skeleton.Find(target);
        boneForTrack_[static_cast<std::size_t>(i)] = bone;
        if (bone < 0) {
            unresolved_.push_back(clip.TrackAt(i).bone);
        }
    }
    std::sort(unresolved_.begin(), unresolved_.end());
    unresolved_.erase(std::unique(unresolved_.begin(), unresolved_.end()), unresolved_.end());

    if (clip.rootBone.empty()) {
        rootBone_ = skeleton.BoneCount() > 0 ? 0 : -1;
    } else {
        rootBone_ = skeleton.Find(retarget.Resolve(clip.rootBone));
    }
}

int ClipBinding::BoneFor(int track) const {
    if (track < 0 || track >= static_cast<int>(boneForTrack_.size())) {
        return -1;
    }
    return boneForTrack_[static_cast<std::size_t>(track)];
}

// --- sampling -----------------------------------------------------------------

void SampleClip(const Clip& clip, const ClipBinding& binding, const Skeleton& skeleton,
                float time, Pose& out) {
    out.Reset(skeleton);
    if (skeleton.BoneCount() == 0) {
        return;
    }
    const float at = clip.WrapTime(time);

    // Euler dikumpulkan per bone lebih dulu, baru diubah menjadi kuaternion
    // sekali. Mengubahnya per kanal berarti tiga kali komposisi kuaternion untuk
    // sebuah rotasi yang hanya punya satu jawaban — dan pada rig 100 bone itu
    // 300 komposisi per frame yang tidak menghasilkan apa pun.
    const auto boneCount = static_cast<std::size_t>(skeleton.BoneCount());
    std::vector<Vec3>& euler = out.EulerScratch(boneCount);
    std::vector<uint8_t>& touched = out.TouchedScratch(boneCount);

    for (int i = 0; i < clip.TrackCount(); ++i) {
        const int bone = binding.BoneFor(i);
        if (bone < 0) {
            continue;
        }
        const Track& track = clip.TrackAt(i);
        if (track.curve.Keys().empty()) {
            continue;
        }
        const float value = track.curve.Evaluate(at);
        BoneTransform& local = out.Local(bone);
        const auto slot = static_cast<std::size_t>(bone);
        switch (track.channel) {
            case Channel::TranslationX: local.translation.x = value; break;
            case Channel::TranslationY: local.translation.y = value; break;
            case Channel::TranslationZ: local.translation.z = value; break;
            case Channel::ScaleX: local.scale.x = value; break;
            case Channel::ScaleY: local.scale.y = value; break;
            case Channel::ScaleZ: local.scale.z = value; break;
            case Channel::RotationX:
                euler[slot].x = value;
                touched[slot] |= 1u;
                break;
            case Channel::RotationY:
                euler[slot].y = value;
                touched[slot] |= 2u;
                break;
            case Channel::RotationZ:
                euler[slot].z = value;
                touched[slot] |= 4u;
                break;
        }
    }

    for (std::size_t i = 0; i < boneCount; ++i) {
        if (touched[i] == 0u) {
            continue;
        }
        // Kanal rotasi yang tidak punya track diambil dari bind pose, bukan dari
        // nol — klip yang hanya menganimasikan yaw tidak boleh meluruskan pitch
        // dan roll yang memang bagian dari rig.
        Vec3 angles = euler[i];
        if (touched[i] != 7u) {
            const Vec3 bind = QuatToEuler(skeleton.Bone(static_cast<int>(i)).bind.rotation);
            if ((touched[i] & 1u) == 0u) {
                angles.x = bind.x;
            }
            if ((touched[i] & 2u) == 0u) {
                angles.y = bind.y;
            }
            if ((touched[i] & 4u) == 0u) {
                angles.z = bind.z;
            }
        }
        out.Local(static_cast<int>(i)).rotation = EulerToQuat(angles);
    }

    if (clip.extractRootMotion && binding.RootBone() >= 0) {
        // Root dikembalikan ke identitas: gerakannya sudah diserahkan ke
        // pemanggil lewat `SampleRootMotion`, dan membiarkannya di dua tempat
        // berarti karakter bergerak dua kali secepat yang tertulis di klip.
        BoneTransform& root = out.Local(binding.RootBone());
        root.translation = Vec3(0.0f);
        root.rotation = Quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
}

BoneTransform SampleRootMotion(const Clip& clip, const ClipBinding& binding,
                               const Skeleton& skeleton, float fromTime, float toTime) {
    BoneTransform delta;
    if (!clip.extractRootMotion || binding.RootBone() < 0) {
        return delta;
    }

    // Menerima waktu yang **sudah** dibungkus, dan bukan membungkusnya sendiri.
    // Kalau ia membungkus sendiri, tidak ada cara meminta nilai di ujung klip:
    // `WrapTime(duration)` melipatnya kembali ke nol, jadi "posisi root di akhir
    // lintasan" akan terbaca sebagai "posisi root di awal lintasan" — dan
    // selisih untuk lintasan yang membungkus menjadi sepanjang seluruh klip,
    // dengan tanda terbalik.
    const auto sampleRootAt = [&](float at) {
        BoneTransform root = skeleton.Bone(binding.RootBone()).bind;
        Vec3 euler = QuatToEuler(root.rotation);
        for (int i = 0; i < clip.TrackCount(); ++i) {
            if (binding.BoneFor(i) != binding.RootBone()) {
                continue;
            }
            const Track& track = clip.TrackAt(i);
            if (track.curve.Keys().empty()) {
                continue;
            }
            const float value = track.curve.Evaluate(at);
            switch (track.channel) {
                case Channel::TranslationX: root.translation.x = value; break;
                case Channel::TranslationY: root.translation.y = value; break;
                case Channel::TranslationZ: root.translation.z = value; break;
                case Channel::RotationX: euler.x = value; break;
                case Channel::RotationY: euler.y = value; break;
                case Channel::RotationZ: euler.z = value; break;
                default: break;
            }
        }
        root.rotation = EulerToQuat(euler);
        return root;
    };

    const BoneTransform from = sampleRootAt(clip.WrapTime(fromTime));
    if (clip.looping && toTime < fromTime) {
        // Lintasan yang membungkus dijumlahkan sebagai dua ruas: dari `from`
        // sampai ujung klip, lalu dari awal sampai `to`. Selisih langsung akan
        // melemparkan karakter mundur sepanjang seluruh klip tepat pada frame
        // tempat ia berulang.
        const BoneTransform end = sampleRootAt(clip.duration);
        const BoneTransform begin = sampleRootAt(0.0f);
        const BoneTransform to = sampleRootAt(clip.WrapTime(toTime));
        delta.translation = (end.translation - from.translation) +
                            (to.translation - begin.translation);
        delta.rotation = (to.rotation * glm::inverse(begin.rotation)) *
                         (end.rotation * glm::inverse(from.rotation));
        return delta;
    }

    const BoneTransform to = sampleRootAt(clip.WrapTime(toTime));
    delta.translation = to.translation - from.translation;
    delta.rotation = to.rotation * glm::inverse(from.rotation);
    return delta;
}

}  // namespace sim::animation
