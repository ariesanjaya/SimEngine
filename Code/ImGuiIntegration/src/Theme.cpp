#include "Sim/ImGuiIntegration/Theme.h"

#include <imgui.h>

namespace sim::imguix {
namespace {

constexpr ImVec4 Rgb(int r, int g, int b, float a = 1.0f) {
    return ImVec4(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
                  static_cast<float>(b) / 255.0f, a);
}

// Gaya dasar sebelum penskalaan DPI. Disimpan supaya ApplyScale() bisa selalu
// menghitung dari nol, bukan menumpuk skala di atas skala sebelumnya.
ImGuiStyle g_baseStyle;
bool g_baseStyleCaptured = false;

}  // namespace

void ApplyDarkTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark();

    style.WindowRounding = 0.0f;
    style.ChildRounding = 2.0f;
    style.FrameRounding = 3.0f;
    style.PopupRounding = 3.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 3.0f;

    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.TabBorderSize = 0.0f;

    // Jarak diselaraskan dengan font UI 13 px. Padding yang terlalu longgar
    // pada font kecil membuat panel terasa kosong dan memaksa scroll lebih
    // awal — di editor, jumlah baris yang muat sekali pandang lebih berharga
    // daripada kelegaan.
    style.WindowPadding = ImVec2(7.0f, 6.0f);
    style.FramePadding = ImVec2(6.0f, 3.0f);
    style.CellPadding = ImVec2(5.0f, 2.0f);
    style.ItemSpacing = ImVec2(7.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(5.0f, 4.0f);
    style.IndentSpacing = 16.0f;
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 10.0f;

    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.SeparatorTextBorderSize = 1.0f;

    ImVec4* colors = style.Colors;
    const ImVec4 accent = Rgb(58, 122, 200);
    const ImVec4 accentDim = Rgb(45, 92, 150);

    colors[ImGuiCol_Text] = Rgb(219, 221, 224);
    colors[ImGuiCol_TextDisabled] = Rgb(122, 126, 133);
    colors[ImGuiCol_WindowBg] = Rgb(40, 42, 46);
    colors[ImGuiCol_ChildBg] = Rgb(36, 38, 42);
    colors[ImGuiCol_PopupBg] = Rgb(46, 48, 53);
    colors[ImGuiCol_Border] = Rgb(24, 25, 28);
    colors[ImGuiCol_BorderShadow] = Rgb(0, 0, 0, 0.0f);

    colors[ImGuiCol_FrameBg] = Rgb(30, 31, 35);
    colors[ImGuiCol_FrameBgHovered] = Rgb(52, 55, 61);
    colors[ImGuiCol_FrameBgActive] = Rgb(60, 64, 71);

    colors[ImGuiCol_TitleBg] = Rgb(28, 29, 33);
    colors[ImGuiCol_TitleBgActive] = Rgb(33, 35, 39);
    colors[ImGuiCol_TitleBgCollapsed] = Rgb(28, 29, 33);
    colors[ImGuiCol_MenuBarBg] = Rgb(45, 47, 52);

    colors[ImGuiCol_ScrollbarBg] = Rgb(30, 31, 35);
    colors[ImGuiCol_ScrollbarGrab] = Rgb(69, 72, 79);
    colors[ImGuiCol_ScrollbarGrabHovered] = Rgb(85, 89, 97);
    colors[ImGuiCol_ScrollbarGrabActive] = accentDim;

    colors[ImGuiCol_CheckMark] = accent;
    colors[ImGuiCol_SliderGrab] = Rgb(96, 101, 110);
    colors[ImGuiCol_SliderGrabActive] = accent;

    colors[ImGuiCol_Button] = Rgb(56, 59, 65);
    colors[ImGuiCol_ButtonHovered] = Rgb(70, 74, 82);
    colors[ImGuiCol_ButtonActive] = accentDim;

    colors[ImGuiCol_Header] = Rgb(52, 55, 61);
    colors[ImGuiCol_HeaderHovered] = Rgb(64, 68, 76);
    colors[ImGuiCol_HeaderActive] = accentDim;

    colors[ImGuiCol_Separator] = Rgb(28, 29, 33);
    colors[ImGuiCol_SeparatorHovered] = accentDim;
    colors[ImGuiCol_SeparatorActive] = accent;

    colors[ImGuiCol_ResizeGrip] = Rgb(60, 64, 71, 0.0f);
    colors[ImGuiCol_ResizeGripHovered] = accentDim;
    colors[ImGuiCol_ResizeGripActive] = accent;

    colors[ImGuiCol_Tab] = Rgb(35, 37, 41);
    colors[ImGuiCol_TabHovered] = Rgb(58, 62, 69);
    colors[ImGuiCol_TabSelected] = Rgb(48, 51, 57);
    colors[ImGuiCol_TabSelectedOverline] = accent;
    colors[ImGuiCol_TabDimmed] = Rgb(31, 33, 36);
    colors[ImGuiCol_TabDimmedSelected] = Rgb(41, 43, 48);
    colors[ImGuiCol_TabDimmedSelectedOverline] = Rgb(60, 64, 71);

    colors[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
    colors[ImGuiCol_DockingEmptyBg] = Rgb(26, 27, 30);

    colors[ImGuiCol_TableHeaderBg] = Rgb(45, 47, 52);
    colors[ImGuiCol_TableBorderStrong] = Rgb(28, 29, 33);
    colors[ImGuiCol_TableBorderLight] = Rgb(34, 36, 40);
    colors[ImGuiCol_TableRowBg] = Rgb(0, 0, 0, 0.0f);
    colors[ImGuiCol_TableRowBgAlt] = Rgb(255, 255, 255, 0.02f);

    colors[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    colors[ImGuiCol_NavCursor] = accent;
    colors[ImGuiCol_DragDropTarget] = Rgb(231, 175, 61);

    g_baseStyle = style;
    g_baseStyleCaptured = true;
}

void ApplyScale(float scale) {
    if (!g_baseStyleCaptured) {
        ApplyDarkTheme();
    }
    if (scale <= 0.0f) {
        scale = 1.0f;
    }

    ImGuiStyle scaled = g_baseStyle;
    scaled.ScaleAllSizes(scale);
    ImGui::GetStyle() = scaled;
}

}  // namespace sim::imguix
