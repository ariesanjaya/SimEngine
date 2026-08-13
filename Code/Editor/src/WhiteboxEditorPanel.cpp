// Panel Whitebox: menyunting blok yang dirancang di dalam editor.
//
// **Sisi, bukan segitiga.** Yang dipilih, didorong, dan diberi material adalah
// poligon — bahwa sisi itu tersusun dari beberapa segitiga adalah urusan mesin.

#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Editor/Command.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Notifications.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Editor/Selection.h"
#include "Sim/Editor/WhiteboxCommands.h"
#include "Sim/Editor/WhiteboxStore.h"
#include "Sim/Scene/Components.h"
#include "Sim/Whitebox/WhiteboxIo.h"

#include <imgui.h>

#include <cstdio>
#include <memory>
#include <string>

namespace sim::editor {
namespace {

class WhiteboxEditorPanel final : public Panel {
public:
    WhiteboxEditorPanel()
        : Panel(panel_id::kWhiteboxEditor, std::string(icons::kWhitebox) + "  Whitebox",
                PanelCategory::Authoring) {
        SetOpen(false);
    }

    void OnDraw(EditorContext& context) override {
        if (context.world == nullptr || context.selection == nullptr ||
            context.whiteboxes == nullptr || context.assets == nullptr) {
            ImGui::TextDisabled("No scene open.");
            return;
        }

        const scene::Entity entity = ToEntity(context.selection->Primary());
        if (!context.world->IsAlive(entity)) {
            ImGui::TextDisabled("Select an entity with a Whitebox component.");
            return;
        }

        const auto* component = context.world->TryGet<scene::WhiteboxComponent>(entity);
        if (component == nullptr || !component->whitebox.IsValid()) {
            ImGui::TextDisabled("'%s' has no Whitebox asset.",
                                context.world->NameOf(entity).c_str());
            ImGui::TextWrapped(
                "Add a Whitebox component and point it at a .simwhitebox asset.");
            return;
        }

        const assets::AssetRecord* record = context.assets->Find(component->whitebox.guid);
        if (record == nullptr) {
            ImGui::TextDisabled("Whitebox asset is missing from the project.");
            return;
        }
        const std::filesystem::path path = context.assets->AbsolutePath(*record);

        whitebox::WhiteboxMesh* box = context.whiteboxes->Get(component->whitebox.guid, path);
        if (box == nullptr) {
            ImGui::TextDisabled("Whitebox asset could not be read. See the Console.");
            return;
        }

        DrawSummary(*box);
        ImGui::Separator();
        DrawSides(context, *box, component->whitebox.guid);
        ImGui::Separator();
        DrawActions(context, *box, component->whitebox.guid, path);
    }

private:
    /// Sisi yang sedang disorot, milik store supaya viewport melihat yang sama.
    ///
    /// Diperiksa asetnya, bukan diambil begitu saja: berpindah ke whitebox lain
    /// meninggalkan sorotan pada yang lama, dan nomor poligon yang sah di sana
    /// juga sah di sini — sorotan akan pindah ke sisi yang tidak pernah diklik.
    static whitebox::PolygonHandle Selected(const EditorContext& context, const Uuid& guid) {
        const SideSelection& side = context.whiteboxes->Selected();
        return side.asset == guid ? side.polygon : whitebox::PolygonHandle::Invalid;
    }

    void DrawSummary(const whitebox::WhiteboxMesh& box) const {
        ImGui::Text("%zu sides, %zu faces, %zu vertices", box.Polygons().PolygonCount(),
                    box.Mesh().FaceCount(), box.Mesh().VertexCount());
        ImGui::Text("%zu material slots in use", box.UsedMaterialCount());
    }

    void DrawSides(EditorContext& context, whitebox::WhiteboxMesh& box, const Uuid& guid) {
        ImGui::TextUnformatted("Sides");
        if (!ImGui::BeginChild("##whitebox-sides", ImVec2(0.0f, 160.0f), ImGuiChildFlags_Borders)) {
            ImGui::EndChild();
            return;
        }

        for (const whitebox::PolygonHandle polygon : box.Polygons().Polygons()) {
            const uint32_t number = static_cast<uint32_t>(polygon);
            const int material = box.PolygonMaterial(polygon);
            const Vec3 normal = box.Polygons().PolygonNormal(box.Mesh(), polygon);

            char label[128];
            if (material == whitebox::kNoMaterial) {
                std::snprintf(label, sizeof(label), "Side %u — no material   (%.2f %.2f %.2f)",
                              number, normal.x, normal.y, normal.z);
            } else {
                std::snprintf(label, sizeof(label), "Side %u — slot %d   (%.2f %.2f %.2f)",
                              number, material, normal.x, normal.y, normal.z);
            }

            if (ImGui::Selectable(label, Selected(context, guid) == polygon)) {
                context.whiteboxes->Select(guid, polygon);
            }
        }
        ImGui::EndChild();

        if (!IsValid(Selected(context, guid))) {
            return;
        }

        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt("Material slot", &materialSlot_);
        ImGui::SameLine();
        if (ImGui::Button("Assign")) {
            // Lewat riwayat, bukan langsung: menetapkan material adalah
            // suntingan seperti yang lain, dan yang tidak bisa dibatalkan akan
            // membuat orang takut mencoba.
            context.history->Execute(std::make_unique<SetPolygonMaterialCommand>(
                context.whiteboxes, guid, Selected(context, guid), materialSlot_));
            context.whiteboxes->MarkDirty(guid);
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            context.history->Execute(std::make_unique<SetPolygonMaterialCommand>(
                context.whiteboxes, guid, Selected(context, guid), whitebox::kNoMaterial));
            context.whiteboxes->MarkDirty(guid);
        }
    }

    void DrawActions(EditorContext& context, whitebox::WhiteboxMesh& box, const Uuid& guid,
                     const std::filesystem::path& path) {
        ImGui::SetNextItemWidth(120.0f);
        ImGui::DragFloat("Distance", &distance_, 0.01f, -10.0f, 10.0f, "%.2f m");

        const bool hasSide = IsValid(Selected(context, guid));
        ImGui::BeginDisabled(!hasSide);
        if (ImGui::Button("Extrude")) {
            Apply(context, box, guid, /*extrude=*/true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Move")) {
            Apply(context, box, guid, /*extrude=*/false);
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Merge coplanar")) {
            const whitebox::WhiteboxData before = box.ToData();
            const std::size_t merged = box.MergeCoplanar();
            if (merged > 0) {
                context.history->Execute(std::make_unique<WhiteboxEditCommand>(
                    context.whiteboxes, guid, before, box.ToData(), "Merge Sides"));
                context.history->CloseMergeGroup();
                context.whiteboxes->MarkDirty(guid);
                context.whiteboxes->ClearSelection();
            }
        }

        ImGui::Separator();
        if (ImGui::Button("Save whitebox")) {
            std::string error;
            if (context.whiteboxes->Save(guid, path, error)) {
                if (context.notifications != nullptr) {
                    context.notifications->Success("Whitebox saved");
                }
            } else if (context.notifications != nullptr) {
                context.notifications->Error("Whitebox not saved: " + error);
            }
        }
    }

    void Apply(EditorContext& context, whitebox::WhiteboxMesh& box, const Uuid& guid,
               bool extrude) {
        const whitebox::WhiteboxData before = box.ToData();
        const Vec3 normal = box.Polygons().PolygonNormal(box.Mesh(), Selected(context, guid));

        const whitebox::EditResult result =
            extrude ? box.Extrude(Selected(context, guid), distance_)
                    : box.Translate(Selected(context, guid), normal * distance_);
        if (!result.ok) {
            if (context.notifications != nullptr) {
                context.notifications->Warning("Whitebox edit failed: " + result.error);
            }
            return;
        }

        context.history->Execute(std::make_unique<WhiteboxEditCommand>(
            context.whiteboxes, guid, before, box.ToData(),
            extrude ? "Extrude Side" : "Move Side"));
        // Ditutup segera: tombol adalah satu gerakan yang selesai, bukan seretan
        // yang masih berlangsung. Membiarkannya terbuka membuat penekanan
        // berikutnya menyatu dengan yang ini, dan satu Ctrl+Z membatalkan
        // keduanya.
        context.history->CloseMergeGroup();
        context.whiteboxes->MarkDirty(guid);
        context.whiteboxes->Select(guid, result.polygon);
    }

    float distance_ = 0.5f;
    int materialSlot_ = 0;
};

}  // namespace

SIM_REGISTER_PANEL(WhiteboxEditorPanel, 24)

}  // namespace sim::editor
