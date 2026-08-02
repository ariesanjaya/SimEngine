#include "Sim/Editor/PropertyGrid.h"

#include "Sim/Core/Math.h"
#include "Sim/Core/Uuid.h"
#include "Sim/Editor/Widgets.h"
#include "Sim/Reflect/TypeRegistry.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace sim::editor {
namespace {

using reflect::Attributes;
using reflect::FieldDesc;
using reflect::FieldKind;

bool HasRange(const Attributes& attributes) {
    return attributes.max > attributes.min;
}

float DragSpeedFor(const Attributes& attributes) {
    if (attributes.speed > 0.0f) {
        return attributes.speed;
    }
    // Tanpa petunjuk apa pun, kecepatan diturunkan dari rentangnya supaya
    // menyeret dari ujung ke ujung butuh jarak layar yang wajar.
    return HasRange(attributes) ? (attributes.max - attributes.min) / 400.0f : 0.01f;
}

std::string LabelOf(const FieldDesc& field) {
    return field.label.empty() ? field.name : field.label;
}

void ApplyTooltip(const FieldDesc& field) {
    if (!field.attributes.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("%s", field.attributes.tooltip.c_str());
    }
}

PropertyGridResult DrawScalarFloat(const FieldDesc& field, float* value) {
    PropertyGridResult result;
    const Attributes& attributes = field.attributes;

    // Radian di data, derajat di layar. Konversi hanya di sini — berkas level
    // dan seluruh matematika tetap radian, sehingga tidak ada tempat kedua yang
    // bisa lupa mengonversi.
    float shown = attributes.degrees ? (*value) * kRadToDeg : *value;
    const float min = attributes.degrees ? attributes.min * kRadToDeg : attributes.min;
    const float max = attributes.degrees ? attributes.max * kRadToDeg : attributes.max;
    const char* format = attributes.degrees ? "%.1f°" : "%.3f";

    ImGui::SetNextItemWidth(-widgets::kPanelRightMargin);
    if (HasRange(attributes)) {
        result.edited = ImGui::SliderFloat("##v", &shown, min, max, format);
    } else {
        result.edited = ImGui::DragFloat("##v", &shown, DragSpeedFor(attributes), 0.0f, 0.0f,
                                         format);
    }
    result.finished = ImGui::IsItemDeactivatedAfterEdit();
    if (result.edited) {
        *value = attributes.degrees ? shown * kDegToRad : shown;
    }
    return result;
}

PropertyGridResult DrawQuat(const FieldDesc& field, Quat* value) {
    PropertyGridResult result;
    // Quaternion tidak bisa disunting langsung dengan cara yang masuk akal bagi
    // manusia, jadi ditampilkan sebagai sudut Euler. Nilai Euler-nya diturunkan
    // ulang tiap frame dari quaternion supaya tidak ada keadaan bayangan yang
    // bisa menyimpang dari data sebenarnya.
    Vec3 euler = glm::degrees(glm::eulerAngles(*value));
    std::array<float, 3> values{euler.x, euler.y, euler.z};

    const widgets::Vec3Result drag = widgets::DragVec3(field.name.c_str(), values.data(), 0.25f,
                                                       "%.2f");
    result.edited = drag.edited;
    result.finished = drag.finished;
    if (drag.edited) {
        *value = Quat(glm::radians(Vec3(values[0], values[1], values[2])));
    }
    (void)field;
    return result;
}

}  // namespace

PropertyGridResult DrawField(const FieldDesc& field, void* object) {
    PropertyGridResult result;
    if (field.attributes.hidden) {
        return result;
    }

    void* data = field.access(object);
    ImGui::PushID(field.name.c_str());
    ImGui::BeginDisabled(field.attributes.readOnly);

    const std::string label = LabelOf(field);

    switch (field.kind) {
        case FieldKind::Bool: {
            result.edited = ImGui::Checkbox(label.c_str(), static_cast<bool*>(data));
            result.finished = result.edited;  // centang selesai seketika
            ApplyTooltip(field);
            break;
        }
        case FieldKind::Int: {
            widgets::PropertyLabel(label.c_str());
            ApplyTooltip(field);
            ImGui::SetNextItemWidth(-widgets::kPanelRightMargin);
            result.edited = ImGui::DragInt("##v", static_cast<int*>(data));
            result.finished = ImGui::IsItemDeactivatedAfterEdit();
            break;
        }
        case FieldKind::UInt: {
            widgets::PropertyLabel(label.c_str());
            ApplyTooltip(field);
            ImGui::SetNextItemWidth(-widgets::kPanelRightMargin);
            int shown = static_cast<int>(*static_cast<uint32_t*>(data));
            result.edited = ImGui::DragInt("##v", &shown, 1.0f, 0, 0);
            result.finished = ImGui::IsItemDeactivatedAfterEdit();
            if (result.edited) {
                *static_cast<uint32_t*>(data) = static_cast<uint32_t>(std::max(0, shown));
            }
            break;
        }
        case FieldKind::Float: {
            widgets::PropertyLabel(label.c_str());
            ApplyTooltip(field);
            result = DrawScalarFloat(field, static_cast<float*>(data));
            break;
        }
        case FieldKind::Vec2: {
            ImGui::TextUnformatted(label.c_str());
            ApplyTooltip(field);
            ImGui::SetNextItemWidth(-widgets::kPanelRightMargin);
            result.edited = ImGui::DragFloat2("##v", &static_cast<Vec2*>(data)->x,
                                              DragSpeedFor(field.attributes));
            result.finished = ImGui::IsItemDeactivatedAfterEdit();
            break;
        }
        case FieldKind::Vec3: {
            if (field.attributes.isColor) {
                widgets::PropertyLabel(label.c_str());
                ApplyTooltip(field);
                ImGui::SetNextItemWidth(-widgets::kPanelRightMargin);
                result.edited = ImGui::ColorEdit3("##v", &static_cast<Vec3*>(data)->x);
                result.finished = ImGui::IsItemDeactivatedAfterEdit();
            } else {
                ImGui::TextUnformatted(label.c_str());
                ApplyTooltip(field);
                const widgets::Vec3Result drag =
                    widgets::DragVec3(field.name.c_str(), &static_cast<Vec3*>(data)->x,
                                      DragSpeedFor(field.attributes));
                result.edited = drag.edited;
                result.finished = drag.finished;
            }
            break;
        }
        case FieldKind::Vec4: {
            widgets::PropertyLabel(label.c_str());
            ApplyTooltip(field);
            ImGui::SetNextItemWidth(-widgets::kPanelRightMargin);
            if (field.attributes.isColor) {
                result.edited = ImGui::ColorEdit4("##v", &static_cast<Vec4*>(data)->x);
            } else {
                result.edited = ImGui::DragFloat4("##v", &static_cast<Vec4*>(data)->x,
                                                  DragSpeedFor(field.attributes));
            }
            result.finished = ImGui::IsItemDeactivatedAfterEdit();
            break;
        }
        case FieldKind::Quat: {
            ImGui::TextUnformatted(label.c_str());
            ApplyTooltip(field);
            result = DrawQuat(field, static_cast<Quat*>(data));
            break;
        }
        case FieldKind::String: {
            widgets::PropertyLabel(label.c_str());
            ApplyTooltip(field);
            ImGui::SetNextItemWidth(-widgets::kPanelRightMargin);
            result.edited = ImGui::InputText("##v", static_cast<std::string*>(data));
            result.finished = ImGui::IsItemDeactivatedAfterEdit();
            break;
        }
        case FieldKind::Uuid: {
            widgets::PropertyLabel(label.c_str());
            ImGui::TextDisabled("%s", static_cast<const Uuid*>(data)->ToString().c_str());
            break;
        }
        case FieldKind::AssetRef: {
            widgets::PropertyLabel(label.c_str());
            ApplyTooltip(field);
            auto* ref = static_cast<AssetRef*>(data);

            const std::string name = ResolveAssetName(ref->guid);
            ImGui::Button(name.c_str(), ImVec2(-widgets::kPanelRightMargin, 0.0f));

            // Kotaknya adalah sasaran lepas untuk seretan dari Asset Browser.
            // Muatannya GUID, bukan path — path bisa berubah, GUID tidak.
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SIM_ASSET")) {
                    if (payload->DataSize == sizeof(Uuid)) {
                        ref->guid = *static_cast<const Uuid*>(payload->Data);
                        result.edited = true;
                        result.finished = true;
                    }
                }
                ImGui::EndDragDropTarget();
            }
            // Klik kanan mengosongkan rujukan. Tanpa ini, satu-satunya cara
            // melepas aset adalah menyeret aset lain ke tempatnya.
            if (ref->IsValid() && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                ref->Clear();
                result.edited = true;
                result.finished = true;
            }
            break;
        }
        case FieldKind::Enum: {
            widgets::PropertyLabel(label.c_str());
            ApplyTooltip(field);
            const auto& names = field.attributes.enumNames;
            auto* value = static_cast<uint8_t*>(data);
            const char* preview =
                *value < names.size() ? names[*value].c_str() : "(unknown)";
            ImGui::SetNextItemWidth(-widgets::kPanelRightMargin);
            if (ImGui::BeginCombo("##v", preview)) {
                for (std::size_t i = 0; i < names.size(); ++i) {
                    const bool selected = *value == i;
                    if (ImGui::Selectable(names[i].c_str(), selected)) {
                        *value = static_cast<uint8_t>(i);
                        result.edited = true;
                        result.finished = true;
                    }
                }
                ImGui::EndCombo();
            }
            break;
        }
        case FieldKind::Struct: {
            const reflect::TypeDesc* type = reflect::TypeRegistry::Get().Find(field.type);
            if (type != nullptr && ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
                const PropertyGridResult nested = DrawProperties(*type, data);
                result.edited |= nested.edited;
                result.finished |= nested.finished;
                ImGui::TreePop();
            }
            break;
        }
        case FieldKind::Vector: {
            const reflect::VectorOps* ops = field.vector;
            if (ops == nullptr) {
                break;
            }
            const std::size_t count = ops->size(data);
            const std::string header = label + " (" + std::to_string(count) + ")";
            if (ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
                FieldDesc element = field;
                element.kind = ops->elementKind;
                element.type = ops->elementType;
                element.vector = nullptr;
                for (std::size_t i = 0; i < count; ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    element.name = std::to_string(i);
                    element.label = "[" + std::to_string(i) + "]";
                    // Elemen diakses lewat ops, jadi accessor field disetel
                    // menunjuk ke elemen itu sendiri.
                    void* elementData = ops->at(data, i);
                    element.access = [](void* self) { return self; };
                    const PropertyGridResult nested = DrawField(element, elementData);
                    result.edited |= nested.edited;
                    result.finished |= nested.finished;
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            break;
        }
    }

    ImGui::EndDisabled();
    ImGui::PopID();
    return result;
}

namespace {
AssetNameResolver g_assetNameResolver;
}  // namespace

void SetAssetNameResolver(AssetNameResolver resolver) {
    g_assetNameResolver = std::move(resolver);
}

std::string ResolveAssetName(const Uuid& guid) {
    if (!guid.IsValid()) {
        return "None";
    }
    if (g_assetNameResolver) {
        std::string name = g_assetNameResolver(guid);
        if (!name.empty()) {
            return name;
        }
    }
    // Aset yang GUID-nya tidak dikenal database: berkasnya mungkin terhapus
    // atau belum terpindai. Ditampilkan apa adanya supaya masih bisa dilacak,
    // bukan disembunyikan jadi "None" yang menyesatkan.
    return "Missing (" + guid.ToString().substr(0, 8) + ")";
}

PropertyGridResult DrawProperties(const reflect::TypeDesc& type, void* object,
                                  std::span<const std::string> mixedFields) {
    PropertyGridResult result;
    for (const FieldDesc& field : type.fields) {
        const bool mixed =
            std::find(mixedFields.begin(), mixedFields.end(), field.name) != mixedFields.end();

        // Field bernilai campuran ditulis redup dan diberi keterangan "—".
        // Nilainya sendiri tetap ditampilkan dan tetap bisa disunting: yang
        // perlu diberitahu adalah bahwa angka itu belum berlaku untuk semua.
        if (mixed) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.6f);
        }
        const PropertyGridResult single = DrawField(field, object);
        if (mixed) {
            ImGui::PopStyleVar();
            ImGui::SameLine();
            ImGui::TextDisabled("—");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                ImGui::SetTooltip("Different values across the selection");
            }
        }

        result.edited |= single.edited;
        result.finished |= single.finished;
    }
    return result;
}

}  // namespace sim::editor
