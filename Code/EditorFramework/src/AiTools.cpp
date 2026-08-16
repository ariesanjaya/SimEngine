#include "Sim/Editor/AiTools.h"

#include "Sim/AIBridge/ToolRegistry.h"
#include "Sim/Editor/EditorApp.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Render/IViewportRenderer.h"
#include "Sim/Scene/Project.h"
#include "Sim/Scene/World.h"

#include <nlohmann/json.hpp>

namespace sim::editor {
namespace {

using nlohmann::json;

ai::ToolDefinition EditorStatus(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "editor.status";
    tool.description =
        "What the editor currently has open: project, level, entity count, play state, and "
        "viewport size. Call this first — it tells you whether a project is open at all, and "
        "most other tools are meaningless until one is.";
    tool.permission = ai::ToolPermission::Read;
    // Membaca `World` dan renderer viewport. Keduanya milik main thread.
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view) {
        const scene::Project& project = app.CurrentProject();
        const EditorContext& context = app.Context();

        json status;
        status["hasProject"] = app.HasProject();
        status["project"] = json{{"name", project.name}, {"root", project.root.string()}};
        status["level"] = json{{"name", context.levelName}};
        // World bisa saja belum ada — project manager tampil sebelum satu pun
        // project dibuka, dan "nol entity" adalah jawaban yang berbeda dari
        // "belum ada scene sama sekali".
        status["entityCount"] =
            context.world != nullptr ? json(context.world->Count()) : json(nullptr);
        status["playing"] = app.IsPlaying();
        if (context.viewportRenderer != nullptr) {
            status["viewport"] = json{{"width", context.viewportRenderer->Width()},
                                      {"height", context.viewportRenderer->Height()},
                                      {"renderer", context.viewportRenderer->Name()}};
        } else {
            status["viewport"] = nullptr;
        }

        ai::ToolResult result;
        result.text = status.dump(2);
        return result;
    };
    return tool;
}

}  // namespace

void RegisterEditorTools(ai::ToolRegistry& tools, EditorApp& app) {
    tools.Register(EditorStatus(app));
}

}  // namespace sim::editor
