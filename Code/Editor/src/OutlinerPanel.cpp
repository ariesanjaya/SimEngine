#include "Sim/Editor/Command.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Editor/Selection.h"
#include "Sim/Editor/Widgets.h"
#include "Sim/Scene/Components.h"
#include "Sim/Scene/World.h"

#include <imgui.h>

#include <array>
#include <string>

namespace sim::editor {
namespace {

/// Ikon yang mewakili sebuah entity, dipilih dari komponen yang dimilikinya.
///
/// Diturunkan, bukan disimpan: satu field "icon" pada entity akan segera basi
/// begitu komponennya berubah, dan tidak ada yang mengingatkan.
const char* IconFor(const scene::World& world, scene::Entity entity) {
    if (world.TryGet<scene::LightComponent>(entity) != nullptr) {
        const auto* light = world.TryGet<scene::LightComponent>(entity);
        return light->type == scene::LightType::Directional ? icons::kSunLight : icons::kLight;
    }
    if (world.TryGet<scene::CameraComponent>(entity) != nullptr) {
        return icons::kCamera;
    }
    if (world.TryGet<scene::MeshRendererComponent>(entity) != nullptr) {
        return icons::kMesh;
    }
    return world.ChildrenOf(entity).empty() ? icons::kEntity : icons::kEntityGroup;
}

class OutlinerPanel final : public Panel {
public:
    OutlinerPanel()
        : Panel(panel_id::kOutliner, std::string(icons::kPanelOutliner) + "  Entity Outliner",
                PanelCategory::Scene) {}

    void OnDraw(EditorContext& context) override {
        widgets::SearchField("outlinersearch", search_.data(), search_.size());
        ImGui::Separator();

        scene::World* world = context.world;
        if (world == nullptr) {
            ImGui::TextDisabled("No world.");
            return;
        }

        if (!ImGui::BeginChild("##tree")) {
            ImGui::EndChild();
            return;
        }
        for (const scene::Entity root : world->Roots()) {
            DrawEntity(context, *world, root);
        }

        // Klik di ruang kosong membatalkan seleksi. Tanpa ini satu-satunya cara
        // melepas seleksi adalah lewat menu, yang tidak akan ditemukan siapa pun.
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered() &&
            context.selection != nullptr) {
            context.selection->Clear();
        }
        ImGui::EndChild();
    }

private:
    bool Matches(const scene::World& world, scene::Entity entity) const {
        if (search_[0] == '\0') {
            return true;
        }
        if (world.NameOf(entity).find(search_.data()) != std::string::npos) {
            return true;
        }
        // Induk tetap ditampilkan bila ada keturunannya yang cocok, kalau tidak
        // hasil pencarian kehilangan konteksnya.
        for (const scene::Entity child : world.ChildrenOf(entity)) {
            if (Matches(world, child)) {
                return true;
            }
        }
        return false;
    }

    void DrawEntity(EditorContext& context, scene::World& world, scene::Entity entity) {
        if (!Matches(world, entity)) {
            return;
        }
        Selection* selection = context.selection;
        const uint64_t id = ToSelectionId(entity);
        const bool selected = selection != nullptr && selection->Contains(id);
        const bool hasChildren = !world.ChildrenOf(entity).empty();

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                   ImGuiTreeNodeFlags_SpanAvailWidth |
                                   ImGuiTreeNodeFlags_DefaultOpen;
        if (selected) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        if (!hasChildren) {
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }

        ImGui::PushID(static_cast<int>(entity));
        const std::string label =
            std::string(IconFor(world, entity)) + "  " + world.NameOf(entity);
        const bool open = ImGui::TreeNodeEx(label.c_str(), flags);

        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen() && selection != nullptr) {
            if (ImGui::GetIO().KeyCtrl) {
                selection->Toggle(id);
            } else {
                selection->SelectOnly(id);
            }
        }

        if (open && hasChildren) {
            // Disalin: menghapus entity dari menu konteks akan mengubah daftar
            // anak sementara kita menelusurinya.
            const std::vector<scene::Entity> children = world.ChildrenOf(entity);
            for (const scene::Entity child : children) {
                DrawEntity(context, world, child);
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    std::array<char, 96> search_{};
};

}  // namespace

SIM_REGISTER_PANEL(OutlinerPanel, 10)

}  // namespace sim::editor
