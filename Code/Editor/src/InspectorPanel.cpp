#include "Sim/Editor/Command.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Editor/Notifications.h"
#include "Sim/Editor/PropertyGrid.h"
#include "Sim/Editor/SceneCommands.h"
#include "Sim/Editor/Selection.h"
#include "Sim/Editor/Widgets.h"
#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Scene/ComponentRegistry.h"
#include "Sim/Scene/Serialization.h"
#include "Sim/Scene/World.h"

#if SIM_WITH_LUA
#include "Sim/Script/ScriptRuntime.h"
#endif

#include <imgui.h>

#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sim::editor {
namespace {

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
        // Entity terpilih yang masih hidup. Yang utama ditaruh di depan: ia
        // yang datanya digambar, dan yang lain mengikuti suntingannya.
        std::vector<scene::Entity> entities;
        const scene::Entity primary = ToEntity(selection->Primary());
        if (world->IsAlive(primary)) {
            entities.push_back(primary);
        }
        for (const uint64_t id : selection->Items()) {
            const scene::Entity entity = ToEntity(id);
            if (entity != primary && world->IsAlive(entity)) {
                entities.push_back(entity);
            }
        }
        if (entities.empty()) {
            ImGui::TextDisabled("Selection no longer exists.");
            return;
        }

        DrawHeader(context, *world, entities);
        ImGui::Separator();
        DrawComponents(context, *world, entities);
    }

private:
    void DrawHeader(EditorContext& context, scene::World& world,
                    const std::vector<scene::Entity>& entities) {
        if (entities.size() > 1) {
            ImGui::TextDisabled("%zu entities selected", entities.size());
            ImGui::TextDisabled("Editing applies to all of them");
            ImGui::Spacing();
            DrawAddComponent(context, world, entities);
            return;
        }
        const scene::Entity entity = entities.front();

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
        DrawAddComponent(context, world, entities);
    }

    void DrawAddComponent(EditorContext& context, scene::World& world,
                          const std::vector<scene::Entity>& entities) {
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
            // Ditawarkan bila ada satu saja entity terpilih yang belum
            // memilikinya; yang sudah punya dilewati saat perintah dijalankan.
            const bool anyMissing = std::any_of(
                entities.begin(), entities.end(), [&](scene::Entity entity) {
                    return ops.tryGet(world.Registry(), scene::World::ToEntt(entity)) == nullptr;
                });
            if (!anyMissing) {
                continue;
            }
            anyAvailable = true;
            if (ImGui::MenuItem(ops.type->name.c_str()) && context.history != nullptr) {
                context.history->CloseMergeGroup();
                context.history->BeginTransaction("Add " + ops.type->name);
                for (const scene::Entity entity : entities) {
                    if (ops.tryGet(world.Registry(), scene::World::ToEntt(entity)) == nullptr) {
                        context.history->Execute<AddComponentCommand>(&world, entity, &ops);
                    }
                }
                context.history->EndTransaction();
            }
        }
        if (!anyAvailable) {
            ImGui::TextDisabled("All components already added");
        }
        ImGui::EndPopup();
    }

    void DrawComponents(EditorContext& context, scene::World& world,
                        const std::vector<scene::Entity>& entities) {
        const scene::Entity primary = entities.front();

        for (const scene::ComponentOps& ops : scene::ComponentRegistry::Get().All()) {
            void* data = ops.tryGet(world.Registry(), scene::World::ToEntt(primary));
            if (data == nullptr) {
                continue;
            }
            // Hanya komponen yang dimiliki seluruh seleksi yang ditampilkan.
            // Menampilkan yang hanya dimiliki sebagian akan membuat suntingan
            // diam-diam tidak berlaku untuk sebagian objek.
            const bool sharedByAll =
                std::all_of(entities.begin(), entities.end(), [&](scene::Entity entity) {
                    return ops.tryGet(world.Registry(), scene::World::ToEntt(entity)) != nullptr;
                });
            if (!sharedByAll) {
                continue;
            }

            ImGui::PushID(ops.type->name.c_str());

            // Cuplikan seluruh seleksi diambil sebelum widget menyentuh datanya,
            // supaya command punya keadaan "sebelum" yang benar untuk setiap
            // objek — bukan hanya untuk yang datanya kebetulan digambar.
            std::vector<SetComponentsCommand::Item> items;
            items.reserve(entities.size());
            for (const scene::Entity entity : entities) {
                void* other = ops.tryGet(world.Registry(), scene::World::ToEntt(entity));
                items.push_back({world.GuidOf(entity), scene::SerializeComponent(*ops.type, other),
                                 std::string{}});
            }

            const std::vector<std::string> mixed = MixedFields(items);
            const bool open = widgets::ComponentHeader(IconForComponent(ops.type->name),
                                                       ops.type->name.c_str());
            DrawComponentMenu(context, world, entities, ops, items);

            if (open) {
                PropertyGridResult result = DrawProperties(*ops.type, data, mixed);
                const PropertyGridResult exposed = DrawExposedProperties(context, ops, data);
                result.edited = result.edited || exposed.edited;
                result.finished = result.finished || exposed.finished;

                if (result.edited && context.history != nullptr) {
                    ApplyEdit(context, world, ops, items, data);
                }
                if (result.finished && context.history != nullptr) {
                    context.history->CloseMergeGroup();
                }
            }
            ImGui::PopID();
        }
    }

    /// Properti yang diekspos berkas skrip yang sedang dirujuk.
    ///
    /// Digambar di sini dan bukan lewat grid generik karena bentuknya bukan
    /// milik pengguna: daftar ini ditentukan berkas skrip, dan tombol
    /// tambah/hapus elemen — yang akan muncul sendiri untuk sebuah field vektor —
    /// menjanjikan sesuatu yang tidak berlaku.
    ///
    /// Suntingannya menumpang jalur yang sama dengan field biasa: widget
    /// menyentuh komponennya langsung, lalu DrawComponents yang membandingkan
    /// dengan cuplikan sebelum-gambar dan menerbitkan satu command. Undo dan
    /// multi-select karena itu berlaku tanpa kode tambahan.
    PropertyGridResult DrawExposedProperties(EditorContext& context,
                                             const scene::ComponentOps& ops,
                                             void* data) {
        PropertyGridResult result;
#if SIM_WITH_LUA
        // Script dan Graph sama-sama menjalankan Lua dan sama-sama menyimpan
        // nilai propertinya per-entity; yang berbeda hanya aset yang dirujuk.
        // Kompiler graph membangkitkan deklarasi `properties` yang bentuknya
        // sama persis dengan milik skrip tulis tangan, jadi kode di bawah tidak
        // perlu tahu ia sedang melayani yang mana.
        const AssetRef* asset = nullptr;
        std::vector<scene::ScriptProperty>* properties = nullptr;
        if (ops.type->name == "Script") {
            auto* component = static_cast<scene::ScriptComponent*>(data);
            asset = &component->script;
            properties = &component->properties;
        } else if (ops.type->name == "Graph") {
            auto* component = static_cast<scene::GraphComponent*>(data);
            asset = &component->graph;
            properties = &component->properties;
        }
        if (properties == nullptr || context.scripts == nullptr || !asset->IsValid()) {
            return result;
        }
        SyncWithDeclaration(context, *asset, *properties);
        if (properties->empty()) {
            return result;
        }

        ImGui::SeparatorText("Exposed");
        for (scene::ScriptProperty& property : *properties) {
            ImGui::PushID(property.name.c_str());
            ImGui::TextUnformatted(property.name.c_str());
            ImGui::SameLine(ImGui::GetFontSize() * 9.0f);
            ImGui::SetNextItemWidth(-1.0f);
            switch (property.kind) {
                case scene::ScriptPropertyKind::Bool:
                    result.edited |= ImGui::Checkbox("##value", &property.flag);
                    break;
                case scene::ScriptPropertyKind::Text: {
                    // Buffer sementara: InputText butuh penyangga yang bisa
                    // ditulisi, dan std::string tidak menyediakannya tanpa
                    // callback resize.
                    std::array<char, 256> buffer{};
                    const std::size_t length =
                        std::min(property.text.size(), buffer.size() - 1);
                    std::copy_n(property.text.begin(), length, buffer.begin());
                    if (ImGui::InputText("##value", buffer.data(), buffer.size())) {
                        property.text = buffer.data();
                        result.edited = true;
                    }
                    break;
                }
                case scene::ScriptPropertyKind::Number:
                    result.edited |= ImGui::DragFloat("##value", &property.number, 0.01f);
                    break;
            }
            result.finished |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::PopID();
        }
#else
        (void)context;
        (void)ops;
        (void)data;
#endif
        return result;
    }

#if SIM_WITH_LUA
    /// Menyesuaikan daftar properti komponen dengan deklarasi asetnya:
    /// yang hilang dibuang, yang baru diambil beserta bawaannya, dan yang
    /// namanya masih ada mempertahankan nilai yang sudah disunting.
    ///
    /// Diterapkan langsung ke komponen tanpa melewati command, dengan sengaja.
    /// Mengadopsi deklarasi skrip bukan suntingan pengguna — memperlakukannya
    /// sebagai suntingan akan menaruh entri undo di riwayat hanya karena sebuah
    /// entity kebetulan dipilih.
    void SyncWithDeclaration(EditorContext& context, const AssetRef& asset,
                             std::vector<scene::ScriptProperty>& properties) {
        const uint64_t version = context.assets != nullptr ? context.assets->Version() : 0;
        if (version != declarationVersion_) {
            // Berkas skripnya mungkin berubah. Menjalankan ulang chunk-nya tiap
            // frame hanya untuk membaca tabel yang sama adalah pemborosan yang
            // tidak terlihat sampai ada seratus entity berskrip.
            //
            // Untuk graph, cache ini menahan lebih dari itu: membaca deklarasi
            // sebuah graph berarti memastikan hasil kompilasinya mutakhir, dan
            // itu bukan pekerjaan yang boleh dilakukan setiap frame hanya karena
            // sebuah entity kebetulan dipilih.
            declarations_.clear();
            declarationVersion_ = version;
        }
        auto it = declarations_.find(asset.guid);
        if (it == declarations_.end()) {
            it = declarations_.emplace(asset.guid, context.scripts->DeclaredProperties(asset.guid))
                     .first;
        }

        std::vector<scene::ScriptProperty> merged;
        merged.reserve(it->second.size());
        for (const scene::ScriptProperty& declared : it->second) {
            const auto stored = std::find_if(properties.begin(), properties.end(),
                                             [&](const scene::ScriptProperty& candidate) {
                                                 return candidate.name == declared.name;
                                             });
            // Tipe yang berganti di skrip mengalahkan nilai tersimpan: sebuah
            // angka yang menjadi teks tidak punya nilai lama yang masuk akal.
            merged.push_back(
                stored != properties.end() && stored->kind == declared.kind ? *stored : declared);
        }
        if (merged.size() != properties.size() ||
            !std::equal(merged.begin(), merged.end(), properties.begin(),
                        [](const scene::ScriptProperty& a, const scene::ScriptProperty& b) {
                            return a.name == b.name && a.kind == b.kind && a.number == b.number &&
                                   a.flag == b.flag && a.text == b.text;
                        })) {
            properties = std::move(merged);
        }
    }

    std::unordered_map<Uuid, std::vector<scene::ScriptProperty>> declarations_;
    uint64_t declarationVersion_ = 0;
#endif

    /// Field yang nilainya tidak seragam di seluruh seleksi.
    static std::vector<std::string> MixedFields(
        const std::vector<SetComponentsCommand::Item>& items) {
        std::vector<std::string> mixed;
        for (std::size_t i = 1; i < items.size(); ++i) {
            for (const std::string& field :
                 scene::DiffComponentFields(items.front().before, items[i].before)) {
                if (std::find(mixed.begin(), mixed.end(), field) == mixed.end()) {
                    mixed.push_back(field);
                }
            }
        }
        return mixed;
    }

    /// Meneruskan suntingan pada objek utama ke seluruh seleksi.
    void ApplyEdit(EditorContext& context, scene::World& world, const scene::ComponentOps& ops,
                   std::vector<SetComponentsCommand::Item>& items, void* primaryData) {
        const std::string after = scene::SerializeComponent(*ops.type, primaryData);
        // Hanya field yang benar-benar berubah yang diteruskan. Menyalin
        // seluruh komponen akan menyeragamkan field yang tidak pernah
        // disentuh — sepuluh lampu dengan warna berbeda akan mendadak seragam
        // hanya karena intensitas salah satunya digeser.
        const std::vector<std::string> changed =
            scene::DiffComponentFields(items.front().before, after);
        if (changed.empty()) {
            return;
        }

        items.front().after = after;
        for (std::size_t i = 1; i < items.size(); ++i) {
            items[i].after = scene::MergeComponentFields(items[i].before, after, changed);
        }
        context.history->Execute<SetComponentsCommand>(&world, &ops, items);
    }

    void DrawComponentMenu(EditorContext& context, scene::World& world,
                           const std::vector<scene::Entity>& entities,
                           const scene::ComponentOps& ops,
                           const std::vector<SetComponentsCommand::Item>& items) {
        if (!ImGui::BeginPopupContextItem("##componentmenu")) {
            return;
        }
        if (ImGui::MenuItem("Copy")) {
            ImGui::SetClipboardText(items.front().before.c_str());
            context.notifications->Info("Copied " + ops.type->name);
        }
        const char* clipboard = ImGui::GetClipboardText();
        const bool pasteable =
            clipboard != nullptr && scene::IsComponentSnapshot(*ops.type, clipboard);
        if (ImGui::MenuItem("Paste", nullptr, false, pasteable)) {
            PasteInto(context, world, ops, items, clipboard);
        }
        if (ImGui::MenuItem("Reset")) {
            ResetToDefault(context, world, ops, items);
        }
        if (ops.removable) {
            ImGui::Separator();
            if (ImGui::MenuItem("Remove")) {
                context.history->CloseMergeGroup();
                context.history->BeginTransaction("Remove " + ops.type->name);
                for (std::size_t i = 0; i < entities.size(); ++i) {
                    context.history->Execute<RemoveComponentCommand>(&world, entities[i], &ops,
                                                                     items[i].before);
                }
                context.history->EndTransaction();
            }
        }
        ImGui::EndPopup();
    }

    void PasteInto(EditorContext& context, scene::World& world, const scene::ComponentOps& ops,
                   std::vector<SetComponentsCommand::Item> items, const char* clipboard) {
        for (auto& item : items) {
            item.after = clipboard;
        }
        context.history->CloseMergeGroup();
        context.history->Execute<SetComponentsCommand>(&world, &ops, std::move(items));
    }

    void ResetToDefault(EditorContext& context, scene::World& world,
                        const scene::ComponentOps& ops,
                        std::vector<SetComponentsCommand::Item> items) {
        // Nilai bawaan diambil dari instance yang baru dibuat, bukan dari daftar
        // konstanta terpisah: satu sumber kebenaran, dan mustahil basi ketika
        // nilai awal di struct komponennya berubah.
        scene::World scratch;
        const scene::Entity temporary = scratch.Create("default");
        void* fresh = ops.emplace(scratch.Registry(), scene::World::ToEntt(temporary));
        const std::string defaults = scene::SerializeComponent(*ops.type, fresh);

        for (auto& item : items) {
            item.after = defaults;
        }
        context.history->CloseMergeGroup();
        context.history->Execute<SetComponentsCommand>(&world, &ops, std::move(items));
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
