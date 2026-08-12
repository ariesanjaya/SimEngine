#include "Sim/Editor/EditorShell.h"

#include "Sim/Core/FrameLimiter.h"
#include "Sim/Editor/Actions.h"
#include "Sim/Editor/Command.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelManager.h"
#include "Sim/Editor/Selection.h"
#include "Sim/Editor/Widgets.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <cstdio>

namespace sim::editor {
namespace {

constexpr const char* kHostWindowName = "##SimEditorHost";
constexpr const char* kDockspaceName = "SimEditorDockspace";

float StatusBarHeight() {
    const ImGuiStyle& style = ImGui::GetStyle();
    return ImGui::GetFontSize() + style.FramePadding.y * 2.0f + style.WindowPadding.y;
}

/// Item menu untuk hal yang belum ada implementasinya. Sengaja tetap
/// ditampilkan supaya struktur menu tidak berubah lagi setelah pengguna
/// terbiasa dengan posisinya.
void PlannedMenuItem(const char* icon, const char* label, const char* shortcut = nullptr) {
    const std::string text = std::string(icon) + "  " + label;
    ImGui::MenuItem(text.c_str(), shortcut, false, false);
}

/// Item menu yang membuka sebuah panel dan memberinya fokus.
///
/// **Tercentang saat panelnya terbuka**, sama seperti menu Window: entri menu
/// yang tidak memperlihatkan keadaan membuat orang mengkliknya dua kali dan
/// menutup kembali panel yang baru saja dibukanya.
void PanelMenuItem(PanelManager& panels, const char* icon, const char* label, const char* id) {
    Panel* panel = panels.Find(id);
    if (panel == nullptr) {
        // Panelnya tidak terdaftar di build ini — Lua mati, misalnya. Ditampilkan
        // nonaktif alih-alih disembunyikan: susunan menu yang berubah menurut
        // opsi build adalah susunan yang tidak bisa diajarkan.
        PlannedMenuItem(icon, label);
        return;
    }
    const std::string text = std::string(icon) + "  " + label;
    if (ImGui::MenuItem(text.c_str(), nullptr, panel->IsOpen())) {
        panel->SetOpen(true);
        panel->SetFocused(true);
    }
}

}  // namespace

void EditorShell::SetWorkspace(Workspace workspace) {
    workspace_ = workspace;
    resetLayoutRequested_ = true;
}

void EditorShell::Draw(EditorContext& context, PanelManager& panels) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float statusHeight = StatusBarHeight();

    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, viewport->WorkSize.y - statusHeight));
    ImGui::SetNextWindowViewport(viewport->ID);

    // Jendela host adalah wadah dockspace. Ia tidak boleh punya padding,
    // rounding, atau border sendiri — kalau punya, akan terlihat sebagai
    // bingkai ganda di sekeliling seluruh editor.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    constexpr ImGuiWindowFlags kHostFlags =
        ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::Begin(kHostWindowName, nullptr, kHostFlags);
    ImGui::PopStyleVar(3);

    DrawMenuBar(context, panels);
    DrawToolbar(context);

    const ImGuiID dockspaceId = ImGui::GetID(kDockspaceName);
    // Layout bawaan hanya dibangun kalau belum ada yang tersimpan. Membangunnya
    // tanpa syarat akan menghapus susunan panel yang sudah diatur pengguna
    // setiap kali editor dijalankan.
    if (!layoutInitialized_) {
        layoutInitialized_ = true;
        if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr) {
            resetLayoutRequested_ = true;
        }
    }
    if (resetLayoutRequested_) {
        resetLayoutRequested_ = false;
        BuildLayout(dockspaceId, workspace_);
    }
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);

    ImGui::End();

    panels.Draw(context);
    DrawStatusBar(context);

    if (showImGuiDemo_) {
        ImGui::ShowDemoWindow(&showImGuiDemo_);
    }
    if (showStyleEditor_) {
        ImGui::Begin("Style Editor", &showStyleEditor_);
        ImGui::ShowStyleEditor();
        ImGui::End();
    }
}

void EditorShell::DrawMenuBar(EditorContext& context, PanelManager& panels) {
    if (!ImGui::BeginMenuBar()) {
        return;
    }
    ActionRegistry* actions = context.actions;

    if (ImGui::BeginMenu("File")) {
        if (actions != nullptr) {
            actions->MenuItem("level.new");
            actions->MenuItem("level.open");
            actions->MenuItem("level.save");
            actions->MenuItem("level.save_as");
            actions->MenuItem("level.reload");
            ImGui::Separator();
            actions->MenuItem("project.close");
            ImGui::Separator();
            actions->MenuItem("editor.exit");
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        if (actions != nullptr) {
            actions->MenuItem("edit.undo");
            actions->MenuItem("edit.redo");
            ImGui::Separator();
            actions->MenuItem("entity.copy");
            actions->MenuItem("entity.paste");
            actions->MenuItem("entity.paste_as_child");
            actions->MenuItem("entity.duplicate");
            actions->MenuItem("entity.delete");
            ImGui::Separator();
            actions->MenuItem("selection.clear");
            actions->MenuItem("edit.clear_history");
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Entity")) {
        if (actions != nullptr) {
            actions->MenuItem("entity.create");
            actions->MenuItem("entity.duplicate");
            actions->MenuItem("entity.delete");
        }
        ImGui::Separator();
        // Prefab belum punya aksinya; entrinya tetap ditampilkan supaya susunan
        // menu tidak berubah lagi setelah orang terbiasa dengan posisinya.
        PlannedMenuItem(icons::kPrefab, "Save as Prefab...");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Tools")) {
        // Membuka panelnya, bukan aksi tersendiri: "Material Editor" di menu ini
        // dan "Material Editor" di menu Window adalah hal yang sama, dan dua
        // jalur menuju satu panel adalah dua jalur yang suatu saat berselisih
        // soal panel mana yang sedang terbuka.
        PanelMenuItem(panels, icons::kMaterialEditor, "Material Editor",
                      panel_id::kMaterialEditor);
        PanelMenuItem(panels, icons::kParticleEditor, "Particle Editor",
                      panel_id::kParticleEditor);
        PanelMenuItem(panels, icons::kTerrainEditor, "Terrain Editor", panel_id::kTerrainEditor);
        PanelMenuItem(panels, icons::kVegetationEditor, "Vegetation Editor",
                      panel_id::kVegetationEditor);
        PanelMenuItem(panels, icons::kAnimationEditor, "Animation Editor",
                      panel_id::kAnimationEditor);
        ImGui::Separator();
        PanelMenuItem(panels, icons::kLua, "Lua Console", panel_id::kLuaConsole);
        PanelMenuItem(panels, icons::kScript, "Script Editor", panel_id::kScriptEditor);
        PanelMenuItem(panels, icons::kNodeGraph, "Graph Editor", panel_id::kGraphEditor);
        ImGui::Separator();
        // Keduanya memang belum ada panelnya.
        PlannedMenuItem(icons::kAiAssistant, "AI Assistant");
        PlannedMenuItem(icons::kAiBridge, "AI Bridge");
        ImGui::EndMenu();
    }

    if (context.drawScriptMenu) {
        if (ImGui::BeginMenu("Scripts")) {
            context.drawScriptMenu();
            ImGui::EndMenu();
        }
    }

    if (ImGui::BeginMenu("View")) {
        if (ImGui::BeginMenu("Workspace")) {
            constexpr std::array<Workspace, 3> kWorkspaces{Workspace::Level, Workspace::Authoring,
                                                           Workspace::Debug};
            for (Workspace workspace : kWorkspaces) {
                if (ImGui::MenuItem(WorkspaceName(workspace), nullptr, workspace_ == workspace)) {
                    SetWorkspace(workspace);
                }
            }
            ImGui::EndMenu();
        }
        if (actions != nullptr) {
            actions->MenuItem("view.reset_layout");
        }
        ImGui::Separator();
        ImGui::MenuItem("Dear ImGui Demo", nullptr, &showImGuiDemo_);
        ImGui::MenuItem("Style Editor", nullptr, &showStyleEditor_);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Window")) {
        panels.DrawWindowMenu();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        PlannedMenuItem(icons::kSettings, "Documentation");
        PlannedMenuItem(icons::kEntity, "About SimEngine");
        ImGui::EndMenu();
    }

    // Kontrol Play didorong ke kanan menu bar, seperti pada tata letak acuan.
    const float playControlsWidth = ImGui::GetFontSize() * 7.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - playControlsWidth);
    const bool playing = context.playing;
    ImGui::BeginDisabled(playing || context.scripts == nullptr);
    if (ImGui::SmallButton(icons::kPlay) && context.requestPlay) {
        context.requestPlay();
    }
    ImGui::EndDisabled();
    widgets::Tooltip(context.scripts == nullptr ? "Lua is not built in" : "Play");

    ImGui::SameLine();
    // Pause masih menunggu pemisahan waktu simulasi dari waktu editor (E9).
    ImGui::BeginDisabled();
    ImGui::SmallButton(icons::kPause);
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!playing);
    if (ImGui::SmallButton(icons::kStop) && context.requestStop) {
        context.requestStop();
    }
    ImGui::EndDisabled();
    widgets::Tooltip("Stop and restore the scene");

    ImGui::EndMenuBar();
}

void EditorShell::DrawToolbar(EditorContext& context) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight() + 8.0f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
    ImGui::BeginChild("##toolbar", ImVec2(0.0f, height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPosX(style.WindowPadding.x);

    // Alat transform dan snapping baru hidup di E4 bersama gizmo.
    ImGui::BeginDisabled();
    widgets::ToolbarButton(icons::kSelect, "Select (Q)", true);
    widgets::ToolbarButton(icons::kMove, "Move (W)");
    widgets::ToolbarButton(icons::kRotate, "Rotate (E)");
    widgets::ToolbarButton(icons::kScale, "Scale (R)");
    widgets::ToolbarSeparator();
    widgets::ToolbarButton(icons::kSpaceWorld, "Space: World");
    widgets::ToolbarButton(icons::kPivot, "Pivot: Center");
    widgets::ToolbarButton(icons::kSnap, "Snapping");
    ImGui::EndDisabled();
    widgets::ToolbarSeparator();

    ActionRegistry* actions = context.actions;
    if (actions != nullptr) {
        ImGui::BeginDisabled(!actions->IsEnabled("edit.undo"));
        if (widgets::ToolbarButton(icons::kUndo, "Undo (Ctrl+Z)")) {
            actions->Invoke("edit.undo");
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!actions->IsEnabled("edit.redo"));
        if (widgets::ToolbarButton(icons::kRedo, "Redo (Ctrl+Shift+Z)")) {
            actions->Invoke("edit.redo");
        }
        ImGui::EndDisabled();
    }

    widgets::ToolbarSeparator();
    ImGui::AlignTextToFramePadding();
    const bool dirty = context.history != nullptr && context.history->IsDirty();
    ImGui::TextDisabled("%s %s%s", icons::kAssetLevel, context.levelName.c_str(),
                        dirty ? " *" : "");

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void EditorShell::DrawStatusBar(EditorContext& context) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float height = StatusBarHeight();

    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - height));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, height));
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::Begin("##statusbar", nullptr, kFlags);
    ImGui::AlignTextToFramePadding();

    const std::size_t selected = context.selection != nullptr ? context.selection->Count() : 0;
    if (selected > 0) {
        ImGui::Text("%s  %zu selected", icons::kSelect, selected);
    } else {
        ImGui::Text("%s  Ready", icons::kLogInfo);
    }

    const float fps = context.frameLimiter != nullptr
                          ? static_cast<float>(context.frameLimiter->SmoothedFps())
                          : ImGui::GetIO().Framerate;
    char right[256];
    std::snprintf(right, sizeof(right), "%s %.1f fps  |  locked to %.0f Hz (%s)  |  %s %s",
                  icons::kPanelStatistics, static_cast<double>(fps),
                  static_cast<double>(context.lockedFps), context.frameLockReason.c_str(),
                  icons::kFolder,
                  context.projectPath.empty() ? "(no project)" : context.projectPath.c_str());
    const float textWidth = ImGui::CalcTextSize(right).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - textWidth - ImGui::GetStyle().WindowPadding.x);
    ImGui::TextDisabled("%s", right);

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

}  // namespace sim::editor
