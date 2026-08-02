#include "Sim/Editor/Actions.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Notifications.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Editor/Widgets.h"

#include <imgui.h>

#include <array>
#include <cstring>
#include <string>

namespace sim::editor {
namespace {

/// Menangkap kombinasi tombol berikutnya yang ditekan pengguna.
///
/// Mengembalikan chord, atau ImGuiKey_None bila belum ada. Modifier saja tidak
/// dihitung sebagai chord — kalau dihitung, menekan Ctrl untuk mengetik Ctrl+S
/// sudah langsung mengunci binding-nya.
ImGuiKeyChord CaptureChord() {
    ImGuiKeyChord mods = 0;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl) {
        mods |= ImGuiMod_Ctrl;
    }
    if (io.KeyShift) {
        mods |= ImGuiMod_Shift;
    }
    if (io.KeyAlt) {
        mods |= ImGuiMod_Alt;
    }
    if (io.KeySuper) {
        mods |= ImGuiMod_Super;
    }

    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
        const auto imKey = static_cast<ImGuiKey>(key);
        if (imKey >= ImGuiKey_LeftCtrl && imKey <= ImGuiKey_RightSuper) {
            continue;
        }
        if (imKey == ImGuiKey_ReservedForModCtrl || imKey == ImGuiKey_ReservedForModShift ||
            imKey == ImGuiKey_ReservedForModAlt || imKey == ImGuiKey_ReservedForModSuper) {
            continue;
        }
        if (ImGui::IsKeyPressed(imKey, false)) {
            return mods | imKey;
        }
    }
    return ImGuiKey_None;
}

class PreferencesPanel final : public Panel {
public:
    PreferencesPanel()
        : Panel(panel_id::kPreferences, std::string(icons::kSettings) + "  Preferences",
                PanelCategory::Debug) {
        SetOpen(false);
    }

    void OnDraw(EditorContext& context) override {
        ActionRegistry* actions = context.actions;
        if (actions == nullptr) {
            ImGui::TextDisabled("No action registry.");
            return;
        }

        if (ImGui::Button((std::string(icons::kSave) + "  Save").c_str())) {
            actions->Invoke("preferences.save_shortcuts");
        }
        ImGui::SameLine();
        if (ImGui::Button((std::string(icons::kRefresh) + "  Reset all").c_str())) {
            for (const Action& action : actions->All()) {
                actions->ResetShortcut(action.id);
            }
            if (context.notifications != nullptr) {
                context.notifications->Info("Shortcuts reset to defaults");
            }
        }
        ImGui::SameLine();
        widgets::SearchField("prefsearch", filter_.data(), filter_.size(), "Filter actions...");
        ImGui::Separator();

        if (!capturingId_.empty()) {
            ImGui::TextColored(ImVec4(0.95f, 0.76f, 0.35f, 1.0f),
                               "%s  Press a key combination for '%s' (Escape to cancel)",
                               icons::kLogWarn, capturingId_.c_str());
            HandleCapture(*actions, context);
            ImGui::Separator();
        }

        if (!ImGui::BeginTable("##shortcuts", 3,
                               ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                   ImGuiTableFlags_SizingStretchProp)) {
            return;
        }
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthStretch, 0.35f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.20f);
        ImGui::TableHeadersRow();

        std::string lastCategory;
        for (const Action& action : actions->All()) {
            if (!Matches(action)) {
                continue;
            }
            if (action.category != lastCategory) {
                lastCategory = action.category;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::SeparatorText(action.category.c_str());
            }

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(action.id.c_str());
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(action.label.c_str());
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                ImGui::SetTooltip("%s", action.id.c_str());
            }

            ImGui::TableNextColumn();
            const std::string text = actions->ShortcutText(action.id);
            const std::string buttonLabel = text.empty() ? "(none)" : text;
            if (ImGui::Button(buttonLabel.c_str(), ImVec2(-1.0f, 0.0f))) {
                capturingId_ = action.id;
            }

            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Reset")) {
                actions->ResetShortcut(action.id);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) {
                actions->SetShortcut(action.id, ImGuiKey_None);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

private:
    bool Matches(const Action& action) const {
        if (filter_[0] == '\0') {
            return true;
        }
        return action.label.find(filter_.data()) != std::string::npos ||
               action.id.find(filter_.data()) != std::string::npos;
    }

    void HandleCapture(ActionRegistry& actions, EditorContext& context) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            capturingId_.clear();
            return;
        }
        const ImGuiKeyChord chord = CaptureChord();
        if (chord == ImGuiKey_None) {
            return;
        }
        if (ActionRegistry::ChordToString(chord).empty()) {
            // Tombol yang tidak punya nama stabil tidak bisa disimpan ke
            // config, jadi lebih baik ditolak sekarang daripada hilang diam-diam
            // saat editor dijalankan lagi.
            if (context.notifications != nullptr) {
                context.notifications->Warning("That key cannot be used as a shortcut");
            }
            capturingId_.clear();
            return;
        }

        const std::string conflict = actions.FindConflict(chord, capturingId_);
        actions.SetShortcut(capturingId_, chord);
        if (!conflict.empty() && context.notifications != nullptr) {
            // Bentrokan diizinkan tapi diberitahukan: kadang memang disengaja
            // (aksi yang tidak pernah aktif bersamaan), dan menolaknya akan
            // lebih mengganggu daripada membantu.
            context.notifications->Warning("Also bound to '" + conflict + "'");
        }
        capturingId_.clear();
    }

    std::array<char, 96> filter_{};
    std::string capturingId_;
};

}  // namespace

SIM_REGISTER_PANEL(PreferencesPanel, 70)

}  // namespace sim::editor
