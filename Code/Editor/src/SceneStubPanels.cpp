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


        // --- Post-process (E8.8) ---
        //
        // Sakelar mode ada di sini, bukan tersembunyi, karena eksposur otomatis
        // punya satu kasus di mana ia justru menghalangi: menyetel material dan
        // lampu. Setiap kali sebuah lampu dicerahkan, pengukur mengimbanginya —
        // dan yang terlihat adalah lampu yang "tidak berpengaruh apa-apa".
        ImGui::Separator();
        ImGui::Checkbox("Post-process", &context.post.enabled);
        if (context.post.enabled) {
            const char* modes[] = {"Automatic", "Manual"};
            int mode = static_cast<int>(context.post.exposureMode);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
            if (ImGui::Combo("Exposure", &mode, modes, IM_ARRAYSIZE(modes))) {
                context.post.exposureMode = static_cast<render::ExposureMode>(mode);
            }
            if (context.post.exposureMode == render::ExposureMode::Manual) {
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
                ImGui::SliderFloat("EV100", &context.post.manualEv100, -8.0f, 8.0f, "%.1f");
            } else {
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
                ImGui::SliderFloat("Adapt bright", &context.post.adaptationBrightenSeconds, 0.0f,
                                   4.0f, "%.2f s");
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
                ImGui::SliderFloat("Adapt dark", &context.post.adaptationDarkenSeconds, 0.0f,
                                   4.0f, "%.2f s");
            }
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
            ImGui::SliderFloat("Compensation", &context.post.exposureCompensation, -5.0f, 5.0f,
                               "%.2f EV");
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

        ImGui::Checkbox("Screen-space layer", &context.gi.screenTrace);

        const char* views[] = {"Off",       "Albedo",      "Normal",     "Irradiance",
                               "Ray count", "March steps", "Trace layer"};
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
            // Biaya komposit clipmap. Di CPU, jadi ia tidak muncul di tabel
            // pass GPU di atas — dan justru angka inilah yang dibatasi anggaran
            // 0,4 ms rencana GI.
            const uint64_t voxels = context.viewportRenderer->SdfVoxelsWritten();
            const float updateMs = context.viewportRenderer->SdfUpdateMilliseconds();
            if (context.gi.enabled) {
                // **Puncak jendela, bukan hanya angka frame ini.** Yang dibatasi
                // anggaran adalah frame terberat — yaitu frame saat kamera
                // melintasi batas voxel — dan frame itu satu di antara belasan.
                // Angka sesaat memperlihatkannya hanya kalau kebetulan terbaca
                // pada frame yang tepat.
                sdfHistory_[sdfCursor_] = {updateMs, voxels};
                sdfCursor_ = (sdfCursor_ + 1) % sdfHistory_.size();
                const SdfSample peak = *std::max_element(
                    sdfHistory_.begin(), sdfHistory_.end(),
                    [](const SdfSample& a, const SdfSample& b) { return a.ms < b.ms; });
                ImGui::Text("SDF CPU : %.3f ms  (%llu voxels)", static_cast<double>(updateMs),
                            static_cast<unsigned long long>(voxels));
                ImGui::Text("SDF peak: %.3f ms  (%llu voxels)", static_cast<double>(peak.ms),
                            static_cast<unsigned long long>(peak.voxels));
            } else {
                sdfHistory_.fill({});
            }
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

private:
    struct SdfSample {
        float ms = 0.0f;
        uint64_t voxels = 0;
    };

    // Dua detik pada 60 Hz: cukup panjang untuk memuat lintasan batas voxel,
    // cukup pendek untuk melupakan pembangunan penuh saat GI baru dinyalakan.
    std::array<SdfSample, 120> sdfHistory_{};
    std::size_t sdfCursor_ = 0;
};

}  // namespace

SIM_REGISTER_PANEL(StatisticsPanel, 55)

}  // namespace sim::editor
