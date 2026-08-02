#include "Sim/Editor/Command.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"

#include <imgui.h>

#include <string>

namespace sim::editor {
namespace {

/// Daftar seluruh langkah yang bisa dibatalkan, dengan penanda posisi saat ini.
///
/// Bukan sekadar alat debug: melihat riwayat sebagai daftar membuat "mundur
/// lima langkah" jadi satu klik alih-alih menekan Ctrl+Z sambil menghitung, dan
/// membuat jelas operasi mana yang tergabung jadi satu entri.
class HistoryPanel final : public Panel {
public:
    HistoryPanel()
        : Panel(panel_id::kHistory, std::string(icons::kUndo) + "  History",
                PanelCategory::Debug) {
        SetOpen(false);
    }

    void OnDraw(EditorContext& context) override {
        CommandHistory* history = context.history;
        if (history == nullptr) {
            ImGui::TextDisabled("No history.");
            return;
        }

        ImGui::BeginDisabled(!history->CanUndo());
        if (ImGui::Button(icons::kUndo)) {
            history->Undo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!history->CanRedo());
        if (ImGui::Button(icons::kRedo)) {
            history->Redo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%zu steps · %.1f KB", history->Entries().size(),
                            static_cast<double>(history->MemoryUsed()) / 1024.0);
        ImGui::Separator();

        if (!ImGui::BeginChild("##entries")) {
            ImGui::EndChild();
            return;
        }

        // Entri nol adalah keadaan sebelum perubahan apa pun. Tanpa baris ini,
        // tidak ada cara mengklik untuk kembali ke titik awal.
        if (ImGui::Selectable("(initial state)", history->Cursor() == 0)) {
            history->JumpTo(0);
        }

        const auto& entries = history->Entries();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const int step = static_cast<int>(i) + 1;
            const bool applied = step <= history->Cursor();
            ImGui::PushID(static_cast<int>(i));
            // Langkah yang sudah di-undo tetap ditampilkan tapi diredupkan:
            // masih bisa dicapai lewat redo sampai ada perubahan baru.
            if (!applied) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            }
            if (ImGui::Selectable(entries[i].name.c_str(), step == history->Cursor())) {
                history->JumpTo(step);
            }
            if (!applied) {
                ImGui::PopStyleColor();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
};

}  // namespace

SIM_REGISTER_PANEL(HistoryPanel, 60)

}  // namespace sim::editor
