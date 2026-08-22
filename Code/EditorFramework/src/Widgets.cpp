#include "Sim/Editor/Widgets.h"

#include "Sim/Editor/Icons.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

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

void Tooltip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("%s", text);
    }
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

bool ViewportButton(const char* icon, const char* tooltip, bool active) {
    // Lebih besar daripada IconButton biasa, dan itu disengaja. Tombol ini
    // melayang di atas gambar 3D yang ramai, bukan di bilah alat berlatar rata:
    // pada ukuran bilah alat, ikonnya tenggelam di antara garis grid dan
    // wireframe. Glyph-nya ikut diperbesar, bukan hanya kotaknya — memperbesar
    // kotak saja hanya menambah ruang kosong di sekeliling ikon yang tetap kecil.
    const float size = ImGui::GetFrameHeight() * kViewportButtonScale;
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * kViewportIconScale);
    ImGui::PushID(tooltip);
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    const bool pressed = ImGui::Button(icon, ImVec2(size, size));
    if (active) {
        ImGui::PopStyleColor();
    }
    ImGui::PopID();
    ImGui::PopFont();
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


// =============================================================================
// Penyunting kurva & gradient
//
// Ditulis sendiri, bukan diambil dari pustaka: keduanya dipakai tiga fase editor
// dengan kebutuhan yang spesifik (lihat docs/DEPENDENCIES.md), dan pustaka
// pihak ketiga untuk widget semacam ini umumnya tidak terawat.
// =============================================================================

namespace {

constexpr float kKeyRadius = 4.5f;
constexpr float kStopWidth = 10.0f;
constexpr float kHandleLength = 34.0f;

ImU32 Col(float r, float g, float b, float a = 1.0f) {
    return ImGui::GetColorU32(ImVec4(r, g, b, a));
}

/// Kunci yang sedang terpilih, per widget. Disimpan di storage milik jendela
/// supaya dua penyunting kurva di panel yang sama tidak saling mengganggu.
int& SelectedKey(const char* id) {
    ImGuiStorage* storage = ImGui::GetStateStorage();
    return *storage->GetIntRef(ImGui::GetID(id), -1);
}

}  // namespace

const char* ToString(CurvePreset preset) {
    switch (preset) {
        case CurvePreset::Constant: return "Constant";
        case CurvePreset::Linear: return "Linear";
        case CurvePreset::EaseIn: return "Ease In";
        case CurvePreset::EaseOut: return "Ease Out";
        case CurvePreset::EaseInOut: return "Ease In-Out";
        case CurvePreset::Spike: return "Spike";
    }
    return "Linear";
}

void ApplyPreset(Curve& curve, CurvePreset preset) {
    // Rentang nilainya dipertahankan: pengguna yang menekan "Ease In" pada kurva
    // 0..5 mengharapkan kurva 0..5 yang melengkung, bukan kurva 0..1.
    float low = 0.0f;
    float high = 1.0f;
    if (!curve.Empty()) {
        low = curve.Keys().front().value;
        high = curve.Keys().back().value;
        if (low == high) {
            high = low + 1.0f;
        }
    }
    curve.Keys().clear();

    switch (preset) {
        case CurvePreset::Constant:
            curve.AddKey({0.0f, low, 0.0f, 0.0f, Interpolation::Constant});
            curve.AddKey({1.0f, low, 0.0f, 0.0f, Interpolation::Constant});
            break;
        case CurvePreset::Linear:
            curve.AddKey({0.0f, low, 0.0f, 0.0f, Interpolation::Linear});
            curve.AddKey({1.0f, high, 0.0f, 0.0f, Interpolation::Linear});
            break;
        case CurvePreset::EaseIn:
            curve.AddKey({0.0f, low, 0.0f, 0.0f, Interpolation::Bezier});
            curve.AddKey({1.0f, high, 2.0f * (high - low), 0.0f, Interpolation::Bezier});
            break;
        case CurvePreset::EaseOut:
            curve.AddKey({0.0f, low, 0.0f, 2.0f * (high - low), Interpolation::Bezier});
            curve.AddKey({1.0f, high, 0.0f, 0.0f, Interpolation::Bezier});
            break;
        case CurvePreset::EaseInOut:
            curve.AddKey({0.0f, low, 0.0f, 0.0f, Interpolation::Bezier});
            curve.AddKey({1.0f, high, 0.0f, 0.0f, Interpolation::Bezier});
            break;
        case CurvePreset::Spike:
            curve.AddKey({0.0f, low, 0.0f, 0.0f, Interpolation::Bezier});
            curve.AddKey({0.3f, high, 0.0f, 0.0f, Interpolation::Bezier});
            curve.AddKey({1.0f, low, 0.0f, 0.0f, Interpolation::Bezier});
            break;
    }
}

bool CurveEditor(const char* id, Curve& curve, const ImVec2& size, float minValue,
                 float maxValue) {
    ImGui::PushID(id);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##area", size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    if (maxValue - minValue < 1e-4f) {
        maxValue = minValue + 1.0f;
    }
    const auto toScreen = [&](float t, float v) {
        return ImVec2(origin.x + t * size.x,
                      origin.y + (1.0f - (v - minValue) / (maxValue - minValue)) * size.y);
    };
    const auto toCurve = [&](const ImVec2& p, float& outT, float& outV) {
        outT = std::clamp((p.x - origin.x) / size.x, 0.0f, 1.0f);
        outV = minValue + (1.0f - std::clamp((p.y - origin.y) / size.y, 0.0f, 1.0f)) *
                              (maxValue - minValue);
    };

    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                        Col(0.10f, 0.11f, 0.13f));
    // Garis bantu di 0, 0.5, 1 pada sumbu nilai. Tanpa acuan, kurva yang landai
    // tidak bisa dibedakan dari kurva yang datar.
    for (int i = 0; i <= 4; ++i) {
        const float y = origin.y + size.y * static_cast<float>(i) / 4.0f;
        draw->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + size.x, y),
                      Col(1.0f, 1.0f, 1.0f, i % 2 == 0 ? 0.10f : 0.05f));
    }

    // Kurvanya digambar dengan mencuplik evaluatornya, bukan dengan menghitung
    // ulang bezier di sini. Itu yang menjamin apa yang terlihat sama dengan apa
    // yang dievaluasi simulasi — dua rumus untuk satu kurva adalah dua rumus
    // yang akan berbeda.
    constexpr int kSamples = 96;
    ImVec2 previous = toScreen(0.0f, curve.Evaluate(0.0f));
    for (int i = 1; i <= kSamples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSamples);
        const ImVec2 point = toScreen(t, curve.Evaluate(t));
        draw->AddLine(previous, point, Col(0.55f, 0.80f, 0.45f), 2.0f);
        previous = point;
    }

    bool changed = false;
    int& selected = SelectedKey("selection");
    if (selected >= static_cast<int>(curve.Keys().size())) {
        selected = -1;
    }

    // --- pegangan tangen kunci terpilih ---
    if (selected >= 0) {
        const CurveKey& key = curve.Keys()[static_cast<std::size_t>(selected)];
        if (key.interpolation == Interpolation::Bezier) {
            const ImVec2 anchor = toScreen(key.time, key.value);
            const float scale = size.y / (maxValue - minValue);
            for (int side = 0; side < 2; ++side) {
                const float slope = side == 0 ? key.inTangent : key.outTangent;
                const float dir = side == 0 ? -1.0f : 1.0f;
                const ImVec2 tip(anchor.x + dir * kHandleLength,
                                 anchor.y - dir * slope * kHandleLength * scale /
                                                (size.x / 1.0f) * size.x / size.x);
                draw->AddLine(anchor, tip, Col(0.95f, 0.80f, 0.30f, 0.8f), 1.5f);
                draw->AddCircleFilled(tip, 3.0f, Col(0.95f, 0.80f, 0.30f));

                ImGui::SetCursorScreenPos(ImVec2(tip.x - 5.0f, tip.y - 5.0f));
                ImGui::PushID(side);
                ImGui::InvisibleButton("##tangent", ImVec2(10.0f, 10.0f));
                if (ImGui::IsItemActive()) {
                    const ImVec2 mouse = ImGui::GetIO().MousePos;
                    const float dy = anchor.y - mouse.y;
                    const float dx = std::max(1.0f, std::abs(mouse.x - anchor.x));
                    const float newSlope = (dy / dx) * (maxValue - minValue) / size.y * size.x;
                    CurveKey& mutableKey = curve.Keys()[static_cast<std::size_t>(selected)];
                    (side == 0 ? mutableKey.inTangent : mutableKey.outTangent) = newSlope;
                    changed = true;
                }
                ImGui::PopID();
            }
        }
    }

    // --- kunci ---
    int removeIndex = -1;
    for (std::size_t i = 0; i < curve.Keys().size(); ++i) {
        const CurveKey& key = curve.Keys()[i];
        const ImVec2 point = toScreen(key.time, key.value);
        const bool isSelected = static_cast<int>(i) == selected;
        draw->AddCircleFilled(point, kKeyRadius,
                              isSelected ? Col(1.0f, 1.0f, 1.0f) : Col(0.55f, 0.80f, 0.45f));

        ImGui::SetCursorScreenPos(ImVec2(point.x - kKeyRadius - 2.0f, point.y - kKeyRadius - 2.0f));
        ImGui::PushID(static_cast<int>(i));
        ImGui::InvisibleButton("##key", ImVec2(kKeyRadius * 2.0f + 4.0f, kKeyRadius * 2.0f + 4.0f));
        if (ImGui::IsItemActivated()) {
            selected = static_cast<int>(i);
        }
        if (ImGui::IsItemActive()) {
            float t = 0.0f;
            float v = 0.0f;
            toCurve(ImGui::GetIO().MousePos, t, v);
            // Kunci pertama dan terakhir tidak boleh bergeser waktunya: kurva
            // yang tidak lagi mencakup 0..1 membuat nilai di ujungnya ditahan,
            // dan itu terlihat seperti kurva yang rusak.
            if (i == 0) {
                t = 0.0f;
            } else if (i + 1 == curve.Keys().size()) {
                t = 1.0f;
            }
            selected = static_cast<int>(curve.MoveKey(i, t, v));
            changed = true;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
            curve.Keys().size() > 2 && i != 0 && i + 1 != curve.Keys().size()) {
            removeIndex = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    if (removeIndex >= 0) {
        curve.RemoveKey(static_cast<std::size_t>(removeIndex));
        selected = -1;
        changed = true;
    }

    // Klik ganda di ruang kosong menambah kunci di situ — di tempat yang
    // ditunjuk, bukan lewat tombol yang menaruhnya entah di mana.
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        float t = 0.0f;
        float v = 0.0f;
        toCurve(ImGui::GetIO().MousePos, t, v);
        selected = static_cast<int>(curve.AddKey({t, v, 0.0f, 0.0f, Interpolation::Bezier}));
        changed = true;
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + size.y));
    ImGui::PopID();
    return changed;
}

bool GradientEditor(const char* id, Gradient& gradient, const ImVec2& size) {
    ImGui::PushID(id);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##area", size);
    ImDrawList* draw = ImGui::GetWindowDrawList();

    const float rowHeight = std::max(12.0f, size.y * 0.30f);
    const float barTop = origin.y + rowHeight;
    const float barBottom = origin.y + size.y - rowHeight;

    // Pita gradient dicuplik dari evaluatornya, alasan yang sama dengan kurva.
    constexpr int kSamples = 128;
    for (int i = 0; i < kSamples; ++i) {
        const float t0 = static_cast<float>(i) / static_cast<float>(kSamples);
        const float t1 = static_cast<float>(i + 1) / static_cast<float>(kSamples);
        const Vec4 c = gradient.Evaluate(t0);
        draw->AddRectFilled(ImVec2(origin.x + t0 * size.x, barTop),
                            ImVec2(origin.x + t1 * size.x, barBottom),
                            ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, 1.0f)));
        // Alpha ditampilkan sebagai tinggi terisi, bukan sebagai papan catur.
        // Papan catur memberitahu ADA transparansi; tingginya memberitahu
        // BERAPA, yang justru sedang disunting.
        const float h = (barBottom - barTop) * (1.0f - c.w);
        draw->AddRectFilled(ImVec2(origin.x + t0 * size.x, barTop),
                            ImVec2(origin.x + t1 * size.x, barTop + h),
                            Col(0.10f, 0.11f, 0.13f, 0.85f));
    }
    draw->AddRect(ImVec2(origin.x, barTop), ImVec2(origin.x + size.x, barBottom),
                  Col(1.0f, 1.0f, 1.0f, 0.15f));

    bool changed = false;

    // --- perhentian alpha (atas) ---
    for (std::size_t i = 0; i < gradient.AlphaStops().size(); ++i) {
        GradientStop& stop = gradient.AlphaStops()[i];
        const float x = origin.x + stop.position * size.x;
        const ImU32 shade = Col(stop.alpha, stop.alpha, stop.alpha);
        draw->AddRectFilled(ImVec2(x - kStopWidth * 0.5f, origin.y),
                            ImVec2(x + kStopWidth * 0.5f, barTop), shade);
        draw->AddRect(ImVec2(x - kStopWidth * 0.5f, origin.y),
                      ImVec2(x + kStopWidth * 0.5f, barTop), Col(1.0f, 1.0f, 1.0f, 0.5f));

        ImGui::SetCursorScreenPos(ImVec2(x - kStopWidth * 0.5f, origin.y));
        ImGui::PushID(static_cast<int>(i) + 1000);
        ImGui::InvisibleButton("##alpha", ImVec2(kStopWidth, rowHeight));
        if (ImGui::IsItemActive()) {
            stop.position =
                std::clamp((ImGui::GetIO().MousePos.x - origin.x) / size.x, 0.0f, 1.0f);
            changed = true;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            ImGui::OpenPopup("##alphaedit");
        }
        if (ImGui::BeginPopup("##alphaedit")) {
            changed |= ImGui::SliderFloat("Alpha", &stop.alpha, 0.0f, 1.0f);
            if (gradient.AlphaStops().size() > 1 && ImGui::Button("Remove")) {
                gradient.RemoveAlphaStop(i);
                changed = true;
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                ImGui::PopID();
                break;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    // --- perhentian warna (bawah) ---
    for (std::size_t i = 0; i < gradient.ColorStops().size(); ++i) {
        GradientStop& stop = gradient.ColorStops()[i];
        const float x = origin.x + stop.position * size.x;
        const ImU32 shade = ImGui::GetColorU32(ImVec4(stop.color.x, stop.color.y, stop.color.z,
                                                      1.0f));
        draw->AddRectFilled(ImVec2(x - kStopWidth * 0.5f, barBottom),
                            ImVec2(x + kStopWidth * 0.5f, origin.y + size.y), shade);
        draw->AddRect(ImVec2(x - kStopWidth * 0.5f, barBottom),
                      ImVec2(x + kStopWidth * 0.5f, origin.y + size.y),
                      Col(1.0f, 1.0f, 1.0f, 0.5f));

        ImGui::SetCursorScreenPos(ImVec2(x - kStopWidth * 0.5f, barBottom));
        ImGui::PushID(static_cast<int>(i) + 2000);
        ImGui::InvisibleButton("##color", ImVec2(kStopWidth, rowHeight));
        if (ImGui::IsItemActive()) {
            stop.position =
                std::clamp((ImGui::GetIO().MousePos.x - origin.x) / size.x, 0.0f, 1.0f);
            changed = true;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            ImGui::OpenPopup("##coloredit");
        }
        if (ImGui::BeginPopup("##coloredit")) {
            changed |= ImGui::ColorPicker3("##pick", &stop.color.x);
            if (gradient.ColorStops().size() > 1 && ImGui::Button("Remove")) {
                gradient.RemoveColorStop(i);
                changed = true;
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                ImGui::PopID();
                break;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + size.y));
    ImGui::PopID();
    return changed;
}

void PinIcon(PinShape shape, bool connected, const ImVec4& color, float size) {
    const float side = size > 0.0f ? size : ImGui::GetTextLineHeight();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    // Ruangnya dipesan lebih dulu supaya tata letak baris tetap benar meski
    // yang menggambar adalah draw list, bukan widget.
    ImGui::Dummy(ImVec2(side, side));

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 fill = ImGui::GetColorU32(color);
    // Bagian dalam ikon yang tidak tersambung dibiarkan sewarna latar node,
    // bukan tembus pandang: pin berongga yang menampakkan kabel di belakangnya
    // terbaca sebagai pin yang tersambung.
    const ImU32 hollow = ImGui::GetColorU32(ImGuiCol_WindowBg);
    constexpr float kBorder = 1.6f;

    // Ikon tidak mengisi seluruh kotaknya: sisi kanan disisakan untuk nub
    // segitiga, dan tepinya diberi napas supaya garis batasnya tidak terpotong.
    const float pad = side * 0.12f;
    const float body = side - pad * 2.0f;
    const float nub = side * 0.22f;
    const ImVec2 centre(origin.x + pad + (body - nub) * 0.5f, origin.y + side * 0.5f);
    const float radius = (body - nub) * 0.5f;

    switch (shape) {
        case PinShape::Circle:
            draw->AddCircleFilled(centre, radius, connected ? fill : hollow, 16);
            draw->AddCircle(centre, radius, fill, 16, kBorder);
            break;
        case PinShape::Square: {
            const ImVec2 min(centre.x - radius, centre.y - radius);
            const ImVec2 max(centre.x + radius, centre.y + radius);
            draw->AddRectFilled(min, max, connected ? fill : hollow, 1.0f);
            draw->AddRect(min, max, fill, 1.0f, 0, kBorder);
            break;
        }
        case PinShape::Diamond: {
            const ImVec2 points[4] = {ImVec2(centre.x, centre.y - radius),
                                      ImVec2(centre.x + radius, centre.y),
                                      ImVec2(centre.x, centre.y + radius),
                                      ImVec2(centre.x - radius, centre.y)};
            draw->AddConvexPolyFilled(points, 4, connected ? fill : hollow);
            draw->AddPolyline(points, 4, fill, ImDrawFlags_Closed, kBorder);
            break;
        }
        case PinShape::Arrow: {
            // Panah alur: sisi kiri rata, ujung kanan lancip — bentuk yang sama
            // dengan pin eksekusi di editor blueprint, dan yang membuatnya
            // terbaca sebagai "jalan terus", bukan sebagai nilai.
            const float half = radius * 1.15f;
            const ImVec2 points[3] = {ImVec2(centre.x - half * 0.75f, centre.y - half),
                                      ImVec2(centre.x + half, centre.y),
                                      ImVec2(centre.x - half * 0.75f, centre.y + half)};
            draw->AddConvexPolyFilled(points, 3, connected ? fill : hollow);
            draw->AddPolyline(points, 3, fill, ImDrawFlags_Closed, kBorder);
            break;
        }
    }

    // Nub: segitiga kecil di kanan ikon, selalu terisi. Ia yang membuat arah
    // pin terbaca — dari mana kabelnya berangkat — tanpa perlu melihat di sisi
    // mana node pin itu berada.
    if (shape != PinShape::Arrow) {
        const float top = centre.y - nub * 0.55f;
        const ImVec2 points[3] = {ImVec2(origin.x + side - pad - nub, top),
                                  ImVec2(origin.x + side - pad, centre.y),
                                  ImVec2(origin.x + side - pad - nub, centre.y + nub * 0.55f)};
        draw->AddConvexPolyFilled(points, 3, fill);
    }
}

}  // namespace sim::editor::widgets
