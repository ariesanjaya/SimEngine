#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/NodeGraph.h"
#include "Sim/Editor/Notifications.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"

#if SIM_WITH_LUA
#include "Sim/Script/Graph.h"
#include "Sim/Script/GraphCache.h"
#include "Sim/Script/GraphCompiler.h"
#include "Sim/Script/NodeCatalog.h"
#include "Sim/Script/ScriptRuntime.h"
#endif

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sim::editor {
namespace {

#if SIM_WITH_LUA

using namespace sim::script;

constexpr ImVec4 kErrorColor(0.94f, 0.45f, 0.42f, 1.0f);
constexpr ImVec4 kOkColor(0.45f, 0.80f, 0.55f, 1.0f);
constexpr ImVec4 kHintColor(0.55f, 0.57f, 0.60f, 1.0f);
constexpr ImVec4 kBreakColor(0.95f, 0.68f, 0.25f, 1.0f);
constexpr ImVec4 kLineHighlight(0.28f, 0.36f, 0.50f, 0.55f);
constexpr ImVec4 kGroupColor(0.45f, 0.55f, 0.70f, 1.0f);

/// Ukuran grup baru, dan batas bawah yang masih bisa dipegang tepinya.
constexpr float kDefaultGroupSize = 260.0f;
constexpr float kMinGroupSize = 40.0f;
/// Jarak antara tepi grup dan node terluar yang dibungkusnya.
constexpr float kGroupPadding = 24.0f;

/// Ruang id terpisah untuk node/link dan pin.
///
/// imgui-node-editor menyimpan ketiganya di satu ruang angka. Menabrakkannya —
/// sebuah pin yang kebetulan bernomor sama dengan sebuah node — menghasilkan
/// kanvas yang berperilaku aneh tanpa satu pun pesan kesalahan, jadi bit teratas
/// dipakai untuk memisahkannya.
constexpr uint64_t kPinFlag = 0x4000'0000'0000'0000ULL;

uint64_t PinId(uint64_t nodeId, std::size_t pinIndex) {
    return kPinFlag | (nodeId << 12) | (pinIndex + 1);
}

uint64_t NodeOfPin(uint64_t pinId) {
    return (pinId & ~kPinFlag) >> 12;
}

std::size_t PinIndexOf(uint64_t pinId) {
    return static_cast<std::size_t>((pinId & 0xFFFULL) - 1);
}

/// Warna pin menurut tipenya. Warna adalah cara tercepat melihat apa yang bisa
/// disambung ke apa, jauh sebelum pengguna mencoba dan ditolak.
ImVec4 ColorOf(PinKind kind) {
    switch (kind) {
        case PinKind::Exec: return ImVec4(0.90f, 0.90f, 0.90f, 1.0f);
        case PinKind::Bool: return ImVec4(0.85f, 0.35f, 0.35f, 1.0f);
        case PinKind::Number: return ImVec4(0.55f, 0.80f, 0.45f, 1.0f);
        case PinKind::String: return ImVec4(0.85f, 0.45f, 0.80f, 1.0f);
        case PinKind::Vec3: return ImVec4(0.95f, 0.80f, 0.30f, 1.0f);
        case PinKind::Quat: return ImVec4(0.45f, 0.70f, 0.95f, 1.0f);
        case PinKind::Entity: return ImVec4(0.35f, 0.85f, 0.80f, 1.0f);
        case PinKind::Any: return ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
    }
    return ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
}

Vec4 ToVec4(const ImVec4& v) {
    return Vec4(v.x, v.y, v.z, v.w);
}

bool Contains(const std::vector<Uuid>& list, const Uuid& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

/// Penyunting graph visual scripting.
///
/// **Yang dilihat pengguna di panel "Compiled Lua" berasal dari CompileGraph
/// yang sama** dengan yang dipakai runtime saat Play. Itu bukan kebetulan yang
/// menyenangkan melainkan syarat: hasil yang berbeda antara editor dan runtime
/// adalah kelas bug yang tidak boleh dibuka, dan satu-satunya cara menutupnya
/// adalah tidak pernah punya dua jalur.
///
/// **Suntingan graph tidak melewati CommandHistory**, sama seperti Script
/// Editor yang menyunting teks. Riwayat undo utama menjanjikan pembatalan
/// perubahan *scene*; menaruh suntingan dokumen di sana akan membuat Ctrl+Z
/// melompat bolak-balik antara dua hal yang tidak berhubungan. Yang tersedia di
/// sini adalah Ctrl+Z milik panel ini sendiri.
class GraphEditorPanel final : public Panel {
public:
    GraphEditorPanel()
        : Panel(panel_id::kGraphEditor, std::string(icons::kNodeGraph) + "  Graph Editor",
                PanelCategory::Authoring) {
        // Alasan yang sama dengan Script Editor: tidak di-dock, jadi jangan
        // terbuka sendiri.
        SetOpen(false);
    }

    void OnDraw(EditorContext& context) override {
        if (context.assets == nullptr || context.scripts == nullptr) {
            ImGui::TextDisabled("No asset database.");
            return;
        }
        canvas_.Initialize();
        // Pustaka graph dipegang cache milik runtime — sumber yang sama dengan
        // yang dipakai compiler saat Play, sehingga pin yang digambar di kanvas
        // tidak pernah berbeda dari pin yang dikompilasi.
        library_ = &context.scripts->Graphs();

        RefreshRuntimeFailure(context);
        DrawToolbar(context);
        ImGui::Separator();

        const float listWidth = ImGui::GetFontSize() * 10.0f;
        if (ImGui::BeginChild("##graphs", ImVec2(listWidth, 0.0f), ImGuiChildFlags_ResizeX)) {
            DrawGraphList(context);
        }
        ImGui::EndChild();

        ImGui::SameLine();
        if (openGuid_.IsValid()) {
            DrawCanvasAndSide(context);
        } else {
            ImGui::TextColored(kHintColor, "Pick a graph on the left, or create one.");
        }
    }

private:
    // --- kanvas & pemisah panel samping ------------------------------------

    /// Kanvas dan panel samping, dipisah pegangan yang bisa digeser.
    ///
    /// Pemisahnya ditulis sendiri, bukan memakai `ImGuiChildFlags_ResizeX`
    /// seperti daftar graph di kiri, karena dua hal yang tidak bisa didapat dari
    /// flag itu — keduanya ketahuan saat mencobanya lebih dulu.
    ///
    /// **Arah pertumbuhannya.** Flag itu hanya bisa memasang pegangan di tepi
    /// KANAN sebuah child, jadi yang tersimpan adalah lebar *kanvas* dan panel
    /// samping mendapat sisanya. Akibatnya melebarkan jendela melebarkan panel
    /// samping, bukan kanvas — terbalik dari yang diinginkan, karena kanvaslah
    /// tempat pekerjaan terjadi.
    ///
    /// **Batasnya.** Lebar kanvas yang tersimpan bersifat mutlak, jadi begitu
    /// jendelanya menyempit di bawah lebar itu, panel samping terhimpit sampai
    /// hilang sama sekali. Dan karena ukuran child yang bisa di-resize ikut
    /// tersimpan di layout.ini, ia tidak kembali meski jendelanya dilebarkan
    /// lagi: tab Details menjadi tak terjangkau sampai berkas layout dihapus.
    void DrawCanvasAndSide(EditorContext& context) {
        const float avail = ImGui::GetContentRegionAvail().x;
        // Selebar jarak antar-item supaya tata letaknya persis seperti sebelum
        // ada pegangan — yang dulu memisahkan keduanya memang celah itu.
        const float handle = ImGui::GetStyle().ItemSpacing.x;

        if (sideWidth_ <= 0.0f) {
            sideWidth_ = avail * 0.36f;
        }
        // Batas atas dihitung lebih dulu: pada jendela yang sangat sempit, batas
        // bawah harus mengalah, bukan menyilang batas atas.
        // Batas bawah panel samping dipilih supaya kedua label tab masih muat:
        // lebih sempit dari itu, tab bar memunculkan panah geser dan panelnya
        // terlihat rusak alih-alih sekadar sempit.
        const float maxSide = std::max(avail - ImGui::GetFontSize() * 12.0f - handle, 0.0f);
        sideWidth_ = std::clamp(sideWidth_, std::min(ImGui::GetFontSize() * 13.0f, maxSide),
                                maxSide);

        if (ImGui::BeginChild("##canvas", ImVec2(std::max(avail - sideWidth_ - handle, 1.0f),
                                                 0.0f))) {
            DrawCanvas(context);
        }
        ImGui::EndChild();

        ImGui::SameLine(0.0f, 0.0f);
        DrawSideSplitter(handle);
        ImGui::SameLine(0.0f, 0.0f);

        if (ImGui::BeginChild("##side", ImVec2(0.0f, 0.0f))) {
            DrawSidePanel(context);
        }
        ImGui::EndChild();
    }

    /// Pegangan pemisah: satu tombol tak terlihat setinggi panel.
    void DrawSideSplitter(float width) {
        // -FLT_MIN, bukan 0: nol berarti "ukuran bawaan" bagi InvisibleButton,
        // sedangkan negatif berarti "sisa ruang".
        ImGui::InvisibleButton("##sidesplit", ImVec2(width, -FLT_MIN));
        const bool active = ImGui::IsItemActive();
        const bool touched = active || ImGui::IsItemHovered();
        if (touched) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (active) {
            // Ke kiri berarti panel samping melebar; kanvas mendapat sisanya.
            sideWidth_ -= ImGui::GetIO().MouseDelta.x;
        }
        // Digambar hanya saat disentuh. Garis permanen di tengah panel menambah
        // satu batas visual yang tidak membawa informasi apa pun — celahnya
        // sendiri sudah cukup memisahkan kanvas dari panel.
        if (touched) {
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                ImGui::GetColorU32(active ? ImGuiCol_SeparatorActive
                                          : ImGuiCol_SeparatorHovered));
        }
    }

    // --- toolbar & daftar --------------------------------------------------

    void DrawToolbar(EditorContext& context) {
        ImGui::BeginDisabled(!dirty_ || !openGuid_.IsValid());
        const std::string saveLabel = std::string(icons::kSave) + "  Save";
        if (ImGui::Button(saveLabel.c_str())) {
            Save(context);
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(!openGuid_.IsValid());
        if (ImGui::Button("Fit")) {
            canvas_.FitToContent();
        }
        ImGui::EndDisabled();

        // Di toolbar, bukan di menu klik-kanan: klik kanan di latar kanvas
        // menghapus seleksi lebih dulu, jadi "Group selection" di sana tidak
        // akan pernah punya apa pun untuk dibungkus.
        ImGui::SameLine();
        const std::vector<uint64_t> selected =
            openGuid_.IsValid() ? canvas_.SelectedNodes() : std::vector<uint64_t>{};
        ImGui::BeginDisabled(selected.empty());
        if (ImGui::Button("Group selection")) {
            GroupSelection(selected);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(selected.empty()
                                  ? "Ctrl+G — pilih beberapa node dulu; grup dibuat "
                                    "mengelilinginya."
                                  : "Ctrl+G");
        }

        if (!openGuid_.IsValid()) {
            return;
        }
        ImGui::SameLine();
        ImGui::TextColored(kHintColor, "%s%s", openName_.c_str(), dirty_ ? " *" : "");

        ImGui::SameLine();
        if (compiled_.ok) {
            ImGui::TextColored(kOkColor, "%s  compiles", icons::kLogInfo);
        } else {
            ImGui::TextColored(kErrorColor, "%s  %d error(s)", icons::kLogError,
                               static_cast<int>(compiled_.errors.size()));
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                ImGui::BeginTooltip();
                for (const CompileError& error : compiled_.errors) {
                    ImGui::TextUnformatted(error.message.c_str());
                }
                ImGui::EndTooltip();
            }
        }

        // Ctrl+S hanya berlaku selagi panel ini yang difokuskan, supaya tidak
        // bertabrakan dengan Save Level. Ctrl+G mengikuti aturan yang sama —
        // ia tidak terdaftar di ActionRegistry karena hanya berarti di dalam
        // panel ini, dan pintasan global yang tidak melakukan apa-apa di mana
        // pun kecuali satu panel justru lebih membingungkan daripada berguna.
        if (!IsFocused()) {
            return;
        }
        if (dirty_ && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
            Save(context);
        }
        if (!selected.empty() && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_G)) {
            GroupSelection(selected);
        }
    }

    void DrawGraphList(EditorContext& context) {
        if (ImGui::Button("New Graph", ImVec2(-1.0f, 0.0f))) {
            CreateGraph(context);
        }
        ImGui::Separator();
        for (const assets::AssetRecord& record : context.assets->All()) {
            if (record.type != assets::AssetType::Graph) {
                continue;
            }
            const bool selected = record.guid == openGuid_;
            if (ImGui::Selectable(record.name.c_str(), selected)) {
                Open(context, record);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                ImGui::SetTooltip("%s", record.relativePath.c_str());
            }
        }
    }

    // --- kanvas ------------------------------------------------------------

    void DrawCanvas(EditorContext& context) {
        canvas_.Begin("graph", Vec2(0.0f, 0.0f));

        for (const GraphNode& node : graph_.nodes) {
            DrawNode(node);
        }
        for (const GraphLink& link : graph_.links) {
            DrawLink(link);
        }

        HandleCreate();
        HandleDelete();
        HandleRerouteInsert();
        HandleNodeFocus();
        HandleContextMenus(context);

        canvas_.End();

        if (pendingFocus_ != 0) {
            canvas_.CenterOnNode(pendingFocus_);
            pendingFocus_ = 0;
        }
        if (pendingFit_) {
            // Ditunda satu frame: ukuran node baru diketahui pustaka setelah
            // digambar sekali, dan memanggilnya lebih awal memusatkan kanvas ke
            // kotak yang masih kosong.
            canvas_.FitToContent();
            pendingFit_ = false;
        }
    }

    void DrawNode(const GraphNode& node) {
        const NodeType* type = NodeCatalog::Get().Find(node.type);
        const uint64_t id = IdOf(node.guid);

        // Posisi berpindah dua arah: dari berkas ke kanvas saat graph dibuka,
        // dan dari kanvas ke berkas begitu pengguna menyeret node. Yang
        // menentukan arahnya adalah `placed_` — tanpa itu, menyeret node akan
        // langsung ditimpa balik oleh posisi yang tersimpan di berkas.
        if (!placed_.contains(node.guid)) {
            canvas_.SetNodePosition(id, node.position);
            placed_.insert(node.guid);
        } else if (GraphNode* mutableNode = FindNode(node.guid)) {
            // Ambang setengah piksel, bukan perbandingan persis: pustaka
            // membulatkan posisi node ke bilangan bulat, dan perbandingan persis
            // akan menandai graph kotor pada frame pertama setelah dibuka —
            // tombol Save menyala tanpa pengguna menyentuh apa pun.
            const Vec2 current = canvas_.GetNodePosition(id);
            if (std::abs(current.x - mutableNode->position.x) > 0.5f ||
                std::abs(current.y - mutableNode->position.y) > 0.5f) {
                mutableNode->position = current;
                // dirty_ langsung, bukan lewat Touch(): posisi node tidak
                // mengubah satu baris pun Lua yang dihasilkan, dan mengompilasi
                // ulang di setiap frame seretan adalah pekerjaan yang hasilnya
                // dijamin sama.
                dirty_ = true;
            }
        }

        if (node.type == "group") {
            DrawGroupNode(node, id);
            return;
        }
        if (node.type == "reroute") {
            DrawRerouteNode(node, id);
            return;
        }

        canvas_.BeginNode(id);
        if (node.type == "comment") {
            ImGui::TextColored(kHintColor, "%s", node.Setting("text", "Comment").c_str());
            canvas_.EndNode();
            return;
        }

        ImGui::TextUnformatted(type != nullptr ? type->label.c_str() : node.type.c_str());
        ImGui::Dummy(ImVec2(0.0f, 2.0f));

        const std::vector<GraphPin> pins = PinsOf(graph_, node, library_);
        std::vector<std::size_t> inputs;
        std::vector<std::size_t> outputs;
        for (std::size_t i = 0; i < pins.size(); ++i) {
            (pins[i].direction == PinDirection::Input ? inputs : outputs).push_back(i);
        }

        // Dua kolom: masukan di kiri, keluaran di kanan — bentuk yang dikenali
        // siapa pun yang pernah memakai graph editor lain.
        //
        // Kolomnya WAJIB menyesuaikan isi, bukan meregang. Sebuah node
        // menentukan ukurannya sendiri dari isinya, sementara kolom yang
        // meregang mengambil selebar ruang yang tersedia — dan di dalam node,
        // "ruang yang tersedia" adalah selebar kanvas. Keduanya saling
        // memberi makan: setiap node melebar sampai tepi kanvas, dan
        // menyeretnya hanya melebarkannya lagi alih-alih memindahkannya.
        //
        // NoHostExtendX yang menutupnya: tanpa itu lebar luar tabel tetap
        // mengambil ruang yang tersedia walau kolomnya sudah menyesuaikan isi,
        // dan node tetap terukur selebar kanvas.
        constexpr ImGuiTableFlags kPinTableFlags =
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX;
        if (ImGui::BeginTable("pins", 2, kPinTableFlags)) {
            const std::size_t rows = std::max(inputs.size(), outputs.size());
            for (std::size_t row = 0; row < rows; ++row) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (row < inputs.size()) {
                    DrawPin(node, pins[inputs[row]], PinId(id, inputs[row]));
                }
                ImGui::TableSetColumnIndex(1);
                if (row < outputs.size()) {
                    DrawPin(node, pins[outputs[row]], PinId(id, outputs[row]));
                }
            }
            ImGui::EndTable();
        }
        canvas_.EndNode();

        if (Contains(errorNodes_, node.guid) || Contains(runtimeNodes_, node.guid)) {
            canvas_.DrawNodeBackground(id, ToVec4(kErrorColor), 2.5f);
        } else if (Contains(breakpoints_, node.guid)) {
            canvas_.DrawNodeBackground(id, ToVec4(kBreakColor), 2.5f);
        }
    }



    /// Titik belok: sekecil mungkin, karena ia bagian dari kabel dan bukan
    /// sebuah langkah. Node berukuran penuh dengan judul justru memutus alur
    /// yang sedang dirapikan.
    void DrawRerouteNode(const GraphNode& node, uint64_t id) {
        const std::vector<GraphPin> pins = PinsOf(graph_, node, library_);
        const ImVec4 color = pins.empty() ? kHintColor : ColorOf(pins.front().kind);

        canvas_.BeginNode(id);
        canvas_.BeginInputPin(PinId(id, 0));
        ImGui::TextColored(color, "%s", pins.empty() || pins.front().kind != PinKind::Exec
                                            ? "\u25cf"
                                            : "\u25b6");
        canvas_.EndPin();
        ImGui::SameLine();
        canvas_.BeginOutputPin(PinId(id, 1));
        ImGui::TextColored(color, " ");
        canvas_.EndPin();
        canvas_.EndNode();
    }

    /// Node grup: kotak yang membawa serta node di dalamnya ketika digeser.
    ///
    /// Ukurannya milik graph, bukan milik pustaka, jadi ia disetel dari model
    /// pada frame pertama dan dibaca balik sesudahnya — persis seperti posisi.
    /// Tanpa itu, mengubah ukuran grup tidak akan pernah sampai ke berkas.
    void DrawGroupNode(const GraphNode& node, uint64_t id) {
        Vec2 size = node.size;
        if (size.x < kMinGroupSize || size.y < kMinGroupSize) {
            size = Vec2(kDefaultGroupSize, kDefaultGroupSize * 0.66f);
        }
        const bool justPlaced = !sized_.contains(node.guid);
        if (justPlaced) {
            canvas_.SetGroupSize(id, size);
            sized_.insert(node.guid);
        }

        canvas_.BeginGroupNode(id, ToVec4(kGroupColor));
        DrawGroupTitle(node);
        canvas_.GroupArea(size);
        canvas_.EndGroupNode();

        const Vec2 outer = canvas_.GetNodeSize(id);
        if (outer.x <= 0.0f) {
            return;
        }
        // Yang disetel adalah luas KOTAKNYA, yang dibaca balik adalah ukuran
        // NODE — judul dan bingkainya ikut terhitung. Menyimpan yang kedua ke
        // tempat yang pertama membuat grup tumbuh sedikit setiap kali graph
        // dibuka lalu disimpan; terukur +16 x +33 piksel per putaran sebelum
        // ini ada.
        //
        // Selisihnya tidak dipatok angka melainkan diukur sekali dari grup itu
        // sendiri: ia bergantung pada tinggi font dan gaya kanvas, dan angka
        // yang ditulis tangan akan salah begitu salah satunya berubah.
        if (justPlaced) {
            groupInset_[node.guid] = outer - size;
            return;
        }
        const auto inset = groupInset_.find(node.guid);
        if (inset == groupInset_.end()) {
            return;
        }
        const Vec2 area = outer - inset->second;
        if (GraphNode* mutableNode = FindNode(node.guid)) {
            if (std::abs(area.x - mutableNode->size.x) > 0.5f ||
                std::abs(area.y - mutableNode->size.y) > 0.5f) {
                mutableNode->size = area;
                dirty_ = true;
            }
        }
    }

    /// Judul grup, yang bisa disunting di tempat.
    ///
    /// Klik ganda pada judulnya, bukan lewat panel terpisah: nama grup adalah
    /// hal yang dilihat di kanvas, dan mengubahnya di tempat yang sama dengan
    /// tempat ia terbaca membuat tidak ada yang perlu dicari. Field di tab
    /// Details tetap ada untuk yang lebih suka papan ketik.
    void DrawGroupTitle(const GraphNode& node) {
        if (renamingNode_ != node.guid) {
            ImGui::TextUnformatted(node.Setting("text", "Group").c_str());
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                renamingNode_ = node.guid;
                renameBuffer_ = node.Setting("text", "Group");
                focusRename_ = true;
            }
            return;
        }

        if (focusRename_) {
            ImGui::SetKeyboardFocusHere();
            focusRename_ = false;
        }
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        const bool committed = ImGui::InputText("##rename", &renameBuffer_,
                                                ImGuiInputTextFlags_EnterReturnsTrue);
        // Selesai juga ketika fokus berpindah, bukan hanya saat Enter: pengguna
        // yang mengetik nama lalu mengklik kanvas jelas bermaksud menyimpannya,
        // dan membuangnya diam-diam adalah kehilangan yang tidak dia minta.
        const bool finished = committed || ImGui::IsItemDeactivated();
        if (!finished) {
            return;
        }
        if (GraphNode* mutableNode = FindNode(node.guid)) {
            const std::string trimmed = renameBuffer_.empty() ? "Group" : renameBuffer_;
            if (mutableNode->Setting("text", "Group") != trimmed) {
                mutableNode->settings["text"] = trimmed;
                Touch();
            }
        }
        renamingNode_ = Uuid{};
    }

    void DrawPin(const GraphNode& node, const GraphPin& pin, uint64_t pinId) {
        const std::string label = pin.label.empty() ? pin.name : pin.label;
        const bool input = pin.direction == PinDirection::Input;
        if (input) {
            canvas_.BeginInputPin(pinId);
        } else {
            canvas_.BeginOutputPin(pinId);
        }
        ImGui::TextColored(ColorOf(pin.kind), "%s", pin.kind == PinKind::Exec ? "▶" : "●");
        ImGui::SameLine();
        ImGui::TextUnformatted(label.c_str());
        canvas_.EndPin();

        // Pin data masukan yang tidak tersambung menampilkan nilainya di sini,
        // supaya angka sederhana tidak menuntut sebuah node literal tersendiri.
        if (input && pin.kind != PinKind::Exec &&
            graph_.LinkInto(node.guid, pin.name) == nullptr) {
            ImGui::SameLine();
            ImGui::PushID(static_cast<int>(pinId & 0xFFFFFFFFULL));
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
            std::string value = node.pinValues.count(pin.name) != 0
                                    ? node.pinValues.at(pin.name)
                                    : std::string{};
            if (ImGui::InputText("##value", &value)) {
                GraphNode* mutableNode = FindNode(node.guid);
                if (mutableNode != nullptr) {
                    if (value.empty()) {
                        mutableNode->pinValues.erase(pin.name);
                    } else {
                        mutableNode->pinValues[pin.name] = value;
                    }
                    Touch();
                }
            }
            ImGui::PopID();
        }
    }

    void DrawLink(const GraphLink& link) {
        const GraphNode* from = graph_.FindNode(link.fromNode);
        const GraphNode* to = graph_.FindNode(link.toNode);
        if (from == nullptr || to == nullptr) {
            return;
        }
        const std::vector<GraphPin> fromPins = PinsOf(graph_, *from, library_);
        const std::vector<GraphPin> toPins = PinsOf(graph_, *to, library_);
        const std::size_t fromIndex = IndexOf(fromPins, link.fromPin);
        const std::size_t toIndex = IndexOf(toPins, link.toPin);
        if (fromIndex == fromPins.size() || toIndex == toPins.size()) {
            return;
        }
        canvas_.Link(IdOf(link.guid), PinId(IdOf(from->guid), fromIndex),
                     PinId(IdOf(to->guid), toIndex),
                     ToVec4(ColorOf(fromPins[fromIndex].kind)),
                     fromPins[fromIndex].kind == PinKind::Exec ? 2.5f : 1.8f);
    }

    static std::size_t IndexOf(const std::vector<GraphPin>& pins, const std::string& name) {
        for (std::size_t i = 0; i < pins.size(); ++i) {
            if (pins[i].name == name) {
                return i;
            }
        }
        return pins.size();
    }

    void HandleCreate() {
        if (!canvas_.BeginCreate()) {
            canvas_.EndCreate();
            return;
        }
        uint64_t fromPinId = 0;
        uint64_t toPinId = 0;
        // SEKALI per frame, bukan diulang sampai habis. Percobaan koneksi baru
        // bukan antrean: pustaka melaporkan calon yang SAMA setiap kali
        // ditanya, jadi menanyakannya di dalam `while` berputar selamanya
        // begitu kedua ujungnya sah — dan yang terlihat pengguna adalah editor
        // yang membeku persis saat kabel dijatuhkan ke sebuah pin.
        //
        // Berbeda dengan penghapusan di bawah, yang memang antrean dan memang
        // harus dikuras.
        if (canvas_.QueryNewLink(fromPinId, toPinId)) {
            TryConnect(fromPinId, toPinId);
        }
        canvas_.EndCreate();
    }

    /// Menilai satu percobaan koneksi, lalu menerima atau menolaknya.
    void TryConnect(uint64_t fromPinId, uint64_t toPinId) {
        GraphPin fromPin;
        GraphPin toPin;
        // Pin dikembalikan BY VALUE. Versi sebelumnya meminjamkan pointer ke
        // satu penyangga bersama, sehingga panggilan kedua menimpa isi yang
        // ditunjuk hasil panggilan pertama — pin asal dinilai memakai data pin
        // tujuan, dan koneksi yang sah bisa tertolak tanpa sebab yang terlihat.
        const GraphNode* fromNode = ResolvePin(fromPinId, fromPin);
        const GraphNode* toNode = ResolvePin(toPinId, toPin);
        if (fromNode == nullptr || toNode == nullptr) {
            canvas_.RejectLink();
            return;
        }
        // Pengguna boleh menyeret dari arah mana saja; yang disimpan selalu
        // keluaran → masukan.
        if (fromPin.direction == PinDirection::Input) {
            std::swap(fromNode, toNode);
            std::swap(fromPin, toPin);
        }
        if (fromPin.direction != PinDirection::Output ||
            toPin.direction != PinDirection::Input || fromNode == toNode ||
            !PinAccepts(toPin.kind, fromPin.kind)) {
            // Ditolak sebelum terbentuk, bukan dilaporkan setelahnya: koneksi
            // yang tidak masuk akal tidak pernah sempat ada di berkas, jadi
            // tidak ada keadaan rusak yang harus dibereskan.
            canvas_.RejectLink();
            return;
        }
        if (!canvas_.AcceptLink()) {
            return;
        }

        // Pin masukan hanya boleh punya satu sumber: yang lama diputus.
        RemoveLinkInto(toNode->guid, toPin.name);

        GraphLink link;
        link.guid = Uuid::Generate();
        link.fromNode = fromNode->guid;
        link.fromPin = fromPin.name;
        link.toNode = toNode->guid;
        link.toPin = toPin.name;
        graph_.links.push_back(std::move(link));
        Touch();
    }

    /// Klik ganda pada sebuah node memusatkan pandangan ke node itu.
    ///
    /// Berguna justru ketika graph sudah besar: mencari satu node di antara
    /// puluhan lebih mudah lewat daftar atau pesan kesalahan yang bisa diklik
    /// daripada dengan menggeser kanvas mencari-cari.
    void HandleNodeFocus() {
        const uint64_t nodeId = canvas_.DoubleClickedNode();
        if (nodeId == 0) {
            return;
        }
        const Uuid* guid = GuidOf(nodeId);
        const GraphNode* node = guid != nullptr ? graph_.FindNode(*guid) : nullptr;
        // Pada grup, klik ganda sudah berarti "ganti nama". Satu gerakan yang
        // berarti dua hal sekaligus membuat keduanya terasa tidak bisa dipercaya.
        if (node == nullptr || node->type == "group") {
            return;
        }
        // Ditunda sampai kanvas ditutup — lihat catatan di NodeCanvas::CenterOnNode.
        pendingFocus_ = nodeId;
    }

    /// Klik ganda pada sebuah kabel menyisipkan titik belok tepat di situ.
    ///
    /// Kabelnya dipecah, bukan ditambahi: A→B menjadi A→R dan R→B. Yang
    /// dikompilasi tetap sama persis, karena titik belok hanya meneruskan
    /// nilainya — merapikan tampilan tidak boleh mengubah perilaku.
    void HandleRerouteInsert() {
        const uint64_t linkId = canvas_.DoubleClickedLink();
        if (linkId == 0) {
            return;
        }
        const Uuid* guid = GuidOf(linkId);
        if (guid == nullptr) {
            return;
        }
        const auto it = std::find_if(graph_.links.begin(), graph_.links.end(),
                                     [guid](const GraphLink& link) { return link.guid == *guid; });
        if (it == graph_.links.end()) {
            return;
        }
        const GraphLink original = *it;

        GraphNode reroute;
        reroute.guid = Uuid::Generate();
        reroute.type = "reroute";
        // Posisi kursor dipakai APA ADANYA. Di dalam kanvas, pustaka sudah
        // menulis ulang posisi mouse ImGui ke ruang kanvas — itu yang membuat
        // widget di dalam node bisa diklik dengan benar saat di-zoom.
        // Menerjemahkannya sekali lagi menerapkan transformasi dua kali, dan
        // titik beloknya mendarat ribuan piksel dari kursor.
        const ImVec2 mouse = ImGui::GetMousePos();
        reroute.position = Vec2(mouse.x, mouse.y);
        const Uuid rerouteGuid = reroute.guid;
        graph_.nodes.push_back(std::move(reroute));

        graph_.links.erase(it);

        GraphLink incoming;
        incoming.guid = Uuid::Generate();
        incoming.fromNode = original.fromNode;
        incoming.fromPin = original.fromPin;
        incoming.toNode = rerouteGuid;
        incoming.toPin = "in";
        graph_.links.push_back(std::move(incoming));

        GraphLink outgoing;
        outgoing.guid = Uuid::Generate();
        outgoing.fromNode = rerouteGuid;
        outgoing.fromPin = "out";
        outgoing.toNode = original.toNode;
        outgoing.toPin = original.toPin;
        graph_.links.push_back(std::move(outgoing));

        // Node baru ditempatkan dari model pada frame berikutnya.
        placed_.erase(rerouteGuid);
        Touch();
    }

    void HandleDelete() {
        if (!canvas_.BeginDelete()) {
            canvas_.EndDelete();
            return;
        }
        uint64_t id = 0;
        while (canvas_.QueryDeletedLink(id)) {
            if (canvas_.AcceptDeletion()) {
                const Uuid* guid = GuidOf(id);
                if (guid != nullptr) {
                    RemoveLink(*guid);
                    Touch();
                }
            }
        }
        while (canvas_.QueryDeletedNode(id)) {
            if (canvas_.AcceptDeletion()) {
                const Uuid* guid = GuidOf(id);
                if (guid != nullptr) {
                    RemoveNode(*guid);
                    Touch();
                }
            }
        }
        canvas_.EndDelete();
    }

    void HandleContextMenus(EditorContext& context) {
        // Ditangguhkan supaya popup digambar di koordinat layar, bukan ikut
        // ter-zoom bersama kanvas.
        canvas_.Suspend();
        if (canvas_.RequestedBackgroundMenu()) {
            paletteFilter_.clear();
            ImGui::OpenPopup("##palette");
        }
        DrawPalette(context);
        canvas_.Resume();
    }

    void DrawPalette(EditorContext& context) {
        if (!ImGui::BeginPopup("##palette")) {
            return;
        }
        ImGui::TextColored(kHintColor, "Add node");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::InputTextWithHint("##filter", "Search...", &paletteFilter_);
        ImGui::Separator();

        if (ImGui::BeginChild("##list", ImVec2(ImGui::GetFontSize() * 16.0f,
                                               ImGui::GetFontSize() * 18.0f))) {
            // Graph lain di proyek muncul sebagai node siap pakai, bukan lewat
            // dialog terpisah: memakai ulang sebuah graph harus semudah
            // memasang node biasa, atau ia tidak akan dipakai.
            DrawSubgraphPalette(context);

            std::string category;
            for (const NodeType& type : NodeCatalog::Get().All()) {
                if (!Matches(type)) {
                    continue;
                }
                if (type.category != category) {
                    category = type.category;
                    ImGui::TextColored(kHintColor, "%s", category.c_str());
                }
                ImGui::Indent();
                if (ImGui::Selectable(type.label.c_str())) {
                    AddNode(type);
                    ImGui::CloseCurrentPopup();
                }
                if (!type.tooltip.empty() &&
                    ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                    ImGui::SetTooltip("%s", type.tooltip.c_str());
                }
                ImGui::Unindent();
            }
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }

    /// Graph lain yang bisa dipanggil dari graph ini.
    void DrawSubgraphPalette(EditorContext& context) {
        if (context.assets == nullptr) {
            return;
        }
        bool heading = false;
        for (const assets::AssetRecord& record : context.assets->All()) {
            if (record.type != assets::AssetType::Graph || record.guid == openGuid_) {
                continue;
            }
            // Hanya graph yang punya antarmuka. Yang tanpa Input tidak bisa
            // dipanggil, dan menawarkannya berarti menjanjikan sesuatu yang
            // pasti gagal saat dikompilasi.
            const Graph* target = library_ != nullptr ? library_->Find(record.guid) : nullptr;
            if (target == nullptr || !target->IsSubgraph()) {
                continue;
            }
            if (!MatchesText(record.name)) {
                continue;
            }
            if (!heading) {
                ImGui::TextColored(kHintColor, "Graphs");
                heading = true;
            }
            ImGui::Indent();
            if (ImGui::Selectable((std::string(icons::kNodeGraph) + "  " + record.name).c_str())) {
                AddSubgraphCall(record);
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                ImGui::SetTooltip("%s", record.relativePath.c_str());
            }
            ImGui::Unindent();
        }
    }

    void AddSubgraphCall(const assets::AssetRecord& record) {
        GraphNode node;
        node.guid = Uuid::Generate();
        node.type = "graph.call";
        node.settings["graph"] = record.guid.ToString();
        node.position = Vec2(nextSpawn_, nextSpawn_);
        nextSpawn_ += 24.0f;
        if (nextSpawn_ > 260.0f) {
            nextSpawn_ = 40.0f;
        }
        graph_.nodes.push_back(std::move(node));
        Touch();
    }

    bool MatchesText(std::string_view label) const {
        if (paletteFilter_.empty()) {
            return true;
        }
        const auto lower = [](std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return text;
        };
        return lower(std::string(label)).find(lower(paletteFilter_)) != std::string::npos;
    }

    bool Matches(const NodeType& type) const {
        if (paletteFilter_.empty()) {
            return true;
        }
        const auto lower = [](std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return text;
        };
        const std::string needle = lower(paletteFilter_);
        return lower(type.label).find(needle) != std::string::npos ||
               lower(type.key).find(needle) != std::string::npos;
    }

    /// Membungkus node terpilih dengan sebuah grup.
    ///
    /// Kotaknya dihitung dari letak dan ukuran node di kanvas, bukan dari
    /// posisi yang tersimpan di model: yang harus terbungkus adalah apa yang
    /// dilihat pengguna sekarang, termasuk node yang baru saja digeser dan
    /// belum disimpan.
    void GroupSelection(const std::vector<uint64_t>& selected) {
        bool any = false;
        Vec2 min(0.0f, 0.0f);
        Vec2 max(0.0f, 0.0f);
        for (const uint64_t id : selected) {
            const Uuid* guid = GuidOf(id);
            const GraphNode* node = guid != nullptr ? graph_.FindNode(*guid) : nullptr;
            // Grup tidak membungkus grup: menyeret yang luar akan memindahkan
            // yang dalam beserta isinya, dan menjelaskan mana yang membawa apa
            // menjadi lebih rumit daripada gunanya.
            if (node == nullptr || node->type == "group") {
                continue;
            }
            const Vec2 position = canvas_.GetNodePosition(id);
            const Vec2 size = canvas_.GetNodeSize(id);
            if (!any) {
                min = position;
                max = position + size;
                any = true;
                continue;
            }
            min = Vec2(std::min(min.x, position.x), std::min(min.y, position.y));
            max = Vec2(std::max(max.x, position.x + size.x),
                       std::max(max.y, position.y + size.y));
        }
        if (!any) {
            return;
        }

        GraphNode group;
        group.guid = Uuid::Generate();
        group.type = "group";
        group.settings["text"] = "Group";
        // Ruang di atas disisakan lebih banyak: di situlah judul grup digambar,
        // dan tanpa itu judulnya menindih node teratas.
        group.position = Vec2(min.x - kGroupPadding, min.y - kGroupPadding * 2.0f);
        group.size = Vec2(max.x - min.x + kGroupPadding * 2.0f,
                          max.y - min.y + kGroupPadding * 3.0f);
        graph_.nodes.push_back(std::move(group));
        Touch();
    }

    void AddNode(const NodeType& type) {
        GraphNode node;
        node.guid = Uuid::Generate();
        node.type = type.key;
        // Ditempatkan di tengah kanvas. Menghitung posisi kursor di ruang kanvas
        // butuh transformasi yang pustaka tidak ekspos lewat pembungkus ini, dan
        // node yang muncul di tengah lalu bisa diseret jauh lebih baik daripada
        // node yang muncul di tempat yang salah.
        node.position = Vec2(nextSpawn_, nextSpawn_);
        nextSpawn_ += 24.0f;
        if (nextSpawn_ > 260.0f) {
            nextSpawn_ = 40.0f;
        }
        if (type.key == "variable.get" || type.key == "variable.set") {
            if (!graph_.variables.empty()) {
                node.settings["variable"] = graph_.variables.front().name;
            }
        }
        graph_.nodes.push_back(std::move(node));
        Touch();
    }

    // --- panel samping -----------------------------------------------------

    /// Details dan Compiled Lua sebagai tab, bukan berdampingan.
    ///
    /// Berdampingan memang membuat keduanya terbaca sekaligus, tapi ongkosnya
    /// diambil dari kanvas — dan kanvaslah tempat pekerjaan sebenarnya terjadi.
    /// Panel Lua paling sering dibuka sebentar untuk memastikan sesuatu, lalu
    /// ditinggalkan; menukar lebar kanvas secara permanen demi itu adalah
    /// pertukaran yang salah arah.
    void DrawSidePanel(EditorContext& context) {
        if (!ImGui::BeginTabBar("##side")) {
            return;
        }
        // Details lebih dulu: ia yang disunting, sementara Lua hanya dibaca.
        if (ImGui::BeginTabItem("Details")) {
            DrawDetails(context);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Compiled Lua")) {
            DrawCompiledLua();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    void DrawCompiledLua() {
        if (!runtimeMessage_.empty()) {
            ImGui::TextColored(kErrorColor, "%s  %s", icons::kLogError, runtimeMessage_.c_str());
            ImGui::Separator();
        }
        if (!compiled_.errors.empty()) {
            for (const CompileError& error : compiled_.errors) {
                ImGui::TextColored(kErrorColor, "%s  %s", icons::kLogError,
                                   error.message.c_str());
                if (error.node.IsValid() && ImGui::IsItemClicked()) {
                    canvas_.CenterOnNode(IdOf(error.node));
                }
            }
            ImGui::Separator();
        }
        if (!ImGui::BeginChild("##lua", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders,
                               ImGuiWindowFlags_HorizontalScrollbar)) {
            ImGui::EndChild();
            return;
        }
        // Baris milik node yang sedang dipilih disorot. Inilah yang membuat peta
        // sumber terasa: pengguna melihat langsung potongan Lua mana yang
        // dihasilkan node mana, dan bisa lulus dari graph ke Lua tanpa jurang.
        const std::vector<uint64_t> selected = canvas_.SelectedNodes();
        int line = 0;
        std::size_t begin = 0;
        while (begin <= compiled_.lua.size()) {
            const std::size_t end = compiled_.lua.find('\n', begin);
            const std::string text = compiled_.lua.substr(
                begin, end == std::string::npos ? std::string::npos : end - begin);
            ++line;
            const bool highlight =
                !selected.empty() &&
                std::any_of(selected.begin(), selected.end(), [this, line](uint64_t id) {
                    const Uuid* guid = GuidOf(id);
                    if (guid == nullptr) {
                        return false;
                    }
                    const std::vector<Uuid> nodes = compiled_.NodesAtLine(line);
                    return Contains(nodes, *guid);
                });
            if (highlight) {
                const ImVec2 min = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    min, ImVec2(min.x + ImGui::GetContentRegionAvail().x,
                                min.y + ImGui::GetTextLineHeight()),
                    ImGui::GetColorU32(kLineHighlight));
            }
            ImGui::TextColored(kHintColor, "%4d", line);
            ImGui::SameLine();
            ImGui::TextUnformatted(text.c_str());
            if (end == std::string::npos) {
                break;
            }
            begin = end + 1;
        }
        ImGui::EndChild();
    }

    void DrawDetails(EditorContext& context) {
        DrawInterface();
        ImGui::Separator();
        DrawVariables();
        ImGui::Separator();
        DrawSelectedNode(context);
    }

    /// Antarmuka graph: parameter masuk dan hasil keluar.
    ///
    /// Menambah satu di sini adalah satu-satunya yang dibutuhkan untuk membuat
    /// sebuah graph bisa dipakai ulang — begitu ia punya antarmuka, ia muncul di
    /// palet graph lain sebagai node siap pasang.
    void DrawInterface() {
        ImGui::TextColored(kHintColor, "Interface (makes this graph reusable)");
        DrawPortList("Input", graph_.inputs, true);
        DrawPortList("Output", graph_.outputs, false);
        if (graph_.IsSubgraph() && !HasNode("graph.input")) {
            ImGui::TextColored(kErrorColor, "%s  Add an Input node to start from",
                               icons::kLogError);
        }
    }

    bool HasNode(std::string_view type) const {
        return std::any_of(graph_.nodes.begin(), graph_.nodes.end(),
                           [type](const GraphNode& node) { return node.type == type; });
    }

    void DrawPortList(const char* label, std::vector<GraphPort>& ports, bool withDefault) {
        ImGui::PushID(label);
        if (ImGui::Button((std::string("Add ") + label).c_str())) {
            GraphPort port;
            port.name = std::string(label) + std::to_string(ports.size() + 1);
            port.kind = PinKind::Number;
            port.defaultValue = withDefault ? "0" : "";
            ports.push_back(std::move(port));
            Touch();
        }
        for (std::size_t i = 0; i < ports.size(); ++i) {
            GraphPort& port = ports[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
            if (ImGui::InputText("##name", &port.name)) {
                Touch();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
            if (ImGui::BeginCombo("##kind", ToString(port.kind))) {
                for (const PinKind kind : {PinKind::Number, PinKind::Bool, PinKind::String,
                                           PinKind::Vec3, PinKind::Quat, PinKind::Entity}) {
                    if (ImGui::Selectable(ToString(kind), port.kind == kind)) {
                        port.kind = kind;
                        Touch();
                    }
                }
                ImGui::EndCombo();
            }
            if (withDefault) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
                if (ImGui::InputText("##default", &port.defaultValue)) {
                    Touch();
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                ports.erase(ports.begin() + static_cast<std::ptrdiff_t>(i));
                Touch();
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    }

    void DrawVariables() {
        ImGui::TextColored(kHintColor, "Variables");
        if (ImGui::Button("Add Variable")) {
            GraphVariable variable;
            variable.name = "var" + std::to_string(graph_.variables.size() + 1);
            variable.kind = PinKind::Number;
            variable.defaultValue = "0";
            graph_.variables.push_back(std::move(variable));
            Touch();
        }
        for (std::size_t i = 0; i < graph_.variables.size(); ++i) {
            GraphVariable& variable = graph_.variables[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
            if (ImGui::InputText("##name", &variable.name)) {
                Touch();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
            if (ImGui::BeginCombo("##kind", ToString(variable.kind))) {
                for (const PinKind kind : {PinKind::Number, PinKind::Bool, PinKind::String,
                                           PinKind::Vec3, PinKind::Quat}) {
                    if (ImGui::Selectable(ToString(kind), variable.kind == kind)) {
                        variable.kind = kind;
                        Touch();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
            if (ImGui::InputText("##default", &variable.defaultValue)) {
                Touch();
            }
            ImGui::SameLine();
            // Yang diekspos muncul di Inspector, lewat deklarasi `properties`
            // yang dibangkitkan kompiler — jalur yang sama dengan skrip biasa.
            if (ImGui::Checkbox("Exposed", &variable.exposed)) {
                Touch();
            }
            ImGui::PopID();
        }
    }

    /// Meneruskan breakpoint ke GraphCache, lalu memaksa kompilasi ulang.
    ///
    /// Paksa, karena berkas graph-nya sendiri tidak berubah: perbandingan waktu
    /// ubah tidak akan melihat apa pun, dan breakpoint yang baru dipasang tidak
    /// akan pernah sampai ke berkas yang dijalankan saat Play.
    void PublishBreakpoints(EditorContext& context) {
        if (context.scripts == nullptr || !openGuid_.IsValid() || openPath_.empty()) {
            return;
        }
        context.scripts->Graphs().SetBreakpoints(openGuid_, breakpoints_);
        context.scripts->Graphs().Rebuild(openGuid_, openPath_);
    }

    void DrawSelectedNode(EditorContext& context) {
        const std::vector<uint64_t> selected = canvas_.SelectedNodes();
        if (selected.size() != 1) {
            ImGui::TextColored(kHintColor, "Select a single node to edit it.");
            return;
        }
        const Uuid* guid = GuidOf(selected.front());
        GraphNode* node = guid != nullptr ? FindNode(*guid) : nullptr;
        if (node == nullptr) {
            return;
        }
        const NodeType* type = NodeCatalog::Get().Find(node->type);
        ImGui::TextColored(kHintColor, "%s", type != nullptr ? type->label.c_str()
                                                             : node->type.c_str());

        bool hasBreakpoint = Contains(breakpoints_, node->guid);
        if (ImGui::Checkbox("Breakpoint", &hasBreakpoint)) {
            if (hasBreakpoint) {
                breakpoints_.push_back(node->guid);
            } else {
                breakpoints_.erase(
                    std::remove(breakpoints_.begin(), breakpoints_.end(), node->guid),
                    breakpoints_.end());
            }
            PublishBreakpoints(context);
            Recompile();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            ImGui::SetTooltip(
                "Menjeda Play saat node ini dijalankan. Frame yang sedang\n"
                "berjalan tetap diselesaikan — yang berhenti adalah frame\n"
                "berikutnya, bukan tumpukan panggilan Lua.");
        }

        // Setelan per tipe node. Sengaja hanya untuk yang memang punya, alih-alih
        // sebuah editor kunci/nilai generik yang mengundang salah ketik.
        if (node->type.rfind("literal.", 0) == 0) {
            DrawSetting(*node, "value", "Value");
            if (node->type == "literal.vec3") {
                DrawSetting(*node, "x", "X");
                DrawSetting(*node, "y", "Y");
                DrawSetting(*node, "z", "Z");
            }
        } else if (node->type == "variable.get" || node->type == "variable.set") {
            DrawVariablePicker(*node);
        } else if (node->type == "flow.sequence") {
            DrawSetting(*node, "count", "Outputs");
        } else if (node->type == "comment") {
            DrawSetting(*node, "text", "Text");
        } else if (node->type == "group") {
            DrawSetting(*node, "text", "Title");
            ImGui::TextDisabled("Klik ganda judulnya di kanvas juga bisa.");
        }
    }

    void DrawSetting(GraphNode& node, const char* key, const char* label) {
        std::string value = node.Setting(key);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        if (ImGui::InputText(label, &value)) {
            node.settings[key] = value;
            Touch();
        }
    }

    void DrawVariablePicker(GraphNode& node) {
        const std::string current = node.Setting("variable");
        if (!ImGui::BeginCombo("Variable", current.c_str())) {
            return;
        }
        for (const GraphVariable& variable : graph_.variables) {
            if (ImGui::Selectable(variable.name.c_str(), variable.name == current)) {
                node.settings["variable"] = variable.name;
                Touch();
            }
        }
        ImGui::EndCombo();
    }

    // --- dokumen -----------------------------------------------------------

    void Open(EditorContext& context, const assets::AssetRecord& record) {
        Graph loaded;
        const GraphIoResult result =
            LoadGraphFromFile(loaded, context.assets->AbsolutePath(record));
        if (!result.ok) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Cannot open " + record.name + ": " + result.error);
            }
            return;
        }
        graph_ = std::move(loaded);
        openGuid_ = record.guid;
        openName_ = record.name;
        openPath_ = context.assets->AbsolutePath(record);
        dirty_ = false;
        ids_.clear();
        guids_.clear();
        placed_.clear();
        sized_.clear();
        groupInset_.clear();
        renamingNode_ = Uuid{};
        breakpoints_.clear();
        nextId_ = 1;
        pendingFit_ = true;
        Recompile();
    }

    void CreateGraph(EditorContext& context) {
        const std::filesystem::path folder = context.assets->Root() / "Graphs";
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);

        // Nama yang belum terpakai, dicari dengan menambah angka. Menimpa graph
        // yang sudah ada karena namanya kebetulan sama bukan hal yang boleh
        // terjadi diam-diam.
        std::filesystem::path path = folder / "NewGraph.simgraph";
        int suffix = 1;
        while (std::filesystem::exists(path)) {
            path = folder / ("NewGraph" + std::to_string(++suffix) + ".simgraph");
        }

        Graph fresh;
        GraphNode start;
        start.guid = Uuid::Generate();
        start.type = "event.update";
        start.position = Vec2(40.0f, 40.0f);
        fresh.nodes.push_back(std::move(start));

        if (!SaveGraphToFile(fresh, path).ok) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Cannot create " + path.filename().string());
            }
            return;
        }
        // Berkasnya sudah ada di disk; pemantau berkas yang akan memasukkannya
        // ke indeks. Panel tidak menunggu — graph-nya langsung dibuka dari
        // memori, dan GUID-nya menyusul saat indeks menyusul.
        graph_ = std::move(fresh);
        openPath_ = path;
        openName_ = path.filename().string();
        openGuid_ = Uuid{};
        dirty_ = false;
        ids_.clear();
        guids_.clear();
        placed_.clear();
        sized_.clear();
        groupInset_.clear();
        renamingNode_ = Uuid{};
        breakpoints_.clear();
        nextId_ = 1;
        pendingFit_ = true;
        Recompile();
        if (context.notifications != nullptr) {
            context.notifications->Success("Created " + openName_);
        }
    }

    void Save(EditorContext& context) {
        if (openPath_.empty()) {
            return;
        }
        // Posisi tidak dibaca ulang di sini: ia sudah mengalir balik dari kanvas
        // ke model begitu node digeser — lihat DrawNode(). Membacanya lagi di
        // sini berarti dua mekanisme untuk satu hal, dan yang kedua akan diam-
        // diam menang setiap kali keduanya tidak sepakat.
        const GraphIoResult result = SaveGraphToFile(graph_, openPath_);
        if (!result.ok) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Save failed: " + result.error);
            }
            return;
        }
        dirty_ = false;
        // Kompilasi ulangnya tidak dipicu di sini: pemantau berkas yang sama
        // yang menangani skrip akan melihat perubahannya dan meminta
        // GraphCache mengompilasi. Satu jalur, satu perilaku.
        if (context.notifications != nullptr) {
            context.notifications->Info("Saved " + openName_);
        }
    }

    void Touch() {
        dirty_ = true;
        Recompile();
    }

    /// Menerjemahkan kesalahan runtime terakhir menjadi node yang disorot.
    ///
    /// Inilah yang membuat kriteria terima nomor 7 berarti: traceback Lua
    /// menyebut nomor baris di berkas hasil kompilasi — berkas yang pengguna
    /// graph tidak pernah lihat — dan peta sumber mengubahnya kembali menjadi
    /// node di kanvas. Seluruh node yang ikut menghasilkan baris itu disorot,
    /// karena satu baris memang bisa memuat beberapa.
    void RefreshRuntimeFailure(EditorContext& context) {
        runtimeNodes_.clear();
        runtimeMessage_.clear();
        if (context.scripts == nullptr || !openGuid_.IsValid()) {
            return;
        }
        const RuntimeFailure* failure = context.scripts->LastFailure(openGuid_);
        if (failure == nullptr) {
            return;
        }
        runtimeMessage_ = failure->message;
        if (failure->line > 0) {
            // Peta sumber milik cache, bukan milik hasil kompilasi di panel:
            // yang berjalan saat Play adalah berkas dari cache, dan nomor baris
            // di traceback merujuk ke sana.
            const CompileResult* compiled = context.scripts->Graphs().LastResult(openGuid_);
            if (compiled != nullptr) {
                runtimeNodes_ = compiled->NodesAtLine(failure->line);
            }
        }
    }

    void Recompile() {
        CompileOptions options;
        options.breakpoints = breakpoints_;
        options.library = library_;
        compiled_ = CompileGraph(graph_, openName_, options);
        errorNodes_.clear();
        for (const CompileError& error : compiled_.errors) {
            if (error.node.IsValid()) {
                errorNodes_.push_back(error.node);
            }
        }
    }

    // --- model ------------------------------------------------------------

    GraphNode* FindNode(const Uuid& guid) {
        const auto it = std::find_if(graph_.nodes.begin(), graph_.nodes.end(),
                                     [&guid](const GraphNode& n) { return n.guid == guid; });
        return it == graph_.nodes.end() ? nullptr : &*it;
    }

    void RemoveLink(const Uuid& guid) {
        graph_.links.erase(std::remove_if(graph_.links.begin(), graph_.links.end(),
                                          [&guid](const GraphLink& link) {
                                              return link.guid == guid;
                                          }),
                           graph_.links.end());
    }

    void RemoveLinkInto(const Uuid& node, const std::string& pin) {
        graph_.links.erase(std::remove_if(graph_.links.begin(), graph_.links.end(),
                                          [&node, &pin](const GraphLink& link) {
                                              return link.toNode == node && link.toPin == pin;
                                          }),
                           graph_.links.end());
    }

    void RemoveNode(const Uuid& guid) {
        // Menghapus titik belok menyambung kembali kabelnya, bukan memutusnya.
        // Ia bagian dari kabel; membuangnya berarti "aku tidak jadi membelokkan
        // di sini", bukan "aku tidak jadi menyambungkan keduanya".
        const GraphNode* node = graph_.FindNode(guid);
        if (node != nullptr && node->type == "reroute") {
            RejoinThrough(guid);
        }

        graph_.nodes.erase(std::remove_if(graph_.nodes.begin(), graph_.nodes.end(),
                                          [&guid](const GraphNode& n) { return n.guid == guid; }),
                           graph_.nodes.end());
        // Koneksinya ikut dibuang. Meninggalkannya akan menghasilkan berkas yang
        // menyimpan koneksi ke node yang tidak ada — sesuatu yang pemuatnya
        // memang bereskan, tapi tidak boleh sengaja diproduksi.
        graph_.links.erase(std::remove_if(graph_.links.begin(), graph_.links.end(),
                                          [&guid](const GraphLink& link) {
                                              return link.fromNode == guid || link.toNode == guid;
                                          }),
                           graph_.links.end());
        breakpoints_.erase(std::remove(breakpoints_.begin(), breakpoints_.end(), guid),
                           breakpoints_.end());
    }

    /// Menyambung ulang kabel yang melewati sebuah titik belok: A→R→B jadi A→B.
    void RejoinThrough(const Uuid& reroute) {
        const GraphLink* incoming = graph_.LinkInto(reroute, "in");
        if (incoming == nullptr) {
            return;
        }
        const Uuid fromNode = incoming->fromNode;
        const std::string fromPin = incoming->fromPin;

        std::vector<GraphLink> rejoined;
        for (const GraphLink& link : graph_.links) {
            if (link.fromNode != reroute || link.fromPin != "out") {
                continue;
            }
            GraphLink direct;
            direct.guid = Uuid::Generate();
            direct.fromNode = fromNode;
            direct.fromPin = fromPin;
            direct.toNode = link.toNode;
            direct.toPin = link.toPin;
            rejoined.push_back(std::move(direct));
        }
        for (GraphLink& link : rejoined) {
            graph_.links.push_back(std::move(link));
        }
    }

    /// Id kanvas untuk sebuah GUID, dibuat saat pertama diminta.
    ///
    /// Dialokasikan dari penghitung, bukan diturunkan dari indeks: indeks
    /// bergeser setiap node dihapus, dan pustaka menyimpan posisi node
    /// berdasarkan id-nya — pergeseran satu langkah akan menukar posisi seluruh
    /// node sesudahnya.
    uint64_t IdOf(const Uuid& guid) {
        const auto it = ids_.find(guid);
        if (it != ids_.end()) {
            return it->second;
        }
        const uint64_t id = nextId_++;
        ids_.emplace(guid, id);
        guids_.emplace(id, guid);
        return id;
    }

    const Uuid* GuidOf(uint64_t id) const {
        const auto it = guids_.find(id);
        return it == guids_.end() ? nullptr : &it->second;
    }

    /// Node dan pin yang ditunjuk sebuah id pin. Null bila id-nya sudah basi.
    const GraphNode* ResolvePin(uint64_t pinId, GraphPin& outPin) const {
        const Uuid* guid = GuidOf(NodeOfPin(pinId));
        if (guid == nullptr) {
            return nullptr;
        }
        const GraphNode* node = graph_.FindNode(*guid);
        if (node == nullptr) {
            return nullptr;
        }
        const std::vector<GraphPin> pins = PinsOf(graph_, *node, library_);
        const std::size_t index = PinIndexOf(pinId);
        if (index >= pins.size()) {
            return nullptr;
        }
        outPin = pins[index];
        return node;
    }

    NodeCanvas canvas_;
    const GraphLibrary* library_ = nullptr;
    Graph graph_;
    CompileResult compiled_;

    Uuid openGuid_;
    std::string openName_;
    std::filesystem::path openPath_;
    bool dirty_ = false;
    bool pendingFit_ = false;
    /// Node yang akan dipusatkan setelah kanvas ditutup. 0 = tidak ada.
    uint64_t pendingFocus_ = 0;
    /// Lebar panel samping, dalam piksel. 0 = belum ditentukan.
    float sideWidth_ = 0.0f;
    float nextSpawn_ = 40.0f;

    std::vector<Uuid> errorNodes_;
    /// Node yang disorot karena kesalahan RUNTIME, bukan kompilasi.
    std::vector<Uuid> runtimeNodes_;
    std::string runtimeMessage_;
    std::vector<Uuid> breakpoints_;

    std::unordered_map<Uuid, uint64_t> ids_;
    std::unordered_map<uint64_t, Uuid> guids_;
    std::unordered_set<Uuid> placed_;
    /// Grup yang ukurannya sudah diserahkan ke kanvas sekali.
    std::unordered_set<Uuid> sized_;
    /// Selisih antara ukuran node grup dan luas kotaknya — judul dan bingkai.
    /// Diukur sekali per grup, bukan dipatok angka.
    std::unordered_map<Uuid, Vec2> groupInset_;
    uint64_t nextId_ = 1;


    /// Grup yang judulnya sedang disunting di kanvas, dan penyangganya.
    Uuid renamingNode_;
    std::string renameBuffer_;
    bool focusRename_ = false;

    std::string paletteFilter_;
};

#else

class GraphEditorPanel final : public Panel {
public:
    GraphEditorPanel()
        : Panel(panel_id::kGraphEditor, std::string(icons::kNodeGraph) + "  Graph Editor",
                PanelCategory::Authoring) {
        // Stub tanpa Lua ikut tertutup, supaya build dengan dan tanpa Lua
        // menampilkan panel yang sama saat pertama dijalankan.
        SetOpen(false);
    }

    void OnDraw(EditorContext& /*context*/) override {
        ImGui::TextDisabled("This build was configured without Lua.");
    }
};

#endif  // SIM_WITH_LUA

}  // namespace

SIM_REGISTER_PANEL(GraphEditorPanel, 26)

}  // namespace sim::editor
