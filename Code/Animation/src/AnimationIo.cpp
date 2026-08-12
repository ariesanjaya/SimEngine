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

    // Track rotasi kuaternion, jalur klip yang diimpor. Deret angka datar dengan
    // alasan yang sama seperti di atas — lima angka per kunci, dan klip impor
    // punya satu kunci per bone per frame.
    Json rotations = Json::array();
    for (const RotationTrack& track : clip.RotationTracks()) {
        Json entry;
        entry["bone"] = track.bone;
        Json keys = Json::array();
        for (const RotationKey& key : track.keys) {
            keys.push_back(key.time);
            // Urutan x, y, z, w — sama dengan tata letak memori glm, dan sama
            // dengan yang ditulis setiap format yang menyimpan kuaternion.
            // Menuliskan w lebih dulu di satu tempat saja menghasilkan rotasi
            // yang hampir benar, dan "hampir" adalah jenis kesalahan yang paling
            // lama tidak ketahuan.
            keys.push_back(key.rotation.x);
            keys.push_back(key.rotation.y);
            keys.push_back(key.rotation.z);
            keys.push_back(key.rotation.w);
        }
        entry["keys"] = std::move(keys);
        rotations.push_back(std::move(entry));
    }
    root["rotationTracks"] = std::move(rotations);

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

    std::vector<RotationTrack> rotations;
    if (const auto it = root.find("rotationTracks"); it != root.end() && it->is_array()) {
        for (const Json& entry : *it) {
            if (!entry.is_object()) {
                continue;
            }
            RotationTrack track;
            track.bone = entry.value("bone", std::string{});
            if (const auto keys = entry.find("keys");
                keys != entry.end() && keys->is_array() && keys->size() % 5 == 0) {
                for (std::size_t i = 0; i + 4 < keys->size(); i += 5) {
                    RotationKey key;
                    key.time = (*keys)[i].get<float>();
                    key.rotation = Quat((*keys)[i + 4].get<float>(), (*keys)[i + 1].get<float>(),
                                        (*keys)[i + 2].get<float>(), (*keys)[i + 3].get<float>());
                    track.AddKey(key);
                }
            }
            if (!track.bone.empty()) {
                rotations.push_back(std::move(track));
            }
        }
    }
    clip.SetRotationTracks(rotations);

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

// --- graph --------------------------------------------------------------------

namespace {

Json WriteMotion(const Motion& motion) {
    Json entry;
    entry["kind"] = ToString(motion.kind);
    entry["clip"] = motion.clip.guid.ToString();
    entry["parameterX"] = motion.parameterX;
    entry["parameterY"] = motion.parameterY;
    entry["speed"] = motion.speed;
    entry["syncPhase"] = motion.syncPhase;
    Json children = Json::array();
    for (const MotionChild& child : motion.children) {
        Json node;
        node["clip"] = child.clip.guid.ToString();
        node["position"] = Json::array({child.position.x, child.position.y});
        node["speed"] = child.speed;
        children.push_back(std::move(node));
    }
    entry["children"] = std::move(children);
    return entry;
}

Motion ReadMotion(const Json& entry) {
    Motion motion;
    if (!entry.is_object()) {
        return motion;
    }
    motion.kind = MotionKindFromString(entry.value("kind", std::string{}));
    motion.clip = AssetRef{Uuid::Parse(entry.value("clip", std::string{}))};
    motion.parameterX = entry.value("parameterX", std::string{});
    motion.parameterY = entry.value("parameterY", std::string{});
    motion.speed = entry.value("speed", 1.0f);
    motion.syncPhase = entry.value("syncPhase", true);
    if (const auto children = entry.find("children");
        children != entry.end() && children->is_array()) {
        for (const Json& node : *children) {
            if (!node.is_object()) {
                continue;
            }
            MotionChild child;
            child.clip = AssetRef{Uuid::Parse(node.value("clip", std::string{}))};
            const Vec3 position = ReadVec3(node.value("position", Json{}), Vec3(0.0f));
            child.position = Vec2(position.x, position.y);
            if (const auto at = node.find("position");
                at != node.end() && at->is_array() && at->size() == 2) {
                child.position = Vec2((*at)[0].get<float>(), (*at)[1].get<float>());
            }
            child.speed = node.value("speed", 1.0f);
            motion.children.push_back(std::move(child));
        }
    }
    return motion;
}

}  // namespace

std::string SaveGraphToString(const AnimationGraph& graph) {
    Json root;
    root["version"] = kGraphSchemaVersion;
    root["name"] = graph.name;
    root["skeleton"] = graph.skeleton.guid.ToString();

    Json parameters = Json::array();
    for (const Parameter& parameter : graph.parameters.All()) {
        parameters.push_back(Json{{"name", parameter.name},
                                  {"type", ToString(parameter.type)},
                                  {"value", parameter.value}});
    }
    root["parameters"] = std::move(parameters);

    Json layers = Json::array();
    for (const Layer& layer : graph.Layers()) {
        Json entry;
        entry["name"] = layer.name;
        entry["weight"] = layer.weight;
        entry["additive"] = layer.additive;
        entry["maskRootBone"] = layer.maskRootBone;
        entry["defaultState"] = layer.defaultState;

        Json states = Json::array();
        for (const State& state : layer.states) {
            Json node;
            node["name"] = state.name;
            node["canvas"] = Json::array({state.canvas.x, state.canvas.y});
            node["motion"] = WriteMotion(state.motion);
            states.push_back(std::move(node));
        }
        entry["states"] = std::move(states);

        Json transitions = Json::array();
        for (const Transition& transition : layer.transitions) {
            Json node;
            node["from"] = transition.from;
            node["to"] = transition.to;
            node["duration"] = transition.duration;
            node["hasExitTime"] = transition.hasExitTime;
            node["exitTime"] = transition.exitTime;
            Json conditions = Json::array();
            for (const Condition& condition : transition.conditions) {
                conditions.push_back(Json{{"parameter", condition.parameter},
                                          {"comparison", ToString(condition.comparison)},
                                          {"value", condition.value}});
            }
            node["conditions"] = std::move(conditions);
            transitions.push_back(std::move(node));
        }
        entry["transitions"] = std::move(transitions);
        layers.push_back(std::move(entry));
    }
    root["layers"] = std::move(layers);
    return root.dump(2) + "\n";
}

AnimationIoResult LoadGraphFromString(AnimationGraph& graph, const std::string& text) {
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

    graph = AnimationGraph{};
    result.sourceVersion = root.value("version", kGraphSchemaVersion);
    graph.name = root.value("name", std::string{});
    graph.skeleton = AssetRef{Uuid::Parse(root.value("skeleton", std::string{}))};

    std::vector<Parameter> parameters;
    if (const auto it = root.find("parameters"); it != root.end() && it->is_array()) {
        for (const Json& entry : *it) {
            if (!entry.is_object()) {
                continue;
            }
            Parameter parameter;
            parameter.name = entry.value("name", std::string{});
            parameter.type = ParameterTypeFromString(entry.value("type", std::string{}));
            parameter.value = entry.value("value", 0.0f);
            if (!parameter.name.empty()) {
                parameters.push_back(std::move(parameter));
            }
        }
    }
    graph.parameters.SetAll(parameters);

    std::vector<Layer> layers;
    if (const auto it = root.find("layers"); it != root.end() && it->is_array()) {
        for (const Json& entry : *it) {
            if (!entry.is_object()) {
                continue;
            }
            Layer layer;
            layer.name = entry.value("name", std::string{"Base"});
            layer.weight = entry.value("weight", 1.0f);
            layer.additive = entry.value("additive", false);
            layer.maskRootBone = entry.value("maskRootBone", std::string{});
            layer.defaultState = entry.value("defaultState", 0);

            if (const auto states = entry.find("states");
                states != entry.end() && states->is_array()) {
                for (const Json& node : *states) {
                    if (!node.is_object()) {
                        continue;
                    }
                    State state;
                    state.name = node.value("name", std::string{});
                    if (const auto canvas = node.find("canvas");
                        canvas != node.end() && canvas->is_array() && canvas->size() == 2) {
                        state.canvas = Vec2((*canvas)[0].get<float>(), (*canvas)[1].get<float>());
                    }
                    state.motion = ReadMotion(node.value("motion", Json{}));
                    layer.states.push_back(std::move(state));
                }
            }
            if (const auto transitions = entry.find("transitions");
                transitions != entry.end() && transitions->is_array()) {
                for (const Json& node : *transitions) {
                    if (!node.is_object()) {
                        continue;
                    }
                    Transition transition;
                    transition.from = node.value("from", -1);
                    transition.to = node.value("to", 0);
                    transition.duration = node.value("duration", 0.2f);
                    transition.hasExitTime = node.value("hasExitTime", false);
                    transition.exitTime = node.value("exitTime", 1.0f);
                    if (const auto conditions = node.find("conditions");
                        conditions != node.end() && conditions->is_array()) {
                        for (const Json& item : *conditions) {
                            if (!item.is_object()) {
                                continue;
                            }
                            Condition condition;
                            condition.parameter = item.value("parameter", std::string{});
                            condition.comparison =
                                ComparisonFromString(item.value("comparison", std::string{}));
                            condition.value = item.value("value", 0.0f);
                            transition.conditions.push_back(std::move(condition));
                        }
                    }
                    layer.transitions.push_back(std::move(transition));
                }
            }
            layers.push_back(std::move(layer));
        }
    }
    graph.SetLayers(layers);
    result.ok = true;
    return result;
}

AnimationIoResult SaveGraph(const AnimationGraph& graph, const std::filesystem::path& path) {
    AnimationIoResult result;
    result.ok = WriteFile(path, SaveGraphToString(graph), result.error);
    return result;
}

AnimationIoResult LoadGraph(AnimationGraph& graph, const std::filesystem::path& path) {
    std::string text;
    AnimationIoResult result = ReadFile(path, text);
    if (!result.ok) {
        return result;
    }
    return LoadGraphFromString(graph, text);
}

}  // namespace sim::animation
