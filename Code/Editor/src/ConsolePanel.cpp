#include "Sim/Core/Log.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Editor/Widgets.h"

#include <imgui.h>

#include <array>
#include <cstring>

namespace sim::editor {
namespace {

ImVec4 LevelColor(spdlog::level::level_enum level) {
    switch (level) {
        case spdlog::level::trace: return ImVec4(0.55f, 0.57f, 0.60f, 1.0f);
        case spdlog::level::debug: return ImVec4(0.62f, 0.72f, 0.85f, 1.0f);
        case spdlog::level::warn: return ImVec4(0.95f, 0.76f, 0.35f, 1.0f);
        case spdlog::level::err: return ImVec4(0.94f, 0.45f, 0.42f, 1.0f);
        case spdlog::level::critical: return ImVec4(1.00f, 0.35f, 0.55f, 1.0f);
        default: return ImVec4(0.86f, 0.87f, 0.88f, 1.0f);
    }
}

const char* LevelIcon(spdlog::level::level_enum level) {
    switch (level) {
        case spdlog::level::trace: return icons::kLogTrace;
        case spdlog::level::debug: return icons::kLogDebug;
        case spdlog::level::warn: return icons::kLogWarn;
        case spdlog::level::err: return icons::kLogError;
        case spdlog::level::critical: return icons::kLogCritical;
        default: return icons::kLogInfo;
    }
}

class ConsolePanel final : public Panel {
public:
    ConsolePanel()
        : Panel(panel_id::kConsole, std::string(icons::kPanelConsole) + "  Console",
                PanelCategory::Debug) {}

    void OnDraw(EditorContext& /*context*/) override {
        DrawToolbar();
        ImGui::Separator();
        DrawEntries();
    }

private:
    void DrawToolbar() {
        const std::string clearLabel = std::string(icons::kDelete) + "  Clear";
        if (ImGui::Button(clearLabel.c_str())) {
            LogRing::Get().Clear();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &autoScroll_);
        ImGui::SameLine();

        ImGui::TextDisabled("%s", icons::kFilter);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
        constexpr std::array<const char*, 5> kLevels{"Trace", "Debug", "Info", "Warn", "Error"};
        ImGui::Combo("##level", &minLevelIndex_, kLevels.data(),
                     static_cast<int>(kLevels.size()));

        ImGui::SameLine();
        ImGui::TextDisabled("%s", icons::kSearch);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##filter", "search...", filter_.data(), filter_.size());
    }

    void DrawEntries() {
        const std::vector<LogEntry> entries = LogRing::Get().Snapshot();
        const auto minLevel = static_cast<spdlog::level::level_enum>(minLevelIndex_);
        const bool hasFilter = filter_[0] != '\0';

        if (!ImGui::BeginChild("##log", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                               ImGuiWindowFlags_HorizontalScrollbar)) {
            ImGui::EndChild();
            return;
        }

        // Baris log bisa puluhan ribu. Clipper membuat hanya baris yang terlihat
        // yang benar-benar digambar, sehingga panel tetap ringan.
        std::vector<const LogEntry*> visible;
        visible.reserve(entries.size());
        for (const LogEntry& entry : entries) {
            if (entry.level < minLevel) {
                continue;
            }
            if (hasFilter && entry.message.find(filter_.data()) == std::string::npos &&
                entry.channel.find(filter_.data()) == std::string::npos) {
                continue;
            }
            visible.push_back(&entry);
        }

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(visible.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const LogEntry& entry = *visible[static_cast<std::size_t>(i)];
                ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(entry.level));
                ImGui::Text("%s  [%s] %s", LevelIcon(entry.level), entry.channel.c_str(),
                            entry.message.c_str());
                ImGui::PopStyleColor();
            }
        }
        clipper.End();

        // Hanya menggulir otomatis kalau pengguna memang sedang di dasar —
        // kalau tidak, membaca log lama jadi mustahil saat pesan terus masuk.
        if (autoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }

    std::array<char, 128> filter_{};
    int minLevelIndex_ = static_cast<int>(spdlog::level::debug);
    bool autoScroll_ = true;
};

}  // namespace

SIM_REGISTER_PANEL(ConsolePanel, 50)

}  // namespace sim::editor
