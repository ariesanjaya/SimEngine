#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Core/Log.h"
#include "Sim/Editor/Command.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Material/MaterialGraph.h"
#include "Sim/Material/MaterialNodeCatalog.h"
#include "Sim/Platform/FileDialog.h"
#include "Sim/Editor/Notifications.h"
#include "Sim/Editor/SourceImport.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Assets/TextureSettings.h"
#include "Sim/Editor/TextureSettingsCommand.h"
#include "Sim/Editor/Widgets.h"
#include "Sim/Whitebox/WhiteboxIo.h"

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
        case AssetType::Particle: return icons::kParticle;
        case AssetType::Graph: return icons::kNodeGraph;
        case AssetType::Terrain: return icons::kTerrain;
        case AssetType::Vegetation: return icons::kVegetation;
        case AssetType::Volume: return icons::kVolume;
        case AssetType::Skeleton: return icons::kBone;
        case AssetType::AnimationClip: return icons::kAssetAnimation;
        case AssetType::AnimationGraph: return icons::kNodeGraph;
        case AssetType::Level: return icons::kAssetLevel;
        case AssetType::Prefab: return icons::kPrefab;
        case AssetType::Whitebox: return icons::kWhitebox;
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
        // **Dua pustaka, satu panel.** Isi bawaan editor dan aset project punya
        // bentuk yang sama persis — folder, GUID, thumbnail — jadi menampilkan
        // keduanya lewat panel yang berbeda berarti dua salinan kode yang harus
        // sepakat. Yang berbeda hanya satu: yang bawaan tidak boleh ditulisi.
        if (browsingBuiltin_ && context.builtinAssets == nullptr) {
            browsingBuiltin_ = false;
        }
        assets::AssetDatabase* db = browsingBuiltin_ ? context.builtinAssets : context.assets;
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

        DrawSourceTabs(context);
        DrawToolbar(*db, showDetails);
        if (browsingBuiltin_) {
            ImGui::TextDisabled(
                "Editor built-ins — usable as they are, but not editable here.");
        }
        ImGui::Separator();

        // AlwaysUseWindowPadding: child tanpa border secara bawaan tidak punya
        // padding sama sekali, sehingga isinya menempel persis di pemisah.
        ImGui::BeginChild("##tree", ImVec2(treeWidth, 0.0f),
                          ImGuiChildFlags_ResizeX | ImGuiChildFlags_AlwaysUseWindowPadding);
        DrawFolderTree(context, *db);
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

        DrawPrompt(context, *db);
        DrawDeletePrompt(context, *db);
        DrawImportOptions(context, *db);
        // Dijalankan setelah seluruh pohon digambar: memindahkan berkas memicu
        // pemindaian ulang yang mengganti daftar folder yang sedang ditelusuri.
        ResolvePendingMove(context, *db);
        ResolveFolderMove(context, *db);
    }

private:
    /// Apa yang sedang dinamai. `None` berarti tidak ada prompt yang terbuka.
    enum class PromptKind { None, NewFolder, NewMaterial, NewWhitebox, RenameFolder };

    /// Sakelar Project / Built-in. Digambar lebih dulu supaya ia tidak ikut
    /// menyusut bersama bilah alat di panel sempit: mengetahui pustaka mana yang
    /// sedang dilihat lebih penting daripada penyaring tipe, karena aksi yang
    /// tersedia berbeda di antara keduanya.
    void DrawSourceTabs(EditorContext& context) {
        if (context.builtinAssets == nullptr) {
            return;
        }
        if (ImGui::BeginTabBar("##assetSource")) {
            const bool project = ImGui::BeginTabItem("Project");
            if (project) {
                ImGui::EndTabItem();
            }
            const bool builtin = ImGui::BeginTabItem("Built-in");
            if (builtin) {
                ImGui::EndTabItem();
            }
            if (builtin != browsingBuiltin_) {
                browsingBuiltin_ = builtin;
                // Folder dan pencarian dikosongkan: nama folder pustaka yang satu
                // tidak berarti apa-apa di pustaka yang lain, dan panel yang
                // membuka "Meshes/" yang tidak ada di sana hanya memperlihatkan
                // ruang kosong tanpa sebab yang terlihat.
                currentFolder_.clear();
                selected_ = Uuid{};
            }
            ImGui::EndTabBar();
        }
    }

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

    void DrawFolderTree(EditorContext& context, assets::AssetDatabase& db) {
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
        DrawFolderDropTarget("");
        DrawFolderContextMenu(context, db, "");

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
            if (ImGui::BeginDragDropSource()) {
                // Muatannya jalur relatif, bukan penunjuk ke dalam daftar folder:
                // daftar itu dibangun ulang setiap kali indeks berubah, dan
                // penunjuknya sudah menggantung sebelum jatuhannya sempat diproses.
                ImGui::SetDragDropPayload("SIM_FOLDER", folder.c_str(), folder.size() + 1);
                ImGui::TextUnformatted(LeafName(folder).c_str());
                ImGui::EndDragDropSource();
            }
            DrawFolderDropTarget(folder);
            DrawFolderContextMenu(context, db, folder);
            ImGui::PopID();

            ImGui::Unindent((depth + 1.0f) * ImGui::GetFontSize());
        }
    }

    /// Menjatuhkan aset ke sebuah folder memindahkan berkasnya.
    ///
    /// GUID tidak ikut berubah dan `.meta` berpindah bersamanya, jadi tidak ada
    /// level yang perlu disunting — sama seperti mengganti nama.
    void DrawFolderDropTarget(const std::string& folder) {
        if (!ImGui::BeginDragDropTarget()) {
            return;
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SIM_FOLDER")) {
            if (payload->Data != nullptr) {
                pendingFolderMove_ = {std::string(static_cast<const char*>(payload->Data)), folder,
                                      true};
            }
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SIM_ASSET")) {
            if (payload->DataSize == sizeof(Uuid)) {
                pendingMove_ = {*static_cast<const Uuid*>(payload->Data), folder, true};
            }
        }
        ImGui::EndDragDropTarget();
    }

    void DrawFolderContextMenu(EditorContext& context, assets::AssetDatabase& db,
                               const std::string& folder) {
        if (!ImGui::BeginPopupContextItem("##foldermenu")) {
            return;
        }
        ImGui::TextDisabled("%s", folder.empty() ? "Assets" : folder.c_str());
        ImGui::Separator();

        // Pustaka bawaan ikut pemasangan editor: apa pun yang ditulis ke sana
        // hilang pada pemasangan berikutnya.
        ImGui::BeginDisabled(browsingBuiltin_ || dialogOpen_);
        if (ImGui::MenuItem("New folder")) {
            BeginPrompt(PromptKind::NewFolder, folder);
        }
        if (ImGui::MenuItem("New material")) {
            BeginPrompt(PromptKind::NewMaterial, folder);
        }
        if (ImGui::MenuItem("New whitebox")) {
            BeginPrompt(PromptKind::NewWhitebox, folder);
        }
        if (ImGui::MenuItem("Import assets...")) {
            ImportAssets(context, db, folder);
        }
        ImGui::EndDisabled();
        // Akar tidak ikut: "Assets" adalah folder aset project itu sendiri, dan
        // namanya bukan milik panel ini.
        ImGui::BeginDisabled(browsingBuiltin_ || dialogOpen_ || folder.empty());
        if (ImGui::MenuItem("Rename")) {
            BeginPrompt(PromptKind::RenameFolder, folder);
        }
        ImGui::EndDisabled();
        ImGui::Separator();

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

    /// Membuka prompt nama. Berkasnya belum dibuat di sini.
    ///
    /// **Nama dulu, berkas belakangan.** Aset yang lahir dengan nama bawaan lalu
    /// menunggu diganti nama meninggalkan "New Folder 3" di mana-mana setiap kali
    /// seseorang berubah pikiran di tengah jalan — dan berkas yang terlanjur ada
    /// adalah sampah yang harus dibersihkan sendiri, bukan sesuatu yang batal
    /// dengan menekan Escape.
    /// `folder` adalah folder tujuan untuk yang dibuat, dan folder yang
    /// bersangkutan sendiri untuk yang diganti nama.
    void BeginPrompt(PromptKind kind, const std::string& folder) {
        promptKind_ = kind;
        promptFolder_ = folder;
        promptName_ = kind == PromptKind::NewFolder     ? "New Folder"
                      : kind == PromptKind::NewMaterial  ? "NewMaterial"
                      : kind == PromptKind::NewWhitebox  ? "NewWhitebox"
                                                         : LeafName(folder);
        promptError_.clear();
        promptFocus_ = true;
    }

    /// Induk sebuah folder relatif; "" untuk yang tepat di akar aset.
    static std::string ParentFolder(const std::string& folder) {
        const std::size_t slash = folder.rfind('/');
        return slash == std::string::npos ? std::string{} : folder.substr(0, slash);
    }

    /// Menyesuaikan keadaan panel yang masih menyebut jalur folder lama.
    ///
    /// **Termasuk seluruh keturunannya, bukan hanya yang persis sama namanya.**
    /// Folder yang berpindah membawa serta isinya; folder yang sedang dibuka dan
    /// yang ditandai favorit sama-sama disimpan sebagai jalur, dan jalur yang
    /// menunjuk tempat yang sudah tidak ada membuat panel menampilkan folder
    /// kosong tanpa sebab yang terlihat.
    void RepathFolders(const std::string& from, const std::string& to) {
        const auto repath = [&](std::string& path) {
            if (path == from) {
                path = to;
            } else if (path.size() > from.size() && path.compare(0, from.size(), from) == 0 &&
                       path[from.size()] == '/') {
                path = to + path.substr(from.size());
            }
        };
        repath(currentFolder_);
        for (std::string& favourite : favourites_) {
            repath(favourite);
        }
    }

    /// Mengganti nama sebuah folder di tempatnya.
    ///
    /// GUID aset di dalamnya tidak berubah: `.meta` ikut berpindah bersama
    /// berkasnya, jadi tidak ada level yang perlu disunting — sama seperti
    /// memindahkan satu aset.
    bool RenameFolder(EditorContext& context, assets::AssetDatabase& db,
                      const std::string& folder, const std::string& name) {
        if (!ValidateName(name)) {
            return false;
        }
        const std::string parent = ParentFolder(folder);
        const std::string target = parent.empty() ? name : parent + "/" + name;
        if (target == folder) {
            return true;
        }
        std::error_code error;
        const std::filesystem::path to = db.Root() / std::filesystem::path(target);
        if (std::filesystem::exists(to, error)) {
            promptError_ = "\"" + name + "\" already exists here.";
            return false;
        }
        std::filesystem::rename(db.Root() / std::filesystem::path(folder), to, error);
        if (error) {
            promptError_ = "Cannot rename the folder: " + error.message();
            return false;
        }
        db.ScanNow();
        RepathFolders(folder, target);
        context.notifications->Success("Renamed to " + name);
        return true;
    }

    /// Memindahkan sebuah folder ke dalam folder lain.
    void MoveFolder(EditorContext& context, assets::AssetDatabase& db, const std::string& folder,
                    const std::string& destination) {
        // **Tujuan yang berada di dalam folder itu sendiri ditolak.** Yang
        // dilakukan `rename` pada folder yang dipindahkan ke dalam keturunannya
        // bergantung pada sistem berkas, dan yang paling ringan pun meninggalkan
        // folder yang tidak lagi bisa dijangkau dari akar.
        if (destination == folder ||
            (destination.size() > folder.size() &&
             destination.compare(0, folder.size(), folder) == 0 &&
             destination[folder.size()] == '/')) {
            context.notifications->Error("A folder cannot be moved into itself.");
            return;
        }
        if (ParentFolder(folder) == destination) {
            return;
        }
        const std::string leaf = LeafName(folder);
        const std::string target = destination.empty() ? leaf : destination + "/" + leaf;
        std::error_code error;
        const std::filesystem::path to = db.Root() / std::filesystem::path(target);
        if (std::filesystem::exists(to, error)) {
            context.notifications->Error("\"" + leaf + "\" already exists in the destination.");
            return;
        }
        std::filesystem::rename(db.Root() / std::filesystem::path(folder), to, error);
        if (error) {
            context.notifications->Error("Cannot move the folder: " + error.message());
            return;
        }
        db.ScanNow();
        RepathFolders(folder, target);
        context.notifications->Success("Moved " + leaf + " to " +
                                       (destination.empty() ? "Assets" : destination));
    }

    static std::string TrimName(const std::string& text) {
        const std::size_t first = text.find_first_not_of(" \t");
        if (first == std::string::npos) {
            return {};
        }
        return text.substr(first, text.find_last_not_of(" \t") - first + 1);
    }

    /// Menolak nama yang tidak bisa menjadi satu entri di dalam foldernya.
    ///
    /// **Ditolak dengan alasannya, bukan dibersihkan diam-diam.** Nama yang
    /// mengandung "/" adalah orang yang sedang mengetik jalur; membuang bagian
    /// depannya tanpa berkata apa-apa membuat berkasnya muncul di folder yang
    /// tidak ia maksud, dan ".." membuatnya muncul di luar folder aset sama
    /// sekali.
    bool ValidateName(const std::string& name) {
        if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
            promptError_ = "The name cannot contain a path.";
            return false;
        }
        if (name == "." || name == "..") {
            promptError_ = "That name is reserved.";
            return false;
        }
        return true;
    }

    /// Membuat folder bernama `name` di dalam `parent` (relatif terhadap akar
    /// aset). False berarti promptnya tetap terbuka dengan `promptError_` terisi.
    bool CreateFolder(EditorContext& context, assets::AssetDatabase& db,
                      const std::string& parent, const std::string& name) {
        if (!ValidateName(name)) {
            return false;
        }
        const std::filesystem::path base =
            parent.empty() ? db.Root() : db.Root() / std::filesystem::path(parent);
        const std::filesystem::path target = base / name;
        std::error_code error;
        if (std::filesystem::exists(target, error)) {
            promptError_ = "\"" + name + "\" already exists here.";
            return false;
        }
        std::filesystem::create_directories(target, error);
        if (error) {
            promptError_ = "Cannot create the folder: " + error.message();
            return false;
        }
        // Pemindaian dipaksa selesai supaya foldernya langsung terlihat.
        // Menunggu pemindaian latar berarti aksi yang tampak tidak melakukan
        // apa pun.
        db.ScanNow();
        currentFolder_ = parent.empty() ? name : parent + "/" + name;
        context.notifications->Success("Folder " + name + " created");
        return true;
    }

    /// Membuat material kosong bernama `name` di dalam `folder`.
    ///
    /// **Di folder tempat orang mengklik kanan, bukan selalu di `Materials/`.**
    /// Material Editor membuat miliknya di folder tetap karena ia tidak punya
    /// gagasan "folder yang sedang dibuka"; panel ini punya, dan menaruh berkas
    /// di tempat lain daripada yang ditunjuk orang adalah cara tercepat membuat
    /// orang kehilangan berkasnya sendiri.
    /// Pengaturan impor sebuah tekstur, disunting lewat riwayat undo.
    ///
    /// **Dimuat sekali per aset, bukan tiap frame.** Membaca berkas pengaturan
    /// enam puluh kali per detik untuk menggambar lima kotak pilihan adalah
    /// pembacaan disk yang tidak menghasilkan apa pun — dan yang lebih buruk, ia
    /// akan menimpa suntingan yang sedang berlangsung dengan isi disk.
    void DrawTextureSettings(EditorContext& context, const assets::AssetRecord& record) {
        const std::filesystem::path path = context.assets->AbsolutePath(record);
        if (settingsGuid_ != record.guid) {
            settingsGuid_ = record.guid;
            assets::LoadTextureSettings(settings_, path);
        }

        ImGui::Separator();
        ImGui::TextDisabled("Import");

        const assets::TextureSettings before = settings_;
        assets::TextureSettings edited = settings_;

        static const char* const kUsage[] = {"Color", "Normal Map", "Mask", "HDR", "Height"};
        int usage = static_cast<int>(edited.usage);
        ImGui::SetNextItemWidth(-widgets::kPanelRightMargin);
        if (ImGui::Combo("Usage", &usage, kUsage, IM_ARRAYSIZE(kUsage))) {
            edited.usage = static_cast<assets::TextureUsage>(usage);
        }
        widgets::Tooltip(
            "Menentukan format blok dan colorspace saat di-bake.\n"
            "Ditebak dari nama berkasnya, dan tebakan itu boleh ditolak.");

        static const char* const kQuality[] = {"Fast", "Balanced", "Best"};
        int quality = static_cast<int>(edited.quality);
        ImGui::SetNextItemWidth(-widgets::kPanelRightMargin);
        if (ImGui::Combo("Quality", &quality, kQuality, IM_ARRAYSIZE(kQuality))) {
            edited.quality = static_cast<assets::TextureQuality>(quality);
        }

        static const char* const kAlpha[] = {"None", "Punch-through", "Full"};
        int alpha = static_cast<int>(edited.alpha);
        ImGui::SetNextItemWidth(-widgets::kPanelRightMargin);
        if (ImGui::Combo("Alpha", &alpha, kAlpha, IM_ARRAYSIZE(kAlpha))) {
            edited.alpha = static_cast<assets::TextureAlpha>(alpha);
        }

        ImGui::Checkbox("Compress", &edited.compress);
        ImGui::SameLine();
        ImGui::Checkbox("Mips", &edited.generateMips);

        if (edited == before) {
            return;
        }
        settings_ = edited;
        // Lewat riwayat, bukan langsung: mengubah pengaturan impor adalah
        // suntingan seperti yang lain, dan yang tidak bisa dibatalkan membuat
        // orang takut mencobanya.
        context.history->Execute(
            std::make_unique<SetTextureSettingsCommand>(path, before, edited));
        context.history->CloseMergeGroup();
    }

    /// Akhiran berkas yang dijanjikan sebuah prompt, atau nullptr untuk yang
    /// tidak membuat berkas.
    static const char* PromptExtension(PromptKind kind) {
        switch (kind) {
            case PromptKind::NewMaterial: return ".simmat";
            case PromptKind::NewWhitebox: return ".simwhitebox";
            case PromptKind::NewFolder:
            case PromptKind::RenameFolder:
            case PromptKind::None: break;
        }
        return nullptr;
    }

    /// Jalur tujuan sebuah aset baru, atau false beserta sebabnya di
    /// `promptError_`.
    ///
    /// Dipakai bersama oleh setiap jenis aset yang bisa dibuat dari sini.
    /// Aturannya sama untuk semuanya, dan menyalinnya per jenis berarti jenis
    /// keenam yang lupa memeriksa berkas yang sudah ada akan menimpanya
    /// diam-diam.
    bool PrepareAssetPath(assets::AssetDatabase& db, const std::string& folder,
                          const std::string& name, const std::string& extension,
                          std::filesystem::path& outPath, std::string& outFileName) {
        if (!ValidateName(name)) {
            return false;
        }
        const std::filesystem::path base =
            folder.empty() ? db.Root() : db.Root() / std::filesystem::path(folder);
        std::error_code error;
        std::filesystem::create_directories(base, error);

        // Ekstensinya ditambahkan hanya kalau belum diketik: yang mengetik
        // "Batu.simmat" tidak sedang meminta "Batu.simmat.simmat".
        outFileName = name;
        if (outFileName.size() < extension.size() ||
            outFileName.compare(outFileName.size() - extension.size(), extension.size(),
                                extension) != 0) {
            outFileName += extension;
        }

        outPath = base / outFileName;
        if (std::filesystem::exists(outPath, error)) {
            promptError_ = "\"" + outFileName + "\" already exists here.";
            return false;
        }
        return true;
    }

    /// Memindai ulang, memilih yang baru, dan memberitahu. Sama untuk setiap
    /// jenis: aset yang dibuat tetapi tidak terpilih membuat orang mencarinya di
    /// daftar yang baru saja bertambah panjang.
    void FinishCreatedAsset(EditorContext& context, assets::AssetDatabase& db,
                            const std::string& folder, const std::string& fileName) {
        db.ScanNow();
        if (const assets::AssetRecord* record = db.FindByRelativePath(
                folder.empty() ? fileName : folder + "/" + fileName)) {
            selected_ = record->guid;
        }
        context.notifications->Success(fileName + " created");
    }

    /// Whitebox baru: kubus satuan.
    ///
    /// **Bukan berkas kosong.** Blockout dimulai dengan mendorong sisi, dan
    /// tidak ada sisi yang bisa didorong pada mesh tanpa isi — yang membuatnya
    /// akan mengira asetnya rusak.
    bool CreateWhitebox(EditorContext& context, assets::AssetDatabase& db,
                        const std::string& folder, const std::string& name) {
        std::filesystem::path path;
        std::string fileName;
        if (!PrepareAssetPath(db, folder, name, ".simwhitebox", path, fileName)) {
            return false;
        }

        const whitebox::WhiteboxMesh box = whitebox::WhiteboxMesh::MakeCube();
        const whitebox::WhiteboxIoResult result = whitebox::SaveToFile(box, path);
        if (!result.ok) {
            promptError_ = result.error;
            return false;
        }
        FinishCreatedAsset(context, db, folder, fileName);
        return true;
    }

    bool CreateMaterial(EditorContext& context, assets::AssetDatabase& db,
                        const std::string& folder, const std::string& name) {
        std::filesystem::path path;
        std::string fileName;
        if (!PrepareAssetPath(db, folder, name, ".simmat", path, fileName)) {
            return false;
        }

        // Satu simpul keluaran permukaan, sama dengan yang dibuat Material
        // Editor: material kosong yang sudah sah, bukan berkas kosong yang baru
        // menjadi material setelah disunting.
        material::MaterialGraph graph;
        material::MaterialNode output;
        output.guid = Uuid::Generate();
        output.type = std::string(material::kSurfaceOutputType);
        output.position = Vec2(240.0f, 80.0f);
        graph.nodes.push_back(std::move(output));

        if (!material::SaveMaterialToFile(graph, path).ok) {
            promptError_ = "Cannot write " + fileName;
            return false;
        }
        FinishCreatedAsset(context, db, folder, fileName);
        return true;
    }

    /// Prompt nama untuk aset baru. Menyusul `DrawDeletePrompt`: keduanya modal
    /// di lingkup panel, bukan di dalam child mana pun, supaya ID popup-nya tidak
    /// ikut berpindah bersama isi yang sedang digambar.
    void DrawPrompt(EditorContext& context, assets::AssetDatabase& db) {
        if (promptKind_ == PromptKind::None) {
            return;
        }
        const bool renaming = promptKind_ == PromptKind::RenameFolder;
        const char* const extension = PromptExtension(promptKind_);
        const char* title = renaming                                  ? "Rename folder"
                            : promptKind_ == PromptKind::NewMaterial  ? "New material"
                            : promptKind_ == PromptKind::NewWhitebox  ? "New whitebox"
                                                                      : "New folder";
        if (!ImGui::IsPopupOpen(title)) {
            ImGui::OpenPopup(title);
        }

        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            return;
        }

        const std::string context_folder = renaming ? ParentFolder(promptFolder_) : promptFolder_;
        ImGui::TextDisabled("in %s", context_folder.empty() ? "Assets" : context_folder.c_str());
        ImGui::Spacing();

        // Fokus diarahkan ke kotaknya sekali saat prompt muncul. Prompt yang
        // meminta nama tapi menunggu satu klik lagi sebelum bisa diketik adalah
        // langkah tambahan yang tidak menghasilkan apa pun.
        if (promptFocus_) {
            ImGui::SetKeyboardFocusHere();
            promptFocus_ = false;
        }
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
        const bool submitted =
            ImGui::InputText("##createname", &promptName_,
                             ImGuiInputTextFlags_EnterReturnsTrue |
                                 ImGuiInputTextFlags_AutoSelectAll);
        // Ekstensinya diperlihatkan, bukan disembunyikan: nama yang diketik
        // menjadi nama berkas, dan yang tidak tahu akhirannya akan mengetiknya
        // sendiri lalu mendapat dua.
        if (extension != nullptr) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", extension);
        }
        if (!promptError_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.40f, 1.0f));
            ImGui::TextUnformatted(promptError_.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        const std::string name = TrimName(promptName_);
        ImGui::BeginDisabled(name.empty());
        const bool confirm =
            ImGui::Button("Create", ImVec2(120.0f, 0.0f)) || (submitted && !name.empty());
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool cancel =
            ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape);

        if (confirm) {
            promptError_.clear();
            bool created = false;
            switch (promptKind_) {
                case PromptKind::NewFolder:
                    created = CreateFolder(context, db, promptFolder_, name);
                    break;
                case PromptKind::NewMaterial:
                    created = CreateMaterial(context, db, promptFolder_, name);
                    break;
                case PromptKind::NewWhitebox:
                    created = CreateWhitebox(context, db, promptFolder_, name);
                    break;
                case PromptKind::RenameFolder:
                    created = RenameFolder(context, db, promptFolder_, name);
                    break;
                case PromptKind::None:
                    break;
            }
            // Yang gagal membiarkan promptnya terbuka bersama pesannya. Menutupnya
            // berarti seluruh langkahnya harus diulang untuk memperbaiki satu huruf.
            if (created) {
                promptKind_ = PromptKind::None;
                ImGui::CloseCurrentPopup();
            } else {
                promptFocus_ = true;
            }
        } else if (cancel) {
            promptKind_ = PromptKind::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    /// Menyalin berkas yang dipilih pengguna ke dalam sebuah folder aset.
    ///
    /// **Menyalin, bukan menautkan.** Aset yang tinggal di luar folder project
    /// hilang begitu project dipindahkan atau dibuka di mesin lain, dan yang
    /// terlihat adalah level yang kehilangan separuh isinya tanpa sebab.
    void ImportAssets(EditorContext& context, assets::AssetDatabase& db,
                      const std::string& folder) {
        if (dialogOpen_) {
            return;
        }
        dialogOpen_ = true;
        // Penyaringnya menyebut yang bisa diimpor, dengan "semua berkas" di
        // ujungnya: daftar yang hanya memuat yang dikenal membuat berkas yang
        // sebenarnya bisa dipakai tampak tidak didukung.
        static const std::array<platform::FileDialogFilter, 5> kFilters{
            platform::FileDialogFilter{"All assets",
                                       "fbx;obj;gltf;glb;usd;usda;usdc;usdz;png;jpg;jpeg;tga;hdr;"
                                       "lua;simmat;simanim;simskel"},
            platform::FileDialogFilter{"Mesh", "fbx;obj;gltf;glb;usd;usda;usdc;usdz"},
            platform::FileDialogFilter{"Texture", "png;jpg;jpeg;tga;hdr"},
            platform::FileDialogFilter{"Script", "lua"},
            platform::FileDialogFilter{"All files", "*"}};

        const std::filesystem::path target =
            folder.empty() ? db.Root() : db.Root() / std::filesystem::path(folder);
        platform::ShowOpenFileDialog(
            kFilters, target.string(),
            [this, &context, &db, target](const platform::FileDialogResult& result) {
                dialogOpen_ = false;
                if (!result.error.empty()) {
                    context.notifications->Error(result.error);
                    return;
                }
                int copied = 0;
                for (const std::filesystem::path& source : result.paths) {
                    // **Berkas mesh tidak disalin begitu saja.** Satu FBX membawa
                    // geometri, rangka, take animasi, dan material sekaligus;
                    // menyalinnya sebagai satu berkas menyerahkan pekerjaan
                    // memecahnya kembali kepada yang mengimpornya. Yang ini
                    // ditahan dulu, lalu dialog pilihan yang memutuskan bagian
                    // mana yang menjadi aset.
                    if (IsMeshSource(source)) {
                        pendingImports_.push_back(source);
                        continue;
                    }
                    std::filesystem::path destination = target / source.filename();
                    std::error_code error;
                    // Yang sudah ada diberi akhiran, tidak ditimpa: menimpa aset
                    // yang sudah dipakai level adalah kehilangan yang tidak bisa
                    // dibatalkan dari sini.
                    if (std::filesystem::exists(destination, error)) {
                        const std::string stem = destination.stem().string();
                        const std::string extension = destination.extension().string();
                        int suffix = 1;
                        do {
                            destination.replace_filename(stem + " (" + std::to_string(++suffix) +
                                                         ")" + extension);
                        } while (std::filesystem::exists(destination, error));
                    }
                    std::filesystem::copy_file(source, destination, error);
                    if (error) {
                        context.notifications->Error(source.filename().string() + ": " +
                                                     error.message());
                        continue;
                    }
                    ++copied;
                }
                if (copied > 0) {
                    db.ScanNow();
                    context.notifications->Success(std::to_string(copied) + " assets imported");
                }
                if (!pendingImports_.empty()) {
                    importFolder_ = target;
                    importOptions_ = editor::SourceImportOptions{};
                    importContents_ = editor::InspectSource(pendingImports_.front());
                }
            },
            /*allowMany=*/true);
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

    /// Menu klik kanan pada ruang kosong daftar aset.
    ///
    /// **Digambar sesudah isinya, dan itu yang membuat urutannya benar.**
    /// `BeginPopupContextWindow` menyerah kalau sebuah item sudah mengklaim
    /// klik kanan pada frame itu, jadi menu aset tetap menang di atas asetnya
    /// sendiri — dan ruang kosong di sekitarnya mendapat menu folder ini.
    void DrawEmptySpaceMenu(EditorContext& context, assets::AssetDatabase& db) {
        if (!ImGui::BeginPopupContextWindow("##contentmenu",
                                            ImGuiPopupFlags_MouseButtonRight |
                                                ImGuiPopupFlags_NoOpenOverItems)) {
            return;
        }
        ImGui::TextDisabled("%s", currentFolder_.empty() ? "Assets" : currentFolder_.c_str());
        ImGui::Separator();
        ImGui::BeginDisabled(browsingBuiltin_ || dialogOpen_);
        if (ImGui::MenuItem("New folder")) {
            BeginPrompt(PromptKind::NewFolder, currentFolder_);
        }
        if (ImGui::MenuItem("New material")) {
            BeginPrompt(PromptKind::NewMaterial, currentFolder_);
        }
        if (ImGui::MenuItem("New whitebox")) {
            BeginPrompt(PromptKind::NewWhitebox, currentFolder_);
        }
        if (ImGui::MenuItem("Import assets...")) {
            ImportAssets(context, db, currentFolder_);
        }
        ImGui::EndDisabled();
        if (browsingBuiltin_) {
            ImGui::Separator();
            ImGui::TextDisabled("The built-in library cannot be edited");
        }
        ImGui::EndPopup();
    }

    void DrawItems(EditorContext& context, assets::AssetDatabase& db) {
        // Pencarian berlaku untuk seluruh sub-pohon, bukan hanya folder ini.
        // Mencari sesuatu lalu tidak menemukannya karena ia satu tingkat lebih
        // dalam adalah kegagalan yang tidak terlihat sebagai kegagalan.
        // Daftar yang terlihat disusun ulang hanya ketika ada yang berubah,
        // bukan tiap frame. InFolder() menelusuri seluruh indeks; pada 10.000
        // aset itu 600.000 pemeriksaan per detik untuk menghasilkan daftar yang
        // sama persis enam puluh kali berturut-turut. Inilah gunanya
        // AssetDatabase::Version() — ia naik hanya saat isinya benar-benar
        // berubah.
        const bool searching = !search_.empty();
        const ViewKey key{db.Version(), currentFolder_, search_, typeFilter_};
        if (key != viewKey_) {
            viewKey_ = key;
            visible_.clear();
            for (const AssetRecord* record : db.InFolder(currentFolder_, searching)) {
                if (Matches(*record)) {
                    visible_.push_back(record);
                }
            }
        }
        if (visible_.empty()) {
            ImGui::TextDisabled(searching ? "No assets match the search."
                                          : "This folder is empty.");
            // Justru folder kosong yang paling butuh menu ini: ia tempat orang
            // membuat folder pertamanya dan mengimpor aset pertamanya.
            DrawEmptySpaceMenu(context, db);
            return;
        }

        // Hanya baris yang benar-benar terlihat yang diajukan ke ImGui.
        //
        // Tanpa ini, folder berisi 10.000 aset mengajukan 10.000 widget setiap
        // frame — terukur lebih dari satu inti CPU penuh hanya untuk menggulir,
        // padahal yang terlihat di layar tidak sampai lima puluh. ImGuiListClipper
        // menuntut tinggi baris yang seragam, dan itulah alasan nama berkas
        // dipotong menjadi satu baris alih-alih dibiarkan membungkus.
        const auto count = static_cast<int>(visible_.size());
        if (gridMode_) {
            const float cell = thumbnailSize_ + ImGui::GetStyle().ItemSpacing.x;
            const int columns =
                std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / cell));
            const int rows = (count + columns - 1) / columns;
            const float rowHeight =
                thumbnailSize_ + ImGui::GetTextLineHeightWithSpacing() +
                ImGui::GetStyle().ItemSpacing.y;

            ImGuiListClipper clipper;
            clipper.Begin(rows, rowHeight);
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    for (int column = 0; column < columns; ++column) {
                        const int index = row * columns + column;
                        if (index >= count) {
                            break;
                        }
                        if (column > 0) {
                            ImGui::SameLine();
                        }
                        DrawGridItem(context, db, *visible_[static_cast<std::size_t>(index)]);
                    }
                }
            }
        } else {
            ImGuiListClipper clipper;
            clipper.Begin(count, ImGui::GetTextLineHeightWithSpacing());
            while (clipper.Step()) {
                for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
                    DrawListItem(context, db, *visible_[static_cast<std::size_t>(index)]);
                }
            }
        }
        DrawEmptySpaceMenu(context, db);
    }

    /// Memotong teks agar muat dalam `width`, dengan elipsis bila perlu.
    static std::string Ellipsize(const std::string& text, float width) {
        if (ImGui::CalcTextSize(text.c_str()).x <= width) {
            return text;
        }
        std::string result = text;
        while (!result.empty() &&
               ImGui::CalcTextSize((result + "...").c_str()).x > width) {
            result.pop_back();
        }
        return result + "...";
    }

    void DrawGridItem(EditorContext& context, assets::AssetDatabase& db,
                      const AssetRecord& record) {
        ImGui::PushID(record.relativePath.c_str());
        ImGui::BeginGroup();

        const bool selected = selected_ == record.guid;
        const ImVec2 origin = ImGui::GetCursorScreenPos();

        // Selectable digambar lebih dulu supaya ia yang menerima klik dan
        // seretan; gambarnya ditumpangkan setelahnya lewat drawlist. Urutan
        // sebaliknya membuat thumbnail menutupi daerah kliknya sendiri.
        ImGui::PushFont(nullptr, thumbnailSize_ * 0.45f);
        const char* label = HasPreview(context, record) ? "##thumb" : IconFor(record.type);
        if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_AllowDoubleClick,
                              ImVec2(thumbnailSize_, thumbnailSize_))) {
            selected_ = record.guid;
        }
        ImGui::PopFont();

        DrawPreview(context, db, record, origin);
        HandleItemInteraction(context, db, record);

        if (renaming_ == record.guid) {
            ImGui::SetNextItemWidth(thumbnailSize_);
            DrawRenameField(context, db, record);
        } else {
            // Satu baris, dipotong dengan elipsis. Membungkus ke baris kedua
            // membuat tinggi sel tidak seragam, dan ImGuiListClipper — yang
            // membuat folder besar tetap mulus — menuntut tinggi yang tetap.
            ImGui::TextUnformatted(Ellipsize(record.name, thumbnailSize_).c_str());
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                ImGui::SetTooltip("%s", record.name.c_str());
            }
        }

        ImGui::EndGroup();
        ImGui::PopID();
    }

    /// True bila aset ini punya (atau akan punya) gambar pratinjau.
    ///
    /// Hanya tekstur. Meminta thumbnail untuk berkas 3,6 MB yang bukan gambar
    /// berarti membaca dan mem-hash seluruh isinya di thread latar hanya untuk
    /// menyimpulkan bahwa ia memang bukan gambar.
    static bool HasPreview(const EditorContext& context, const AssetRecord& record) {
        return context.thumbnails != nullptr && record.type == AssetType::Texture;
    }

    void DrawPreview(EditorContext& context, const assets::AssetDatabase& db,
                     const AssetRecord& record, const ImVec2& origin) {
        if (!HasPreview(context, record)) {
            return;
        }
        // Penanda isi: ukuran dan waktu ubah. Cukup untuk menyadari berkasnya
        // ditimpa dari luar editor, tanpa perlu membaca isinya lagi di sini.
        const uint64_t contentTag = (static_cast<uint64_t>(record.fileSize) * 1099511628211ull) ^
                                    static_cast<uint64_t>(record.modifiedSeconds);
        const render::TextureHandle handle =
            context.thumbnails->Request(record.guid, db.AbsolutePath(record), contentTag);
        if (handle == render::kInvalidTexture) {
            // Belum siap: ikon tipe tetap digambar sebagai pengganti, sehingga
            // grid tidak berkedip antara kosong dan terisi.
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 centre(origin.x + thumbnailSize_ * 0.5f,
                                origin.y + thumbnailSize_ * 0.5f);
            const float glyphSize = thumbnailSize_ * 0.45f;
            const ImVec2 extent =
                ImGui::GetFont()->CalcTextSizeA(glyphSize, FLT_MAX, 0.0f, IconFor(record.type));
            draw->AddText(ImGui::GetFont(), glyphSize,
                          ImVec2(centre.x - extent.x * 0.5f, centre.y - extent.y * 0.5f),
                          ImGui::GetColorU32(ImGuiCol_TextDisabled), IconFor(record.type));
            return;
        }

        // Proporsi gambar dijaga di dalam sel persegi. Meregangkannya penuh
        // membuat tekstur panjang tampak gepeng — persis ciri yang dipakai
        // untuk mengenalinya sekilas.
        const float aspect = record.height > 0 ? static_cast<float>(record.width) /
                                                     static_cast<float>(record.height)
                                               : 1.0f;
        float width = thumbnailSize_;
        float height = thumbnailSize_;
        if (aspect > 1.0f) {
            height = thumbnailSize_ / aspect;
        } else if (aspect > 0.0f) {
            width = thumbnailSize_ * aspect;
        }
        const ImVec2 min(origin.x + (thumbnailSize_ - width) * 0.5f,
                         origin.y + (thumbnailSize_ - height) * 0.5f);
        ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(handle), min,
                                            ImVec2(min.x + width, min.y + height));
    }

    void DrawListItem(EditorContext& context, assets::AssetDatabase& db,
                      const AssetRecord& record) {
        ImGui::PushID(record.relativePath.c_str());

        if (renaming_ == record.guid) {
            DrawRenameField(context, db, record);
            ImGui::PopID();
            return;
        }

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

        // Klik ganda membuka asetnya di editor yang menanganinya. Panel yang
        // tidak punya editornya diam saja — dan itu disebutkan, karena klik ganda
        // yang tidak melakukan apa pun terbaca sebagai editor yang menggantung.
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
            context.openAsset) {
            if (!context.openAsset(record.guid)) {
                context.notifications->Info("No editor for " + record.name);
            }
        }

        if (ImGui::BeginPopupContextItem("##assetmenu")) {
            selected_ = record.guid;
            ImGui::TextDisabled("%s", record.name.c_str());
            ImGui::Separator();

            if (browsingBuiltin_) {
                // **Menyalin, bukan menyunting.** Isi bawaan ikut pemasangan
                // editor: apa pun yang ditulis ke sana hilang pada pemasangan
                // berikutnya, dan berkas yang lenyap tanpa sebab jauh lebih
                // membingungkan daripada aksi yang memang tidak ditawarkan.
                const bool hasProject = context.assets != nullptr;
                ImGui::BeginDisabled(!hasProject);
                if (ImGui::MenuItem("Copy to project")) {
                    CopyBuiltinToProject(context, db, record);
                }
                ImGui::EndDisabled();
                if (ImGui::MenuItem("Copy GUID")) {
                    ImGui::SetClipboardText(record.guid.ToString().c_str());
                    context.notifications->Info("GUID copied");
                }
                ImGui::EndPopup();
                return;
            }

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
                // Pemakai di luar indeks aset — scene yang sedang dibuka dan
                // berkas level milik editor — ditanyakan ke EditorApp. Tanpa
                // ini, dialognya bisa berkata "tidak ada yang memakai" tepat
                // ketika yang memakainya adalah level yang sedang disunting.
                deleteExternalUsers_.clear();
                if (context.findExternalAssetUsers) {
                    deleteExternalUsers_ = context.findExternalAssetUsers(record.guid);
                }
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

        // Nama diganti di itemnya sendiri, bukan di sini: panel detail
        // menghilang pada panel sempit, dan menaruh kotak teksnya di sana
        // membuat menu "Rename" tampak tidak melakukan apa pun.
        ImGui::TextUnformatted(record->name.c_str());
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
        // Pengaturan impor tekstur. **Di sini, bukan di Inspector**: Inspector
        // menampilkan komponen sebuah entity, dan tekstur bukan entity. Yang
        // memilih aset adalah panel ini, dan panel inilah yang sudah memuat
        // rincian aset di sebelahnya.
        if (record->type == assets::AssetType::Texture) {
            DrawTextureSettings(context, *record);
        }

        if (record->triangleCount > 0) {
            char stats[80];
            std::snprintf(stats, sizeof(stats), "%u tris, %u verts", record->triangleCount,
                          record->vertexCount);
            DetailRow("Geometry", stats);
        } else if (record->type == assets::AssetType::Mesh) {
            // Disebutkan, bukan dikosongkan. Baris yang hilang terbaca sebagai
            // "mesh ini tidak punya segitiga"; yang ini menjelaskan bahwa
            // angkanya datang dari yang memuatnya, dan mesh ini belum pernah
            // dimuat.
            DetailRow("Geometry", "not loaded yet");
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
        DrawReferenceList("Used by", db.UsersOf(record->guid), db,
                          context.findExternalAssetUsers
                              ? context.findExternalAssetUsers(record->guid)
                              : std::vector<std::string>{});
        DrawReferenceList("Uses", record->dependencies, db);
    }

    /// Menyalin sebuah aset bawaan ke dalam project yang sedang dibuka.
    ///
    /// **Berkas `.meta`-nya sengaja TIDAK ikut disalin.** GUID yang sama di dua
    /// tempat berarti dua berkas yang mengaku aset yang sama, dan yang menang
    /// bergantung urutan pemindaian — rujukan di dalam level lalu menunjuk salah
    /// satunya secara acak. Salinan mendapat identitas barunya sendiri, seperti
    /// berkas baru mana pun yang jatuh ke folder aset.
    void CopyBuiltinToProject(EditorContext& context, assets::AssetDatabase& builtin,
                              const assets::AssetRecord& record) {
        if (context.assets == nullptr) {
            return;
        }
        const std::filesystem::path source = builtin.AbsolutePath(record);
        // Ditaruh pada jalur relatif yang sama seperti di pustaka bawaan:
        // "Materials/Kayu.simmat" mendarat di "Materials/Kayu.simmat".
        std::filesystem::path target = context.assets->Root() / record.relativePath;

        std::error_code error;
        std::filesystem::create_directories(target.parent_path(), error);
        // Yang sudah ada tidak ditimpa melainkan diberi akhiran. Menimpa aset
        // yang sudah disunting orang dengan versi bawaannya adalah kehilangan
        // pekerjaan yang tidak bisa dibatalkan dari sini.
        if (std::filesystem::exists(target, error)) {
            const std::string stem = target.stem().string();
            const std::string extension = target.extension().string();
            int suffix = 1;
            do {
                target.replace_filename(stem + " (" + std::to_string(++suffix) + ")" + extension);
            } while (std::filesystem::exists(target, error));
        }

        std::filesystem::copy_file(source, target, error);
        if (error) {
            context.notifications->Error("Cannot copy: " + error.message());
            return;
        }
        // Pemindaian dipaksa selesai supaya asetnya langsung terlihat. Menunggu
        // pemindaian latar berarti aksi yang tampak tidak melakukan apa pun.
        context.assets->ScanNow();
        context.notifications->Success("Copied to " + target.filename().string());
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
                           const assets::AssetDatabase& db,
                           const std::vector<std::string>& external = {}) {
        ImGui::TextDisabled("%s (%zu)", title, guids.size() + external.size());
        // Pemakai di luar indeks aset tidak bisa diklik — ia bukan aset, jadi
        // tidak ada yang bisa dipilih di panel ini.
        for (const std::string& user : external) {
            ImGui::BulletText("%s", user.c_str());
        }
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


    /// Benar untuk berkas yang bisa membawa lebih dari satu jenis aset.
    static bool IsMeshSource(const std::filesystem::path& path) {
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return extension == ".fbx" || extension == ".obj" || extension == ".gltf" ||
               extension == ".glb" || extension == ".usd" || extension == ".usda" ||
               extension == ".usdc" || extension == ".usdz";
    }

    /// Pilihan bagian mana dari berkas sumber yang menjadi aset.
    ///
    /// **Isinya dibaca lebih dulu, dan yang kosong diredupkan.** Kotak centang
    /// "Animation" yang bisa dicentang pada berkas tanpa animasi menjanjikan
    /// sesuatu yang tidak akan terjadi, dan yang mengimpornya baru tahu setelah
    /// mencari aset yang tidak pernah ditulis.
    void DrawImportOptions(EditorContext& context, assets::AssetDatabase& db) {
        if (pendingImports_.empty()) {
            return;
        }
        constexpr const char* kTitle = "Import options";
        if (!ImGui::IsPopupOpen(kTitle)) {
            ImGui::OpenPopup(kTitle);
        }
        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            return;
        }

        const editor::SourceContents& contents = importContents_;
        if (pendingImports_.size() == 1) {
            ImGui::Text("%s", pendingImports_.front().filename().string().c_str());
        } else {
            ImGui::Text("%zu files", pendingImports_.size());
            ImGui::TextDisabled("Options apply to all of them; the counts below are for \"%s\".",
                                pendingImports_.front().filename().string().c_str());
        }
        ImGui::Separator();

        const auto option = [](const char* label, bool available, int count, bool* value,
                               const char* empty) {
            ImGui::BeginDisabled(!available);
            if (!available) {
                *value = false;
            }
            ImGui::Checkbox(label, value);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (available) {
                ImGui::TextDisabled("%d", count);
            } else {
                ImGui::TextDisabled("%s", empty);
            }
        };

        option("Mesh", contents.hasMesh, 1, &importOptions_.mesh, "none");
        option("Skeleton", contents.boneCount > 0, contents.boneCount, &importOptions_.skeleton,
               "no bones");
        option("Animation", contents.clipCount > 0, contents.clipCount, &importOptions_.animation,
               "no clips");
        option("Materials", contents.materialCount > 0, contents.materialCount,
               &importOptions_.materials, "none");

        if (!contents.error.empty()) {
            ImGui::TextDisabled("%s", contents.error.c_str());
        }

        ImGui::Separator();
        const bool nothing = !importOptions_.mesh && !importOptions_.skeleton &&
                             !importOptions_.animation && !importOptions_.materials;
        ImGui::BeginDisabled(nothing);
        if (ImGui::Button("Import")) {
            RunPendingImports(context, db);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            pendingImports_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void RunPendingImports(EditorContext& context, assets::AssetDatabase& db) {
        std::size_t written = 0;
        std::size_t failed = 0;
        for (const std::filesystem::path& source : pendingImports_) {
            const editor::SourceImportResult result =
                editor::ImportSource(source, importFolder_, importOptions_);
            if (!result.ok) {
                ++failed;
                if (context.notifications != nullptr) {
                    context.notifications->Error(source.filename().string() + ": " + result.error);
                }
                continue;
            }
            written += result.written.size();
        }
        pendingImports_.clear();
        db.ScanNow();
        if (written > 0 && context.notifications != nullptr) {
            context.notifications->Success(std::to_string(written) + " assets imported");
        }
        (void)failed;
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
        const std::size_t userCount = deleteUsers_.size() + deleteExternalUsers_.size();
        if (userCount > 0) {
            // Daftar pemakainya ditampilkan, bukan sekadar jumlahnya. "3 aset
            // memakai ini" tidak cukup untuk memutuskan apa pun; yang dibutuhkan
            // adalah tahu aset mana, supaya bisa diperiksa lebih dulu.
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
                               "Still used in %zu place(s):", userCount);
            // Yang di luar indeks lebih dulu: scene yang sedang dibuka adalah
            // pemakai yang paling mungkin membuat pengguna membatalkan.
            for (const std::string& user : deleteExternalUsers_) {
                ImGui::BulletText("%s", user.c_str());
            }
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

    void ResolveFolderMove(EditorContext& context, assets::AssetDatabase& db) {
        if (!pendingFolderMove_.valid) {
            return;
        }
        const PendingFolderMove move = pendingFolderMove_;
        pendingFolderMove_ = {};
        if (browsingBuiltin_) {
            context.notifications->Error("The built-in library cannot be edited.");
            return;
        }
        MoveFolder(context, db, move.source, move.destination);
    }

    void ResolvePendingMove(EditorContext& context, assets::AssetDatabase& db) {
        if (!pendingMove_.valid) {
            return;
        }
        const PendingMove move = pendingMove_;
        pendingMove_ = {};

        const AssetRecord* record = db.Find(move.guid);
        if (record == nullptr) {
            return;
        }
        // Menjatuhkan ke folder asalnya sendiri bukan kesalahan, hanya tidak
        // ada yang perlu dikerjakan.
        const std::size_t slash = record->relativePath.find_last_of('/');
        const std::string currentParent =
            slash == std::string::npos ? std::string{} : record->relativePath.substr(0, slash);
        if (currentParent == move.folder) {
            return;
        }

        std::string error;
        if (db.Move(move.guid, move.folder, error)) {
            context.notifications->Success("Moved to " +
                                           (move.folder.empty() ? "Assets" : move.folder));
        } else {
            context.notifications->Error("Move failed: " + error);
        }
    }

    static void DetailRow(const char* label, const char* value) {
        ImGui::TextDisabled("%s", label);
        ImGui::TextWrapped("%s", value);
        ImGui::Spacing();
    }

    /// Pemindahan yang diminta lewat seret-lepas, dijalankan setelah traversal.
    struct PendingMove {
        Uuid guid;
        std::string folder;
        bool valid = false;
    };

    /// Penentu apakah daftar terlihat perlu disusun ulang.
    struct ViewKey {
        uint64_t version = 0;
        std::string folder;
        std::string search;
        int typeFilter = -1;

        friend bool operator==(const ViewKey& a, const ViewKey& b) {
            return a.version == b.version && a.folder == b.folder && a.search == b.search &&
                   a.typeFilter == b.typeFilter;
        }
        friend bool operator!=(const ViewKey& a, const ViewKey& b) { return !(a == b); }
    };

    ViewKey viewKey_{0, {}, {}, -1};
    std::vector<const AssetRecord*> visible_;
    PendingMove pendingMove_;

    /// Perpindahan folder yang menunggu. Dikerjakan setelah seluruh pohon
    /// digambar, sama alasannya dengan `pendingMove_`: memindahkan folder
    /// mengganti daftar yang sedang diiterasi.
    struct PendingFolderMove {
        std::string source;
        std::string destination;
        bool valid = false;
    };
    PendingFolderMove pendingFolderMove_;
    std::string search_;
    std::string currentFolder_;
    std::string renameBuffer_;

    /// Aset yang pengaturannya sedang dipegang `settings_`, supaya berkasnya
    /// dibaca sekali per pilihan alih-alih tiap frame.
    Uuid settingsGuid_;
    assets::TextureSettings settings_;

    PromptKind promptKind_ = PromptKind::None;
    /// Folder tujuan, dibekukan saat prompt dibuka: orang bisa berpindah folder
    /// di balik prompt, dan berkasnya harus tetap lahir di tempat yang ditunjuk.
    std::string promptFolder_;
    std::string promptName_;
    std::string promptError_;
    bool promptFocus_ = false;
    std::vector<std::string> favourites_;
    std::vector<Uuid> deleteUsers_;
    /// Pemakai di luar indeks aset, sudah berbentuk teks siap tampil.
    std::vector<std::string> deleteExternalUsers_;
    Uuid selected_;
    Uuid renaming_;
    Uuid pendingDelete_;
    float thumbnailSize_ = 72.0f;
    int typeFilter_ = 0;
    bool gridMode_ = true;
    /// Sedang melihat pustaka bawaan, bukan aset project.
    bool browsingBuiltin_ = false;
    /// Sebuah dialog sistem sedang terbuka. Aksinya dimatikan selama itu — dua
    /// dialog yang keduanya menyalin ke folder yang sama akan selesai dalam
    /// urutan yang tidak bisa ditebak.
    bool dialogOpen_ = false;
    /// Berkas mesh yang menunggu dialog pilihan impor.
    std::vector<std::filesystem::path> pendingImports_;
    std::filesystem::path importFolder_;
    editor::SourceImportOptions importOptions_;
    editor::SourceContents importContents_;
    bool justStartedRenaming_ = false;
};

}  // namespace

SIM_REGISTER_PANEL(AssetBrowserPanel, 20)

}  // namespace sim::editor
