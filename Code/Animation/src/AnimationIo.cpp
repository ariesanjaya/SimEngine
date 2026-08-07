#include "Sim/Animation/AnimationIo.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace sim::animation {
namespace {

using Json = nlohmann::ordered_json;

bool WriteFile(const std::filesystem::path& path, const std::string& text, std::string& error) {
    std::error_code code;
    std::filesystem::create_directories(path.parent_path(), code);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "cannot open " + path.string();
        return false;
    }
    stream << text;
    if (!stream) {
        error = "write failed: " + path.string();
        return false;
    }
    return true;
}

AnimationIoResult ReadFile(const std::filesystem::path& path, std::string& text) {
    AnimationIoResult result;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        result.error = "cannot open " + path.string();
        return result;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    text = buffer.str();
    result.ok = true;
    return result;
}

Vec3 ReadVec3(const Json& node, const Vec3& fallback) {
    if (!node.is_array() || node.size() != 3) {
        return fallback;
    }
    return Vec3(node[0].get<float>(), node[1].get<float>(), node[2].get<float>());
}

Json WriteRetarget(const RetargetMap& retarget) {
    Json array = Json::array();
    for (const auto& [source, target] : retarget.Entries()) {
        array.push_back(Json::array({source, target}));
    }
    return array;
}

void ReadRetarget(const Json& node, RetargetMap& retarget) {
    retarget.Clear();
    if (!node.is_array()) {
        return;
    }
    for (const Json& entry : node) {
        if (entry.is_array() && entry.size() == 2) {
            retarget.Set(entry[0].get<std::string>(), entry[1].get<std::string>());
        }
    }
}

}  // namespace

// --- rangka -------------------------------------------------------------------

std::string SaveSkeletonToString(const SkeletonDocument& document, const Skeleton& skeleton) {
    Json root;
    root["version"] = kSkeletonSchemaVersion;
    root["name"] = document.name;

    Json bones = Json::array();
    for (const Bone& bone : skeleton.Bones()) {
        Json entry;
        entry["name"] = bone.name;
        entry["parent"] = bone.parent;
        entry["translation"] =
            Json::array({bone.bind.translation.x, bone.bind.translation.y, bone.bind.translation.z});
        // Kuaternion ditulis apa adanya, bukan sebagai sudut Euler. Bind pose
        // bukan sesuatu yang disunting sebagai kurva, jadi tidak ada alasan
        // membayar bolak-balik Euler yang kehilangan presisi di dekat gimbal
        // lock — dan rangka yang bind pose-nya bergeser sedikit tiap kali
        // disimpan adalah rangka yang tidak bisa di-diff.
        entry["rotation"] = Json::array({bone.bind.rotation.w, bone.bind.rotation.x,
                                         bone.bind.rotation.y, bone.bind.rotation.z});
        entry["scale"] = Json::array({bone.bind.scale.x, bone.bind.scale.y, bone.bind.scale.z});
        bones.push_back(std::move(entry));
    }
    root["bones"] = std::move(bones);
    root["retarget"] = WriteRetarget(document.retarget);
    return root.dump(2) + "\n";
}

AnimationIoResult LoadSkeletonFromString(SkeletonDocument& document, Skeleton& skeleton,
                                         const std::string& text) {
    AnimationIoResult result;
    Json root;
    try {
        root = Json::parse(text);
    } catch (const nlohmann::json::exception& error) {
        result.error = error.what();
        return result;
    }
    if (!root.is_object()) {
        result.error = "root is not an object";
        return result;
    }

    document = SkeletonDocument{};
    result.sourceVersion = root.value("version", kSkeletonSchemaVersion);
    document.name = root.value("name", std::string{});

    std::vector<Bone> bones;
    if (const auto it = root.find("bones"); it != root.end() && it->is_array()) {
        for (const Json& entry : *it) {
            if (!entry.is_object()) {
                continue;
            }
            Bone bone;
            bone.name = entry.value("name", std::string{});
            bone.parent = entry.value("parent", -1);
            bone.bind.translation = ReadVec3(entry.value("translation", Json{}), Vec3(0.0f));
            if (const auto rotation = entry.find("rotation");
                rotation != entry.end() && rotation->is_array() && rotation->size() == 4) {
                bone.bind.rotation = Quat((*rotation)[0].get<float>(), (*rotation)[1].get<float>(),
                                          (*rotation)[2].get<float>(), (*rotation)[3].get<float>());
            }
            bone.bind.scale = ReadVec3(entry.value("scale", Json{}), Vec3(1.0f));
            bones.push_back(std::move(bone));
        }
    }
    if (!skeleton.SetBones(bones)) {
        // Rangka yang melanggar urutan topologis atau punya nama kembar ditolak
        // seluruhnya. Memuat sebagiannya berarti rig yang terlihat hampir benar
        // dan menganimasikan tulang yang salah.
        result.error = "bone list is not in topological order, or has empty/duplicate names";
        return result;
    }
    ReadRetarget(root.value("retarget", Json{}), document.retarget);
    result.ok = true;
    return result;
}

AnimationIoResult SaveSkeleton(const Skeleton& skeleton, const SkeletonDocument& document,
                               const std::filesystem::path& path) {
    AnimationIoResult result;
    result.ok = WriteFile(path, SaveSkeletonToString(document, skeleton), result.error);
    return result;
}

AnimationIoResult LoadSkeleton(Skeleton& skeleton, SkeletonDocument& document,
                               const std::filesystem::path& path) {
    std::string text;
    AnimationIoResult result = ReadFile(path, text);
    if (!result.ok) {
        return result;
    }
    return LoadSkeletonFromString(document, skeleton, text);
}

// --- klip ---------------------------------------------------------------------

std::string SaveClipToString(const ClipDocument& document, const Clip& clip) {
    Json root;
    root["version"] = kClipSchemaVersion;
    root["name"] = clip.name;
    root["skeleton"] = document.skeleton.guid.ToString();
    root["duration"] = clip.duration;
    root["frameRate"] = clip.frameRate;
    root["looping"] = clip.looping;
    root["rootMotion"] = clip.extractRootMotion;
    root["rootBone"] = clip.rootBone;

    Json tracks = Json::array();
    for (const Track& track : clip.Tracks()) {
        Json entry;
        entry["bone"] = track.bone;
        entry["channel"] = ToString(track.channel);
        // Kunci sebagai deret angka datar dengan mode interpolasi di ujungnya.
        // Satu objek per kunci akan mengulang lima nama field pada tiap kunci —
        // dan klip 60 detik pada rig 100 bone punya ratusan ribu kunci, jadi
        // nama yang diulang itu berpuluh megabyte yang tidak membawa informasi.
        Json keys = Json::array();
        for (const CurveKey& key : track.curve.Keys()) {
            keys.push_back(key.time);
            keys.push_back(key.value);
            keys.push_back(key.inTangent);
            keys.push_back(key.outTangent);
            keys.push_back(sim::ToString(key.interpolation));
        }
        entry["keys"] = std::move(keys);
        tracks.push_back(std::move(entry));
    }
    root["tracks"] = std::move(tracks);

    Json events = Json::array();
    for (const Event& event : clip.Events()) {
        events.push_back(Json{{"time", event.time}, {"name", event.name}});
    }
    root["events"] = std::move(events);

    Json sync = Json::array();
    for (const SyncMarker& marker : clip.SyncMarkers()) {
        sync.push_back(Json{{"time", marker.time}, {"name", marker.name}});
    }
    root["sync"] = std::move(sync);
    return root.dump(2) + "\n";
}

AnimationIoResult LoadClipFromString(ClipDocument& document, Clip& clip, const std::string& text) {
    AnimationIoResult result;
    Json root;
    try {
        root = Json::parse(text);
    } catch (const nlohmann::json::exception& error) {
        result.error = error.what();
        return result;
    }
    if (!root.is_object()) {
        result.error = "root is not an object";
        return result;
    }

    document = ClipDocument{};
    clip = Clip{};
    result.sourceVersion = root.value("version", kClipSchemaVersion);
    clip.name = root.value("name", std::string{});
    document.skeleton = AssetRef{Uuid::Parse(root.value("skeleton", std::string{}))};
    clip.duration = std::max(root.value("duration", 1.0f), 1e-6f);
    clip.frameRate = std::max(root.value("frameRate", 30.0f), 1.0f);
    clip.looping = root.value("looping", true);
    clip.extractRootMotion = root.value("rootMotion", false);
    clip.rootBone = root.value("rootBone", std::string{});

    std::vector<Track> tracks;
    if (const auto it = root.find("tracks"); it != root.end() && it->is_array()) {
        for (const Json& entry : *it) {
            if (!entry.is_object()) {
                continue;
            }
            Track track;
            track.bone = entry.value("bone", std::string{});
            track.channel = ChannelFromString(entry.value("channel", std::string{}));
            if (const auto keys = entry.find("keys");
                keys != entry.end() && keys->is_array() && keys->size() % 5 == 0) {
                for (std::size_t i = 0; i + 4 < keys->size(); i += 5) {
                    CurveKey key;
                    key.time = (*keys)[i].get<float>();
                    key.value = (*keys)[i + 1].get<float>();
                    key.inTangent = (*keys)[i + 2].get<float>();
                    key.outTangent = (*keys)[i + 3].get<float>();
                    key.interpolation =
                        sim::InterpolationFromString((*keys)[i + 4].get<std::string>());
                    track.curve.AddKey(key);
                }
            }
            if (!track.bone.empty()) {
                tracks.push_back(std::move(track));
            }
        }
    }
    clip.SetTracks(tracks);

    std::vector<Event> events;
    if (const auto it = root.find("events"); it != root.end() && it->is_array()) {
        for (const Json& entry : *it) {
            if (entry.is_object()) {
                events.push_back(Event{entry.value("time", 0.0f),
                                       entry.value("name", std::string{})});
            }
        }
    }
    clip.SetEvents(events);

    std::vector<SyncMarker> markers;
    if (const auto it = root.find("sync"); it != root.end() && it->is_array()) {
        for (const Json& entry : *it) {
            if (entry.is_object()) {
                markers.push_back(SyncMarker{entry.value("time", 0.0f),
                                             entry.value("name", std::string{})});
            }
        }
    }
    clip.SetSyncMarkers(markers);

    result.ok = true;
    return result;
}

AnimationIoResult SaveClip(const Clip& clip, const ClipDocument& document,
                           const std::filesystem::path& path) {
    AnimationIoResult result;
    result.ok = WriteFile(path, SaveClipToString(document, clip), result.error);
    return result;
}

AnimationIoResult LoadClip(Clip& clip, ClipDocument& document,
                           const std::filesystem::path& path) {
    std::string text;
    AnimationIoResult result = ReadFile(path, text);
    if (!result.ok) {
        return result;
    }
    return LoadClipFromString(document, clip, text);
}

}  // namespace sim::animation
