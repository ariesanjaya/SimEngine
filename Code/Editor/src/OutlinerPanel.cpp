#include "Sim/Core/Log.h"
#include "Sim/Editor/Actions.h"
#include "Sim/Editor/Command.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Notifications.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Editor/SceneCommands.h"
#include "Sim/SceneView/Selection.h"
#include "Sim/Editor/Widgets.h"
#include "Sim/Scene/Components.h"
#include "Sim/Scene/Project.h"
#include "Sim/Scene/World.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace sim::editor {
namespace {

/// Lebar kolom tombol visibility + lock di ujung kanan baris.
constexpr float kRowButtonsWidth = 44.0f;

/// Ikon yang mewakili sebuah entity, dipilih dari komponen yang dimilikinya.
///
/// Diturunkan, bukan disimpan: satu field "icon" pada entity akan segera basi
/// begitu komponennya berubah, dan tidak ada yang mengingatkan.
const char* IconFor(const scene::World& world, scene::Entity entity) {
    if (const auto* light = world.TryGet<scene::LightComponent>(entity)) {
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

/// Nama berkas yang aman dari sebuah nama entity.
std::string SanitizeFileName(const std::string& name) {
    std::string result;
    result.reserve(name.size());
    for (const char c : name) {
        result.push_back(std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_'
                             ? c
                             : '_');
    }
    return result.empty() ? "prefab" : result;
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
        if (world == nullptr || context.selection == nullptr) {
            ImGui::TextDisabled("No world.");
            return;
        }

        if (!ImGui::BeginChild("##tree")) {
            ImGui::EndChild();
            return;
        }

        // Urutan baris dikumpulkan ulang tiap frame selagi menggambar. Itulah
        // yang membuat Shift-klik bisa memilih rentang: rentang hanya punya arti
        // dalam urutan yang benar-benar terlihat, termasuk cabang yang sedang
        // terlipat dan baris yang tersaring pencarian.
        rowOrder_.clear();
        pendingRangeTarget_ = scene::kNullEntity;
        pendingDrop_ = {};

        for (const scene::Entity root : world->Roots()) {
            DrawEntity(context, *world, root);
        }

        DrawEmptySpaceBehaviour(context, *world);
        ImGui::EndChild();

        // Diproses setelah seluruh pohon digambar: sebelum itu daftar barisnya
        // belum lengkap, dan aksi yang mengubah hierarki tidak boleh berjalan
        // saat daftar anak sedang ditelusuri.
        ResolveRangeSelection(context);
        ResolvePendingDrop(context, *world);
        ResolvePendingAction(context, *world);
    }

private:
    /// Pemindahan yang diminta lewat seret-lepas, dijalankan setelah traversal.
    struct PendingDrop {
        scene::Entity child = scene::kNullEntity;
        scene::Entity newParent = scene::kNullEntity;
        bool valid = false;
    };

    enum class Action { None, CreateChild, Duplicate, Delete, SavePrefab };

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
        const bool selected = selection->Contains(id);
        const bool hasChildren = !world.ChildrenOf(entity).empty();

        rowOrder_.push_back(entity);
        ImGui::PushID(static_cast<int>(entity));

        // Baris sedang diganti namanya: kotak teks menggantikan seluruh baris.
        if (renaming_ == entity) {
            DrawRenameField(context, world, entity);
            ImGui::PopID();
            return;
        }

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                   ImGuiTreeNodeFlags_SpanAvailWidth |
                                   ImGuiTreeNodeFlags_DefaultOpen;
        if (selected) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        if (!hasChildren) {
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }

        const auto* visibility = world.TryGet<scene::VisibilityComponent>(entity);
        const bool visible = visibility == nullptr || visibility->visible;
        const bool locked = visibility != nullptr && visibility->locked;

        // Entity tersembunyi ditulis redup. Tanpa penanda di teksnya, satu-
        // satunya petunjuk adalah ikon kecil di ujung kanan yang mudah terlewat.
        if (!visible) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        }
        const std::string label =
            std::string(IconFor(world, entity)) + "  " + world.NameOf(entity);
        const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
        if (!visible) {
            ImGui::PopStyleColor();
        }

        HandleRowInteraction(context, world, entity, id);
        DrawRowButtons(context, world, entity, visible, locked);

        if (open && hasChildren) {
            // Disalin: aksi dari menu konteks bisa mengubah daftar anak
            // sementara kita menelusurinya.
            const std::vector<scene::Entity> children = world.ChildrenOf(entity);
            for (const scene::Entity child : children) {
                DrawEntity(context, world, child);
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    void DrawRenameField(EditorContext& context, scene::World& world, scene::Entity entity) {
        ImGui::SetNextItemWidth(-1.0f);
        if (justStartedRenaming_) {
            ImGui::SetKeyboardFocusHere();
            justStartedRenaming_ = false;
        }
        const bool committed = ImGui::InputText("##rename", &renameBuffer_,
                                                ImGuiInputTextFlags_EnterReturnsTrue |
                                                    ImGuiInputTextFlags_AutoSelectAll);
        // Batal lewat Escape, selesai lewat Enter atau kehilangan fokus.
        // Kehilangan fokus dihitung selesai, bukan batal: mengklik ke tempat
        // lain setelah mengetik nama baru jelas bermaksud menyimpannya.
        const bool cancelled = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        const bool lostFocus = ImGui::IsItemDeactivated() && !committed && !cancelled;

        if (committed || lostFocus) {
            const std::string before = world.NameOf(entity);
            if (!renameBuffer_.empty() && renameBuffer_ != before) {
                context.history->CloseMergeGroup();
                context.history->Execute<RenameEntityCommand>(&world, world.GuidOf(entity), before,
                                                              renameBuffer_);
            }
            renaming_ = scene::kNullEntity;
        } else if (cancelled) {
            renaming_ = scene::kNullEntity;
        }
    }

    void HandleRowInteraction(EditorContext& context, scene::World& world, scene::Entity entity,
                              uint64_t id) {
        Selection* selection = context.selection;

        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            ImGuiIO& io = ImGui::GetIO();
            if (io.KeyShift && rangeAnchor_ != scene::kNullEntity) {
                // Diselesaikan setelah traversal — lihat ResolveRangeSelection.
                pendingRangeTarget_ = entity;
            } else if (io.KeyCtrl) {
                selection->Toggle(id);
                rangeAnchor_ = entity;
            } else {
                selection->SelectOnly(id);
                rangeAnchor_ = entity;
            }
        }

        // F2 dan klik-ganda sama-sama memulai penggantian nama.
        const bool doubleClicked =
            ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        if (doubleClicked ||
            (selection->Contains(id) && ImGui::IsWindowFocused() &&
             ImGui::IsKeyPressed(ImGuiKey_F2, false))) {
            renaming_ = entity;
            renameBuffer_ = world.NameOf(entity);
            justStartedRenaming_ = true;
        }

        DrawDragDrop(world, entity);
        DrawContextMenu(context, world, entity, id);
    }

    void DrawDragDrop(const scene::World& world, scene::Entity entity) {
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
            const auto payload = static_cast<uint32_t>(entity);
            ImGui::SetDragDropPayload("SIM_ENTITY", &payload, sizeof(payload));
            ImGui::Text("%s  %s", IconFor(world, entity), world.NameOf(entity).c_str());
            ImGui::EndDragDropSource();
        }

        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SIM_ENTITY")) {
                const auto raw = *static_cast<const uint32_t*>(payload->Data);
                pendingDrop_ = PendingDrop{static_cast<scene::Entity>(raw), entity, true};
            }
            ImGui::EndDragDropTarget();
        }
    }

    void DrawRowButtons(EditorContext& context, scene::World& world, scene::Entity entity,
                        bool visible, bool locked) {
        // Ditempel ke tepi kanan, bukan mengikuti alur baris: baris yang
        // menjorok makin dalam akan membuat tombolnya bergeser-geser dan tidak
        // ada satu pun kolom yang bisa dibidik dengan cepat.
        const float right =
            ImGui::GetWindowWidth() - kRowButtonsWidth - ImGui::GetStyle().WindowPadding.x;
        ImGui::SameLine(right);

        const std::vector<Uuid> targets = TargetGuids(context, world, entity);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

        if (ImGui::SmallButton(visible ? icons::kVisible : icons::kHidden)) {
            context.history->CloseMergeGroup();
            context.history->Execute<SetVisibilityCommand>(
                &world, targets, SetVisibilityCommand::Flag::Visible, !visible);
        }
        widgets::Tooltip(visible ? "Hide" : "Show");

        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::SmallButton(locked ? icons::kLocked : icons::kUnlocked)) {
            context.history->CloseMergeGroup();
            context.history->Execute<SetVisibilityCommand>(
                &world, targets, SetVisibilityCommand::Flag::Locked, !locked);
        }
        widgets::Tooltip(locked ? "Unlock" : "Lock");

        ImGui::PopStyleColor();
    }

    /// Entity yang akan terkena aksi baris ini.
    ///
    /// Kalau barisnya termasuk seleksi, aksinya berlaku untuk seluruh seleksi —
    /// menyembunyikan sepuluh objek terpilih harus satu klik, bukan sepuluh.
    /// Kalau tidak, hanya baris itu sendiri.
    std::vector<Uuid> TargetGuids(const EditorContext& context, const scene::World& world,
                                  scene::Entity entity) const {
        std::vector<Uuid> guids;
        if (context.selection->Contains(ToSelectionId(entity))) {
            for (const uint64_t id : context.selection->Items()) {
                const scene::Entity selected = ToEntity(id);
                if (world.IsAlive(selected)) {
                    guids.push_back(world.GuidOf(selected));
                }
            }
        }
        if (guids.empty()) {
            guids.push_back(world.GuidOf(entity));
        }
        return guids;
    }

    void DrawContextMenu(EditorContext& context, scene::World& world, scene::Entity entity,
                         uint64_t id) {
        if (!ImGui::BeginPopupContextItem("##entitymenu")) {
            return;
        }
        // Klik kanan pada baris di luar seleksi memindahkan seleksi ke sana
        // dulu, supaya isi menunya tidak pernah berbeda dari yang tersorot.
        if (!context.selection->Contains(id)) {
            context.selection->SelectOnly(id);
            rangeAnchor_ = entity;
        }

        ImGui::TextDisabled("%s", world.NameOf(entity).c_str());
        ImGui::Separator();

        if (ImGui::MenuItem("Create Child")) {
            actionTarget_ = entity;
            action_ = Action::CreateChild;
        }
        if (ImGui::MenuItem("Rename", "F2")) {
            renaming_ = entity;
            renameBuffer_ = world.NameOf(entity);
            justStartedRenaming_ = true;
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
            actionTarget_ = entity;
            action_ = Action::Duplicate;
        }
        if (ImGui::MenuItem("Save as Prefab")) {
            actionTarget_ = entity;
            action_ = Action::SavePrefab;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Move to Root", nullptr, false,
                            scene::IsValid(world.ParentOf(entity)))) {
            pendingDrop_ = PendingDrop{entity, scene::kNullEntity, true};
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete", "Del")) {
            actionTarget_ = entity;
            action_ = Action::Delete;
        }
        ImGui::EndPopup();
    }

    void DrawEmptySpaceBehaviour(EditorContext& context, scene::World& world) {
        // Sisa ruang di bawah pohon dijadikan sasaran lepas untuk melepas
        // entity dari induknya. Tanpa ini, satu-satunya cara menjadikan entity
        // sebagai akar adalah lewat menu konteks.
        const ImVec2 available = ImGui::GetContentRegionAvail();
        if (available.y > 4.0f) {
            ImGui::Dummy(ImVec2(-1.0f, available.y));
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SIM_ENTITY")) {
                    const auto raw = *static_cast<const uint32_t*>(payload->Data);
                    pendingDrop_ =
                        PendingDrop{static_cast<scene::Entity>(raw), scene::kNullEntity, true};
                }
                ImGui::EndDragDropTarget();
            }
        }

        // Klik di ruang kosong membatalkan seleksi. Tanpa ini satu-satunya cara
        // melepas seleksi adalah lewat menu, yang tidak akan ditemukan siapa pun.
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()) {
            context.selection->Clear();
            rangeAnchor_ = scene::kNullEntity;
        }
        (void)world;
    }

    void ResolveRangeSelection(EditorContext& context) {
        if (pendingRangeTarget_ == scene::kNullEntity) {
            return;
        }
        const auto anchorIt = std::find(rowOrder_.begin(), rowOrder_.end(), rangeAnchor_);
        const auto targetIt = std::find(rowOrder_.begin(), rowOrder_.end(), pendingRangeTarget_);
        if (anchorIt == rowOrder_.end() || targetIt == rowOrder_.end()) {
            context.selection->SelectOnly(ToSelectionId(pendingRangeTarget_));
            rangeAnchor_ = pendingRangeTarget_;
            return;
        }

        auto first = anchorIt;
        auto last = targetIt;
        if (first > last) {
            std::swap(first, last);
        }
        std::vector<uint64_t> ids;
        for (auto it = first; it <= last; ++it) {
            ids.push_back(ToSelectionId(*it));
        }
        context.selection->SetItems(std::move(ids));
        // Jangkar tidak dipindah: Shift-klik berulang harus memperluas dari
        // titik yang sama, bukan merayap mengikuti klik terakhir.
    }

    void ResolvePendingDrop(EditorContext& context, scene::World& world) {
        if (!pendingDrop_.valid || !world.IsAlive(pendingDrop_.child)) {
            return;
        }
        const scene::Entity child = pendingDrop_.child;
        const scene::Entity parent = pendingDrop_.newParent;

        if (child == parent || world.ParentOf(child) == parent) {
            return;  // tidak ada yang berubah
        }
        // Menjatuhkan induk ke dalam keturunannya sendiri akan memutus pohon.
        // World::SetParent menolaknya, tapi menolaknya di sini juga membuat
        // riwayat tidak terisi langkah yang sebenarnya tidak pernah terjadi.
        if (scene::IsValid(parent) && world.IsDescendantOf(parent, child)) {
            context.notifications->Warning("Cannot parent an entity to its own descendant");
            return;
        }

        context.history->CloseMergeGroup();
        context.history->Execute<ReparentCommand>(&world, world.GuidOf(child),
                                                  world.GuidOf(parent),
                                                  world.GuidOf(world.ParentOf(child)));
    }

    void ResolvePendingAction(EditorContext& context, scene::World& world) {
        const Action action = action_;
        const scene::Entity target = actionTarget_;
        action_ = Action::None;
        actionTarget_ = scene::kNullEntity;

        if (action == Action::None || !world.IsAlive(target)) {
            return;
        }
        context.history->CloseMergeGroup();

        switch (action) {
            case Action::CreateChild: {
                const Uuid parentGuid = world.GuidOf(target);
                const Uuid guid = Uuid::Generate();
                context.history->Execute(std::make_unique<LambdaCommand>(
                    "Create Child",
                    [&world, guid, parentGuid, selection = context.selection]() {
                        const scene::Entity created = world.CreateWithGuid(
                            guid, "Entity", world.FindByGuid(parentGuid));
                        selection->SelectOnly(ToSelectionId(created));
                    },
                    [&world, guid, selection = context.selection]() {
                        world.Destroy(world.FindByGuid(guid));
                        selection->Clear();
                    }));
                break;
            }
            case Action::Duplicate: {
                std::vector<Uuid> guids;
                for (const uint64_t id : context.selection->Items()) {
                    const scene::Entity entity = ToEntity(id);
                    if (world.IsAlive(entity)) {
                        guids.push_back(world.GuidOf(entity));
                    }
                }
                if (guids.empty()) {
                    guids.push_back(world.GuidOf(target));
                }
                std::vector<std::string> subtrees = CopySubtrees(world, guids);
                if (subtrees.empty()) {
                    return;
                }
                context.history->Execute<PasteEntitiesCommand>(
                    &world, context.selection, std::move(subtrees),
                    world.GuidOf(world.ParentOf(target)), "Duplicate");
                break;
            }
            case Action::Delete:
                // Dialihkan ke aksi global supaya perilakunya sama persis
                // dengan menekan Del, termasuk penyaringan keturunan.
                context.actions->Invoke("entity.delete");
                break;
            case Action::SavePrefab:
                SavePrefab(context, world, target);
                break;
            case Action::None:
                break;
        }
    }

    void SavePrefab(EditorContext& context, scene::World& world, scene::Entity entity) {
        if (context.prefabDir.empty()) {
            context.notifications->Warning("No prefab folder configured");
            return;
        }
        const std::filesystem::path directory(context.prefabDir);
        std::error_code error;
        std::filesystem::create_directories(directory, error);

        const std::filesystem::path path =
            directory / (SanitizeFileName(world.NameOf(entity)) + ".simprefab");
        if (scene::SavePrefab(world, entity, path)) {
            context.notifications->Success("Saved prefab " + path.filename().string());
        } else {
            context.notifications->Error("Failed to save prefab");
        }
    }

    std::array<char, 96> search_{};
    std::vector<scene::Entity> rowOrder_;
    std::string renameBuffer_;
    PendingDrop pendingDrop_;
    scene::Entity renaming_ = scene::kNullEntity;
    scene::Entity rangeAnchor_ = scene::kNullEntity;
    scene::Entity pendingRangeTarget_ = scene::kNullEntity;
    scene::Entity actionTarget_ = scene::kNullEntity;
    Action action_ = Action::None;
    bool justStartedRenaming_ = false;
};

}  // namespace

SIM_REGISTER_PANEL(OutlinerPanel, 10)

}  // namespace sim::editor
