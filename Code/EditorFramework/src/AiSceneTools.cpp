#include "Sim/AIBridge/ResourceRegistry.h"
#include "Sim/AIBridge/ToolRegistry.h"
#include "Sim/Editor/AiTools.h"
#include "Sim/Editor/EditorApp.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/Selection.h"
#include "Sim/Reflect/TypeRegistry.h"
#include "Sim/Scene/ComponentRegistry.h"
#include "Sim/Scene/Components.h"
#include "Sim/Scene/Serialization.h"
#include "Sim/Scene/World.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <memory>
#include <vector>

namespace sim::editor {
namespace {

using nlohmann::json;

json ParseArguments(std::string_view argumentsJson) {
    json arguments = json::parse(argumentsJson, nullptr, /*allow_exceptions=*/false);
    return arguments.is_object() ? arguments : json::object();
}

ai::ToolResult Text(std::string value, bool isError = false) {
    ai::ToolResult result;
    result.isError = isError;
    result.text = std::move(value);
    return result;
}

ai::ToolResult Structured(const json& value) { return Text(value.dump(2)); }

const char* KindName(reflect::FieldKind kind) {
    switch (kind) {
        case reflect::FieldKind::Bool: return "bool";
        case reflect::FieldKind::Int: return "int";
        case reflect::FieldKind::UInt: return "uint";
        case reflect::FieldKind::Float: return "float";
        case reflect::FieldKind::Vec2: return "vec2";
        case reflect::FieldKind::Vec3: return "vec3";
        case reflect::FieldKind::Vec4: return "vec4";
        case reflect::FieldKind::Quat: return "quat";
        case reflect::FieldKind::String: return "string";
        case reflect::FieldKind::Uuid: return "uuid";
        case reflect::FieldKind::AssetRef: return "assetRef";
        case reflect::FieldKind::Enum: return "enum";
        case reflect::FieldKind::Struct: return "struct";
        case reflect::FieldKind::Vector: return "array";
    }
    return "unknown";
}

/// Entity yang disebut argumen, atau `kNullEntity`.
///
/// **GUID, bukan indeks.** Indeks entity dipakai ulang begitu sebuah entity
/// dihapus, jadi agen yang menyimpan satu lalu memakainya kembali beberapa
/// panggilan kemudian bisa mengenai objek yang sama sekali lain — tanpa satu pun
/// galat, karena indeksnya memang sah.
scene::Entity ResolveEntity(const scene::World& world, const json& arguments) {
    if (!arguments.contains("entity") || !arguments.at("entity").is_string()) {
        return scene::kNullEntity;
    }
    return world.FindByGuid(Uuid::Parse(arguments.at("entity").get<std::string>()));
}

json DescribeEntity(const scene::World& world, scene::Entity entity) {
    json summary{{"guid", world.GuidOf(entity).ToString()}, {"name", world.NameOf(entity)}};
    const scene::Entity parent = world.ParentOf(entity);
    summary["parent"] =
        parent == scene::kNullEntity ? json(nullptr) : json(world.GuidOf(parent).ToString());
    summary["childCount"] = world.ChildrenOf(entity).size();

    json components = json::array();
    for (const scene::ComponentOps& ops : scene::ComponentRegistry::Get().All()) {
        if (ops.tryGet(const_cast<entt::registry&>(world.Registry()),
                       static_cast<entt::entity>(entity)) != nullptr) {
            components.push_back(ops.type->name);
        }
    }
    summary["components"] = std::move(components);
    return summary;
}

// --- simengine://docs/components ---------------------------------------------

ai::ResourceDefinition ComponentDocs() {
    ai::ResourceDefinition resource;
    resource.uri = "simengine://docs/components";
    resource.name = "Component reference";
    resource.description =
        "Every component type this engine knows, with its fields and their kinds. Read this "
        "before entity.modify — it is the whole vocabulary you can set, and it is generated "
        "from the engine's own reflection, so it is never out of date.";
    resource.mimeType = "application/json";
    // **Reflection adalah data statis, bukan keadaan scene.** Ia tidak berubah
    // selama editor hidup dan tidak disentuh siapa pun saat frame digambar,
    // jadi membacanya dari thread jaringan aman — dan itu membuat agen bisa
    // mengambil kosakatanya bahkan saat editor sedang tertahan dialog modal.
    resource.needsMainThread = false;
    resource.read = []() {
        json components = json::array();
        for (const scene::ComponentOps& ops : scene::ComponentRegistry::Get().All()) {
            json fields = json::array();
            for (const reflect::FieldDesc& field : ops.type->fields) {
                json described{{"name", field.name}, {"kind", KindName(field.kind)}};
                if (field.kind == reflect::FieldKind::Struct) {
                    const reflect::TypeDesc* nested = reflect::TypeRegistry::Get().Find(field.type);
                    described["struct"] = nested != nullptr ? nested->name : "?";
                }
                if (field.kind == reflect::FieldKind::Vector && field.vector != nullptr) {
                    described["element"] = KindName(field.vector->elementKind);
                }
                fields.push_back(std::move(described));
            }
            components.push_back(json{{"name", ops.type->name},
                                      // Yang tidak `addable` tidak bisa
                                      // ditambahkan agen, dan yang tidak
                                      // `removable` tidak bisa dibuang: keduanya
                                      // disebut supaya agen tidak mencobanya.
                                      {"addable", ops.addable},
                                      {"removable", ops.removable},
                                      {"fields", std::move(fields)}});
        }
        return json{{"components", std::move(components)}}.dump(2);
    };
    return resource;
}

// --- scene.describe -----------------------------------------------------------

/// Hierarki sampai kedalaman tertentu. Yang lebih dalam disebut jumlahnya, bukan
/// dipotong diam-diam.
json DescribeSubtree(const scene::World& world, scene::Entity entity, int depth) {
    json node = DescribeEntity(world, entity);
    const std::vector<scene::Entity>& children = world.ChildrenOf(entity);
    if (children.empty()) {
        return node;
    }
    if (depth <= 0) {
        // **Disebut, bukan dihilangkan.** Cabang yang hilang tanpa jejak membuat
        // agen menyimpulkan scene-nya lebih kecil daripada yang sebenarnya, dan
        // kesimpulan itu tidak akan pernah ia periksa lagi.
        node["childrenOmitted"] = children.size();
        return node;
    }
    json list = json::array();
    for (const scene::Entity child : children) {
        list.push_back(DescribeSubtree(world, child, depth - 1));
    }
    node["children"] = std::move(list);
    return node;
}

ai::ToolDefinition SceneDescribe(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "scene.describe";
    tool.description =
        "A summary of the open level: how many entities, the hierarchy down to a depth you "
        "choose, and the world-space bounds of everything in it. Start here when you need to "
        "know what is in the scene — it is a summary, not the whole level.";
    tool.permission = ai::ToolPermission::Read;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        scene::World* world = app.Context().world;
        if (world == nullptr) {
            return Text("No scene is open.", /*isError=*/true);
        }
        const json arguments = ParseArguments(argumentsJson);
        const int depth = std::clamp(arguments.value("depth", 2), 0, 8);

        json roots = json::array();
        Vec3 minimum(0.0f);
        Vec3 maximum(0.0f);
        bool anyPosition = false;
        world->Registry().view<entt::entity>().each([&](entt::entity handle) {
            const scene::Entity entity = static_cast<scene::Entity>(handle);
            if (world->ParentOf(entity) == scene::kNullEntity) {
                roots.push_back(DescribeSubtree(*world, entity, depth));
            }
            // **Titik asal saja, bukan kotak batas mesh.** Yang tersedia murah
            // di sini adalah transform; menyertakan bentuk setiap mesh berarti
            // membaca seluruh geometri, dan agen yang bertanya "seberapa besar
            // scene ini" tidak sedang meminta ketelitian itu. Namanya menyebut
            // apa adanya supaya tidak ada yang mengira sebaliknya.
            const Mat4& matrix = world->WorldMatrix(entity);
            const Vec3 position(matrix[3][0], matrix[3][1], matrix[3][2]);
            minimum = anyPosition ? glm::min(minimum, position) : position;
            maximum = anyPosition ? glm::max(maximum, position) : position;
            anyPosition = true;
        });

        json bounds = nullptr;
        if (anyPosition) {
            bounds = json{{"min", {minimum.x, minimum.y, minimum.z}},
                          {"max", {maximum.x, maximum.y, maximum.z}},
                          {"note", "origins of entity transforms, not mesh extents"}};
        }
        return Structured(json{{"level", app.Context().levelName},
                               {"entityCount", world->Count()},
                               {"depth", depth},
                               {"originBounds", std::move(bounds)},
                               {"roots", std::move(roots)}});
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "properties": {
    "depth": {"type": "integer", "minimum": 0, "maximum": 8,
              "description": "How many levels of hierarchy to include. Default 2. Deeper branches are reported as a count, not dropped."}
  }
})";
    return tool;
}

// --- scene.query --------------------------------------------------------------

ai::ToolDefinition SceneQuery(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "scene.query";
    tool.description =
        "Find entities by name substring, by component, or by distance from a point. Returns "
        "GUIDs and short summaries, paginated. Use this instead of walking the whole hierarchy "
        "when you know what you are looking for.";
    tool.permission = ai::ToolPermission::Read;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        scene::World* world = app.Context().world;
        if (world == nullptr) {
            return Text("No scene is open.", /*isError=*/true);
        }
        const json arguments = ParseArguments(argumentsJson);

        std::string name = arguments.value("name", std::string{});
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const std::string component = arguments.value("component", std::string{});
        const scene::ComponentOps* wanted =
            component.empty() ? nullptr : scene::ComponentRegistry::Get().Find(component);
        if (!component.empty() && wanted == nullptr) {
            return Text("No component type named \"" + component +
                            "\". Read simengine://docs/components for the list.",
                        /*isError=*/true);
        }

        const bool byDistance = arguments.contains("near") && arguments.at("near").is_array() &&
                                arguments.at("near").size() == 3;
        Vec3 origin(0.0f);
        float radius = 0.0f;
        if (byDistance) {
            origin = Vec3(arguments.at("near")[0].get<float>(),
                          arguments.at("near")[1].get<float>(),
                          arguments.at("near")[2].get<float>());
            radius = arguments.value("radius", 10.0f);
        }

        const std::size_t limit =
            static_cast<std::size_t>(std::clamp(arguments.value("limit", 50), 1, 500));
        const std::size_t offset = static_cast<std::size_t>(std::max(arguments.value("offset", 0), 0));

        std::vector<scene::Entity> matched;
        world->Registry().view<entt::entity>().each([&](entt::entity handle) {
            const scene::Entity entity = static_cast<scene::Entity>(handle);
            if (!name.empty()) {
                std::string candidate = world->NameOf(entity);
                std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (candidate.find(name) == std::string::npos) {
                    return;
                }
            }
            if (wanted != nullptr &&
                wanted->tryGet(world->Registry(), handle) == nullptr) {
                return;
            }
            if (byDistance) {
                const Mat4& matrix = world->WorldMatrix(entity);
                const Vec3 position(matrix[3][0], matrix[3][1], matrix[3][2]);
                if (glm::distance(position, origin) > radius) {
                    return;
                }
            }
            matched.push_back(entity);
        });

        json results = json::array();
        for (std::size_t i = offset; i < matched.size() && results.size() < limit; ++i) {
            results.push_back(DescribeEntity(*world, matched[i]));
        }
        // Total disebut terpisah dari yang dikembalikan: agen yang menerima
        // lima puluh hasil tanpa tahu ada tiga ratus akan mengira sudah melihat
        // semuanya.
        return Structured(json{{"total", matched.size()},
                               {"offset", offset},
                               {"returned", results.size()},
                               {"entities", std::move(results)}});
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "properties": {
    "name": {"type": "string", "description": "Case-insensitive substring of the entity name."},
    "component": {"type": "string", "description": "Only entities that have this component, e.g. \"MeshRenderer\"."},
    "near": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3,
             "description": "World-space point to measure from."},
    "radius": {"type": "number", "description": "Used with \"near\". Default 10."},
    "limit": {"type": "integer", "minimum": 1, "maximum": 500, "description": "Default 50."},
    "offset": {"type": "integer", "minimum": 0, "description": "For paging through the total."}
  }
})";
    return tool;
}

// --- entity.get ---------------------------------------------------------------

ai::ToolDefinition EntityGet(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "entity.get";
    tool.description =
        "Every component on one entity, with all its field values. Identify the entity by the "
        "GUID that scene.query and scene.describe return.";
    tool.permission = ai::ToolPermission::Read;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        scene::World* world = app.Context().world;
        if (world == nullptr) {
            return Text("No scene is open.", /*isError=*/true);
        }
        const json arguments = ParseArguments(argumentsJson);
        const scene::Entity entity = ResolveEntity(*world, arguments);
        if (entity == scene::kNullEntity) {
            return Text("No entity with that GUID. Use scene.query to find one.",
                        /*isError=*/true);
        }

        json components = json::object();
        for (const scene::ComponentOps& ops : scene::ComponentRegistry::Get().All()) {
            void* component = ops.tryGet(world->Registry(), static_cast<entt::entity>(entity));
            if (component == nullptr) {
                continue;
            }
            // Rumus yang sama dengan yang menulis berkas level. Dua jalur
            // serialisasi untuk data yang sama akan berselisih, dan yang
            // berselisih di sini adalah apa yang dibaca agen versus apa yang
            // tersimpan di disk.
            const std::string text = scene::SerializeComponent(*ops.type, component);
            json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
            components[ops.type->name] = parsed.is_discarded() ? json(text) : std::move(parsed);
        }

        json result = DescribeEntity(*world, entity);
        result["componentValues"] = std::move(components);
        return Structured(result);
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["entity"],
  "properties": {
    "entity": {"type": "string", "description": "Entity GUID."}
  }
})";
    return tool;
}

// --- selection ----------------------------------------------------------------

ai::ToolDefinition SelectionGet(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "selection.get";
    tool.description =
        "What the human currently has selected in the editor. Read it before acting so you work "
        "on the same objects they are looking at.";
    tool.permission = ai::ToolPermission::Read;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view) {
        scene::World* world = app.Context().world;
        if (world == nullptr) {
            return Text("No scene is open.", /*isError=*/true);
        }
        json entities = json::array();
        for (const uint64_t id : app.GetSelection().Items()) {
            const scene::Entity entity = ToEntity(id);
            if (world->IsAlive(entity)) {
                entities.push_back(DescribeEntity(*world, entity));
            }
        }
        return Structured(json{{"count", entities.size()}, {"entities", std::move(entities)}});
    };
    return tool;
}

ai::ToolDefinition SelectionSet(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "selection.set";
    tool.description =
        "Select entities by GUID, so the human sees what you are working on. Replaces the "
        "current selection; an empty list clears it.";
    // **Write, tapi sengaja tidak lewat Command.** Seleksi bukan data project —
    // ia tidak tersimpan di berkas level dan tidak ada yang hilang bila salah.
    // Menaruhnya di history justru merusak history: pengguna yang menekan Ctrl+Z
    // untuk membatalkan perubahan sungguhan akan menghabiskan beberapa tekan
    // hanya untuk melewati perubahan seleksi.
    tool.permission = ai::ToolPermission::Write;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        scene::World* world = app.Context().world;
        if (world == nullptr) {
            return Text("No scene is open.", /*isError=*/true);
        }
        const json arguments = ParseArguments(argumentsJson);
        if (!arguments.contains("entities") || !arguments.at("entities").is_array()) {
            return Text("\"entities\" is required and must be an array of GUIDs.",
                        /*isError=*/true);
        }

        std::vector<uint64_t> ids;
        std::vector<std::string> unknown;
        for (const json& item : arguments.at("entities")) {
            if (!item.is_string()) {
                continue;
            }
            const std::string guid = item.get<std::string>();
            const scene::Entity entity = world->FindByGuid(Uuid::Parse(guid));
            if (entity == scene::kNullEntity) {
                unknown.push_back(guid);
                continue;
            }
            ids.push_back(ToSelectionId(entity));
        }
        // GUID yang tidak dikenal disebut, bukan didiamkan: seleksi yang berisi
        // dua dari tiga objek terlihat persis seperti seleksi yang berhasil.
        if (!unknown.empty()) {
            return Text("These GUIDs are not in the scene: " +
                            [&unknown]() {
                                std::string joined;
                                for (const std::string& guid : unknown) {
                                    joined += (joined.empty() ? "" : ", ") + guid;
                                }
                                return joined;
                            }(),
                        /*isError=*/true);
        }

        app.GetSelection().SetItems(std::move(ids));
        return Structured(json{{"selected", app.GetSelection().Count()}});
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["entities"],
  "properties": {
    "entities": {"type": "array", "items": {"type": "string"},
                 "description": "Entity GUIDs. Empty clears the selection."}
  }
})";
    return tool;
}

// --- level.list / level.open / level.save --------------------------------------

ai::ToolDefinition LevelList(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "level.list";
    tool.description =
        "Levels available in the open project, and which one is loaded. Nothing else in this "
        "toolset means anything until a level is open — scene.describe on an empty world "
        "honestly reports zero entities, which reads exactly like an empty level.";
    tool.permission = ai::ToolPermission::Read;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view) {
        if (!app.HasProject()) {
            return Text("No project is open.", /*isError=*/true);
        }
        json names = json::array();
        std::error_code ec;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(app.LevelsDirectory(), ec)) {
            if (entry.is_regular_file() && entry.path().extension() == ".simlevel") {
                names.push_back(entry.path().stem().string());
            }
        }
        std::sort(names.begin(), names.end());
        return Structured(json{{"levels", std::move(names)},
                               {"current", app.Context().levelName},
                               {"entityCount",
                                app.Context().world != nullptr
                                    ? json(app.Context().world->Count())
                                    : json(nullptr)}});
    };
    return tool;
}

ai::ToolDefinition LevelOpen(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "level.open";
    tool.description =
        "Load a level by name, as listed by level.list. Replaces whatever is currently open.";
    // **Dangerous, dan bukan karena ia merusak berkas.** Ia membuang seluruh
    // keadaan scene yang belum disimpan, dan itu tidak tertutup undo: history
    // ikut dibuang bersama level lamanya. Yang tidak bisa dibatalkan tidak boleh
    // berjalan tanpa persetujuan.
    tool.permission = ai::ToolPermission::Dangerous;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        if (!app.HasProject()) {
            return Text("No project is open.", /*isError=*/true);
        }
        const json arguments = ParseArguments(argumentsJson);
        if (!arguments.contains("name") || !arguments.at("name").is_string()) {
            return Text("\"name\" is required and must be a string.", /*isError=*/true);
        }
        const std::string name = arguments.at("name").get<std::string>();
        // Nama, bukan jalur: jalur sembarang dari agen adalah jalan keluar dari
        // folder project, dan tidak ada alasan tool ini perlu membukanya.
        if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
            name.find("..") != std::string::npos) {
            return Text("Level names cannot contain path separators.", /*isError=*/true);
        }
        const std::filesystem::path path = app.LevelsDirectory() / (name + ".simlevel");
        if (!std::filesystem::exists(path)) {
            return Text("No level named \"" + name + "\". Call level.list.", /*isError=*/true);
        }
        if (!app.LoadLevel(path)) {
            return Text("Could not load \"" + name + "\"; the editor log has the reason.",
                        /*isError=*/true);
        }
        return Structured(json{{"opened", name},
                               {"entityCount", app.Context().world != nullptr
                                                   ? json(app.Context().world->Count())
                                                   : json(nullptr)}});
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["name"],
  "properties": {
    "name": {"type": "string", "description": "Level name without the .simlevel extension."}
  }
})";
    return tool;
}

ai::ToolDefinition LevelSave(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "level.save";
    tool.description = "Write the open level back to disk, under its current name.";
    tool.permission = ai::ToolPermission::Write;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view) {
        if (!app.HasProject() || app.Context().world == nullptr) {
            return Text("No level is open.", /*isError=*/true);
        }
        const std::filesystem::path path =
            app.LevelsDirectory() / (app.Context().levelName + ".simlevel");
        if (!app.SaveLevel(path)) {
            return Text("Could not save; the editor log has the reason.", /*isError=*/true);
        }
        return Structured(json{{"saved", app.Context().levelName}, {"path", path.string()}});
    };
    return tool;
}

// --- history.checkpoint / history.rollback -------------------------------------

/// Titik aman yang bisa dikembalikan.
///
/// **Cuplikan levelnya ikut disimpan, bukan hanya posisi kursor history.**
/// Kursor saja sudah cukup untuk mengembalikan keadaan pada jalur yang normal —
/// `JumpTo` membatalkan semuanya secara berurutan — tapi ia tidak bisa
/// membuktikan apa pun. Anggaran memori history membuang entri terlama saat
/// penuh, dan sejak itu kursor lama menunjuk tempat yang berbeda tanpa satu pun
/// tanda. Cuplikannya yang membuat rollback bisa mengatakan "berhasil" atau
/// "tidak sampai", alih-alih mengatakan "berhasil" karena tidak ada yang
/// memeriksanya.
struct Checkpoint {
    std::string label;
    int cursor = 0;
    std::string snapshot;
};

using CheckpointStore = std::vector<std::pair<std::string, Checkpoint>>;

ai::ToolDefinition HistoryCheckpoint(EditorApp& app,
                                     const std::shared_ptr<CheckpointStore>& store) {
    ai::ToolDefinition tool;
    tool.name = "history.checkpoint";
    tool.description =
        "Mark a point you can come back to. Take one before a run of changes; history.rollback "
        "returns the level to exactly this state. Returns an id to pass back.";
    tool.permission = ai::ToolPermission::Read;
    tool.needsMainThread = true;
    tool.handler = [&app, store](std::string_view argumentsJson) {
        scene::World* world = app.Context().world;
        if (world == nullptr) {
            return Text("No scene is open.", /*isError=*/true);
        }
        const json arguments = ParseArguments(argumentsJson);
        Checkpoint checkpoint;
        checkpoint.label = arguments.value("label", std::string("checkpoint"));
        checkpoint.cursor = app.History().Cursor();
        checkpoint.snapshot = scene::SaveLevelToString(*world);

        const std::string id =
            checkpoint.label + "-" + std::to_string(store->size() + 1);
        store->emplace_back(id, std::move(checkpoint));
        return Structured(json{{"checkpoint", id},
                               {"cursor", store->back().second.cursor},
                               {"entityCount", world->Count()}});
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "properties": {
    "label": {"type": "string", "description": "Short name, used in the returned id."}
  }
})";
    return tool;
}

ai::ToolDefinition HistoryRollback(EditorApp& app,
                                   const std::shared_ptr<CheckpointStore>& store) {
    ai::ToolDefinition tool;
    tool.name = "history.rollback";
    tool.description =
        "Undo everything done since a checkpoint. Verifies the level really matches the "
        "checkpoint afterwards, and says so if it does not.";
    tool.permission = ai::ToolPermission::Write;
    tool.needsMainThread = true;
    tool.handler = [&app, store](std::string_view argumentsJson) {
        scene::World* world = app.Context().world;
        if (world == nullptr) {
            return Text("No scene is open.", /*isError=*/true);
        }
        const json arguments = ParseArguments(argumentsJson);
        if (!arguments.contains("checkpoint") || !arguments.at("checkpoint").is_string()) {
            return Text("\"checkpoint\" is required.", /*isError=*/true);
        }
        const std::string id = arguments.at("checkpoint").get<std::string>();
        const auto found = std::find_if(store->begin(), store->end(),
                                        [&](const auto& entry) { return entry.first == id; });
        if (found == store->end()) {
            return Text("No checkpoint called \"" + id + "\".", /*isError=*/true);
        }

        app.History().CloseMergeGroup();
        app.History().JumpTo(found->second.cursor);

        // **Diperiksa, bukan diasumsikan.** Inilah yang membedakan rollback yang
        // benar-benar mengembalikan keadaan dari rollback yang hanya menggerakkan
        // kursor ke angka yang kebetulan sama.
        const std::string now = scene::SaveLevelToString(*world);
        const bool exact = now == found->second.snapshot;
        json result{{"checkpoint", id},
                    {"cursor", app.History().Cursor()},
                    {"entityCount", world->Count()},
                    {"exact", exact}};
        if (!exact) {
            result["note"] =
                "The level does not byte-for-byte match the checkpoint. History entries may "
                "have been dropped by the memory budget, or something changed the scene "
                "outside the command history.";
            ai::ToolResult failure;
            failure.isError = true;
            failure.text = result.dump(2);
            return failure;
        }
        return Structured(result);
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["checkpoint"],
  "properties": {
    "checkpoint": {"type": "string", "description": "Id returned by history.checkpoint."}
  }
})";
    return tool;
}

ai::ToolDefinition LevelNew(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "level.new";
    tool.description = "Create an empty level in the project and open it.";
    // Alasan yang sama dengan level.open: ia membuang scene yang belum disimpan
    // beserta seluruh history-nya, dan itu tidak tertutup undo.
    tool.permission = ai::ToolPermission::Dangerous;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        if (!app.HasProject()) {
            return Text("No project is open.", /*isError=*/true);
        }
        const json arguments = ParseArguments(argumentsJson);
        if (!arguments.contains("name") || !arguments.at("name").is_string()) {
            return Text("\"name\" is required and must be a string.", /*isError=*/true);
        }
        const std::string name = arguments.at("name").get<std::string>();
        if (name.empty() || name.find('/') != std::string::npos ||
            name.find('\\') != std::string::npos || name.find("..") != std::string::npos) {
            return Text("Level names cannot be empty or contain path separators.",
                        /*isError=*/true);
        }
        const std::filesystem::path path = app.LevelsDirectory() / (name + ".simlevel");
        if (std::filesystem::exists(path)) {
            // Menimpa level yang ada tidak tertutup undo dan tidak diminta:
            // "buatkan level baru" tidak pernah berarti "hapus yang lama".
            return Text("A level called \"" + name + "\" already exists.", /*isError=*/true);
        }
        if (!app.CreateLevelFile(name)) {
            return Text("Could not create the level; the editor log has the reason.",
                        /*isError=*/true);
        }
        return Structured(json{{"created", name},
                               {"entityCount", app.Context().world != nullptr
                                                   ? json(app.Context().world->Count())
                                                   : json(nullptr)}});
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["name"],
  "properties": {
    "name": {"type": "string", "description": "Level name without the .simlevel extension."}
  }
})";
    return tool;
}

}  // namespace

void RegisterSceneTools(ai::ToolRegistry& tools, ai::ResourceRegistry& resources,
                        EditorApp& app) {
    tools.Register(SceneDescribe(app));
    tools.Register(SceneQuery(app));
    tools.Register(EntityGet(app));
    tools.Register(SelectionGet(app));
    tools.Register(SelectionSet(app));
    tools.Register(LevelList(app));
    tools.Register(LevelOpen(app));
    tools.Register(LevelSave(app));
    tools.Register(LevelNew(app));

    // Store checkpoint hidup selama registry-nya: handler memegangnya lewat
    // shared_ptr, bukan lewat static, supaya dua server di satu proses — editor
    // dan SimHeadless di uji yang sama — tidak berbagi titik aman.
    auto checkpoints = std::make_shared<CheckpointStore>();
    tools.Register(HistoryCheckpoint(app, checkpoints));
    tools.Register(HistoryRollback(app, checkpoints));

    resources.Register(ComponentDocs());
}

}  // namespace sim::editor
