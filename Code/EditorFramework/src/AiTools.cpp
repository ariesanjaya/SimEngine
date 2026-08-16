#include "Sim/Editor/AiTools.h"

#include "Sim/AIBridge/ResourceRegistry.h"
#include "Sim/AIBridge/ToolRegistry.h"
#include "Sim/Core/Log.h"
#include "Sim/Core/MainThreadQueue.h"
#include "Sim/Editor/Actions.h"
#include "Sim/Editor/Command.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelManager.h"
#include "Sim/Editor/EditorApp.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Render/IViewportRenderer.h"
#include "Sim/Scene/Project.h"
#include "Sim/Scene/World.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <thread>
#include <chrono>
#include <string>

namespace sim::editor {
namespace {

using nlohmann::json;

/// Argumen sebagai objek. Teks yang rusak tidak boleh sampai ke handler.
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

// --- editor.status -----------------------------------------------------------

ai::ToolDefinition EditorStatus(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "editor.status";
    tool.description =
        "What the editor currently has open: project, level, entity count, play state, and "
        "viewport size. Call this first — it tells you whether a project is open at all, and "
        "most other tools are meaningless until one is.";
    tool.permission = ai::ToolPermission::Read;
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
        status["history"] = json{{"canUndo", app.History().CanUndo()},
                                 {"canRedo", app.History().CanRedo()},
                                 {"dirty", app.History().IsDirty()}};
        if (context.viewportRenderer != nullptr) {
            status["viewport"] = json{{"width", context.viewportRenderer->Width()},
                                      {"height", context.viewportRenderer->Height()},
                                      {"renderer", context.viewportRenderer->Name()}};
        } else {
            status["viewport"] = nullptr;
        }
        return Structured(status);
    };
    return tool;
}

// --- editor.actions ----------------------------------------------------------

ai::ToolDefinition EditorActions(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "editor.actions";
    tool.description =
        "Every named editor action, with its id, label, category, and whether it is currently "
        "enabled. These ids are what editor.execute_action takes. List before you invoke — "
        "guessing an id gets you an error, not an action.";
    tool.permission = ai::ToolPermission::Read;
    // `IsEnabled` memanggil predikat milik aksi, dan predikat itu membaca
    // keadaan editor.
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        const json arguments = ParseArguments(argumentsJson);
        const std::string category = arguments.value("category", std::string{});

        json list = json::array();
        for (const Action& action : app.Actions().All()) {
            if (!category.empty() && action.category != category) {
                continue;
            }
            list.push_back(json{{"id", action.id},
                                {"label", action.label},
                                {"category", action.category},
                                {"enabled", app.Actions().IsEnabled(action.id)},
                                {"shortcut", app.Actions().ShortcutText(action.id)}});
        }
        return Structured(json{{"actions", std::move(list)}});
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "properties": {
    "category": {"type": "string", "description": "Only actions in this category, e.g. \"Edit\"."}
  }
})";
    return tool;
}

// --- editor.execute_action ---------------------------------------------------

ai::ToolDefinition EditorExecuteAction(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "editor.execute_action";
    tool.description =
        "Invoke a named editor action — the same path a menu item or keyboard shortcut takes. "
        "One tool for every menu in the editor. Get ids from editor.actions.";
    // Write, bukan Read: sebagian aksi mengubah project. Bukan Dangerous —
    // seluruhnya melewati Command dan karena itu bisa di-undo.
    tool.permission = ai::ToolPermission::Write;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        const json arguments = ParseArguments(argumentsJson);
        if (!arguments.contains("action") || !arguments.at("action").is_string()) {
            return Text("\"action\" is required and must be a string.", /*isError=*/true);
        }
        const std::string id = arguments.at("action").get<std::string>();

        // **Tiga jawaban, bukan dua.** `Invoke` mengembalikan false untuk aksi
        // yang tidak ada dan untuk aksi yang sedang nonaktif, dan agen yang
        // menerima satu kegagalan yang sama akan mencoba memperbaiki hal yang
        // salah — mengganti nama padahal namanya benar dan editornya yang
        // sedang tidak mengizinkan.
        const Action* action = app.Actions().Find(id);
        if (action == nullptr) {
            return Text("No action with id \"" + id + "\". Call editor.actions for the list.",
                        /*isError=*/true);
        }
        if (!app.Actions().IsEnabled(id)) {
            return Text("The action \"" + id + "\" (" + action->label +
                            ") exists but is disabled right now.",
                        /*isError=*/true);
        }
        if (!app.Actions().Invoke(id)) {
            return Text("The action \"" + id + "\" refused to run.", /*isError=*/true);
        }
        return Structured(json{{"invoked", id}, {"label", action->label}});
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["action"],
  "properties": {
    "action": {"type": "string", "description": "Action id, e.g. \"view.reset_layout\"."}
  }
})";
    return tool;
}

// --- editor.undo / editor.redo ----------------------------------------------

ai::ToolDefinition HistoryStep(EditorApp& app, bool undo) {
    ai::ToolDefinition tool;
    tool.name = undo ? "editor.undo" : "editor.redo";
    tool.description = undo ? "Undo the last change, and say which one it was."
                            : "Redo the change that was last undone, and say which one it was.";
    tool.permission = ai::ToolPermission::Write;
    tool.needsMainThread = true;
    tool.handler = [&app, undo](std::string_view) {
        CommandHistory& history = app.History();
        // Namanya dibaca sebelum kursornya bergerak: sesudahnya, entri yang
        // dimaksud tidak lagi berada di tempat yang sama.
        const int cursor = history.Cursor();
        const std::vector<CommandHistory::Entry>& entries = history.Entries();
        const int index = undo ? cursor - 1 : cursor;
        const bool possible = undo ? history.CanUndo() : history.CanRedo();
        if (!possible) {
            // Bukan galat: "tidak ada yang bisa dibatalkan" adalah jawaban yang
            // sah, dan agen yang menerimanya sebagai kegagalan akan mencoba
            // lagi.
            return Structured(json{{"changed", false},
                                   {"reason", undo ? "nothing left to undo"
                                                   : "nothing left to redo"}});
        }
        const std::string name =
            index >= 0 && index < static_cast<int>(entries.size()) ? entries[static_cast<std::size_t>(index)].name
                                                                   : std::string("(unnamed)");
        const bool moved = undo ? history.Undo() : history.Redo();
        return Structured(json{{"changed", moved}, {"command", name},
                               {"canUndo", history.CanUndo()},
                               {"canRedo", history.CanRedo()}});
    };
    return tool;
}

// --- editor.log_tail ---------------------------------------------------------

spdlog::level::level_enum LevelFromString(const std::string& name,
                                          spdlog::level::level_enum fallback) {
    if (name == "trace") return spdlog::level::trace;
    if (name == "debug") return spdlog::level::debug;
    if (name == "info") return spdlog::level::info;
    if (name == "warn" || name == "warning") return spdlog::level::warn;
    if (name == "error" || name == "err") return spdlog::level::err;
    if (name == "critical") return spdlog::level::critical;
    return fallback;
}

/// Ekor log sebagai JSON. **Satu rumus, dipakai tool dan resource** — dua
/// salinan yang sama adalah dua yang akan berselisih, dan selisih antara apa
/// yang dibaca agen sebagai konteks dan apa yang ia dapat saat bertanya adalah
/// selisih yang paling membingungkan.
json LogTail(std::size_t limit, spdlog::level::level_enum minimum) {
    const std::vector<LogEntry> all = LogRing::Get().Snapshot();
    json lines = json::array();
    for (const LogEntry& entry : all) {
        if (entry.level < minimum) {
            continue;
        }
        const auto name = spdlog::level::to_string_view(entry.level);
        lines.push_back(json{{"sequence", entry.sequence},
                             {"level", std::string(name.data(), name.size())},
                             {"channel", entry.channel},
                             {"message", entry.message}});
    }
    // Dipotong dari depan: yang diminta adalah yang terakhir.
    if (lines.size() > limit) {
        lines.erase(lines.begin(), lines.begin() + static_cast<long>(lines.size() - limit));
    }
    return json{{"lines", std::move(lines)}, {"totalHeld", all.size()}};
}

ai::ToolDefinition EditorLogTail() {
    ai::ToolDefinition tool;
    tool.name = "editor.log_tail";
    tool.description =
        "The most recent editor log lines, newest last, optionally filtered by minimum level. "
        "Use it to diagnose what went wrong instead of asking a human to copy the log.";
    tool.permission = ai::ToolPermission::Read;
    // **Tidak lewat main thread, dan itu bukan kelalaian.** `LogRing` punya
    // mutex sendiri, jadi ia aman dibaca dari thread jaringan — dan itulah
    // justru yang membuat tool ini masih menjawab saat editor membeku atau
    // sedang menampilkan dialog modal, yaitu saat log paling dibutuhkan.
    tool.needsMainThread = false;
    tool.handler = [](std::string_view argumentsJson) {
        const json arguments = ParseArguments(argumentsJson);
        const int requested = arguments.value("lines", 100);
        const spdlog::level::level_enum minimum =
            LevelFromString(arguments.value("level", std::string{}), spdlog::level::trace);
        return Structured(LogTail(static_cast<std::size_t>(std::clamp(requested, 1, 1000)),
                                  minimum));
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "properties": {
    "lines": {"type": "integer", "minimum": 1, "maximum": 1000,
              "description": "How many of the most recent lines to return. Default 100."},
    "level": {"type": "string", "enum": ["trace","debug","info","warn","error","critical"],
              "description": "Minimum level to include. Default: everything held."}
  }
})";
    return tool;
}

// --- editor.screenshot -------------------------------------------------------

ai::ToolDefinition EditorScreenshot(ScreenshotFn screenshot) {
    ai::ToolDefinition tool;
    tool.name = "editor.screenshot";
    tool.description =
        "A PNG of the whole editor window as it looks right now. Use it to check your own work "
        "— what a panel actually shows, whether a dialog is in the way, whether the viewport "
        "looks like you expect. Seeing beats guessing.";
    tool.permission = ai::ToolPermission::Read;
    // Menyentuh swapchain, dan swapchain milik main thread.
    tool.needsMainThread = true;
    tool.handler = [screenshot = std::move(screenshot)](std::string_view) {
        std::vector<uint8_t> png;
        std::string error;
        if (!screenshot(/*crop=*/nullptr, png, error)) {
            return Text("Could not capture the window: " + error, /*isError=*/true);
        }
        ai::ToolResult result;
        // Ukurannya disebut di teks. Agen yang menerima gambar tanpa ukuran
        // harus menebaknya dari pikselnya, dan ukuran jendela adalah hal yang
        // sering justru menjadi jawabannya.
        result.text = "Editor window, " + std::to_string(png.size()) + " bytes of PNG.";
        result.imageBytes = std::move(png);
        result.imageMimeType = "image/png";
        return result;
    };
    return tool;
}

// --- viewport.capture ----------------------------------------------------------

ai::ToolDefinition ViewportCapture(EditorApp& app, ScreenshotFn screenshot) {
    ai::ToolDefinition tool;
    tool.name = "viewport.capture";
    tool.description =
        "Look at the scene from a point you choose, then take a picture. Give \"from\" and "
        "\"look_at\" in world space to move the editor camera first; omit them to shoot from "
        "wherever it already is. The image is the whole editor window, with the viewport "
        "showing the angle you asked for.";
    tool.permission = ai::ToolPermission::Read;
    // **Handler ini berjalan di thread jaringan, dan itu disengaja.** Ia harus
    // menunggu beberapa frame di antara menggeser kamera dan memotret, dan
    // menunggu dari dalam main thread berarti menunggu frame yang tidak akan
    // pernah datang — `Drain()` dipanggil sekali per frame, jadi menahannya
    // menahan frame itu sendiri. Yang diantrikan ke main thread hanyalah dua
    // langkah pendeknya.
    tool.needsMainThread = false;
    tool.handler = [&app, screenshot = std::move(screenshot)](std::string_view argumentsJson) {
        const json arguments = ParseArguments(argumentsJson);

        const auto readVec = [&](const char* key, Vec3& out) {
            if (!arguments.contains(key) || !arguments.at(key).is_array() ||
                arguments.at(key).size() != 3) {
                return false;
            }
            out = Vec3(arguments.at(key)[0].get<float>(), arguments.at(key)[1].get<float>(),
                       arguments.at(key)[2].get<float>());
            return true;
        };

        Vec3 from(0.0f);
        Vec3 lookAt(0.0f);
        const bool hasFrom = readVec("from", from);
        const bool hasLookAt = readVec("look_at", lookAt);
        if (hasFrom != hasLookAt) {
            return Text("Give both \"from\" and \"look_at\", or neither.", /*isError=*/true);
        }

        if (hasFrom) {
            std::future<bool> placed = MainThreadQueue::Get().Submit([&app, from, lookAt]() {
                // Panel yang tertutup tidak menggambar, dan yang tidak menggambar
                // tidak pernah membaca permintaan kamera. Membukanya di sini
                // menutup kasus yang paling umum; yang tersisa — panel terbuka
                // tapi berada di belakang tab lain — terdeteksi di bawah.
                if (Panel* viewport = app.Panels().Find(panel_id::kViewport)) {
                    viewport->SetOpen(true);
                    // **Dibuka saja tidak cukup.** Panel yang terbuka tapi
                    // berada di belakang tab lain di dock yang sama tidak
                    // digambar sama sekali, jadi ia tidak pernah membaca
                    // permintaan kamera — dan gambar yang kembali memperlihatkan
                    // panel yang lain. `RequestFocus` yang memilih tab-nya;
                    // itulah yang disebut komentarnya di `PanelManager`.
                    viewport->RequestFocus();
                }
                app.Context().cameraRequest = {true, from, lookAt};
                return true;
            });
            if (placed.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
                return Text("The editor did not answer; it may be showing a modal dialog.",
                            /*isError=*/true);
            }
            // Beberapa frame supaya permintaannya benar-benar terkonsumsi dan
            // tergambar. Menunggu satu frame saja adalah lomba: panel Viewport
            // mungkin sudah lewat titik konsumsinya pada frame yang sedang
            // berjalan.
            std::this_thread::sleep_for(std::chrono::milliseconds(120));

            // **Diperiksa, bukan diasumsikan.** Permintaan yang masih tergantung
            // berarti panel Viewport tidak menggambar sama sekali — tertutup,
            // atau berada di belakang tab lain di dock yang sama. Gambar yang
            // dikembalikan lalu memperlihatkan panel yang lain sama sekali, dan
            // mengirimkannya dengan keterangan "dari sudut yang kamu minta"
            // adalah kebohongan yang tidak akan pernah diperiksa agen.
            std::future<bool> consumed = MainThreadQueue::Get().Submit(
                [&app]() { return !app.Context().cameraRequest.pending; });
            if (consumed.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
                return Text("The editor did not answer; it may be showing a modal dialog.",
                            /*isError=*/true);
            }
            if (!consumed.get()) {
                return Text(
                    "The camera was not moved: the Viewport panel is not being drawn. It is "
                    "probably behind another tab in the same dock. Bring it to the front "
                    "(Window > Viewport) and call this again.",
                    /*isError=*/true);
            }
        }

        std::vector<uint8_t> png;
        std::string error;
        bool cropped = false;
        std::future<bool> captured = MainThreadQueue::Get().Submit(
            [&app, &screenshot, &png, &error, &cropped]() {
                // **Rect diterjemahkan di main thread, bersama tangkapannya.**
                // Membacanya lebih dulu lalu memotong belakangan berarti memakai
                // tata letak frame yang lain — panel bisa saja sudah diseret di
                // antara keduanya, dan potongannya lalu memuat separuh panel
                // tetangga tanpa satu pun tanda.
                const EditorContext::ViewportRect& rect = app.Context().viewportRect;
                if (rect.size.x <= 0.0f || rect.size.y <= 0.0f || rect.mainSize.x <= 0.0f) {
                    return screenshot(/*crop=*/nullptr, png, error);
                }
                // Ukuran gambar belum diketahui sebelum ditangkap, jadi skalanya
                // diminta dari tangkapan itu sendiri: potong dua langkah, yang
                // pertama hanya untuk mengetahui lebarnya.
                std::vector<uint8_t> probe;
                std::string probeError;
                if (!screenshot(/*crop=*/nullptr, probe, probeError)) {
                    error = probeError;
                    return false;
                }
                // Lebar PNG ada di byte 16..19 kepala IHDR-nya.
                if (probe.size() < 24) {
                    png = std::move(probe);
                    return true;
                }
                const uint32_t imageWidth = (uint32_t(probe[16]) << 24) |
                                            (uint32_t(probe[17]) << 16) |
                                            (uint32_t(probe[18]) << 8) | uint32_t(probe[19]);
                const float scale = static_cast<float>(imageWidth) / rect.mainSize.x;
                CaptureRect crop;
                crop.x = static_cast<uint32_t>(std::max(0.0f, rect.position.x * scale));
                crop.y = static_cast<uint32_t>(std::max(0.0f, rect.position.y * scale));
                crop.width = static_cast<uint32_t>(std::max(1.0f, rect.size.x * scale));
                crop.height = static_cast<uint32_t>(std::max(1.0f, rect.size.y * scale));
                if (!screenshot(&crop, png, error)) {
                    return false;
                }
                cropped = true;
                return true;
            });
        if (captured.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
            return Text("The editor did not answer; it may be showing a modal dialog.",
                        /*isError=*/true);
        }
        if (!captured.get()) {
            return Text("Could not capture the viewport: " + error, /*isError=*/true);
        }

        ai::ToolResult result;
        // Disebut apa yang benar-benar dikirim. Gambar seluruh jendela yang
        // diperkenalkan sebagai viewport membuat agen menyimpulkan viewport-nya
        // memuat panel dan menu.
        result.text = std::string(cropped ? "Viewport only" : "Whole editor window (the "
                                                              "viewport rectangle is not known)") +
                      (hasFrom ? ", looking from the point you gave." : ", as it already was.");
        result.imageBytes = std::move(png);
        result.imageMimeType = "image/png";
        return result;
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "properties": {
    "from": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3,
             "description": "World-space eye position."},
    "look_at": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3,
                "description": "World-space point to aim at."}
  }
})";
    return tool;
}

ai::ResourceDefinition RecentLogs() {
    ai::ResourceDefinition resource;
    resource.uri = "simengine://logs/recent";
    resource.name = "Recent editor log";
    resource.description =
        "The last few hundred editor log lines. Attach this when diagnosing — it is the same "
        "data editor.log_tail returns, offered as context so you do not have to ask for it.";
    resource.mimeType = "application/json";
    // Alasan yang sama dengan tool-nya: `LogRing` bermutex, jadi ia terbaca
    // walaupun main thread sedang tertahan dialog modal.
    resource.needsMainThread = false;
    resource.read = []() { return LogTail(200, spdlog::level::trace).dump(2); };
    return resource;
}

}  // namespace

void RegisterEditorTools(ai::ToolRegistry& tools, ai::ResourceRegistry& resources,
                         EditorApp& app, ScreenshotFn screenshot) {
    tools.Register(EditorStatus(app));
    tools.Register(EditorActions(app));
    tools.Register(EditorExecuteAction(app));
    tools.Register(HistoryStep(app, /*undo=*/true));
    tools.Register(HistoryStep(app, /*undo=*/false));
    tools.Register(EditorLogTail());
    // Didaftarkan hanya bila benar-benar bisa. Tool yang ada tapi selalu gagal
    // membuat agen mencoba lagi dengan argumen yang berbeda, karena dari
    // sisinya kegagalan yang berulang terlihat seperti pemakaian yang salah.
    if (screenshot) {
        tools.Register(ViewportCapture(app, screenshot));
        tools.Register(EditorScreenshot(std::move(screenshot)));
    }

    resources.Register(RecentLogs());
}

}  // namespace sim::editor
