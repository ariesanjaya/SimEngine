#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"

#if SIM_WITH_LUA
#include "Sim/Script/ScriptRuntime.h"
#endif

#include <imgui.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace sim::editor {
namespace {

#if SIM_WITH_LUA

constexpr ImVec4 kInputColor(0.62f, 0.72f, 0.85f, 1.0f);
constexpr ImVec4 kErrorColor(0.94f, 0.45f, 0.42f, 1.0f);
constexpr ImVec4 kHintColor(0.55f, 0.57f, 0.60f, 1.0f);

/// Karakter yang boleh menyusun nama untuk autocomplete. Titik ikut karena
/// `sim.get_com` harus dilengkapi sebagai satu kesatuan, bukan dipotong di
/// titiknya.
bool IsNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '.';
}

/// REPL Lua: mengetik, melihat hasilnya, dan membaca traceback saat salah.
///
/// Dipisah dari Console biasa dengan sengaja. Console adalah aliran log yang
/// hanya dibaca; yang ini adalah percakapan, dan mencampur keduanya membuat
/// jawaban atas apa yang baru diketik tenggelam di antara log frame.
class LuaConsolePanel final : public Panel {
public:
    LuaConsolePanel()
        : Panel(panel_id::kLuaConsole, std::string(icons::kPanelLuaConsole) + "  Lua Console",
                PanelCategory::Debug) {}

    void OnDraw(EditorContext& context) override {
        if (context.scripts == nullptr) {
            ImGui::TextColored(kHintColor, "Lua runtime is not available in this build.");
            return;
        }
        DrawToolbar(context);
        ImGui::Separator();
        DrawTranscript();
        DrawInput(context);
    }

private:
    struct Entry {
        enum class Kind { Input, Value, Error, Hint };
        Kind kind = Kind::Input;
        /// Terisi untuk Input, Error, dan Hint.
        std::string text;
        /// Terisi untuk Value.
        script::EvalNode value;
    };

    void DrawToolbar(EditorContext& context) {
        const std::string clearLabel = std::string(icons::kDelete) + "  Clear";
        if (ImGui::Button(clearLabel.c_str())) {
            entries_.clear();
        }
        ImGui::SameLine();
        ImGui::TextColored(kHintColor, "%s",
                           context.playing ? "playing — scripts are live"
                                           : "editing — Play to run scripts");
    }

    void DrawTranscript() {
        // Tinggi disisakan untuk baris input: tanpa ini transkrip mendorong
        // kolom ketiknya keluar dari panel begitu isinya panjang. Dihitung
        // sebagai angka positif, bukan sisa negatif, supaya panel yang dipersempit
        // sampai lebih pendek daripada baris inputnya tetap menyisakan sesuatu
        // untuk digambar alih-alih meminta tinggi negatif.
        const float inputHeight = ImGui::GetFrameHeightWithSpacing();
        const float height = std::max(ImGui::GetContentRegionAvail().y - inputHeight,
                                      ImGui::GetTextLineHeightWithSpacing());
        if (!ImGui::BeginChild("##transcript", ImVec2(0.0f, height), ImGuiChildFlags_None,
                               ImGuiWindowFlags_HorizontalScrollbar)) {
            ImGui::EndChild();
            return;
        }

        for (std::size_t i = 0; i < entries_.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const Entry& entry = entries_[i];
            switch (entry.kind) {
                case Entry::Kind::Input:
                    ImGui::TextColored(kInputColor, "> %s", entry.text.c_str());
                    break;
                case Entry::Kind::Error:
                    ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
                    ImGui::TextUnformatted(entry.text.c_str());
                    ImGui::PopStyleColor();
                    break;
                case Entry::Kind::Hint:
                    ImGui::TextColored(kHintColor, "%s", entry.text.c_str());
                    break;
                case Entry::Kind::Value: DrawValue(entry.value); break;
            }
            ImGui::PopID();
        }

        if (scrollToBottom_) {
            ImGui::SetScrollHereY(1.0f);
            scrollToBottom_ = false;
        }
        ImGui::EndChild();
    }

    void DrawValue(const script::EvalNode& node) {
        const std::string label =
            node.label.empty() ? node.value : node.label + " = " + node.value;
        if (node.children.empty()) {
            ImGui::TextUnformatted(label.c_str());
            return;
        }
        // Tabel dilipat, bukan dicetak rata: mengetik sesuatu yang mengembalikan
        // tabel besar tidak boleh menghapus seluruh transkrip dari layar.
        if (ImGui::TreeNode(label.c_str())) {
            for (const script::EvalNode& child : node.children) {
                DrawValue(child);
            }
            ImGui::TreePop();
        }
    }

    void DrawInput(EditorContext& context) {
        scripts_ = context.scripts;
        constexpr ImGuiInputTextFlags kFlags =
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
            ImGuiInputTextFlags_CallbackCompletion;

        ImGui::SetNextItemWidth(-1.0f);
        const bool submitted =
            ImGui::InputTextWithHint("##lua_input", "lua...", input_.data(), input_.size(),
                                     kFlags, &InputCallback, this);
        if (submitted) {
            Submit(context);
            // Fokus dikembalikan supaya beberapa perintah bisa diketik berturut-
            // turut tanpa mengklik kolomnya lagi.
            ImGui::SetKeyboardFocusHere(-1);
        }
        if (focusInput_) {
            ImGui::SetKeyboardFocusHere(-1);
            focusInput_ = false;
        }
    }

    void Submit(EditorContext& context) {
        const std::string code(input_.data());
        input_[0] = '\0';
        if (code.empty()) {
            return;
        }

        entries_.push_back({Entry::Kind::Input, code, {}});
        // Perintah yang sama persis tidak digandakan di riwayat: menekan Panah
        // Atas setelah menjalankan sesuatu sepuluh kali harus melompati satu
        // entri, bukan sepuluh.
        if (history_.empty() || history_.back() != code) {
            history_.push_back(code);
        }
        historyCursor_ = -1;

        const script::EvalResult result = context.scripts->Evaluate(code);
        if (!result.ok) {
            entries_.push_back({Entry::Kind::Error, result.error, {}});
        } else {
            for (const script::EvalNode& value : result.values) {
                Entry entry;
                entry.kind = Entry::Kind::Value;
                entry.value = value;
                entries_.push_back(std::move(entry));
            }
        }
        scrollToBottom_ = true;
    }

    static int InputCallback(ImGuiInputTextCallbackData* data) {
        auto* panel = static_cast<LuaConsolePanel*>(data->UserData);
        if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
            panel->StepHistory(data);
        } else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
            panel->Complete(data);
        }
        return 0;
    }

    void StepHistory(ImGuiInputTextCallbackData* data) {
        if (history_.empty()) {
            return;
        }
        const int last = static_cast<int>(history_.size()) - 1;
        if (data->EventKey == ImGuiKey_UpArrow) {
            historyCursor_ = historyCursor_ < 0 ? last : std::max(0, historyCursor_ - 1);
        } else if (data->EventKey == ImGuiKey_DownArrow) {
            if (historyCursor_ < 0) {
                return;
            }
            // Melewati entri terbaru mengembalikan kolom ke keadaan kosong,
            // bukan menahannya di perintah terakhir.
            historyCursor_ = historyCursor_ + 1 > last ? -1 : historyCursor_ + 1;
        } else {
            return;
        }
        data->DeleteChars(0, data->BufTextLen);
        if (historyCursor_ >= 0) {
            data->InsertChars(0, history_[static_cast<std::size_t>(historyCursor_)].c_str());
        }
    }

    void Complete(ImGuiInputTextCallbackData* data) {
        if (scripts_ == nullptr) {
            return;
        }
        int start = data->CursorPos;
        while (start > 0 && IsNameChar(data->Buf[start - 1])) {
            --start;
        }
        const std::string word(data->Buf + start,
                               static_cast<std::size_t>(data->CursorPos - start));
        if (word.empty()) {
            return;
        }

        const std::vector<std::string> matches = scripts_->Complete(word);
        if (matches.empty()) {
            return;
        }
        if (matches.size() == 1) {
            data->DeleteChars(start, data->CursorPos - start);
            data->InsertChars(start, matches.front().c_str());
            return;
        }

        // Beberapa kandidat: yang disisipkan hanya awalan bersamanya, lalu
        // daftarnya dicetak. Menyisipkan salah satu kandidat secara sepihak
        // memaksa pengguna menghapusnya kembali.
        std::string prefix = matches.front();
        for (const std::string& match : matches) {
            std::size_t i = 0;
            while (i < prefix.size() && i < match.size() && prefix[i] == match[i]) {
                ++i;
            }
            prefix.resize(i);
        }
        if (prefix.size() > word.size()) {
            data->DeleteChars(start, data->CursorPos - start);
            data->InsertChars(start, prefix.c_str());
        }

        std::string list;
        for (const std::string& match : matches) {
            if (!list.empty()) {
                list += "   ";
            }
            list += match;
        }
        entries_.push_back({Entry::Kind::Hint, list, {}});
        scrollToBottom_ = true;
    }

    std::vector<Entry> entries_;
    std::vector<std::string> history_;
    std::array<char, 512> input_{};
    /// -1 berarti sedang tidak menelusuri riwayat.
    int historyCursor_ = -1;
    bool scrollToBottom_ = false;
    bool focusInput_ = false;
    /// Hanya dipakai di dalam callback InputText, yang tidak menerima
    /// EditorContext. Disegarkan tiap frame dari context.
    script::ScriptRuntime* scripts_ = nullptr;
};

#else

/// Build tanpa Lua tetap punya panelnya, supaya daftar panel — dan karena itu
/// layout dock tersimpan milik pengguna — tidak berubah antar konfigurasi build.
class LuaConsolePanel final : public Panel {
public:
    LuaConsolePanel()
        : Panel(panel_id::kLuaConsole, std::string(icons::kPanelLuaConsole) + "  Lua Console",
                PanelCategory::Debug) {}

    void OnDraw(EditorContext& /*context*/) override {
        ImGui::TextDisabled("This build was configured without Lua.");
    }
};

#endif  // SIM_WITH_LUA

}  // namespace

SIM_REGISTER_PANEL(LuaConsolePanel, 52)

}  // namespace sim::editor
