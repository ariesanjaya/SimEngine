#include "Sim/Editor/Widgets.h"

#include "Sim/Editor/Icons.h"

#include <imgui.h>

namespace sim::editor::widgets {
namespace {

constexpr std::array<const char*, 3> kAxisNames{"X", "Y", "Z"};
constexpr std::array<ImVec4, 3> kAxisColors{ImVec4(0.85f, 0.33f, 0.35f, 1.0f),
                                            ImVec4(0.47f, 0.76f, 0.38f, 1.0f),
                                            ImVec4(0.34f, 0.55f, 0.92f, 1.0f)};

}  // namespace

Vec3Result DragVec3(const char* id, float values[3], float speed, const char* format) {
    Vec3Result result;
    ImGui::PushID(id);

    const ImGuiStyle& style = ImGui::GetStyle();
    const float fullWidth = ImGui::GetContentRegionAvail().x - kPanelRightMargin;

    // Anggaran per kolom mencakup semua yang dipakai kolom itu: huruf sumbu,
    // jarak setelahnya, dan kotak angkanya. Kalau jaraknya tidak ikut dihitung,
    // baris X/Y/Z melewati tepi kanan dan padding-nya terlihat lebih sempit
    // daripada tombol selebar panel di atasnya.
    constexpr float kLabelGap = 4.0f;
    const float labelWidth = ImGui::CalcTextSize("X").x + kLabelGap;
    const float columnWidth = (fullWidth - style.ItemInnerSpacing.x * 2.0f) / 3.0f;
    const float fieldWidth = columnWidth - labelWidth;

    for (int i = 0; i < 3; ++i) {
        const auto axis = static_cast<std::size_t>(i);
        if (i > 0) {
            ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
        }
        ImGui::PushID(i);

        // Huruf sumbu digambar di atas InvisibleButton, bukan lewat Text():
        // teks biasa bukan item interaktif, sehingga IsItemActive() padanya
        // selalu false dan seretnya tidak akan pernah terdeteksi.
        const float frameHeight = ImGui::GetFrameHeight();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##axis", ImVec2(labelWidth, frameHeight));

        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            values[i] += ImGui::GetIO().MouseDelta.x * speed;
            result.edited = true;
        }
        result.finished |= ImGui::IsItemDeactivated();

        const ImVec2 textSize = ImGui::CalcTextSize(kAxisNames[axis]);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(origin.x, origin.y + (frameHeight - textSize.y) * 0.5f),
            ImGui::GetColorU32(kAxisColors[axis]), kAxisNames[axis]);

        // Jarak nol: labelWidth sudah mencakup kLabelGap.
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::SetNextItemWidth(fieldWidth);
        // v_min == v_max berarti tanpa batas: nilai yang diketik diterima apa
        // adanya, termasuk negatif dan di luar jangkauan seret.
        result.edited |= ImGui::DragFloat("##v", &values[i], speed, 0.0f, 0.0f, format);
        result.finished |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::PopID();
    }

    ImGui::PopID();
    return result;
}

bool SearchField(const char* id, char* buffer, std::size_t bufferSize, const char* hint) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", icons::kSearch);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-kPanelRightMargin);
    ImGui::PushID(id);
    const bool changed = ImGui::InputTextWithHint("##search", hint, buffer, bufferSize);
    ImGui::PopID();
    return changed;
}

bool IconButton(const char* icon, const char* tooltip, bool active) {
    const float size = ImGui::GetFrameHeight();
    // PushID pada tooltip, bukan ikon: dua tombol berbeda sering memakai glyph
    // yang sama, dan tanpa ini keduanya berbagi ID lalu saling mencuri klik.
    ImGui::PushID(tooltip);
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    const bool pressed = ImGui::Button(icon, ImVec2(size, size));
    if (active) {
        ImGui::PopStyleColor();
    }
    ImGui::PopID();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return pressed;
}

bool ToolbarButton(const char* icon, const char* tooltip, bool active) {
    const bool pressed = IconButton(icon, tooltip, active);
    ImGui::SameLine();
    return pressed;
}

void ToolbarSeparator() {
    // Tanpa SameLine() di awal: ToolbarButton() sudah menyisakan kursor di
    // baris yang sama, memanggilnya lagi akan menggeser ganda.
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
}

void PropertyLabel(const char* label, float labelWidth) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(labelWidth > 0.0f ? labelWidth : ImGui::GetFontSize() * 7.0f);
}

bool ComponentHeader(const char* icon, const char* label, bool defaultOpen) {
    const std::string text = std::string(icon) + "  " + label;
    return ImGui::CollapsingHeader(text.c_str(),
                                   defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
}

}  // namespace sim::editor::widgets
