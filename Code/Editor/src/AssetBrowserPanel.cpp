#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Core/Log.h"
#include "Sim/Editor/Command.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Notifications.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Editor/Widgets.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace sim::editor {
namespace {

using assets::AssetRecord;
using assets::AssetType;

constexpr float kThumbnailMin = 32.0f;
constexpr float kThumbnailMax = 160.0f;
constexpr float kDetailWidth = 220.0f;

const char* IconFor(AssetType type) {
    switch (type) {
        case AssetType::Texture: return icons::kAssetTexture;
        case AssetType::Mesh: return icons::kAssetMesh;
        case AssetType::Material: return icons::kAssetMaterial;
        case AssetType::Script: return icons::kLua;
        case AssetType::Level: return icons::kAssetLevel;
        case AssetType::Prefab: return icons::kPrefab;
        case AssetType::Json:
        case AssetType::Text: return icons::kScript;
        case AssetType::Unknown: break;
    }
    return icons::kAssetUnknown;
}

/// Ukuran berkas dalam satuan yang enak dibaca.
std::string FormatSize(std::uintmax_t bytes) {
    static constexpr std::array<const char*, 4> kUnits{"B", "KB", "MB", "GB"};
    auto value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < kUnits.size()) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), unit == 0 ? "%.0f %s" : "%.1f %s", value, kUnits[unit]);
    return buffer;
}

/// Nama folder terakhir dari sebuah path relatif.
std::string LeafName(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

bool ContainsNoCase(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                                [](char a, char b) {
                                    return std::tolower(static_cast<unsigned char>(a)) ==
                                           std::tolower(static_cast<unsigned char>(b));
                                });
    return it != haystack.end();
}

class AssetBrowserPanel final : public Panel {
public:
    AssetBrowserPanel()
        : Panel(panel_id::kAssetBrowser, std::string(icons::kPanelAssetBrowser) + "  Asset Browser",
                PanelCategory::Assets) {}

    void OnDraw(EditorContext& context) override {
        assets::AssetDatabase* db = context.assets;
        if (db == nullptr) {
            ImGui::TextDisabled("No asset database.");
            return;
        }

        const float available = ImGui::GetContentRegionAvail().x;

        // Ketiga kolom menyesuaikan lebar panel, bukan dipatok piksel tetap.
        // Panel ini biasanya didock sempit di kiri bawah; lebar detail yang
        // dipatok akan menghimpin area isi sampai tak menyisakan apa pun untuk
        // aset yang justru menjadi tujuan panel ini.
        const bool showDetails = available > 560.0f;
        const float treeWidth = std::clamp(available * 0.24f, 110.0f, 240.0f);
        const float detailWidth =
            showDetails ? std::clamp(available * 0.28f, 180.0f, kDetailWidth) : 0.0f;

        DrawToolbar(*db, showDetails);
        ImGui::Separator();

        // AlwaysUseWindowPadding: child tanpa border secara bawaan tidak punya
        // padding sama sekali, sehingga isinya menempel persis di pemisah.
        ImGui::BeginChild("##tree", ImVec2(treeWidth, 0.0f),
                          ImGuiChildFlags_ResizeX | ImGuiChildFlags_AlwaysUseWindowPadding);
        DrawFolderTree(*db);
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##content", ImVec2(showDetails ? -detailWidth : 0.0f, 0.0f),
                          ImGuiChildFlags_AlwaysUseWindowPadding);
        DrawBreadcrumb();
        ImGui::Separator();
        DrawItems(context, *db);
        ImGui::EndChild();

        if (showDetails) {
            ImGui::SameLine();
            ImGui::BeginChild("##details", ImVec2(0.0f, 0.0f),
                              ImGuiChildFlags_AlwaysUseWindowPadding);
            DrawDetails(context, *db);
            ImGui::EndChild();
        }

        DrawDeletePrompt(context, *db);
    }

private:
    void DrawToolbar(const assets::AssetDatabase& db, bool roomy) {
        // Bilah alat ikut menyusut. Di panel sempit hanya pencarian dan tombol
        // tampilan yang tersisa; memaksakan seluruhnya akan membuat kotak
        // pencarian menyempit sampai tidak bisa dibaca.
        const float available = ImGui::GetContentRegionAvail().x;
        const float buttons = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(std::max(available - buttons - (roomy ? 240.0f : 0.0f), 80.0f));
        ImGui::InputTextWithHint("##search", "Search assets...", &search_);

        if (roomy) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
            static constexpr std::array<const char*, 9> kFilters{
                "All types", "Texture", "Mesh", "Material", "Script", "Level", "Prefab", "Text",
                "JSON"};
            ImGui::Combo("##type", &typeFilter_, kFilters.data(),
                         static_cast<int>(kFilters.size()));

            if (gridMode_) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
                ImGui::SliderFloat("##size", &thumbnailSize_, kThumbnailMin, kThumbnailMax,
                                   "%.0f px");
            }
        }

        ImGui::SameLine();
        if (widgets::IconButton(gridMode_ ? icons::kGrid : icons::kFilter,
                                gridMode_ ? "Grid view" : "List view")) {
            gridMode_ = !gridMode_;
        }

        if (roomy) {
            ImGui::SameLine();
            ImGui::TextDisabled("%zu assets", db.All().size());
        }
    }

    void DrawFolderTree(const assets::AssetDatabase& db) {
        if (!favourites_.empty()) {
            ImGui::TextDisabled("FAVOURITES");
            for (const std::string& folder : favourites_) {
                const std::string label =
                    std::string(icons::kFolder) + "  " +
                    (folder.empty() ? std::string("Assets") : LeafName(folder));
                if (ImGui::Selectable(label.c_str(), currentFolder_ == folder)) {
                    currentFolder_ = folder;
                }
            }
            ImGui::Separator();
        }

        const std::string rootLabel = std::string(
            currentFolder_.empty() ? icons::kFolderOpen : icons::kFolder) + "  Assets";
        if (ImGui::Selectable(rootLabel.c_str(), currentFolder_.empty())) {
            currentFolder_.clear();
        }
        DrawFolderContextMenu("");

        // Daftar folder sudah terurut menurut abjad, jadi induk selalu mendahului
        // anaknya. Indentasi cukup dihitung dari jumlah pemisahnya — tanpa perlu
        // membangun pohon tersendiri yang harus dijaga tetap sinkron.
        for (const std::string& folder : db.Folders()) {
            const auto depth = static_cast<float>(std::count(folder.begin(), folder.end(), '/'));
            ImGui::Indent((depth + 1.0f) * ImGui::GetFontSize());

            const bool selected = currentFolder_ == folder;
            const std::string label =
                std::string(selected ? icons::kFolderOpen : icons::kFolder) + "  " +
                LeafName(folder);
            ImGui::PushID(folder.c_str());
            if (ImGui::Selectable(label.c_str(), selected)) {
                currentFolder_ = folder;
            }
            DrawFolderContextMenu(folder);
            ImGui::PopID();

            ImGui::Unindent((depth + 1.0f) * ImGui::GetFontSize());
        }
    }

    void DrawFolderContextMenu(const std::string& folder) {
        if (!ImGui::BeginPopupContextItem("##foldermenu")) {
            return;
        }
        const auto it = std::find(favourites_.begin(), favourites_.end(), folder);
        const bool isFavourite = it != favourites_.end();
        if (ImGui::MenuItem(isFavourite ? "Remove from favourites" : "Add to favourites")) {
            if (isFavourite) {
                favourites_.erase(it);
            } else {
                favourites_.push_back(folder);
            }
        }
        ImGui::EndPopup();
    }

    void DrawBreadcrumb() {
        if (ImGui::SmallButton("Assets")) {
            currentFolder_.clear();
        }
        // Setiap ruas jadi tombol tersendiri. Menampilkannya sebagai satu teks
        // panjang terlihat sama tapi menghilangkan satu-satunya cara cepat
        // melompat beberapa tingkat ke atas.
        std::size_t start = 0;
        while (start < currentFolder_.size()) {
            const std::size_t slash = currentFolder_.find('/', start);
            const std::size_t end = slash == std::string::npos ? currentFolder_.size() : slash;
            const std::string segment = currentFolder_.substr(start, end - start);

            ImGui::SameLine(0.0f, 4.0f);
            ImGui::TextDisabled("/");
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::PushID(static_cast<int>(start));
            if (ImGui::SmallButton(segment.c_str())) {
                currentFolder_ = currentFolder_.substr(0, end);
            }
            ImGui::PopID();

            if (slash == std::string::npos) {
                break;
            }
            start = slash + 1;
        }
    }

    bool Matches(const AssetRecord& record) const {
        if (typeFilter_ != 0 && static_cast<int>(record.type) != typeFilter_) {
            return false;
        }
        return ContainsNoCase(record.name, search_);
    }

    void DrawItems(EditorContext& context, assets::AssetDatabase& db) {
        // Pencarian berlaku untuk seluruh sub-pohon, bukan hanya folder ini.
        // Mencari sesuatu lalu tidak menemukannya karena ia satu tingkat lebih
        // dalam adalah kegagalan yang tidak terlihat sebagai kegagalan.
        const bool searching = !search_.empty();
        const std::vector<const AssetRecord*> items = db.InFolder(currentFolder_, searching);

        int shown = 0;
        if (gridMode_) {
            const float cell = thumbnailSize_ + ImGui::GetStyle().ItemSpacing.x;
            const auto columns = std::max(1, static_cast<int>(
                                                 ImGui::GetContentRegionAvail().x / cell));
            if (ImGui::BeginTable("##grid", columns)) {
                for (const AssetRecord* record : items) {
                    if (!Matches(*record)) {
                        continue;
                    }
                    ImGui::TableNextColumn();
                    DrawGridItem(context, db, *record);
                    ++shown;
                }
                ImGui::EndTable();
            }
        } else {
            for (const AssetRecord* record : items) {
                if (!Matches(*record)) {
                    continue;
                }
                DrawListItem(context, db, *record);
                ++shown;
            }
        }

        if (shown == 0) {
            ImGui::TextDisabled(searching ? "No assets match the search."
                                          : "This folder is empty.");
        }
    }

    void DrawGridItem(EditorContext& context, assets::AssetDatabase& db,
                      const AssetRecord& record) {
        ImGui::PushID(record.relativePath.c_str());
        ImGui::BeginGroup();

        const bool selected = selected_ == record.guid;
        // Thumbnail gambar sungguhan menyusul bersama jalur unggah tekstur di
        // RHI. Sampai saat itu, ikon per tipe sudah membedakan isi folder
        // dengan cukup jelas untuk bekerja.
        ImGui::PushFont(nullptr, thumbnailSize_ * 0.45f);
        if (ImGui::Selectable(IconFor(record.type), selected,
                              ImGuiSelectableFlags_AllowDoubleClick,
                              ImVec2(thumbnailSize_, thumbnailSize_))) {
            selected_ = record.guid;
        }
        ImGui::PopFont();

        HandleItemInteraction(context, db, record);

        // Nama dipotong agar tidak melebar melewati sel dan merusak kolom.
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + thumbnailSize_);
        ImGui::TextUnformatted(record.name.c_str());
        ImGui::PopTextWrapPos();

        ImGui::EndGroup();
        ImGui::PopID();
    }

    void DrawListItem(EditorContext& context, assets::AssetDatabase& db,
                      const AssetRecord& record) {
        ImGui::PushID(record.relativePath.c_str());

        const bool selected = selected_ == record.guid;
        const std::string label = std::string(IconFor(record.type)) + "  " + record.name;
        if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
            selected_ = record.guid;
        }
        HandleItemInteraction(context, db, record);

        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 4.0f);
        ImGui::TextDisabled("%s", FormatSize(record.fileSize).c_str());

        ImGui::PopID();
    }

    void HandleItemInteraction(EditorContext& context, assets::AssetDatabase& db,
                               const AssetRecord& record) {
        // Sumber seretan. Muatannya GUID, bukan path: path bisa berubah kapan
        // saja, dan yang ditulis ke komponen memang GUID.
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("SIM_ASSET", &record.guid, sizeof(Uuid));
            ImGui::Text("%s  %s", IconFor(record.type), record.name.c_str());
            ImGui::EndDragDropSource();
        }

        if (ImGui::BeginPopupContextItem("##assetmenu")) {
            selected_ = record.guid;
            ImGui::TextDisabled("%s", record.name.c_str());
            ImGui::Separator();

            if (ImGui::MenuItem("Rename")) {
                renaming_ = record.guid;
                renameBuffer_ = record.name;
                justStartedRenaming_ = true;
            }
            if (ImGui::MenuItem("Copy GUID")) {
                ImGui::SetClipboardText(record.guid.ToString().c_str());
                context.notifications->Info("GUID copied");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete")) {
                pendingDelete_ = record.guid;
                deleteUsers_ = db.UsersOf(record.guid);
            }
            ImGui::EndPopup();
        }
    }

    void DrawDetails(EditorContext& context, assets::AssetDatabase& db) {
        const AssetRecord* record = db.Find(selected_);
        if (record == nullptr) {
            ImGui::TextDisabled("No asset selected.");
            return;
        }

        if (renaming_ == record->guid) {
            DrawRenameField(context, db, *record);
        } else {
            ImGui::TextUnformatted(record->name.c_str());
        }
        ImGui::TextDisabled("%s", assets::ToString(record->type));
        ImGui::Separator();

        DetailRow("Path", record->relativePath.c_str());
        DetailRow("Size", FormatSize(record->fileSize).c_str());
        if (record->width > 0) {
            char dimensions[64];
            std::snprintf(dimensions, sizeof(dimensions), "%u x %u (%u ch)", record->width,
                          record->height, record->channels);
            DetailRow("Dimensions", dimensions);
        }
        DetailRow("GUID", record->guid.ToString().c_str());
        if (ImGui::IsItemClicked()) {
            ImGui::SetClipboardText(record->guid.ToString().c_str());
            context.notifications->Info("GUID copied");
        }

        if (record->state == assets::ImportState::Failed) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "Import failed");
            ImGui::TextWrapped("%s", record->error.c_str());
        }

        // Dua arah sekaligus. Sebelum menghapus atau memindahkan aset, yang
        // ingin diketahui bukan hanya "ini memakai apa" tapi terutama "siapa
        // yang akan rusak kalau ini hilang".
        ImGui::Separator();
        DrawReferenceList("Used by", db.UsersOf(record->guid), db);
        DrawReferenceList("Uses", record->dependencies, db);
    }

    void DrawRenameField(EditorContext& context, assets::AssetDatabase& db,
                         const AssetRecord& record) {
        ImGui::SetNextItemWidth(-1.0f);
        if (justStartedRenaming_) {
            ImGui::SetKeyboardFocusHere();
            justStartedRenaming_ = false;
        }
        const bool committed = ImGui::InputText("##rename", &renameBuffer_,
                                                ImGuiInputTextFlags_EnterReturnsTrue |
                                                    ImGuiInputTextFlags_AutoSelectAll);
        const bool cancelled = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (committed) {
            std::string error;
            // GUID tidak berubah, jadi tidak ada level yang perlu disunting —
            // itulah gunanya menyimpan rujukan sebagai GUID sejak awal.
            if (db.Rename(record.guid, renameBuffer_, error)) {
                context.notifications->Success("Renamed to " + renameBuffer_);
            } else {
                context.notifications->Error("Rename failed: " + error);
            }
            renaming_ = Uuid{};
        } else if (cancelled) {
            renaming_ = Uuid{};
        }
    }

    void DrawReferenceList(const char* title, const std::vector<Uuid>& guids,
                           const assets::AssetDatabase& db) {
        ImGui::TextDisabled("%s (%zu)", title, guids.size());
        if (guids.empty()) {
            return;
        }
        for (const Uuid& guid : guids) {
            const AssetRecord* other = db.Find(guid);
            if (other == nullptr) {
                continue;
            }
            ImGui::PushID(guid.ToString().c_str());
            const std::string label = std::string(IconFor(other->type)) + "  " + other->name;
            if (ImGui::Selectable(label.c_str())) {
                selected_ = guid;
            }
            ImGui::PopID();
        }
    }

    void DrawDeletePrompt(EditorContext& context, assets::AssetDatabase& db) {
        if (!pendingDelete_.IsValid()) {
            return;
        }
        constexpr const char* kTitle = "Delete asset";
        if (!ImGui::IsPopupOpen(kTitle)) {
            ImGui::OpenPopup(kTitle);
        }

        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            return;
        }

        const AssetRecord* record = db.Find(pendingDelete_);
        if (record == nullptr) {
            pendingDelete_ = Uuid{};
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        ImGui::Text("Delete \"%s\"?", record->name.c_str());
        if (!deleteUsers_.empty()) {
            // Daftar pemakainya ditampilkan, bukan sekadar jumlahnya. "3 aset
            // memakai ini" tidak cukup untuk memutuskan apa pun; yang dibutuhkan
            // adalah tahu aset mana, supaya bisa diperiksa lebih dulu.
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
                               "Still used by %zu asset(s):", deleteUsers_.size());
            for (const Uuid& guid : deleteUsers_) {
                const AssetRecord* user = db.Find(guid);
                if (user != nullptr) {
                    ImGui::BulletText("%s", user->relativePath.c_str());
                }
            }
            ImGui::TextDisabled("Their references will resolve to Missing.");
        }
        ImGui::Spacing();

        if (ImGui::Button("Delete", ImVec2(120.0f, 0.0f))) {
            std::string error;
            if (db.Delete(pendingDelete_, error)) {
                context.notifications->Success("Deleted");
                selected_ = Uuid{};
            } else {
                context.notifications->Error("Delete failed: " + error);
            }
            pendingDelete_ = Uuid{};
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            pendingDelete_ = Uuid{};
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    static void DetailRow(const char* label, const char* value) {
        ImGui::TextDisabled("%s", label);
        ImGui::TextWrapped("%s", value);
        ImGui::Spacing();
    }

    std::string search_;
    std::string currentFolder_;
    std::string renameBuffer_;
    std::vector<std::string> favourites_;
    std::vector<Uuid> deleteUsers_;
    Uuid selected_;
    Uuid renaming_;
    Uuid pendingDelete_;
    float thumbnailSize_ = 72.0f;
    int typeFilter_ = 0;
    bool gridMode_ = true;
    bool justStartedRenaming_ = false;
};

}  // namespace

SIM_REGISTER_PANEL(AssetBrowserPanel, 20)

}  // namespace sim::editor
