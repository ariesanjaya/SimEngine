#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Notifications.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"

#if SIM_WITH_LUA
#include "Sim/Script/ScriptRuntime.h"
#endif

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace sim::editor {
namespace {

#if SIM_WITH_LUA

constexpr ImVec4 kErrorColor(0.94f, 0.45f, 0.42f, 1.0f);
constexpr ImVec4 kOkColor(0.45f, 0.80f, 0.55f, 1.0f);
constexpr ImVec4 kHintColor(0.55f, 0.57f, 0.60f, 1.0f);

/// Kata kunci Lua, dilengkapi bersama nama yang ada di state.
///
/// Keduanya digabung dengan sengaja: `function` dan `sim.get_component` sama-sama
/// hal yang diketik pengguna, dan memisahkannya jadi dua mekanisme berbeda hanya
/// memaksa pengguna mengingat mana yang bisa dilengkapi dan mana yang tidak.
constexpr std::array<const char*, 22> kKeywords{
    "and",  "break", "do",     "else", "elseif", "end",   "false",  "for",
    "function", "goto", "if",  "in",   "local",  "nil",   "not",    "or",
    "repeat", "return", "then", "true", "until", "while"};

bool IsNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '.';
}

/// Baris tempat Lua melaporkan kesalahan, dari pesan berbentuk "nama:12: ...".
int LineFromError(const std::string& message) {
    const std::size_t colon = message.find(':');
    if (colon == std::string::npos) {
        return 0;
    }
    const std::size_t second = message.find(':', colon + 1);
    if (second == std::string::npos) {
        return 0;
    }
    const std::string digits = message.substr(colon + 1, second - colon - 1);
    if (digits.empty() ||
        !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return 0;
    }
    return std::stoi(digits);
}

/// Penyunting berkas `.lua` dengan pelengkapan nama dan penanda kesalahan.
///
/// Pelengkapannya memakai `ScriptRuntime::Complete`, yaitu state Lua yang
/// sungguh berjalan — bukan daftar nama yang ditulis tangan di panel ini.
/// Konsekuensinya penting: fungsi yang baru ditambahkan ke `sim` langsung ikut
/// terlengkapi, dan daftar yang ditampilkan tidak pernah bisa berbohong tentang
/// apa yang sebenarnya ada.
class ScriptEditorPanel final : public Panel {
public:
    ScriptEditorPanel()
        : Panel(panel_id::kScriptEditor, std::string(icons::kScript) + "  Script Editor",
                PanelCategory::Authoring) {}

    void OnDraw(EditorContext& context) override {
        if (context.assets == nullptr) {
            ImGui::TextDisabled("No asset database.");
            return;
        }
        DrawToolbar(context);
        ImGui::Separator();

        const float listWidth = ImGui::GetFontSize() * 10.0f;
        if (ImGui::BeginChild("##files", ImVec2(listWidth, 0.0f), ImGuiChildFlags_ResizeX)) {
            DrawFileList(context);
        }
        ImGui::EndChild();

        ImGui::SameLine();
        if (ImGui::BeginChild("##editor", ImVec2(0.0f, 0.0f))) {
            DrawEditor(context);
        }
        ImGui::EndChild();
    }

private:
    void DrawToolbar(EditorContext& context) {
        const bool hasFile = !openPath_.empty();
        ImGui::BeginDisabled(!hasFile || !dirty_);
        const std::string saveLabel = std::string(icons::kSave) + "  Save";
        // Ctrl+S diambil alih hanya selagi kursor berada di dalam kotak teks.
        // Di luar itu ia milik aksi Save Level: ActionRegistry melewati
        // pintasan ketika sebuah widget teks aktif, jadi syarat inilah yang
        // membuat keduanya tidak pernah menyala pada penekanan yang sama.
        if (ImGui::Button(saveLabel.c_str()) ||
            (hasFile && dirty_ && editorActive_ &&
             ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))) {
            Save(context);
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (!hasFile) {
            ImGui::TextColored(kHintColor, "Pick a script on the left");
            return;
        }
        ImGui::TextColored(kHintColor, "%s%s", openName_.c_str(), dirty_ ? " *" : "");

        ImGui::SameLine();
        if (syntaxError_.empty()) {
            ImGui::TextColored(kOkColor, "%s  syntax ok", icons::kLogInfo);
        } else {
            ImGui::TextColored(kErrorColor, "%s  line %d", icons::kLogError, errorLine_);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                ImGui::SetTooltip("%s", syntaxError_.c_str());
            }
        }
    }

    void DrawFileList(EditorContext& context) {
        for (const assets::AssetRecord& record : context.assets->All()) {
            if (record.type != assets::AssetType::Script) {
                continue;
            }
            const bool selected = record.relativePath == openPath_;
            if (ImGui::Selectable(record.name.c_str(), selected)) {
                Open(context, record);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                ImGui::SetTooltip("%s", record.relativePath.c_str());
            }
        }
    }

    void DrawEditor(EditorContext& context) {
        if (openPath_.empty()) {
            return;
        }
        // Tinggi disisakan untuk daftar pelengkapan, dihitung positif supaya
        // panel yang dipersempit tidak meminta tinggi negatif.
        const float completionHeight =
            candidates_.empty() ? 0.0f : ImGui::GetFrameHeightWithSpacing() * 3.0f;
        const float editorHeight = std::max(ImGui::GetContentRegionAvail().y - completionHeight,
                                            ImGui::GetTextLineHeightWithSpacing() * 3.0f);

        constexpr ImGuiInputTextFlags kFlags = ImGuiInputTextFlags_AllowTabInput |
                                               ImGuiInputTextFlags_CallbackAlways |
                                               ImGuiInputTextFlags_CallbackCharFilter;
        scripts_ = context.scripts;
        if (ImGui::InputTextMultiline("##text", &text_, ImVec2(-1.0f, editorHeight), kFlags,
                                      &EditorCallback, this)) {
            dirty_ = true;
            Recheck();
        }
        editorActive_ = ImGui::IsItemActive();

        DrawCompletions();
    }

    void DrawCompletions() {
        if (candidates_.empty()) {
            return;
        }
        // Daftarnya digambar di bawah kotak teks, bukan mengambang di dekat
        // kursor. ImGui tidak mengekspos posisi kursor maupun gulungan internal
        // InputTextMultiline, jadi popup mengambang akan meleset begitu berkasnya
        // digulung — dan daftar pelengkapan yang menunjuk tempat yang salah lebih
        // buruk daripada daftar yang selalu di tempat yang sama.
        ImGui::TextColored(kHintColor, "%s  Tab to accept, %s%s to choose, Esc to dismiss",
                           icons::kSearch, icons::kChevronUp, icons::kChevronDown);
        if (!ImGui::BeginChild("##completions", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders,
                               ImGuiWindowFlags_HorizontalScrollbar)) {
            ImGui::EndChild();
            return;
        }
        for (std::size_t i = 0; i < candidates_.size(); ++i) {
            const bool selected = i == static_cast<std::size_t>(selectedCandidate_);
            if (ImGui::Selectable(candidates_[i].c_str(), selected)) {
                pendingInsert_ = candidates_[i];
            }
            if (selected) {
                ImGui::SetScrollHereY(0.5f);
            }
        }
        ImGui::EndChild();
    }

    static int EditorCallback(ImGuiInputTextCallbackData* data) {
        auto* panel = static_cast<ScriptEditorPanel*>(data->UserData);
        if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) {
            return panel->FilterChar(data);
        }
        panel->UpdateCompletions(data);
        return 0;
    }

    /// Tab dipakai untuk menerima pelengkapan, bukan menyisipkan karakter tab —
    /// tapi hanya ketika ada yang bisa diterima. Kalau tidak, ia tetap
    /// berperilaku seperti tab biasa supaya indentasi tidak hilang.
    int FilterChar(ImGuiInputTextCallbackData* data) {
        if (data->EventChar != '\t' || candidates_.empty()) {
            return 0;
        }
        pendingInsert_ = candidates_[static_cast<std::size_t>(selectedCandidate_)];
        return 1;  // dibuang: karakter tab tidak jadi masuk teks
    }

    void UpdateCompletions(ImGuiInputTextCallbackData* data) {
        if (!pendingInsert_.empty()) {
            Insert(data, pendingInsert_);
            pendingInsert_.clear();
            candidates_.clear();
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            candidates_.clear();
            dismissedAt_ = data->CursorPos;
            return;
        }
        if (!candidates_.empty()) {
            const int count = static_cast<int>(candidates_.size());
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
                selectedCandidate_ = (selectedCandidate_ + 1) % count;
            } else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
                selectedCandidate_ = (selectedCandidate_ + count - 1) % count;
            }
        }

        const int start = WordStart(data);
        const std::string word(data->Buf + start,
                               static_cast<std::size_t>(data->CursorPos - start));
        if (word == lastWord_ && data->CursorPos == lastCursor_) {
            return;
        }
        lastWord_ = word;
        lastCursor_ = data->CursorPos;

        // Menunggu dua huruf: satu huruf cocok dengan hampir segalanya, dan
        // daftar yang muncul di setiap ketikan tunggal lebih mengganggu daripada
        // membantu.
        if (word.size() < 2 || data->CursorPos == dismissedAt_) {
            candidates_.clear();
            return;
        }
        Recompute(word);
    }

    int WordStart(const ImGuiInputTextCallbackData* data) const {
        int start = data->CursorPos;
        while (start > 0 && IsNameChar(data->Buf[start - 1])) {
            --start;
        }
        return start;
    }

    void Recompute(const std::string& word) {
        candidates_.clear();
        selectedCandidate_ = 0;
        if (scripts_ != nullptr) {
            candidates_ = scripts_->Complete(word);
        }
        // Kata kunci hanya dicocokkan ketika belum ada titik: `sim.fun` tidak
        // pernah dilanjutkan dengan `function`.
        if (word.find('.') == std::string::npos) {
            for (const char* keyword : kKeywords) {
                const std::string candidate(keyword);
                if (candidate.compare(0, word.size(), word) == 0) {
                    candidates_.push_back(candidate);
                }
            }
        }
        std::sort(candidates_.begin(), candidates_.end());
        candidates_.erase(std::unique(candidates_.begin(), candidates_.end()), candidates_.end());
        // Yang persis sama dengan yang sudah diketik tidak berguna sebagai saran.
        if (candidates_.size() == 1 && candidates_.front() == word) {
            candidates_.clear();
        }
    }

    void Insert(ImGuiInputTextCallbackData* data, const std::string& value) {
        const int start = WordStart(data);
        data->DeleteChars(start, data->CursorPos - start);
        data->InsertChars(start, value.c_str());
        dirty_ = true;
    }

    void Open(EditorContext& context, const assets::AssetRecord& record) {
        std::ifstream stream(context.assets->AbsolutePath(record));
        if (!stream) {
            return;
        }
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        text_ = buffer.str();
        openPath_ = record.relativePath;
        openName_ = record.name;
        dirty_ = false;
        candidates_.clear();
        lastWord_.clear();
        Recheck();
    }

    void Save(EditorContext& context) {
        const assets::AssetRecord* record = context.assets->FindByRelativePath(openPath_);
        if (record == nullptr) {
            return;
        }
        std::ofstream stream(context.assets->AbsolutePath(*record),
                             std::ios::binary | std::ios::trunc);
        if (!stream) {
            return;
        }
        stream << text_;
        stream.close();
        dirty_ = false;
        // Tidak perlu memberi tahu siapa pun bahwa berkasnya berubah: pemantau
        // sistem berkas yang sama yang memicu hot reload dari editor teks luar
        // akan menemukannya juga. Satu jalur, satu perilaku.
        if (context.notifications != nullptr) {
            context.notifications->Info("Saved " + openName_);
        }
    }

    /// Memeriksa sintaks tanpa menjalankan apa pun.
    void Recheck() {
        syntaxError_.clear();
        errorLine_ = 0;
        if (scripts_ == nullptr) {
            return;
        }
        // Hanya dimuat, tidak dipanggil. Menjalankan berkas yang sedang diketik
        // akan mengeksekusi kode setengah jadi setiap ketikan.
        const std::string error = scripts_->CheckSyntax(text_, openName_);
        if (!error.empty()) {
            syntaxError_ = error;
            errorLine_ = LineFromError(error);
        }
    }

    std::string text_;
    std::string openPath_;
    std::string openName_;
    bool dirty_ = false;

    std::string syntaxError_;
    int errorLine_ = 0;

    std::vector<std::string> candidates_;
    int selectedCandidate_ = 0;
    std::string pendingInsert_;
    std::string lastWord_;
    int lastCursor_ = -1;
    /// Posisi kursor saat daftar ditutup dengan Esc; ia tidak muncul lagi
    /// sampai kursornya bergerak.
    int dismissedAt_ = -1;
    /// Apakah kotak teks aktif pada frame lalu; menentukan siapa pemilik Ctrl+S.
    bool editorActive_ = false;

    script::ScriptRuntime* scripts_ = nullptr;
};

#else

class ScriptEditorPanel final : public Panel {
public:
    ScriptEditorPanel()
        : Panel(panel_id::kScriptEditor, std::string(icons::kScript) + "  Script Editor",
                PanelCategory::Authoring) {}

    void OnDraw(EditorContext& /*context*/) override {
        ImGui::TextDisabled("This build was configured without Lua.");
    }
};

#endif  // SIM_WITH_LUA

}  // namespace

SIM_REGISTER_PANEL(ScriptEditorPanel, 25)

}  // namespace sim::editor
