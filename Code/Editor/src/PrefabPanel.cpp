#include "Sim/Core/Log.h"
#include "Sim/Editor/Command.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Editor/SceneCommands.h"
#include "Sim/Editor/Selection.h"
#include "Sim/Scene/World.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace sim::editor {
namespace {

/// Satu prefab yang bisa ditempatkan, beserta grupnya.
struct PrefabEntry {
    std::string group;
    std::string name;
    std::filesystem::path path;
    /// True untuk template bawaan editor. Read-only, dan ditandai berbeda:
    /// yang tinggal di `Resources` sama untuk setiap project, dan menyunting
    /// atau menghapusnya bukan pilihan yang ditawarkan di sini.
    bool builtin = false;
};

bool ContainsFold(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    const auto fold = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                       [&](char a, char b) { return fold(a) == fold(b); }) != haystack.end();
}

/// Mengumpulkan `.simprefab` di bawah sebuah folder.
///
/// **Subfoldernya yang menjadi grup.** Bukan sebuah field di dalam berkasnya:
/// grup yang tersimpan di dalam berkas menuntut setiap berkas dibuka hanya
/// untuk menggambar daftarnya, dan folder sudah menjadi cara orang menyusun
/// aset di setiap alat lain. Berkas yang tergeletak di akar masuk grup
/// "Ungrouped" — terlihat, bukan tersembunyi.
void Collect(const std::filesystem::path& root, bool builtin, std::vector<PrefabEntry>& out) {
    std::error_code code;
    if (root.empty() || !std::filesystem::is_directory(root, code)) {
        return;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, code)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".simprefab") {
            continue;
        }
        const std::filesystem::path relative = std::filesystem::relative(entry.path(), root, code);
        PrefabEntry item;
        item.group = relative.has_parent_path() && !relative.parent_path().empty()
                         ? relative.parent_path().string()
                         : "Ungrouped";
        item.name = entry.path().stem().string();
        item.path = entry.path();
        item.builtin = builtin;
        out.push_back(std::move(item));
    }
}

/// Ikon yang mewakili isi sebuah grup. Bukan hiasan: daftar yang seluruhnya
/// memakai ikon kotak membuat mata harus membaca setiap barisnya.
const char* IconForGroup(std::string_view group) {
    if (group == "Lights") {
        return icons::kLight;
    }
    if (group == "Cameras") {
        return icons::kCamera;
    }
    if (group == "Environment") {
        return icons::kVolume;
    }
    return icons::kPrefab;
}

/// Palet prefab: template bawaan dan prefab milik project, siap ditempatkan.
///
/// **Menempatkan lewat `PasteEntitiesCommand`, bukan `InstantiatePrefab`.**
/// Keduanya membangun sub-pohon yang sama dengan GUID baru, tapi yang pertama
/// sudah berupa perintah — jadi menempatkan prefab bisa di-undo seperti operasi
/// scene lainnya. Prefab yang mendarat di scene tanpa bisa dibatalkan adalah
/// prefab yang orang ragu mengkliknya.
class PrefabPanel final : public Panel {
public:
    PrefabPanel()
        : Panel(panel_id::kPrefabs, std::string(icons::kPrefab) + "  Prefabs",
                PanelCategory::Assets) {
        SetOpen(false);
    }

    void OnDraw(EditorContext& context) override {
        if (context.world == nullptr) {
            ImGui::TextDisabled("No scene open.");
            return;
        }

        Rescan(context);

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##prefab-filter", "Filter prefabs...", &filter_);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::Spacing();

        std::size_t shown = 0;
        std::string currentGroup;
        bool groupOpen = false;

        for (const PrefabEntry& entry : entries_) {
            if (!ContainsFold(entry.name, filter_) && !ContainsFold(entry.group, filter_)) {
                continue;
            }
            if (entry.group != currentGroup) {
                if (!currentGroup.empty() && groupOpen) {
                    ImGui::TreePop();
                }
                currentGroup = entry.group;
                // Terbuka saat ada filter: menyembunyikan hasil pencarian di
                // balik simpul yang tertutup meniadakan gunanya mencari.
                ImGui::SetNextItemOpen(true, filter_.empty() ? ImGuiCond_Once : ImGuiCond_Always);
                groupOpen = ImGui::TreeNode(
                    (std::string(IconForGroup(currentGroup)) + "  " + currentGroup).c_str());
            }
            if (!groupOpen) {
                continue;
            }
            ++shown;
            DrawEntry(context, entry);
        }
        if (!currentGroup.empty() && groupOpen) {
            ImGui::TreePop();
        }

        if (entries_.empty()) {
            ImGui::TextDisabled("No prefabs found.");
            ImGui::TextDisabled("Save one from the Outliner, or add .simprefab files");
            ImGui::TextDisabled("under the project's Prefabs folder.");
        } else if (shown == 0) {
            ImGui::TextDisabled("Nothing matches '%s'.", filter_.c_str());
        }
    }

private:
    void DrawEntry(EditorContext& context, const PrefabEntry& entry) {
        ImGui::PushID(entry.path.string().c_str());
        const std::string label = std::string(icons::kPrefab) + "  " + entry.name;
        if (ImGui::Selectable(label.c_str())) {
            Place(context, entry);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s\n%s", entry.path.string().c_str(),
                              entry.builtin ? "Built-in template — click to place"
                                            : "Project prefab — click to place");
        }
        if (entry.builtin) {
            ImGui::SameLine();
            ImGui::TextDisabled("built-in");
        }
        ImGui::PopID();
    }

    /// Menempatkan sebuah prefab di bawah entity terpilih, atau sebagai akar.
    void Place(EditorContext& context, const PrefabEntry& entry) {
        std::ifstream stream(entry.path);
        if (!stream) {
            SIM_WARN("Editor", "cannot open prefab {}", entry.path.string());
            return;
        }
        std::string text((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());
        if (text.empty()) {
            SIM_WARN("Editor", "prefab {} is empty", entry.path.string());
            return;
        }

        // Ditempatkan di bawah yang sedang terpilih bila ada. Itu yang
        // diharapkan saat menyusun hierarki — dan bila tidak ada yang terpilih,
        // ia menjadi akar alih-alih menolak.
        Uuid parent;
        if (context.selection != nullptr && context.world != nullptr) {
            // `Primary` — yang terakhir diklik, bukan yang pertama di daftar.
            // Itu yang dimaksud orang saat memilih beberapa lalu menempatkan.
            const SelectionId primary = context.selection->Primary();
            if (primary != kInvalidSelection) {
                parent = context.world->GuidOf(ToEntity(primary));
            }
        }

        if (context.history == nullptr) {
            SIM_WARN("Editor", "no command history; prefab not placed");
            return;
        }
        context.history->Execute<PasteEntitiesCommand>(
            context.world, context.selection, std::vector<std::string>{std::move(text)}, parent,
            "Place " + entry.name);
    }

    /// Memindai ulang folder prefab, tapi tidak tiap frame.
    ///
    /// Menyisir dua pohon folder enam puluh kali per detik membebani disk untuk
    /// daftar yang berubah beberapa kali per sesi. Yang memicunya adalah folder
    /// yang berganti — membuka project lain — dan tombol muat ulang di bawah.
    void Rescan(const EditorContext& context) {
        const std::string builtin = context.builtinDir;
        const std::string project = context.prefabDir;
        if (!scanned_ || builtin != scannedBuiltin_ || project != scannedProject_) {
            entries_.clear();
            if (!builtin.empty()) {
                Collect(std::filesystem::path(builtin) / "Prefabs", /*builtin=*/true, entries_);
            }
            Collect(std::filesystem::path(project), /*builtin=*/false, entries_);
            // Diurutkan per grup lalu per nama, sekali di sini — daftar yang
            // urutannya berubah antar-frame membuat mengklik jadi undian.
            std::sort(entries_.begin(), entries_.end(),
                      [](const PrefabEntry& a, const PrefabEntry& b) {
                          return a.group != b.group ? a.group < b.group : a.name < b.name;
                      });
            scanned_ = true;
            scannedBuiltin_ = builtin;
            scannedProject_ = project;
        }
    }

    std::vector<PrefabEntry> entries_;
    std::string filter_;
    bool scanned_ = false;
    std::string scannedBuiltin_;
    std::string scannedProject_;
};

}  // namespace

SIM_REGISTER_PANEL(PrefabPanel, 21)

}  // namespace sim::editor
