// Panel Time of Day.
//
// **Bentuknya mengikuti editor Time-of-Day CryEngine**: satu penggaris waktu di
// atas, tempat dan musim di bawahnya, lalu satu kurva per parameter atmosfer.
// Yang membuatnya berguna bukan jumlah parameternya melainkan bahwa menyeret
// waktunya langsung menggerakkan matahari beserta bayangannya — dan itu berlaku
// karena panel ini tidak menggambar apa pun sendiri: ia hanya menyunting angka
// yang sudah mengalir dari `LightComponent` ke cascade bayangan sejak E8.3.

#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Render/TimeOfDay.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

namespace sim::editor {
namespace {

/// Jam sebagai teks jam:menit. Angka pecahan jam tidak terbaca sebagai waktu,
/// dan waktu adalah satu-satunya hal yang dibaca orang di panel ini.
std::string FormatHour(float hour) {
    const float wrapped = render::WrapHour(hour);
    const int hours = static_cast<int>(wrapped);
    const int minutes = static_cast<int>((wrapped - static_cast<float>(hours)) * 60.0f + 0.5f);
    std::array<char, 16> text{};
    std::snprintf(text.data(), text.size(), "%02d:%02d", hours, minutes % 60);
    return std::string(text.data());
}

/// Menggambar sebuah kurva harian beserta kunci-kuncinya.
///
/// **Kurvanya digambar, bukan hanya didaftar.** Daftar angka tidak
/// memperlihatkan bentuk, dan bentuk itulah yang disetel orang saat menyusun
/// sebuah hari — di mana fajar mulai, seberapa lama senja bertahan.
void DrawCurve(const char* label, render::TimeOfDayCurve& curve, float currentHour,
               bool asColor, float valueMax) {
    if (!ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    ImGui::PushID(label);

    const ImVec2 size(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 4.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("##plot", size);

    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                        IM_COL32(24, 25, 28, 255));

    // Isi kurvanya, satu pita per jam. Untuk kurva warna pitanya diwarnai; untuk
    // kurva skalar digambar sebagai grafik garis.
    constexpr int kSteps = 96;
    for (int i = 0; i < kSteps; ++i) {
        const float hour = 24.0f * static_cast<float>(i) / static_cast<float>(kSteps);
        const Vec3 value = curve.Evaluate(hour, Vec3(0.0f));
        const float x0 = origin.x + size.x * static_cast<float>(i) / static_cast<float>(kSteps);
        const float x1 = origin.x + size.x * static_cast<float>(i + 1) / static_cast<float>(kSteps);
        if (asColor) {
            const ImU32 color = IM_COL32(static_cast<int>(std::clamp(value.x, 0.0f, 1.0f) * 255.0f),
                                         static_cast<int>(std::clamp(value.y, 0.0f, 1.0f) * 255.0f),
                                         static_cast<int>(std::clamp(value.z, 0.0f, 1.0f) * 255.0f),
                                         255);
            draw->AddRectFilled(ImVec2(x0, origin.y), ImVec2(x1 + 1.0f, origin.y + size.y), color);
        } else {
            const float t = std::clamp(value.x / std::max(valueMax, 1e-4f), 0.0f, 1.0f);
            const float y = origin.y + size.y * (1.0f - t);
            draw->AddRectFilled(ImVec2(x0, y), ImVec2(x1 + 1.0f, origin.y + size.y),
                                IM_COL32(90, 140, 220, 200));
        }
    }

    // Penanda kunci, supaya bentuk yang digambar bisa dihubungkan ke angka yang
    // disunting di bawahnya.
    for (std::size_t i = 0; i < curve.KeyCount(); ++i) {
        const float x = origin.x + size.x * render::WrapHour(curve.Key(i).hour) / 24.0f;
        draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + size.y),
                      IM_COL32(255, 255, 255, 90));
    }
    // Saat sekarang.
    const float cursor = origin.x + size.x * render::WrapHour(currentHour) / 24.0f;
    draw->AddLine(ImVec2(cursor, origin.y), ImVec2(cursor, origin.y + size.y),
                  IM_COL32(255, 200, 60, 255), 2.0f);

    // Kunci-kuncinya. Menyunting jamnya lewat `Set` pada jam baru lalu menghapus
    // yang lama akan menukar urutannya di tengah iterasi, jadi perubahannya
    // dikumpulkan dulu.
    int removeIndex = -1;
    struct Move {
        std::size_t index;
        float hour;
    };
    Move move{0, -1.0f};

    for (std::size_t i = 0; i < curve.KeyCount(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const render::TimeOfDayKey& key = curve.Key(i);

        float hour = key.hour;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
        if (ImGui::DragFloat("##hour", &hour, 0.05f, 0.0f, 24.0f, "%.2f")) {
            move = {i, hour};
        }
        ImGui::SameLine();
        Vec3 value = key.value;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        const bool changed = asColor
                                 ? ImGui::ColorEdit3("##value", &value.x,
                                                     ImGuiColorEditFlags_Float |
                                                         ImGuiColorEditFlags_HDR)
                                 : ImGui::DragFloat("##value", &value.x, 0.02f, 0.0f, valueMax);
        if (changed) {
            curve.Set(key.hour, asColor ? value : Vec3(value.x));
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", FormatHour(key.hour).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            removeIndex = static_cast<int>(i);
        }
        ImGui::PopID();
    }

    if (move.hour >= 0.0f) {
        const Vec3 value = curve.Key(move.index).value;
        curve.Remove(move.index);
        curve.Set(move.hour, value);
    } else if (removeIndex >= 0 && curve.KeyCount() > 1) {
        // Kunci terakhir tidak boleh dihapus: kurva kosong mengembalikan nilai
        // mundurnya, dan nilai mundur yang muncul tiba-tiba tampak seperti
        // kerusakan, bukan seperti kurva yang dikosongkan.
        curve.Remove(static_cast<std::size_t>(removeIndex));
    }

    if (ImGui::SmallButton("Add key here")) {
        curve.Set(currentHour, curve.Evaluate(currentHour, Vec3(0.0f)));
    }

    ImGui::PopID();
    ImGui::TreePop();
}

class TimeOfDayPanel final : public Panel {
public:
    TimeOfDayPanel()
        : Panel(panel_id::kTimeOfDay, std::string(icons::kPanelTimeOfDay) + "  Time of Day",
                PanelCategory::Scene) {
        SetOpen(false);
    }

    void OnDraw(EditorContext& context) override {
        ImGui::Checkbox("Drive the sun", &context.timeOfDayEnabled);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "When off, the scene's directional light is left exactly as it is.\n"
                "An editor that silently overwrites a value you just set by hand\n"
                "is an editor you cannot use to set anything.");
        }

        ImGui::Separator();

        // --- Waktu ---
        float hour = context.timeOfDayClock.Hour();
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 6.0f);
        if (ImGui::SliderFloat("##hour", &hour, 0.0f, 24.0f,
                               FormatHour(hour).c_str())) {
            context.timeOfDayClock.SetHour(hour);
        }
        ImGui::SameLine();
        const bool playing = context.timeOfDayClock.Playing();
        if (ImGui::Button(playing ? "Pause" : "Play")) {
            context.timeOfDayClock.SetPlaying(!playing);
        }

        float minutesPerDay = 24.0f / std::max(context.timeOfDayClock.Speed(), 1e-4f) / 60.0f;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        if (ImGui::DragFloat("Day length", &minutesPerDay, 0.25f, 0.1f, 240.0f,
                             "%.1f min")) {
            context.timeOfDayClock.SetSpeed(24.0f / std::max(minutesPerDay, 0.1f) / 60.0f);
        }

        // --- Tempat dan musim ---
        ImGui::Separator();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        ImGui::DragFloat("Latitude", &context.sunPlacement.latitudeDegrees, 0.25f, -90.0f, 90.0f,
                         "%.1f°");
        int day = static_cast<int>(context.sunPlacement.dayOfYear);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        if (ImGui::DragInt("Day of year", &day, 1.0f, 1, 365)) {
            context.sunPlacement.dayOfYear = static_cast<uint32_t>(std::clamp(day, 1, 365));
        }
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        ImGui::DragFloat("North offset", &context.sunPlacement.northOffsetDegrees, 1.0f, -180.0f,
                         180.0f, "%.0f°");

        // --- Keadaan sekarang ---
        render::SunPlacement placement = context.sunPlacement;
        placement.hour = context.timeOfDayClock.Hour();
        const render::TimeOfDayState state =
            render::EvaluateTimeOfDay(context.timeOfDayPreset, placement);

        ImGui::Separator();
        constexpr float kRadToDeg = 57.2957795f;
        ImGui::Text("Sun altitude : %6.1f°", static_cast<double>(state.sun.altitude * kRadToDeg));
        ImGui::Text("Sun azimuth  : %6.1f°", static_cast<double>(state.sun.azimuth * kRadToDeg));
        ImGui::Text("Daylight     : %6.2f", static_cast<double>(state.daylight));
        ImGui::ColorButton("##radiance",
                           ImVec4(state.sunRadiance.x, state.sunRadiance.y, state.sunRadiance.z,
                                  1.0f),
                           ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
        ImGui::SameLine();
        ImGui::Text("Sun radiance");

        // --- Kurva ---
        ImGui::Separator();
        DrawCurve("Sun color", context.timeOfDayPreset.sunColor, placement.hour, true, 1.0f);
        DrawCurve("Sun intensity", context.timeOfDayPreset.sunIntensity, placement.hour, false,
                  8.0f);
        DrawCurve("Sky zenith", context.timeOfDayPreset.skyZenith, placement.hour, true, 1.0f);
        DrawCurve("Sky horizon", context.timeOfDayPreset.skyHorizon, placement.hour, true, 1.0f);
    }
};

}  // namespace

SIM_REGISTER_PANEL(TimeOfDayPanel, 40)

}  // namespace sim::editor
