#include "Sim/Core/Log.h"
#include "Sim/Editor/Command.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Editor/SceneCommands.h"
#include "Sim/SceneView/Selection.h"
#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Editor/Notifications.h"
#include "Sim/Scene/World.h"
#include "Sim/Whitebox/WhiteboxIo.h"
#include "Sim/Whitebox/WhiteboxMesh.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sim::editor {
namespace {

/// Bentuk awal whitebox yang ditawarkan panel ini.
///
/// **Bukan berkas `.simprefab`, dan itu bukan kekurangan.** Sebuah prefab
/// menyimpan GUID asetnya, jadi sepuluh blok yang ditempatkan dari satu prefab
/// whitebox akan menunjuk satu aset yang sama — menyunting salah satunya
/// mengubah kesepuluhnya. Itu kebalikan dari gunanya blockout, tempat setiap
/// blok memang dibentuk sendiri-sendiri.
///
/// Yang ditempatkan entri ini karena itu membuat asetnya lebih dulu, sama
/// dengan "New whitebox" di Asset Browser, lalu menempatkan entity yang
/// menunjuk aset baru itu.
enum class WhiteboxShape { Box, Ramp, Stairs, Cylinder, Cone };

/// Satu prefab yang bisa ditempatkan, beserta grupnya.
struct PrefabEntry {
    std::string group;
    std::string name;
    std::filesystem::path path;
    /// True untuk template bawaan editor. Read-only, dan ditandai berbeda:
    /// yang tinggal di `Resources` sama untuk setiap project, dan menyunting
    /// atau menghapusnya bukan pilihan yang ditawarkan di sini.
    bool builtin = false;
    /// Terisi untuk entri whitebox, yang membangkitkan asetnya saat ditempatkan
    /// alih-alih membaca berkas prefab.
    std::optional<WhiteboxShape> shape;
};

/// Bentuk awal untuk sebuah pilihan. Satu tempat, supaya nama entri di daftar
/// dan geometri yang keluar tidak bisa berselisih.
whitebox::WhiteboxMesh BuildShape(WhiteboxShape shape) {
    switch (shape) {
        case WhiteboxShape::Box: return whitebox::WhiteboxMesh::MakeCube();
        case WhiteboxShape::Ramp: return whitebox::WhiteboxMesh::MakeRamp();
        case WhiteboxShape::Stairs: return whitebox::WhiteboxMesh::MakeStairs();
        case WhiteboxShape::Cylinder: return whitebox::WhiteboxMesh::MakeCylinder();
        case WhiteboxShape::Cone: return whitebox::WhiteboxMesh::MakeCone();
    }
    return whitebox::WhiteboxMesh::MakeCube();
}

const char* ShapeName(WhiteboxShape shape) {
    switch (shape) {
        case WhiteboxShape::Box: return "Box";
        case WhiteboxShape::Ramp: return "Ramp";
        case WhiteboxShape::Stairs: return "Stairs";
        case WhiteboxShape::Cylinder: return "Cylinder";
        case WhiteboxShape::Cone: return "Cone";
    }
    return "Box";
}

/// Folder tempat blok baru mendarat, relatif terhadap akar aset project.
constexpr std::string_view kWhiteboxFolder = "Whitebox";

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
    if (group == "Physics") {
        return icons::kPhysics;
    }
    if (group == "Whitebox") {
        return icons::kWhitebox;
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
        ImGui::PushID(entry.shape ? entry.name.c_str() : entry.path.string().c_str());
        const char* icon = entry.shape ? icons::kWhitebox : icons::kPrefab;
        const std::string label = std::string(icon) + "  " + entry.name;
        if (ImGui::Selectable(label.c_str())) {
            Place(context, entry);
        }
        if (ImGui::IsItemHovered()) {
            if (entry.shape) {
                // **Disebutkan bahwa ia membuat aset.** Prefab lain hanya
                // menambah entity; yang ini menulis berkas ke project, dan
                // undo mengembalikan entity-nya tanpa menghapus berkasnya —
                // aturan yang sama dengan setiap pembuatan aset di editor ini.
                ImGui::SetTooltip(
                    "Blockout shape — click to create a new whitebox asset\n"
                    "in %s/ and place a block that uses it.\n"
                    "Each click makes its own asset, so blocks are edited apart.",
                    std::string(kWhiteboxFolder).c_str());
            } else {
                ImGui::SetTooltip("%s\n%s", entry.path.string().c_str(),
                                  entry.builtin ? "Built-in template — click to place"
                                                : "Project prefab — click to place");
            }
        }
        if (entry.builtin && !entry.shape) {
            ImGui::SameLine();
            ImGui::TextDisabled("built-in");
        }
        ImGui::PopID();
    }

    /// Menempatkan sebuah prefab di bawah entity terpilih, atau sebagai akar.
    void Place(EditorContext& context, const PrefabEntry& entry) {
        std::string text;
        if (entry.shape) {
            text = MakeWhiteboxPrefab(context, *entry.shape);
            if (text.empty()) {
                return;  // sudah dilaporkan ke pengguna
            }
        } else {
            std::ifstream stream(entry.path);
            if (!stream) {
                SIM_WARN("Editor", "cannot open prefab {}", entry.path.string());
                return;
            }
            text.assign(std::istreambuf_iterator<char>(stream),
                        std::istreambuf_iterator<char>());
            if (text.empty()) {
                SIM_WARN("Editor", "prefab {} is empty", entry.path.string());
                return;
            }
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
            AddWhiteboxShapes(entries_);
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

    /// Lima bentuk awal blockout, selalu ada dan tidak datang dari berkas mana
    /// pun. Ditandai `builtin` seperti template `Resources`: sama untuk setiap
    /// project, dan tidak ada yang bisa disunting atau dihapus dari sini.
    static void AddWhiteboxShapes(std::vector<PrefabEntry>& out) {
        for (const WhiteboxShape shape :
             {WhiteboxShape::Box, WhiteboxShape::Ramp, WhiteboxShape::Stairs,
              WhiteboxShape::Cylinder, WhiteboxShape::Cone}) {
            PrefabEntry entry;
            entry.group = "Whitebox";
            entry.name = ShapeName(shape);
            entry.builtin = true;
            entry.shape = shape;
            out.push_back(std::move(entry));
        }
    }

    /// Membuat aset whitebox baru, lalu mengembalikan teks prefab yang menunjuk
    /// ke sana. Kosong bila asetnya tidak bisa ditulis.
    ///
    /// **Teks prefab, bukan entity langsung** — supaya penempatannya menempuh
    /// `Place` yang sama persis dengan prefab berkas: satu jalur, satu perintah
    /// undo, satu aturan tentang di bawah entity mana ia mendarat.
    static std::string MakeWhiteboxPrefab(EditorContext& context, WhiteboxShape shape) {
        if (context.assets == nullptr) {
            return {};
        }
        assets::AssetDatabase& db = *context.assets;
        const std::filesystem::path folder = db.Root() / std::filesystem::path(kWhiteboxFolder);
        std::error_code code;
        std::filesystem::create_directories(folder, code);

        // Nama bernomor sampai yang belum terpakai. Blok kedua yang menimpa
        // bentuk blok pertama adalah kehilangan pekerjaan tanpa satu pun
        // peringatan.
        const std::string base = ShapeName(shape);
        std::filesystem::path path;
        std::string fileName;
        for (int suffix = 0; suffix < 10000; ++suffix) {
            fileName = suffix == 0 ? base + ".simwhitebox"
                                   : base + std::to_string(suffix) + ".simwhitebox";
            path = folder / fileName;
            if (!std::filesystem::exists(path, code)) {
                break;
            }
        }

        const whitebox::WhiteboxIoResult written = whitebox::SaveToFile(BuildShape(shape), path);
        if (!written.ok) {
            if (context.notifications != nullptr) {
                context.notifications->Error(written.error);
            }
            return {};
        }

        // Indeks aset dipindai sekarang, bukan pada denyut berikutnya: GUID-nya
        // lahir di sana, dan prefab yang menunjuk GUID kosong menghasilkan blok
        // tak berbentuk yang tidak bisa dijelaskan dari layar.
        db.ScanNow();
        const std::string relative = std::string(kWhiteboxFolder) + "/" + fileName;
        const assets::AssetRecord* record = db.FindByRelativePath(relative);
        if (record == nullptr) {
            if (context.notifications != nullptr) {
                context.notifications->Error(fileName + " was written but not indexed");
            }
            return {};
        }

        const std::string name = std::filesystem::path(fileName).stem().string();
        // **GUID ikut ditulis.** `RemapPrefabGuids` menggantinya dengan yang baru,
        // tapi ia melewati entity yang tidak punya satu pun — dan yang terjadi
        // bukan galat melainkan aset yang terbuat tanpa entity yang memakainya.
        return std::string(R"({"schemaVersion":3,"entities":[{"guid":")") +
               Uuid::Generate().ToString() + R"(","components":{)" +
               R"("Name":{"name":")" + name + R"("},)" +
               R"("Transform":{"position":[0,0,0],"rotation":[1,0,0,0],"scale":[1,1,1]},)" +
               R"("Whitebox":{"whitebox":")" + record->guid.ToString() +
               R"(","showEdges":true}}}]})";
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
