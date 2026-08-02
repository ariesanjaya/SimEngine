// Panel Asset Browser dan Statistics.
//
// Asset Browser masih memakai data contoh: AssetDatabase sungguhan baru ada di
// E5. Outliner dan Inspector sudah memakai World nyata dan pindah ke berkasnya
// masing-masing.

#include "Sim/Core/FrameLimiter.h"
#include "Sim/Editor/Command.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Editor/Selection.h"
#include "Sim/Editor/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

namespace sim::editor {
namespace {

class AssetBrowserPanel final : public Panel {
public:
    AssetBrowserPanel()
        : Panel(panel_id::kAssetBrowser, std::string(icons::kPanelAssetBrowser) + "  Asset Browser",
                PanelCategory::Assets) {}

    void OnDraw(EditorContext& /*context*/) override {
        widgets::SearchField("assetsearch", search_.data(), search_.size());
        ImGui::Separator();

        const float treeWidth = ImGui::GetContentRegionAvail().x * 0.38f;
        // AlwaysUseWindowPadding: child tanpa border secara bawaan tidak punya
        // padding sama sekali, sehingga thumbnail menempel persis di pemisah.
        ImGui::BeginChild("##tree", ImVec2(treeWidth, 0.0f),
                          ImGuiChildFlags_ResizeX | ImGuiChildFlags_AlwaysUseWindowPadding);
        for (std::size_t i = 0; i < kFolders.size(); ++i) {
            const bool selected = selectedFolder_ == static_cast<int>(i);
            const std::string label =
                std::string(selected ? icons::kFolderOpen : icons::kFolder) + "  " + kFolders[i];
            if (ImGui::Selectable(label.c_str(), selected)) {
                selectedFolder_ = static_cast<int>(i);
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##grid", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::TextDisabled("%s  %s", icons::kFolderOpen,
                            kFolders[static_cast<std::size_t>(selectedFolder_)]);
        ImGui::Separator();
        ImGui::Spacing();

        // Grid thumbnail sungguhan (dengan cache dan pemuatan malas) menyusul
        // di E5 bersama AssetDatabase.
        const ImGuiStyle& style = ImGui::GetStyle();
        const float cell = ImGui::GetFontSize() * 4.6f;
        const float step = cell + style.ItemSpacing.x;
        // +spacing di pembilang: kolom terakhir tidak butuh jarak sesudahnya,
        // jadi tanpa itu satu kolom hilang tepat saat lebarnya pas.
        const int columns =
            std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + style.ItemSpacing.x) /
                                         step));

        for (int i = 0; i < 8; ++i) {
            // SameLine diletakkan sebelum item, bukan sesudah. Pola "SameLine
            // kecuali item terakhir di baris" salah untuk item terakhir secara
            // keseluruhan dan menyisakan baris kosong.
            if (i % columns != 0) {
                ImGui::SameLine();
            }
            ImGui::BeginGroup();
            ImGui::PushID(i);
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::Button("##thumb", ImVec2(cell, cell));
            const ImVec2 iconSize = ImGui::CalcTextSize(icons::kAssetTexture);
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(origin.x + (cell - iconSize.x) * 0.5f,
                       origin.y + (cell - iconSize.y) * 0.5f),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), icons::kAssetTexture);
            ImGui::PopID();
            ImGui::TextDisabled("asset_%02d", i);
            ImGui::EndGroup();
        }
        ImGui::EndChild();
    }

private:
    static constexpr std::array<const char*, 6> kFolders{"materials", "Objects", "Schema",
                                                         "Scripts",   "textures", "Levels"};

    std::array<char, 96> search_{};
    int selectedFolder_ = 4;
};

class StatisticsPanel final : public Panel {
public:
    StatisticsPanel()
        : Panel(panel_id::kStatistics, std::string(icons::kPanelStatistics) + "  Statistics",
                PanelCategory::Debug) {
        SetOpen(false);
    }

    void OnDraw(EditorContext& context) override {
        if (context.frameLimiter != nullptr) {
            ImGui::Text("Frame   : %.2f ms",
                        context.frameLimiter->LastDeltaSeconds() * 1000.0);
            ImGui::Text("FPS     : %.1f", context.frameLimiter->SmoothedFps());
            ImGui::Text("Target  : %.0f Hz", context.frameLimiter->TargetFps());
        }
        ImGui::Separator();
        ImGui::TextWrapped("Frame lock: %s", context.frameLockReason.c_str());
        ImGui::Separator();
        ImGui::Text("Viewport: %ux%u",
                    context.viewportRenderer != nullptr ? context.viewportRenderer->Width() : 0u,
                    context.viewportRenderer != nullptr ? context.viewportRenderer->Height() : 0u);
        ImGui::Text("Renderer: %s", context.viewportRenderer != nullptr
                                        ? context.viewportRenderer->Name()
                                        : "(none)");
        ImGui::Separator();
        ImGui::Text("Selected: %zu",
                    context.selection != nullptr ? context.selection->Count() : 0u);
        ImGui::Text("History : %zu steps",
                    context.history != nullptr ? context.history->Entries().size() : 0u);
    }
};

}  // namespace

SIM_REGISTER_PANEL(AssetBrowserPanel, 20)
SIM_REGISTER_PANEL(StatisticsPanel, 55)

}  // namespace sim::editor
