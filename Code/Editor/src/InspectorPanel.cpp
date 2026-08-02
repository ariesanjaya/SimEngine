#include "Sim/Editor/Command.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Editor/Notifications.h"
#include "Sim/Editor/PropertyGrid.h"
#include "Sim/Editor/Selection.h"
#include "Sim/Editor/Widgets.h"
#include "Sim/Scene/ComponentRegistry.h"
#include "Sim/Scene/Serialization.h"
#include "Sim/Scene/World.h"

#include <imgui.h>

#include <string>
#include <utility>

namespace sim::editor {
namespace {

/// Undo untuk perubahan komponen apa pun.
///
/// Menyimpan cuplikan komponen sebagai JSON, bukan salinan byte: komponen memuat
/// std::string dan std::vector, sehingga memcpy akan merusak. Harganya satu
/// serialisasi kecil per suntingan, dan imbalannya nol command khusus per tipe —
/// komponen baru langsung bisa di-undo tanpa satu baris pun ditambahkan di sini.
class SetComponentCommand final : public ICommand {
public:
    SetComponentCommand(scene::World* world, scene::Entity entity, const scene::ComponentOps* ops,
                        std::string before, std::string after)
        : world_(world),
          entity_(entity),
          ops_(ops),
          before_(std::move(before)),
          after_(std::move(after)) {}

    void Do() override { Apply(after_); }
    void Undo() override { Apply(before_); }

    std::string Name() const override {
        return "Edit " + ops_->type->name + " on " + world_->NameOf(entity_);
    }

    bool MergeWith(const ICommand& next) override {
        const auto* other = dynamic_cast<const SetComponentCommand*>(&next);
        if (other == nullptr || other->world_ != world_ || other->entity_ != entity_ ||
            other->ops_ != ops_) {
            return false;
        }
        after_ = other->after_;
        return true;
    }

    std::size_t MemoryCost() const override {
        return sizeof(SetComponentCommand) + before_.capacity() + after_.capacity();
    }

private:
    void Apply(const std::string& text) {
        if (!world_->IsAlive(entity_)) {
            return;
        }
        void* data = ops_->tryGet(world_->Registry(), scene::World::ToEntt(entity_));
        if (data == nullptr) {
            return;
        }
        scene::DeserializeComponent(*ops_->type, data, text);
        // Transform yang berubah harus merambat ke keturunannya. Memanggilnya
        // untuk komponen lain pun tidak berbahaya dan menghindari pengecualian.
        world_->MarkTransformDirty(entity_);
    }

    scene::World* world_;
    scene::Entity entity_;
    const scene::ComponentOps* ops_;
    std::string before_;
    std::string after_;
};

class AddComponentCommand final : public ICommand {
public:
    AddComponentCommand(scene::World* world, scene::Entity entity, const scene::ComponentOps* ops)
        : world_(world), entity_(entity), ops_(ops) {}

    void Do() override { ops_->emplace(world_->Registry(), scene::World::ToEntt(entity_)); }
    void Undo() override { ops_->remove(world_->Registry(), scene::World::ToEntt(entity_)); }
    std::string Name() const override { return "Add " + ops_->type->name; }

private:
    scene::World* world_;
    scene::Entity entity_;
    const scene::ComponentOps* ops_;
};

class RemoveComponentCommand final : public ICommand {
public:
    RemoveComponentCommand(scene::World* world, scene::Entity entity,
                           const scene::ComponentOps* ops, std::string snapshot)
        : world_(world), entity_(entity), ops_(ops), snapshot_(std::move(snapshot)) {}

    void Do() override { ops_->remove(world_->Registry(), scene::World::ToEntt(entity_)); }
    void Undo() override {
        void* data = ops_->emplace(world_->Registry(), scene::World::ToEntt(entity_));
        scene::DeserializeComponent(*ops_->type, data, snapshot_);
    }
    std::string Name() const override { return "Remove " + ops_->type->name; }

private:
    scene::World* world_;
    scene::Entity entity_;
    const scene::ComponentOps* ops_;
    std::string snapshot_;
};

class InspectorPanel final : public Panel {
public:
    InspectorPanel()
        : Panel(panel_id::kInspector, std::string(icons::kPanelInspector) + "  Entity Inspector",
                PanelCategory::Scene) {}

    void OnDraw(EditorContext& context) override {
        scene::World* world = context.world;
        Selection* selection = context.selection;
        if (world == nullptr || selection == nullptr) {
            ImGui::TextDisabled("No world.");
            return;
        }
        if (selection->Empty()) {
            ImGui::TextDisabled("Nothing selected.");
            return;
        }
        if (selection->Count() > 1) {
            // Penyuntingan banyak objek sekaligus masuk di E4 bersama gizmo,
            // karena keduanya butuh keputusan yang sama soal nilai campuran.
            ImGui::TextDisabled("%zu entities selected.", selection->Count());
            ImGui::TextDisabled("Multi-edit arrives in E4.");
            return;
        }

        const scene::Entity entity = ToEntity(selection->Primary());
        if (!world->IsAlive(entity)) {
            ImGui::TextDisabled("Selection no longer exists.");
            return;
        }

        DrawHeader(context, *world, entity);
        ImGui::Separator();
        DrawComponents(context, *world, entity);
    }

private:
    void DrawHeader(EditorContext& context, scene::World& world, scene::Entity entity) {
        widgets::PropertyLabel("Entity ID", ImGui::GetFontSize() * 5.5f);
        const std::string guid = world.GuidOf(entity).ToString();
        ImGui::TextDisabled("%s", guid.c_str());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            ImGui::SetTooltip("Click to copy");
        }
        if (ImGui::IsItemClicked()) {
            ImGui::SetClipboardText(guid.c_str());
            if (context.notifications != nullptr) {
                context.notifications->Info("Entity ID copied");
            }
        }

        ImGui::Spacing();
        DrawAddComponent(context, world, entity);
    }

    void DrawAddComponent(EditorContext& context, scene::World& world, scene::Entity entity) {
        const std::string label = std::string(icons::kAdd) + "  Add Component";
        if (ImGui::Button(label.c_str(), ImVec2(-widgets::kPanelRightMargin, 0.0f))) {
            ImGui::OpenPopup("##addcomponent");
        }
        if (!ImGui::BeginPopup("##addcomponent")) {
            return;
        }
        bool anyAvailable = false;
        for (const scene::ComponentOps& ops : scene::ComponentRegistry::Get().All()) {
            if (!ops.addable) {
                continue;
            }
            const bool present =
                ops.tryGet(world.Registry(), scene::World::ToEntt(entity)) != nullptr;
            if (present) {
                continue;
            }
            anyAvailable = true;
            if (ImGui::MenuItem(ops.type->name.c_str()) && context.history != nullptr) {
                context.history->CloseMergeGroup();
                context.history->Execute<AddComponentCommand>(&world, entity, &ops);
            }
        }
        if (!anyAvailable) {
            ImGui::TextDisabled("All components already added");
        }
        ImGui::EndPopup();
    }

    void DrawComponents(EditorContext& context, scene::World& world, scene::Entity entity) {
        for (const scene::ComponentOps& ops : scene::ComponentRegistry::Get().All()) {
            void* data = ops.tryGet(world.Registry(), scene::World::ToEntt(entity));
            if (data == nullptr) {
                continue;
            }
            ImGui::PushID(ops.type->name.c_str());

            const bool open = widgets::ComponentHeader(IconForComponent(ops.type->name),
                                                       ops.type->name.c_str());
            DrawComponentMenu(context, world, entity, ops, data);

            if (open) {
                // Cuplikan diambil sebelum widget menyentuh datanya, supaya
                // command punya keadaan "sebelum" yang benar.
                const std::string before = scene::SerializeComponent(*ops.type, data);
                const PropertyGridResult result = DrawProperties(*ops.type, data);

                if (result.edited && context.history != nullptr) {
                    const std::string after = scene::SerializeComponent(*ops.type, data);
                    if (after != before) {
                        context.history->Execute<SetComponentCommand>(&world, entity, &ops, before,
                                                                      after);
                    }
                }
                if (result.finished && context.history != nullptr) {
                    context.history->CloseMergeGroup();
                }
            }
            ImGui::PopID();
        }
    }

    void DrawComponentMenu(EditorContext& context, scene::World& world, scene::Entity entity,
                           const scene::ComponentOps& ops, void* data) {
        if (!ops.removable) {
            return;
        }
        if (!ImGui::BeginPopupContextItem("##componentmenu")) {
            return;
        }
        if (ImGui::MenuItem("Remove") && context.history != nullptr) {
            context.history->CloseMergeGroup();
            context.history->Execute<RemoveComponentCommand>(
                &world, entity, &ops, scene::SerializeComponent(*ops.type, data));
        }
        ImGui::EndPopup();
    }

    static const char* IconForComponent(const std::string& name) {
        if (name == "Transform") {
            return icons::kTransform;
        }
        if (name == "MeshRenderer") {
            return icons::kMesh;
        }
        if (name == "Light") {
            return icons::kLight;
        }
        if (name == "Camera") {
            return icons::kCamera;
        }
        if (name == "Name") {
            return icons::kEntity;
        }
        return icons::kSettings;
    }
};

}  // namespace

SIM_REGISTER_PANEL(InspectorPanel, 40)

}  // namespace sim::editor
