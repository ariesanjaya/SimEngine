#include "Sim/AIBridge/McpServer.h"
#include "Sim/Editor/Command.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"

#include <imgui.h>

#include <string>
#include <vector>

namespace sim::editor {
namespace {

/// Keadaan server MCP: hidup atau tidak, di mana, dan berapa yang sudah dilayani.
///
/// **Ada supaya server ini tidak pernah berjalan tanpa ada yang tahu.** Ia
/// memberi siapa pun yang menyambung kendali atas project yang sedang dibuka —
/// membuat entity, mengubah komponen, menjalankan aksi menu. Sebuah kemampuan
/// sebesar itu yang satu-satunya tandanya adalah sebaris di berkas log adalah
/// kemampuan yang akan mengejutkan seseorang.
///
/// Tombol matinya di sini karena alasan yang sama. Yang bisa dinyalakan harus
/// bisa dimatikan tanpa menutup editor, kalau tidak satu-satunya cara menolak
/// agen adalah berhenti bekerja.
class AiBridgePanel final : public Panel {
public:
    AiBridgePanel()
        : Panel(panel_id::kAiBridge, std::string(icons::kLogInfo) + "  AI Bridge",
                PanelCategory::Debug) {
        SetOpen(false);
    }

    void OnDraw(EditorContext& context) override {
        ai::McpServer* server = context.mcpServer;
        if (server == nullptr) {
            ImGui::TextDisabled("This build has no MCP server.");
            return;
        }

        const bool running = server->IsRunning();
        ImGui::TextUnformatted("Status");
        ImGui::SameLine();
        if (running) {
            ImGui::TextColored(ImVec4(0.35f, 0.80f, 0.40f, 1.0f), "listening");
        } else {
            ImGui::TextDisabled("stopped");
        }

        if (running) {
            const std::string url = server->Url();
            ImGui::Separator();
            ImGui::TextUnformatted(url.c_str());
            ImGui::SameLine();
            // Disalin, bukan hanya ditampilkan: yang dibutuhkan pengguna
            // berikutnya adalah menempelkannya ke `claude mcp add`.
            if (ImGui::SmallButton("Copy##url")) {
                ImGui::SetClipboardText(url.c_str());
            }

            const std::string token = server->BearerToken();
            if (token.empty()) {
                ImGui::TextDisabled("No token — anything on this machine can connect.");
            } else {
                // **Ditampilkan penuh, bukan disamarkan.** Ia dibangkitkan tiap
                // kali editor menyala dan tidak melindungi apa pun di luar mesin
                // ini; yang membutuhkannya adalah orang yang sedang duduk di
                // depannya, dan menyamarkannya hanya membuat ia tidak bisa
                // menyalinnya.
                ImGui::Text("Token %s", token.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Copy##token")) {
                    ImGui::SetClipboardText(token.c_str());
                }
            }

            ImGui::Separator();

            // **Pilihan modenya di sini, dan berlaku untuk semua klien.**
            // Server yang sama melayani Claude Code dan panel AI Assistant;
            // mode yang hanya ditegakkan salah satunya adalah mode yang tidak
            // menjanjikan apa-apa.
            ai::PermissionMode mode = server->Mode();
            const struct {
                ai::PermissionMode value;
                const char* label;
                const char* help;
            } kModes[] = {
                {ai::PermissionMode::ReadOnly, "read-only",
                 "Only tools that read. Everything else is refused, and does not even appear "
                 "in the agent's tool list."},
                {ai::PermissionMode::Ask, "ask",
                 "Every tool that changes data asks first. With nothing able to ask, they are "
                 "refused rather than run quietly."},
                {ai::PermissionMode::Auto, "auto",
                 "Tools that change data run on their own. Tools that cannot be undone still "
                 "ask."},
            };
            ImGui::TextUnformatted("Permission");
            for (const auto& entry : kModes) {
                ImGui::SameLine();
                if (ImGui::RadioButton(entry.label, mode == entry.value)) {
                    server->SetMode(entry.value);
                    mode = entry.value;
                }
            }
            for (const auto& entry : kModes) {
                if (entry.value == mode) {
                    ImGui::TextWrapped("%s", entry.help);
                }
            }

            ImGui::Separator();
            ImGui::Text("Served   %llu",
                        static_cast<unsigned long long>(server->RequestCount()));
            const uint64_t refused = server->RefusedByPolicyCount();
            if (refused > 0) {
                // Bukan merah: ini bukan tanda bahaya, ini mode izin yang
                // bekerja. Yang layak dilihat orang adalah bahwa agen memang
                // sedang mencoba lebih dari yang diizinkan.
                ImGui::TextDisabled("Refused  %llu (permission)",
                                    static_cast<unsigned long long>(refused));
            }
            const uint64_t rejected = server->RejectedCount();
            if (rejected > 0) {
                // Merah, dan bukan karena dramatis: angka ini hanya naik kalau
                // ada sesuatu di mesin ini yang menyambung tanpa token yang
                // benar, dan itu layak dilihat.
                ImGui::TextColored(ImVec4(0.90f, 0.45f, 0.35f, 1.0f), "Rejected %llu",
                                   static_cast<unsigned long long>(rejected));
            } else {
                ImGui::TextDisabled("Rejected 0");
            }
        }

        ImGui::Separator();
        if (running) {
            if (ImGui::Button("Stop server")) {
                server->Stop();
            }
        } else {
            ImGui::BeginDisabled(!context.mcpStart);
            if (ImGui::Button("Start server")) {
                context.mcpStart();
            }
            ImGui::EndDisabled();
        }

        ImGui::Spacing();

        // **Undo semua yang dilakukan agen.** Tiap mutasi agen dibungkus
        // transaksi bernama "AI: <tool>" sejak A1, jadi yang dicari di sini
        // adalah entri `AI:` paling awal yang masih ada di riwayat — bukan
        // sebuah penanda sesi yang harus dijaga tetap benar di tempat kedua.
        if (CommandHistory* history = context.history) {
            const std::vector<CommandHistory::Entry>& entries = history->Entries();
            int firstAgentEntry = -1;
            for (int at = 0; at < static_cast<int>(entries.size()); ++at) {
                if (entries[static_cast<std::size_t>(at)].name.rfind("AI: ", 0) == 0) {
                    firstAgentEntry = at;
                    break;
                }
            }
            ImGui::BeginDisabled(firstAgentEntry < 0);
            if (ImGui::Button("Undo everything the agent did")) {
                history->JumpTo(firstAgentEntry);
            }
            ImGui::EndDisabled();
            if (firstAgentEntry < 0) {
                ImGui::TextDisabled("The agent has not changed anything in this history.");
            } else {
                // Dikatakan apa adanya. Riwayatnya satu garis, jadi mundur ke
                // sebelum perubahan agen yang pertama juga membatalkan
                // suntingan orang yang datang sesudahnya — dan tombol yang
                // tidak menyebutkan itu akan mengejutkan seseorang.
                ImGui::TextWrapped(
                    "Rewinds to before the agent's first change. Anything you changed after "
                    "that point is undone too; redo brings it back.");
            }
            ImGui::Spacing();
        }

        ImGui::TextDisabled("Register with Claude Code:");
        const std::string command =
            "claude mcp add simengine --transport http " +
            (running ? server->Url() : std::string("http://127.0.0.1:7777/mcp"));
        ImGui::TextWrapped("%s", command.c_str());
        if (ImGui::SmallButton("Copy##command")) {
            ImGui::SetClipboardText(command.c_str());
        }
    }
};

}  // namespace

SIM_REGISTER_PANEL(AiBridgePanel, 61)

}  // namespace sim::editor
