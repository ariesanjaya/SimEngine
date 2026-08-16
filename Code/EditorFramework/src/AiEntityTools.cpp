#include "Sim/AIBridge/ToolRegistry.h"
#include "Sim/Editor/AiTools.h"
#include "Sim/Editor/Command.h"
#include "Sim/Editor/EditorApp.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/SceneCommands.h"
#include "Sim/Editor/Selection.h"
#include "Sim/Scene/ComponentRegistry.h"
#include "Sim/Scene/Components.h"
#include "Sim/Scene/Serialization.h"
#include "Sim/Scene/World.h"

#include <nlohmann/json.hpp>

#include <string>
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

ai::ToolResult Structured(const json& value) {
    ai::ToolResult result;
    result.text = value.dump(2);
    return result;
}

/// Satu tool call, satu entri undo, dengan nama yang menyebut asalnya.
///
/// **`AI:` di depan bukan hiasan.** Panel History adalah tempat pertama yang
/// dilihat orang ketika project berubah tanpa mereka lakukan, dan entri yang
/// tampak seperti hasil klik mereka sendiri membuat mereka mencari di tempat
/// yang salah. Awalan ini yang membedakan "saya yang melakukan ini" dari
/// "sesuatu yang lain melakukannya".
class AiTransaction {
public:
    AiTransaction(CommandHistory& history, const char* toolName) : history_(history) {
        // Kelompok merge ditutup lebih dulu: tanpa ini, perubahan agen bisa
        // tergabung ke seretan gizmo yang barusan dilakukan manusia, dan yang
        // muncul di history adalah satu entri yang memuat keduanya.
        history_.CloseMergeGroup();
        history_.BeginTransaction(std::string("AI: ") + toolName);
    }
    ~AiTransaction() {
        history_.EndTransaction();
        history_.CloseMergeGroup();
    }

    AiTransaction(const AiTransaction&) = delete;
    AiTransaction& operator=(const AiTransaction&) = delete;

private:
    CommandHistory& history_;
};

scene::Entity ResolveGuid(const scene::World& world, const json& value) {
    if (!value.is_string()) {
        return scene::kNullEntity;
    }
    return world.FindByGuid(Uuid::Parse(value.get<std::string>()));
}

/// Menerapkan `values` ke sebuah komponen lewat Command, jadi tercatat dan bisa
/// di-undo. Mengembalikan pesan galat, atau string kosong bila berhasil.
std::string ApplyComponentValues(EditorApp& app, scene::Entity entity,
                                 const std::string& componentName, const json& values) {
    scene::World& world = *app.Context().world;
    const scene::ComponentOps* ops = scene::ComponentRegistry::Get().Find(componentName);
    if (ops == nullptr) {
        return "No component type named \"" + componentName +
               "\". Read simengine://docs/components for the list.";
    }
    if (!values.is_object()) {
        return "\"values\" must be an object of field names.";
    }

    const auto handle = static_cast<entt::entity>(entity);
    if (ops->tryGet(world.Registry(), handle) == nullptr) {
        if (!ops->addable) {
            return "The component \"" + componentName + "\" cannot be added to an entity.";
        }
        // **Ditambahkan lewat Command tersendiri lebih dulu, bukan disisipkan
        // di sini.** `SetComponentsCommand::Apply` melewati entity yang belum
        // punya komponennya — dengan sengaja, supaya seleksi jamak yang isinya
        // beragam tidak menumbuhkan komponen di tempat yang tidak diminta.
        // Menyerahkan nilai kepadanya tanpa menambahkan komponennya lebih dulu
        // karena itu tidak menghasilkan galat apa pun; ia hanya diam-diam tidak
        // melakukan apa-apa. Keduanya berada di dalam satu transaksi, jadi
        // manusia tetap melihatnya sebagai satu langkah.
        app.History().Execute<AddComponentCommand>(&world, world.GuidOf(entity), ops);
    }

    void* existing = ops->tryGet(world.Registry(), handle);
    if (existing == nullptr) {
        return "Could not add \"" + componentName + "\" to the entity.";
    }
    // Nilai yang berlaku sekarang, bukan objek kosong: `values` hanya menyebut
    // field yang hendak diubah, dan sisanya harus tetap seperti adanya.
    std::string before = scene::SerializeComponent(*ops->type, existing);

    json target = json::parse(before, nullptr, /*allow_exceptions=*/false);
    if (target.is_discarded()) {
        return "Could not read the current value of \"" + componentName + "\".";
    }
    // merge_patch, bukan penggantian: agen yang menyebut satu field tidak sedang
    // meminta sisanya dikosongkan.
    target.merge_patch(values);

    std::vector<SetComponentsCommand::Item> items;
    items.push_back({world.GuidOf(entity), std::move(before), target.dump()});
    app.History().Execute<SetComponentsCommand>(&world, ops, std::move(items));
    return {};
}

// --- entity.create / entity.create_many ---------------------------------------

/// Satu entity dari deskripsinya. Mengembalikan GUID, atau pesan galat.
std::string CreateOne(EditorApp& app, const json& spec, Uuid& outGuid) {
    scene::World& world = *app.Context().world;

    scene::Entity parent = scene::kNullEntity;
    if (spec.contains("parent") && !spec.at("parent").is_null()) {
        parent = ResolveGuid(world, spec.at("parent"));
        if (parent == scene::kNullEntity) {
            return "No entity with the parent GUID given.";
        }
    }

    const std::string name = spec.value("name", std::string("Entity"));
    auto command = std::make_unique<CreateEntityCommand>(&world, &app.GetSelection(), parent, name);
    // GUID dibaca sebelum command diserahkan: sesudahnya ia milik history.
    outGuid = command->Guid();
    app.History().Execute(std::move(command));

    const scene::Entity created = world.FindByGuid(outGuid);
    if (created == scene::kNullEntity) {
        return "The entity was not created; the editor log has the reason.";
    }

    if (spec.contains("components")) {
        if (!spec.at("components").is_object()) {
            return "\"components\" must be an object keyed by component name.";
        }
        for (const auto& [componentName, values] : spec.at("components").items()) {
            const std::string error = ApplyComponentValues(app, created, componentName, values);
            if (!error.empty()) {
                return error;
            }
        }
    }
    return {};
}

ai::ToolDefinition EntityCreate(EditorApp& app, bool many) {
    ai::ToolDefinition tool;
    tool.name = many ? "entity.create_many" : "entity.create";
    tool.description =
        many ? "Create several entities in one call, each with its own name, parent, and "
               "component values. Use this instead of calling entity.create in a loop — two "
               "hundred separate calls is two hundred undo entries and two hundred round trips."
             : "Create one entity, optionally under a parent and with starting component "
               "values. Returns the GUID you will need for everything else.";
    tool.permission = ai::ToolPermission::Write;
    tool.needsMainThread = true;
    tool.handler = [&app, many](std::string_view argumentsJson) {
        if (app.Context().world == nullptr) {
            return Text("No scene is open.", /*isError=*/true);
        }
        const json arguments = ParseArguments(argumentsJson);

        std::vector<json> specs;
        if (many) {
            if (!arguments.contains("entities") || !arguments.at("entities").is_array()) {
                return Text("\"entities\" is required and must be an array.", /*isError=*/true);
            }
            for (const json& spec : arguments.at("entities")) {
                specs.push_back(spec);
            }
        } else {
            specs.push_back(arguments);
        }
        if (specs.empty()) {
            return Text("Nothing to create.", /*isError=*/true);
        }

        json created = json::array();
        std::string failure;
        {
            AiTransaction transaction(app.History(), many ? "entity.create_many"
                                                          : "entity.create");
            for (const json& spec : specs) {
                Uuid guid;
                failure = CreateOne(app, spec, guid);
                if (!failure.empty()) {
                    break;
                }
                created.push_back(guid.ToString());
            }
        }
        if (!failure.empty()) {
            // **Yang sudah terlanjur dibuat tetap disebut.** Transaksi ini satu
            // entri undo, jadi manusia bisa membatalkannya sekaligus — tapi agen
            // yang tidak diberi tahu apa yang sempat jadi akan membuatnya lagi.
            return Text(failure + " Created before failing: " + std::to_string(created.size()) +
                            ". One editor.undo removes all of them.",
                        /*isError=*/true);
        }
        return Structured(json{{"created", std::move(created)}});
    };
    tool.inputSchemaJson =
        many ? R"({
  "type": "object",
  "required": ["entities"],
  "properties": {
    "entities": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "name": {"type": "string"},
          "parent": {"type": "string", "description": "Parent entity GUID. Omit for a root entity."},
          "components": {"type": "object", "description": "Component name to field values, e.g. {\"Transform\": {\"position\": [0,1,0]}}."}
        }
      }
    }
  }
})"
             : R"({
  "type": "object",
  "properties": {
    "name": {"type": "string"},
    "parent": {"type": "string", "description": "Parent entity GUID. Omit for a root entity."},
    "components": {"type": "object", "description": "Component name to field values, e.g. {\"Transform\": {\"position\": [0,1,0]}}."}
  }
})";
    return tool;
}

// --- entity.modify -------------------------------------------------------------

ai::ToolDefinition EntityModify(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "entity.modify";
    tool.description =
        "Set field values on one component of one entity. Fields you do not name keep their "
        "current values. Adds the component first if the entity does not have it. Read "
        "simengine://docs/components to see what fields exist.";
    tool.permission = ai::ToolPermission::Write;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        scene::World* world = app.Context().world;
        if (world == nullptr) {
            return Text("No scene is open.", /*isError=*/true);
        }
        const json arguments = ParseArguments(argumentsJson);
        if (!arguments.contains("entity") || !arguments.contains("component") ||
            !arguments.contains("values")) {
            return Text("\"entity\", \"component\", and \"values\" are all required.",
                        /*isError=*/true);
        }
        const scene::Entity entity = ResolveGuid(*world, arguments.at("entity"));
        if (entity == scene::kNullEntity) {
            return Text("No entity with that GUID.", /*isError=*/true);
        }
        if (!arguments.at("component").is_string()) {
            return Text("\"component\" must be a string.", /*isError=*/true);
        }

        const std::string componentName = arguments.at("component").get<std::string>();
        std::string failure;
        {
            AiTransaction transaction(app.History(), "entity.modify");
            failure = ApplyComponentValues(app, entity, componentName, arguments.at("values"));
        }
        if (!failure.empty()) {
            return Text(failure, /*isError=*/true);
        }

        // Hasilnya dibaca kembali dari dunia, bukan digemakan dari argumen: yang
        // ingin diketahui agen adalah nilai yang benar-benar berlaku, dan
        // keduanya bisa berbeda — field yang tidak dikenal diabaikan diam-diam
        // oleh deserialisasi.
        const scene::ComponentOps* ops = scene::ComponentRegistry::Get().Find(componentName);
        void* component =
            ops == nullptr ? nullptr
                           : ops->tryGet(world->Registry(), static_cast<entt::entity>(entity));
        // **Diperiksa, walaupun sampai di sini seharusnya tidak mungkin null.**
        // `SerializeComponent` men-dereference apa yang diberikan kepadanya, dan
        // bentuk sebelumnya — yang menganggap komponennya pasti ada karena
        // baru saja diminta — mematikan editor begitu jalur penambahan
        // komponennya ternyata diam-diam tidak melakukan apa-apa.
        json applied = nullptr;
        if (component != nullptr) {
            json parsed = json::parse(scene::SerializeComponent(*ops->type, component), nullptr,
                                      /*allow_exceptions=*/false);
            if (!parsed.is_discarded()) {
                applied = std::move(parsed);
            }
        }
        return Structured(json{{"entity", arguments.at("entity")},
                               {"component", componentName},
                               {"applied", std::move(applied)}});
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["entity", "component", "values"],
  "properties": {
    "entity": {"type": "string", "description": "Entity GUID."},
    "component": {"type": "string", "description": "Component name, e.g. \"Transform\"."},
    "values": {"type": "object", "description": "Field names to values. Unnamed fields keep what they had."}
  }
})";
    return tool;
}

// --- hierarki -------------------------------------------------------------------

std::vector<Uuid> GuidList(const scene::World& world, const json& arguments, const char* key,
                           std::string& failure) {
    std::vector<Uuid> guids;
    if (!arguments.contains(key) || !arguments.at(key).is_array()) {
        failure = std::string("\"") + key + "\" is required and must be an array of GUIDs.";
        return guids;
    }
    for (const json& item : arguments.at(key)) {
        const scene::Entity entity = ResolveGuid(world, item);
        if (entity == scene::kNullEntity) {
            failure = "No entity with GUID " + (item.is_string() ? item.get<std::string>() : "?") +
                      ".";
            return {};
        }
        guids.push_back(world.GuidOf(entity));
    }
    return guids;
}

ai::ToolDefinition EntityDelete(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "entity.delete";
    tool.description =
        "Delete entities and everything under them. Undoable — the whole subtree comes back "
        "with its GUIDs intact.";
    tool.permission = ai::ToolPermission::Write;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        scene::World* world = app.Context().world;
        if (world == nullptr) {
            return Text("No scene is open.", /*isError=*/true);
        }
        const json arguments = ParseArguments(argumentsJson);
        std::string failure;
        std::vector<Uuid> guids = GuidList(*world, arguments, "entities", failure);
        if (!failure.empty()) {
            return Text(failure, /*isError=*/true);
        }
        const std::size_t count = guids.size();
        {
            AiTransaction transaction(app.History(), "entity.delete");
            app.History().Execute<DeleteEntitiesCommand>(world, &app.GetSelection(),
                                                         std::move(guids));
        }
        return Structured(json{{"deleted", count}});
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["entities"],
  "properties": {
    "entities": {"type": "array", "items": {"type": "string"}}
  }
})";
    return tool;
}

ai::ToolDefinition EntityReparent(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "entity.reparent";
    tool.description =
        "Move an entity under a new parent, or to the root by omitting the parent. Refuses to "
        "create a cycle.";
    tool.permission = ai::ToolPermission::Write;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        scene::World* world = app.Context().world;
        if (world == nullptr) {
            return Text("No scene is open.", /*isError=*/true);
        }
        const json arguments = ParseArguments(argumentsJson);
        const scene::Entity child =
            arguments.contains("entity") ? ResolveGuid(*world, arguments.at("entity"))
                                         : scene::kNullEntity;
        if (child == scene::kNullEntity) {
            return Text("\"entity\" is required and must be a GUID in the scene.",
                        /*isError=*/true);
        }

        Uuid newParent;  // nil = jadikan akar
        if (arguments.contains("parent") && !arguments.at("parent").is_null()) {
            const scene::Entity parent = ResolveGuid(*world, arguments.at("parent"));
            if (parent == scene::kNullEntity) {
                return Text("No entity with the parent GUID given.", /*isError=*/true);
            }
            // **Dirinya sendiri diperiksa terpisah dari keturunannya.**
            // `IsDescendantOf(x, x)` bernilai false — sebuah entity bukan
            // keturunan dirinya sendiri — jadi penjagaan siklus saja meloloskan
            // kasus yang paling sederhana. `World::SetParent` menolaknya, tapi
            // menolaknya diam-diam: tool ini sempat melaporkan berhasil dan
            // mencatat satu entri undo untuk sesuatu yang tidak pernah terjadi.
            if (parent == child) {
                return Text("An entity cannot be its own parent.", /*isError=*/true);
            }
            if (world->IsDescendantOf(parent, child)) {
                // Ditolak di sini, bukan dibiarkan gagal diam-diam di dalam
                // command: agen yang menerima "berhasil" untuk sesuatu yang
                // tidak terjadi akan membangun sisa rencananya di atas itu.
                return Text("That would put an entity inside its own subtree.",
                            /*isError=*/true);
            }
            newParent = world->GuidOf(parent);
        }

        const scene::Entity oldParentEntity = world->ParentOf(child);
        const Uuid oldParent = oldParentEntity == scene::kNullEntity ? Uuid{}
                                                                     : world->GuidOf(oldParentEntity);
        {
            AiTransaction transaction(app.History(), "entity.reparent");
            app.History().Execute<ReparentCommand>(world, world->GuidOf(child), newParent,
                                                   oldParent);
        }

        // **Hasilnya diperiksa, bukan diasumsikan.** `World::SetParent` boleh
        // menolak, dan penolakannya tidak sampai ke sini sebagai galat — yang
        // tersisa hanyalah induk yang tidak berubah.
        const scene::Entity actual = world->ParentOf(child);
        const Uuid actualGuid =
            actual == scene::kNullEntity ? Uuid{} : world->GuidOf(actual);
        if (actualGuid != newParent) {
            return Text("The editor refused that reparent; the entity is still where it was.",
                        /*isError=*/true);
        }
        return Structured(json{{"entity", arguments.at("entity")},
                               {"parent", arguments.value("parent", json(nullptr))}});
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["entity"],
  "properties": {
    "entity": {"type": "string", "description": "Entity GUID to move."},
    "parent": {"type": ["string", "null"], "description": "New parent GUID, or null for the root."}
  }
})";
    return tool;
}

ai::ToolDefinition EntityDuplicate(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "entity.duplicate";
    tool.description =
        "Copy entities and their subtrees. The copies get fresh GUIDs, which are returned.";
    tool.permission = ai::ToolPermission::Write;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        scene::World* world = app.Context().world;
        if (world == nullptr) {
            return Text("No scene is open.", /*isError=*/true);
        }
        const json arguments = ParseArguments(argumentsJson);
        std::string failure;
        const std::vector<Uuid> guids = GuidList(*world, arguments, "entities", failure);
        if (!failure.empty()) {
            return Text(failure, /*isError=*/true);
        }

        std::vector<std::string> subtrees = CopySubtrees(*world, guids);
        if (subtrees.empty()) {
            return Text("Nothing to duplicate.", /*isError=*/true);
        }

        Uuid parent;
        if (arguments.contains("parent") && !arguments.at("parent").is_null()) {
            const scene::Entity entity = ResolveGuid(*world, arguments.at("parent"));
            if (entity == scene::kNullEntity) {
                return Text("No entity with the parent GUID given.", /*isError=*/true);
            }
            parent = world->GuidOf(entity);
        }

        json roots = json::array();
        {
            AiTransaction transaction(app.History(), "entity.duplicate");
            auto command = std::make_unique<PasteEntitiesCommand>(
                world, &app.GetSelection(), std::move(subtrees), parent, "Duplicate");
            PasteEntitiesCommand* raw = command.get();
            app.History().Execute(std::move(command));
            // GUID hasilnya baru ada sesudah `Do()`, dan `Do()` dijalankan oleh
            // `Execute`. Command-nya sendiri milik history sejak itu; yang
            // dibaca di sini hanya hasilnya.
            for (const Uuid& guid : raw->CreatedRoots()) {
                roots.push_back(guid.ToString());
            }
        }
        return Structured(json{{"created", std::move(roots)}});
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["entities"],
  "properties": {
    "entities": {"type": "array", "items": {"type": "string"}},
    "parent": {"type": ["string", "null"], "description": "Where to put the copies. Default: same parent as the original."}
  }
})";
    return tool;
}

ai::ToolDefinition EntityRename(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "entity.rename";
    tool.description = "Give an entity a new name.";
    tool.permission = ai::ToolPermission::Write;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        scene::World* world = app.Context().world;
        if (world == nullptr) {
            return Text("No scene is open.", /*isError=*/true);
        }
        const json arguments = ParseArguments(argumentsJson);
        const scene::Entity entity =
            arguments.contains("entity") ? ResolveGuid(*world, arguments.at("entity"))
                                         : scene::kNullEntity;
        if (entity == scene::kNullEntity) {
            return Text("\"entity\" is required and must be a GUID in the scene.",
                        /*isError=*/true);
        }
        if (!arguments.contains("name") || !arguments.at("name").is_string()) {
            return Text("\"name\" is required and must be a string.", /*isError=*/true);
        }
        const std::string after = arguments.at("name").get<std::string>();
        {
            AiTransaction transaction(app.History(), "entity.rename");
            app.History().Execute<RenameEntityCommand>(world, world->GuidOf(entity),
                                                       world->NameOf(entity), after);
        }
        return Structured(json{{"entity", arguments.at("entity")}, {"name", after}});
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["entity", "name"],
  "properties": {
    "entity": {"type": "string"},
    "name": {"type": "string"}
  }
})";
    return tool;
}

}  // namespace

void RegisterEntityTools(ai::ToolRegistry& tools, EditorApp& app) {
    tools.Register(EntityCreate(app, /*many=*/false));
    tools.Register(EntityCreate(app, /*many=*/true));
    tools.Register(EntityModify(app));
    tools.Register(EntityRename(app));
    tools.Register(EntityReparent(app));
    tools.Register(EntityDuplicate(app));
    tools.Register(EntityDelete(app));
}

}  // namespace sim::editor
