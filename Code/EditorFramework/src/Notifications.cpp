#include "Sim/Editor/Notifications.h"

#include "Sim/Editor/Icons.h"

#include <imgui.h>

#include <algorithm>

namespace sim::editor {
namespace {

struct LevelStyle {
    const char* icon;
    ImVec4 color;
};

LevelStyle StyleOf(NotificationLevel level) {
    switch (level) {
        case NotificationLevel::Success:
            return {icons::kLogInfo, ImVec4(0.45f, 0.78f, 0.45f, 1.0f)};
        case NotificationLevel::Warning:
            return {icons::kLogWarn, ImVec4(0.95f, 0.76f, 0.35f, 1.0f)};
        case NotificationLevel::Error:
            return {icons::kLogError, ImVec4(0.94f, 0.45f, 0.42f, 1.0f)};
        default:
            return {icons::kLogInfo, ImVec4(0.62f, 0.72f, 0.85f, 1.0f)};
    }
}

constexpr std::size_t kMaxVisible = 6;

}  // namespace

void Notifications::Post(NotificationLevel level, std::string message, float seconds) {
    // Menumpuk tanpa batas akan menutupi editor. Yang terlama dibuang, bukan
    // yang terbaru, karena pesan paling baru yang paling mungkin relevan.
    if (items_.size() >= kMaxVisible) {
        items_.erase(items_.begin());
    }
    items_.push_back(Item{level, std::move(message), seconds, seconds});
}

void Notifications::Draw(float deltaSeconds) {
    for (Item& item : items_) {
        item.remaining -= deltaSeconds;
    }
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [](const Item& item) { return item.remaining <= 0.0f; }),
                 items_.end());
    if (items_.empty()) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f;
    float y = viewport->WorkPos.y + viewport->WorkSize.y - pad;

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing;

    // Digambar dari bawah ke atas supaya toast terbaru selalu di posisi yang
    // sama, tepat di atas status bar — mata tidak perlu mencari.
    for (std::size_t i = items_.size(); i-- > 0;) {
        const Item& item = items_[i];
        const LevelStyle style = StyleOf(item.level);

        // Memudar di detik terakhir, supaya hilangnya tidak terasa seperti
        // kedipan.
        const float alpha = std::min(1.0f, item.remaining);

        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad, y),
                                ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.92f * alpha);
        ImGui::SetNextWindowViewport(viewport->ID);

        char id[32];
        std::snprintf(id, sizeof(id), "##toast%zu", i);
        if (ImGui::Begin(id, nullptr, kFlags)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(style.color.x, style.color.y,
                                                        style.color.z, alpha));
            ImGui::TextUnformatted(style.icon);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
            ImGui::TextUnformatted(item.message.c_str());
            ImGui::PopStyleVar();
            y = ImGui::GetWindowPos().y - 6.0f;
        }
        ImGui::End();
    }
}

}  // namespace sim::editor
