#include "Sim/Particle/ParticleEffect.h"

#include "CurveIo.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>

namespace sim::particle {
namespace {

using Json = nlohmann::ordered_json;

template <typename Enum, std::size_t N>
const char* NameOf(const std::array<std::pair<Enum, const char*>, N>& table, Enum value,
                   const char* fallback) {
    for (const auto& [key, name] : table) {
        if (key == value) {
            return name;
        }
    }
    return fallback;
}

template <typename Enum, std::size_t N>
Enum ValueOf(const std::array<std::pair<Enum, const char*>, N>& table, std::string_view text,
             Enum fallback) {
    for (const auto& [key, name] : table) {
        if (text == name) {
            return key;
        }
    }
    return fallback;
}

constexpr std::array<std::pair<EmitterShape, const char*>, 4> kShapeNames{{
    {EmitterShape::Point, "point"},
    {EmitterShape::Sphere, "sphere"},
    {EmitterShape::Box, "box"},
    {EmitterShape::Cone, "cone"},
}};

constexpr std::array<std::pair<RenderMode, const char*>, 3> kRenderNames{{
    {RenderMode::Billboard, "billboard"},
    {RenderMode::Stretched, "stretched"},
    {RenderMode::Mesh, "mesh"},
}};

Json WriteVec3(const Vec3& v) {
    return Json::array({v.x, v.y, v.z});
}

Vec3 ReadVec3(const Json& entry, const Vec3& fallback) {
    if (!entry.is_array() || entry.size() != 3) {
        return fallback;
    }
    return Vec3(entry[0].get<float>(), entry[1].get<float>(), entry[2].get<float>());
}

}  // namespace

const char* ToString(EmitterShape shape) {
    return NameOf(kShapeNames, shape, "sphere");
}
EmitterShape ShapeFromString(std::string_view text) {
    return ValueOf(kShapeNames, text, EmitterShape::Sphere);
}
const char* ToString(RenderMode mode) {
    return NameOf(kRenderNames, mode, "billboard");
}
RenderMode RenderModeFromString(std::string_view text) {
    return ValueOf(kRenderNames, text, RenderMode::Billboard);
}

const char* ToString(ModuleKind kind) {
    switch (kind) {
        case ModuleKind::Spawn: return "Spawn";
        case ModuleKind::Shape: return "Shape";
        case ModuleKind::Initial: return "Initial";
        case ModuleKind::OverLifetime: return "Over Lifetime";
        case ModuleKind::Force: return "Force";
        case ModuleKind::Collision: return "Collision";
        case ModuleKind::Render: return "Render";
    }
    return "Unknown";
}

bool ParticleEffect::IsEnabled(ModuleKind kind) const {
    switch (kind) {
        case ModuleKind::Spawn: return spawn.enabled;
        case ModuleKind::Shape: return shape.enabled;
        case ModuleKind::Initial: return initial.enabled;
        case ModuleKind::OverLifetime: return overLifetime.enabled;
        case ModuleKind::Force: return force.enabled;
        case ModuleKind::Collision: return collision.enabled;
        case ModuleKind::Render: return render.enabled;
    }
    return false;
}

void ParticleEffect::SetEnabled(ModuleKind kind, bool value) {
    switch (kind) {
        case ModuleKind::Spawn: spawn.enabled = value; return;
        case ModuleKind::Shape: shape.enabled = value; return;
        case ModuleKind::Initial: initial.enabled = value; return;
        case ModuleKind::OverLifetime: overLifetime.enabled = value; return;
        case ModuleKind::Force: force.enabled = value; return;
        case ModuleKind::Collision: collision.enabled = value; return;
        case ModuleKind::Render: render.enabled = value; return;
    }
}

std::string SaveEffectToString(const ParticleEffect& effect) {
    Json root;
    root["version"] = kEffectSchemaVersion;
    root["name"] = effect.name;
    root["seed"] = effect.seed;
    root["maxParticles"] = effect.maxParticles;

    // Setiap modul selalu ditulis lengkap, termasuk yang dimatikan. Itu yang
    // membuat mematikan modul tidak menghapus datanya — kriteria terima E7.2
    // nomor 3 — dan yang membuat menyalakannya kembali mengembalikan efek yang
    // sama persis, bukan efek dengan nilai bawaan.
    Json spawn;
    spawn["enabled"] = effect.spawn.enabled;
    spawn["rate"] = effect.spawn.rate;
    spawn["burstCount"] = effect.spawn.burstCount;
    spawn["burstTime"] = effect.spawn.burstTime;
    spawn["duration"] = effect.spawn.duration;
    spawn["looping"] = effect.spawn.looping;
    root["spawn"] = std::move(spawn);

    Json shape;
    shape["enabled"] = effect.shape.enabled;
    shape["shape"] = ToString(effect.shape.shape);
    shape["size"] = WriteVec3(effect.shape.size);
    shape["coneAngle"] = effect.shape.coneAngle;
    shape["emitFromShell"] = effect.shape.emitFromShell;
    root["shape"] = std::move(shape);

    Json initial;
    initial["enabled"] = effect.initial.enabled;
    initial["velocity"] = WriteVec3(effect.initial.velocity);
    initial["velocityJitter"] = WriteVec3(effect.initial.velocityJitter);
    initial["size"] = effect.initial.size;
    initial["sizeJitter"] = effect.initial.sizeJitter;
    initial["color"] = WriteVec3(effect.initial.color);
    initial["rotation"] = effect.initial.rotation;
    initial["rotationJitter"] = effect.initial.rotationJitter;
    initial["lifetime"] = effect.initial.lifetime;
    initial["lifetimeJitter"] = effect.initial.lifetimeJitter;
    root["initial"] = std::move(initial);

    Json life;
    life["enabled"] = effect.overLifetime.enabled;
    life["size"] = WriteCurve(effect.overLifetime.sizeOverLife);
    life["velocityScale"] = WriteCurve(effect.overLifetime.velocityScale);
    life["rotationRate"] = WriteCurve(effect.overLifetime.rotationRate);
    life["color"] = WriteGradient(effect.overLifetime.colorOverLife);
    root["overLifetime"] = std::move(life);

    Json force;
    force["enabled"] = effect.force.enabled;
    force["gravity"] = WriteVec3(effect.force.gravity);
    force["drag"] = effect.force.drag;
    force["vortexAxis"] = WriteVec3(effect.force.vortexAxis);
    force["vortexStrength"] = effect.force.vortexStrength;
    force["attractorPosition"] = WriteVec3(effect.force.attractorPosition);
    force["attractorStrength"] = effect.force.attractorStrength;
    force["noiseStrength"] = effect.force.noiseStrength;
    force["noiseFrequency"] = effect.force.noiseFrequency;
    root["force"] = std::move(force);

    Json collision;
    collision["enabled"] = effect.collision.enabled;
    collision["planeHeight"] = effect.collision.planeHeight;
    collision["bounce"] = effect.collision.bounce;
    collision["friction"] = effect.collision.friction;
    collision["killOnContact"] = effect.collision.killOnContact;
    root["collision"] = std::move(collision);

    Json render;
    render["enabled"] = effect.render.enabled;
    render["mode"] = ToString(effect.render.mode);
    render["material"] = effect.render.material.guid.ToString();
    render["mesh"] = effect.render.mesh.guid.ToString();
    render["stretchScale"] = effect.render.stretchScale;
    render["sortByDistance"] = effect.render.sortByDistance;
    root["render"] = std::move(render);

    return root.dump(2) + "\n";
}

EffectIoResult SaveEffectToFile(const ParticleEffect& effect,
                                const std::filesystem::path& path) {
    EffectIoResult result;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        result.error = "cannot open " + path.string();
        return result;
    }
    stream << SaveEffectToString(effect);
    result.ok = static_cast<bool>(stream);
    if (!result.ok) {
        result.error = "write failed: " + path.string();
    }
    return result;
}

EffectIoResult LoadEffectFromString(ParticleEffect& effect, const std::string& text) {
    EffectIoResult result;
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

    effect = ParticleEffect{};
    result.sourceVersion = root.value("version", kEffectSchemaVersion);
    effect.name = root.value("name", std::string{});
    effect.seed = root.value("seed", 12345u);
    effect.maxParticles = root.value("maxParticles", 10000);

    if (const auto it = root.find("spawn"); it != root.end() && it->is_object()) {
        effect.spawn.enabled = it->value("enabled", true);
        effect.spawn.rate = it->value("rate", 20.0f);
        effect.spawn.burstCount = it->value("burstCount", 0);
        effect.spawn.burstTime = it->value("burstTime", 0.0f);
        effect.spawn.duration = it->value("duration", 0.0f);
        effect.spawn.looping = it->value("looping", true);
    }
    if (const auto it = root.find("shape"); it != root.end() && it->is_object()) {
        effect.shape.enabled = it->value("enabled", true);
        effect.shape.shape = ShapeFromString(it->value("shape", std::string("sphere")));
        effect.shape.size = ReadVec3(it->value("size", Json{}), effect.shape.size);
        effect.shape.coneAngle = it->value("coneAngle", 0.436f);
        effect.shape.emitFromShell = it->value("emitFromShell", false);
    }
    if (const auto it = root.find("initial"); it != root.end() && it->is_object()) {
        effect.initial.enabled = it->value("enabled", true);
        effect.initial.velocity = ReadVec3(it->value("velocity", Json{}), effect.initial.velocity);
        effect.initial.velocityJitter =
            ReadVec3(it->value("velocityJitter", Json{}), effect.initial.velocityJitter);
        effect.initial.size = it->value("size", 0.2f);
        effect.initial.sizeJitter = it->value("sizeJitter", 0.05f);
        effect.initial.color = ReadVec3(it->value("color", Json{}), effect.initial.color);
        effect.initial.rotation = it->value("rotation", 0.0f);
        effect.initial.rotationJitter = it->value("rotationJitter", 0.0f);
        effect.initial.lifetime = it->value("lifetime", 2.0f);
        effect.initial.lifetimeJitter = it->value("lifetimeJitter", 0.5f);
    }
    if (const auto it = root.find("overLifetime"); it != root.end() && it->is_object()) {
        effect.overLifetime.enabled = it->value("enabled", true);
        if (const auto curve = it->find("size"); curve != it->end()) {
            ReadCurve(*curve, effect.overLifetime.sizeOverLife);
        }
        if (const auto curve = it->find("velocityScale"); curve != it->end()) {
            ReadCurve(*curve, effect.overLifetime.velocityScale);
        }
        if (const auto curve = it->find("rotationRate"); curve != it->end()) {
            ReadCurve(*curve, effect.overLifetime.rotationRate);
        }
        if (const auto gradient = it->find("color"); gradient != it->end()) {
            ReadGradient(*gradient, effect.overLifetime.colorOverLife);
        }
    }
    if (const auto it = root.find("force"); it != root.end() && it->is_object()) {
        effect.force.enabled = it->value("enabled", true);
        effect.force.gravity = ReadVec3(it->value("gravity", Json{}), effect.force.gravity);
        effect.force.drag = it->value("drag", 0.0f);
        effect.force.vortexAxis = ReadVec3(it->value("vortexAxis", Json{}),
                                           effect.force.vortexAxis);
        effect.force.vortexStrength = it->value("vortexStrength", 0.0f);
        effect.force.attractorPosition =
            ReadVec3(it->value("attractorPosition", Json{}), effect.force.attractorPosition);
        effect.force.attractorStrength = it->value("attractorStrength", 0.0f);
        effect.force.noiseStrength = it->value("noiseStrength", 0.0f);
        effect.force.noiseFrequency = it->value("noiseFrequency", 1.0f);
    }
    if (const auto it = root.find("collision"); it != root.end() && it->is_object()) {
        effect.collision.enabled = it->value("enabled", true);
        effect.collision.planeHeight = it->value("planeHeight", 0.0f);
        effect.collision.bounce = it->value("bounce", 0.4f);
        effect.collision.friction = it->value("friction", 0.2f);
        effect.collision.killOnContact = it->value("killOnContact", false);
    }
    if (const auto it = root.find("render"); it != root.end() && it->is_object()) {
        effect.render.enabled = it->value("enabled", true);
        effect.render.mode = RenderModeFromString(it->value("mode", std::string("billboard")));
        effect.render.material = AssetRef{Uuid::Parse(it->value("material", std::string{}))};
        effect.render.mesh = AssetRef{Uuid::Parse(it->value("mesh", std::string{}))};
        effect.render.stretchScale = it->value("stretchScale", 1.0f);
        effect.render.sortByDistance = it->value("sortByDistance", true);
    }

    result.ok = true;
    return result;
}

EffectIoResult LoadEffectFromFile(ParticleEffect& effect, const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        EffectIoResult result;
        result.error = "cannot open " + path.string();
        return result;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return LoadEffectFromString(effect, buffer.str());
}

}  // namespace sim::particle
