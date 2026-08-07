// Panel Statistics.
//
// Asset Browser pindah ke AssetBrowserPanel.cpp begitu ia memakai AssetDatabase
// sungguhan; Outliner dan Inspector lebih dulu pindah ke berkasnya masing-masing.

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

        // Waktu GPU per pass. **Ini alat diagnostik yang paling sering dipakai**
        // begitu ada pass yang anggarannya harus dijaga — dan yang pertama
        // menuntutnya adalah GI, yang seluruh rencananya disusun sekitar angka
        // 3,0 ms. Angka yang tidak bisa diukur bukan anggaran melainkan harapan.
        if (context.viewportRenderer != nullptr) {
            const std::span<const render::PassTiming> timings =
                context.viewportRenderer->PassTimings();
            ImGui::Separator();
            if (timings.empty()) {
                ImGui::TextDisabled("GPU timing unavailable");
            } else {
                float total = 0.0f;
                for (const render::PassTiming& timing : timings) {
                    total += timing.milliseconds;
                }
                ImGui::Text("GPU     : %.3f ms", static_cast<double>(total));
                if (ImGui::BeginTable("##passes", 2,
                                      ImGuiTableFlags_SizingStretchProp |
                                          ImGuiTableFlags_RowBg)) {
                    for (const render::PassTiming& timing : timings) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(timing.name.data(),
                                               timing.name.data() + timing.name.size());
                        ImGui::TableNextColumn();
                        ImGui::Text("%.3f ms", static_cast<double>(timing.milliseconds));
                    }
                    ImGui::EndTable();
                }
            }
        }


        // --- Global illumination (M0) ---
        //
        // Pemilih backend terlihat sejak sekarang, bukan nanti bersama backend
        // kedua. Mesin ini punya RT core, jadi pemilihan otomatis tidak akan
        // pernah menjalankan jalur SDF — jalur yang justru harus bekerja di
        // setiap GPU. Tanpa sakelar ini, ia berhenti diuji tanpa ada yang
        // menyadarinya.
        ImGui::Separator();
        ImGui::Checkbox("GI enabled", &context.gi.enabled);

        const char* backends[] = {"Auto", "Force SDF", "Force ray query"};
        int backend = static_cast<int>(context.gi.backend);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        if (ImGui::Combo("Backend", &backend, backends, IM_ARRAYSIZE(backends))) {
            context.gi.backend = static_cast<render::TraceBackendPreference>(backend);
        }

        const char* views[] = {"Off",        "Albedo",    "Normal",
                               "Irradiance", "Ray count", "March steps"};
        int view = static_cast<int>(context.gi.debugView);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        if (ImGui::Combo("Debug view", &view, views, IM_ARRAYSIZE(views))) {
            context.gi.debugView = static_cast<render::GiDebugView>(view);
        }

        if (context.viewportRenderer != nullptr) {
            const render::TraceBackendSelection selection =
                context.viewportRenderer->GiBackend();
            // Yang dipakai, bukan yang diminta — keduanya bisa berbeda, dan
            // perbedaan yang tidak ditampilkan adalah perbedaan yang tidak
            // diketahui siapa pun.
            ImGui::Text("Active  : %s", render::ToString(selection.kind));
            if (selection.fellBack) {
                ImGui::TextColored(ImVec4(0.94f, 0.72f, 0.35f, 1.0f), "%s",
                                   selection.reason.c_str());
            } else {
                ImGui::TextDisabled("%s", selection.reason.c_str());
            }
        }

        ImGui::Separator();
        ImGui::Text("Selected: %zu",
                    context.selection != nullptr ? context.selection->Count() : 0u);
        ImGui::Text("History : %zu steps",
                    context.history != nullptr ? context.history->Entries().size() : 0u);
    }
};

}  // namespace

SIM_REGISTER_PANEL(StatisticsPanel, 55)

}  // namespace sim::editor
