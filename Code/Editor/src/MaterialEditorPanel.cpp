#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/NodeGraph.h"
#include "Sim/Editor/Notifications.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Material/MaterialCompiler.h"
#include "Sim/Material/MaterialGraph.h"
#include "Sim/Material/MaterialInstance.h"
#include "Sim/Material/MaterialNodeCatalog.h"
#include "Sim/Material/MaterialValidation.h"

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

using namespace sim::material;

constexpr ImVec4 kErrorColor(0.94f, 0.45f, 0.42f, 1.0f);
constexpr ImVec4 kOkColor(0.45f, 0.80f, 0.55f, 1.0f);
constexpr ImVec4 kHintColor(0.55f, 0.57f, 0.60f, 1.0f);
constexpr ImVec4 kGroupColor(0.45f, 0.55f, 0.70f, 1.0f);

/// Ukuran grup baru, dan batas bawah yang masih bisa dipegang tepinya.
constexpr float kDefaultGroupSize = 260.0f;
constexpr float kMinGroupSize = 40.0f;
/// Jarak antara tepi grup dan node terluar yang dibungkusnya.
constexpr float kGroupPadding = 24.0f;

/// Ruang id terpisah untuk node/link dan pin — alasannya sama persis dengan di
/// Graph Editor: imgui-node-editor menyimpan ketiganya di satu ruang angka, dan
/// menabrakkannya menghasilkan kanvas yang berperilaku aneh tanpa satu pun pesan.
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
///
/// Lebar vektor dibedakan gradasi, bukan warna yang sama sekali berbeda:
/// keempatnya saling bisa disambung lewat pelebaran skalar, dan warna yang
/// terlalu berjauhan akan menyarankan sebaliknya.
ImVec4 ColorOf(ValueKind kind) {
    switch (kind) {
        case ValueKind::Float: return ImVec4(0.55f, 0.80f, 0.45f, 1.0f);
        case ValueKind::Float2: return ImVec4(0.62f, 0.83f, 0.55f, 1.0f);
        case ValueKind::Float3: return ImVec4(0.70f, 0.86f, 0.62f, 1.0f);
        case ValueKind::Float4: return ImVec4(0.78f, 0.90f, 0.70f, 1.0f);
        case ValueKind::Texture: return ImVec4(0.85f, 0.45f, 0.80f, 1.0f);
        case ValueKind::Bool: return ImVec4(0.85f, 0.35f, 0.35f, 1.0f);
        case ValueKind::Numeric: return ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
    }
    return ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
}

Vec4 ToVec4(const ImVec4& v) {
    return Vec4(v.x, v.y, v.z, v.w);
}

bool Contains(const std::vector<Uuid>& list, const Uuid& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

/// Penyunting graph material.
///
/// **Yang dilihat pengguna di tab "Compiled Slang" berasal dari
/// CompileMaterial yang sama** dengan yang akan dipakai renderer. Syarat, bukan
/// kebetulan yang menyenangkan: hasil yang berbeda antara editor dan runtime
/// adalah kelas bug yang tidak boleh dibuka, dan satu-satunya cara menutupnya
/// adalah tidak pernah punya dua jalur.
///
/// **Bentuknya sengaja sama dengan Graph Editor** — daftar aset di kiri, kanvas
/// di tengah, Details dan hasil kompilasi sebagai tab di kanan, pemisah yang
/// bisa digeser. Keduanya adalah editor node, dan pengguna yang berpindah di
/// antara keduanya tidak seharusnya perlu belajar dua kali.
///
/// **Suntingan tidak melewati CommandHistory**, sama seperti Graph Editor dan
/// Script Editor. Riwayat undo utama menjanjikan pembatalan perubahan *scene*.
class MaterialEditorPanel final : public Panel {
public:
    MaterialEditorPanel()
        : Panel(panel_id::kMaterialEditor, std::string(icons::kMaterialEditor) + "  Material Editor",
                PanelCategory::Authoring) {}

    ~MaterialEditorPanel() override { canvas_.Shutdown(); }

    void OnDraw(EditorContext& context) override {
        if (context.assets == nullptr) {
            ImGui::TextDisabled("No asset database.");
            return;
        }
        // Setiap frame, sama seperti Graph Editor: Initialize() bersifat
        // idempoten dan sekaligus menetapkan konteks kanvas yang aktif. Dua
        // panel node yang hidup bersamaan membuat "yang terakhir memanggil"
        // menentukan konteks mana yang dipakai pustaka, jadi memanggilnya sekali
        // saja di awal berarti panel ini bekerja pada konteks milik panel lain.
        canvas_.Initialize();

        DrawToolbar(context);
        ImGui::Separator();

        const float listWidth = ImGui::GetFontSize() * 10.0f;
        if (ImGui::BeginChild("##materials", ImVec2(listWidth, 0.0f), ImGuiChildFlags_ResizeX)) {
            DrawMaterialList(context);
        }
        ImGui::EndChild();

        ImGui::SameLine();
        if (!openGuid_.IsValid()) {
            ImGui::TextColored(kHintColor, "Pick a material on the left, or create one.");
        } else if (instanceMode_) {
            // Instance tidak punya graph sendiri, jadi tidak ada kanvas untuk
            // digambar. Menampilkan graph induknya di sini akan mengundang
            // suntingan yang tidak akan tersimpan ke mana-mana.
            DrawInstanceAndSide();
        } else {
            DrawCanvasAndSide(context);
        }
    }

private:
    // --- toolbar & daftar --------------------------------------------------

    void DrawToolbar(EditorContext& context) {
        ImGui::BeginDisabled(!dirty_ || !openGuid_.IsValid());
        if (ImGui::Button((std::string(icons::kSave) + "  Save").c_str())) {
            Save(context);
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(!openGuid_.IsValid() || instanceMode_);
        if (ImGui::Button("Fit")) {
            pendingFit_ = true;
        }
        ImGui::EndDisabled();

        // Instance dibuat dari material yang sedang dibuka, bukan dari dialog
        // pemilih: "instance dari yang mana" adalah pertanyaan yang jawabannya
        // sudah ada di layar.
        ImGui::SameLine();
        const std::vector<uint64_t> selected =
            openGuid_.IsValid() && !instanceMode_ ? canvas_.SelectedNodes()
                                                  : std::vector<uint64_t>{};
        ImGui::BeginDisabled(selected.empty());
        if (ImGui::Button("Group selection")) {
            GroupSelection(selected);
        }
        ImGui::EndDisabled();
        if (!selected.empty() && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_G)) {
            GroupSelection(selected);
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(!openGuid_.IsValid() || instanceMode_);
        if (ImGui::Button("New Instance")) {
            CreateInstance(context);
        }
        ImGui::EndDisabled();

        if (!openGuid_.IsValid()) {
            return;
        }
        ImGui::SameLine();
        ImGui::TextColored(kHintColor, "%s%s", openName_.c_str(), dirty_ ? " *" : "");
        if (instanceMode_) {
            ImGui::SameLine();
            ImGui::TextColored(kHintColor, "(instance of %s)", parentName_.c_str());
        }

        ImGui::SameLine();
        if (compiled_.ok) {
            ImGui::TextColored(kOkColor, "%s  compiles", icons::kLogInfo);
        } else {
            ImGui::TextColored(kErrorColor, "%s  %d error(s)", icons::kLogError,
                               static_cast<int>(compiled_.errors.size()));
        }
    }

    void DrawMaterialList(EditorContext& context) {
        if (ImGui::Button("New Material", ImVec2(-FLT_MIN, 0.0f))) {
            CreateMaterial(context);
        }
        ImGui::Spacing();

        for (const assets::AssetRecord& record : context.assets->All()) {
            if (record.type != assets::AssetType::Material) {
                continue;
            }
            const bool selected = record.guid == openGuid_;
            if (ImGui::Selectable(record.name.c_str(), selected)) {
                Open(context, record.guid);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                ImGui::SetTooltip("%s", record.relativePath.c_str());
            }
        }
    }

    // --- kanvas & panel samping --------------------------------------------

    /// Pemisah yang bisa digeser, ditulis sendiri dengan alasan yang sama
    /// seperti di Graph Editor: `ImGuiChildFlags_ResizeX` hanya bisa memasang
    /// pegangan di tepi KANAN sebuah child, sehingga yang tersimpan adalah lebar
    /// kanvas dan melebarkan panel justru melebarkan tab — terbalik — dan
    /// menyempitkannya menghimpit tab sampai hilang untuk seterusnya.
    void DrawCanvasAndSide(EditorContext& context) {
        const float avail = ImGui::GetContentRegionAvail().x;
        const float handle = ImGui::GetStyle().ItemSpacing.x;
        if (sideWidth_ <= 0.0f) {
            sideWidth_ = avail * 0.36f;
        }
        const float maxSide = std::max(avail - ImGui::GetFontSize() * 12.0f - handle, 0.0f);
        sideWidth_ = std::clamp(sideWidth_, std::min(ImGui::GetFontSize() * 13.0f, maxSide),
                                maxSide);

        if (ImGui::BeginChild("##canvas",
                              ImVec2(std::max(avail - sideWidth_ - handle, 1.0f), 0.0f))) {
            DrawCanvas(context);
        }
        ImGui::EndChild();

        ImGui::SameLine(0.0f, 0.0f);
        DrawSideSplitter(handle);
        ImGui::SameLine(0.0f, 0.0f);

        if (ImGui::BeginChild("##side", ImVec2(0.0f, 0.0f))) {
            DrawSidePanel();
        }
        ImGui::EndChild();
    }

    /// Instance: daftar parameter di kiri, hasil kompilasi induk di kanan.
    ///
    /// Hasil kompilasinya tetap ditampilkan, dan itu bukan sisa dari mode graph:
    /// yang ingin diketahui pemakai instance adalah parameter mana yang
    /// benar-benar dipakai shader — sesuatu yang hanya terjawab dengan melihat
    /// kodenya.
    void DrawInstanceAndSide() {
        const float avail = ImGui::GetContentRegionAvail().x;
        const float handle = ImGui::GetStyle().ItemSpacing.x;
        if (sideWidth_ <= 0.0f) {
            sideWidth_ = avail * 0.36f;
        }
        const float maxSide = std::max(avail - ImGui::GetFontSize() * 12.0f - handle, 0.0f);
        sideWidth_ = std::clamp(sideWidth_, std::min(ImGui::GetFontSize() * 13.0f, maxSide),
                                maxSide);

        if (ImGui::BeginChild("##params",
                              ImVec2(std::max(avail - sideWidth_ - handle, 1.0f), 0.0f))) {
            DrawInstanceParameters();
        }
        ImGui::EndChild();

        ImGui::SameLine(0.0f, 0.0f);
        DrawSideSplitter(handle);
        ImGui::SameLine(0.0f, 0.0f);

        if (ImGui::BeginChild("##side", ImVec2(0.0f, 0.0f))) {
            DrawCompiledSlang();
        }
        ImGui::EndChild();
    }

    void DrawInstanceParameters() {
        const std::vector<ResolvedParameter> resolved = ResolveParameters(graph_, instance_);
        if (resolved.empty()) {
            ImGui::TextColored(kHintColor,
                               "%s exposes no parameters, so there is nothing to override here.",
                               parentName_.c_str());
            return;
        }

        for (const ResolvedParameter& parameter : resolved) {
            ImGui::PushID(parameter.name.c_str());

            // Penanda timpaan di depan namanya, bukan di kolom terpisah: yang
            // ingin dijawab sekali lihat adalah "mana yang sudah saya ubah",
            // dan kolom terpisah membuat mata harus menyeberang.
            ImGui::TextColored(parameter.overridden ? kOkColor : kHintColor,
                               parameter.overridden ? "*" : " ");
            ImGui::SameLine();
            ImGui::TextUnformatted(parameter.name.c_str());
            if (!parameter.tooltip.empty() &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                ImGui::SetTooltip("%s", parameter.tooltip.c_str());
            }

            ImGui::SameLine(ImGui::GetFontSize() * 10.0f);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);

            MaterialValue edited = parameter.value;
            if (DrawValueWidget(parameter, edited)) {
                instance_.Set(parameter.name, edited);
                dirty_ = true;
            }

            // Mengembalikan ke nilai induk hanya berarti bila memang ditimpa.
            if (parameter.overridden) {
                ImGui::SameLine();
                if (ImGui::SmallButton("revert")) {
                    instance_.Clear(parameter.name);
                    dirty_ = true;
                }
            }
            ImGui::PopID();
        }
    }

    /// Widget menurut tipe parameter. Mengembalikan true bila nilainya berubah.
    static bool DrawValueWidget(const ResolvedParameter& parameter, MaterialValue& value) {
        float* v = value.components.data();
        const bool bounded = parameter.minValue != parameter.maxValue;
        switch (parameter.kind) {
            case ValueKind::Float:
                return bounded ? ImGui::SliderFloat("##value", v, parameter.minValue,
                                                    parameter.maxValue)
                               : ImGui::DragFloat("##value", v, 0.01f);
            case ValueKind::Float2: return ImGui::DragFloat2("##value", v, 0.01f);
            case ValueKind::Float3:
                return parameter.isColor ? ImGui::ColorEdit3("##value", v)
                                         : ImGui::DragFloat3("##value", v, 0.01f);
            case ValueKind::Float4:
                return parameter.isColor ? ImGui::ColorEdit4("##value", v)
                                         : ImGui::DragFloat4("##value", v, 0.01f);
            case ValueKind::Bool: {
                bool flag = v[0] != 0.0f;
                if (ImGui::Checkbox("##value", &flag)) {
                    v[0] = flag ? 1.0f : 0.0f;
                    return true;
                }
                return false;
            }
            case ValueKind::Texture:
            case ValueKind::Numeric: break;
        }
        ImGui::TextDisabled("(not editable)");
        return false;
    }

    void DrawSideSplitter(float width) {
        ImGui::InvisibleButton("##sidesplit", ImVec2(width, -FLT_MIN));
        const bool active = ImGui::IsItemActive();
        const bool touched = active || ImGui::IsItemHovered();
        if (touched) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (active) {
            sideWidth_ -= ImGui::GetIO().MouseDelta.x;
        }
        if (touched) {
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                ImGui::GetColorU32(active ? ImGuiCol_SeparatorActive
                                          : ImGuiCol_SeparatorHovered));
        }
    }

    void DrawCanvas(EditorContext& context) {
        canvas_.Begin("##materialcanvas", Vec2(0.0f, 0.0f));

        for (const MaterialNode& node : graph_.nodes) {
            DrawNode(node);
        }
        for (const MaterialLink& link : graph_.links) {
            DrawLink(link);
        }

        HandleCreate();
        HandleDelete();
        HandleContextMenus(context);

        canvas_.End();

        if (pendingFit_) {
            // Ditunda satu frame: ukuran node baru diketahui pustaka setelah
            // digambar sekali, dan memanggilnya lebih awal memusatkan kanvas ke
            // kotak yang masih kosong.
            canvas_.FitToContent();
            pendingFit_ = false;
        }
    }

    void DrawNode(const MaterialNode& node) {
        const MaterialNodeType* type = MaterialNodeCatalog::Get().Find(node.type);
        const uint64_t id = IdOf(node.guid);

        // Posisi berpindah dua arah: dari berkas ke kanvas saat material dibuka,
        // dan dari kanvas ke berkas begitu node diseret. `placed_` yang
        // menentukan arahnya — tanpa itu, menyeret node langsung ditimpa balik
        // oleh posisi yang tersimpan.
        if (!placed_.contains(node.guid)) {
            canvas_.SetNodePosition(id, node.position);
            placed_.insert(node.guid);
        } else if (MaterialNode* mutableNode = FindNode(node.guid)) {
            const Vec2 current = canvas_.GetNodePosition(id);
            if (std::abs(current.x - mutableNode->position.x) > 0.5f ||
                std::abs(current.y - mutableNode->position.y) > 0.5f) {
                mutableNode->position = current;
                // dirty_ langsung, bukan lewat Touch(): posisi node tidak
                // mengubah satu baris pun Slang yang dihasilkan, dan
                // mengompilasi ulang tiap frame seretan adalah pekerjaan yang
                // hasilnya dijamin sama.
                dirty_ = true;
            }
        }

        if (node.type == "group") {
            DrawGroupNode(node, id);
            return;
        }

        canvas_.BeginNode(id);
        if (node.type == "comment") {
            ImGui::TextColored(kHintColor, "%s", node.Setting("text", "Comment").c_str());
            canvas_.EndNode();
            return;
        }

        ImGui::TextUnformatted(type != nullptr ? type->label.c_str() : node.type.c_str());
        DrawNodeSubtitle(node);
        ImGui::Dummy(ImVec2(0.0f, 2.0f));

        const std::vector<MaterialPin> pins = PinsOf(graph_, node);
        std::vector<std::size_t> inputs;
        std::vector<std::size_t> outputs;
        for (std::size_t i = 0; i < pins.size(); ++i) {
            (pins[i].direction == PinDirection::Input ? inputs : outputs).push_back(i);
        }

        // Kolom WAJIB menyesuaikan isi, bukan meregang — lihat catatan panjang
        // di GraphEditorPanel: node yang meregang tidak bisa digeser, hanya
        // melebar.
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

        if (Contains(errorNodes_, node.guid)) {
            canvas_.DrawNodeBackground(id, ToVec4(kErrorColor), 2.5f);
        }
    }

    /// Node grup: kotak yang membawa serta node di dalamnya ketika digeser.
    ///
    /// Disalin dari Graph Editor beserta seluruh jebakannya, karena jebakannya
    /// milik pustaka kanvas — bukan milik salah satu panel. Yang paling mahal:
    /// yang DISETEL adalah luas kotaknya, yang DIBACA BALIK adalah ukuran node
    /// berikut judul dan bingkainya. Menyimpan yang kedua ke tempat yang pertama
    /// membuat grup tumbuh sedikit setiap kali graph dibuka lalu disimpan.
    ///
    /// Selisihnya diukur sekali dari grup itu sendiri, bukan dipatok angka: ia
    /// bergantung pada tinggi font dan gaya kanvas, dan angka yang ditulis
    /// tangan akan salah begitu salah satunya berubah.
    void DrawGroupNode(const MaterialNode& node, uint64_t id) {
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
        if (justPlaced) {
            groupInset_[node.guid] = outer - size;
            return;
        }
        const auto inset = groupInset_.find(node.guid);
        if (inset == groupInset_.end()) {
            return;
        }
        const Vec2 area = outer - inset->second;
        if (MaterialNode* mutableNode = FindNode(node.guid)) {
            if (std::abs(area.x - mutableNode->size.x) > 0.5f ||
                std::abs(area.y - mutableNode->size.y) > 0.5f) {
                mutableNode->size = area;
                dirty_ = true;
            }
        }
    }

    /// Judul grup, bisa disunting di tempat lewat klik ganda — di tempat yang
    /// sama dengan tempat ia terbaca, sehingga tidak ada yang perlu dicari.
    void DrawGroupTitle(const MaterialNode& node) {
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
        // Selesai juga saat fokus berpindah, bukan hanya saat Enter: pengguna
        // yang mengetik nama lalu mengklik kanvas jelas bermaksud menyimpannya.
        if (!committed && !ImGui::IsItemDeactivated()) {
            return;
        }
        if (MaterialNode* mutableNode = FindNode(node.guid)) {
            const std::string trimmed = renameBuffer_.empty() ? "Group" : renameBuffer_;
            if (mutableNode->Setting("text", "Group") != trimmed) {
                mutableNode->settings["text"] = trimmed;
                Touch();
            }
        }
        renamingNode_ = Uuid{};
    }

    /// Membungkus node terpilih dengan sebuah grup baru.
    void GroupSelection(const std::vector<uint64_t>& selected) {
        bool any = false;
        Vec2 min(0.0f, 0.0f);
        Vec2 max(0.0f, 0.0f);
        for (const uint64_t id : selected) {
            const Uuid* guid = GuidOf(id);
            const MaterialNode* node = guid != nullptr ? graph_.FindNode(*guid) : nullptr;
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

        MaterialNode group;
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

    /// Baris kedua judul, untuk node yang identitasnya ada di setelannya:
    /// parameter mana, tekstur mana. Tanpa ini, tiga node Parameter di kanvas
    /// tampak persis sama.
    void DrawNodeSubtitle(const MaterialNode& node) {
        if (node.type == "param.get") {
            ImGui::TextColored(kHintColor, "%s", node.Setting("parameter", "(none)").c_str());
        } else if (node.type == "input.texture") {
            ImGui::TextColored(kHintColor, "%s", node.Setting("name", "(unnamed)").c_str());
        } else if (node.type == "input.constant") {
            ImGui::TextColored(kHintColor, "%s", node.Setting("value", "0.0").c_str());
        }
    }

    void DrawPin(const MaterialNode& node, const MaterialPin& pin, uint64_t pinId) {
        const std::string label = pin.label.empty() ? pin.name : pin.label;
        const bool input = pin.direction == PinDirection::Input;
        // Tipe yang DISIMPULKAN, bukan yang dideklarasikan: sebuah Multiply
        // yang diberi float3 memang menghasilkan float3, dan pin abu-abu yang
        // tidak pernah berubah warna menyembunyikan justru informasi yang
        // paling berguna saat menyambung.
        const ValueKind kind = input ? types_.Into(graph_, node, pin.name)
                                     : types_.Of(graph_, node, pin.name);
        const ImVec4 color = ColorOf(kind);

        if (input) {
            canvas_.BeginInputPin(pinId);
            ImGui::TextColored(color, "%s", pin.kind == ValueKind::Texture ? "■" : "●");
            ImGui::SameLine();
            ImGui::TextUnformatted(label.c_str());
        } else {
            canvas_.BeginOutputPin(pinId);
            ImGui::TextUnformatted(label.c_str());
            ImGui::SameLine();
            ImGui::TextColored(color, "%s", pin.kind == ValueKind::Texture ? "■" : "●");
        }
        canvas_.EndPin();

        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            ImGui::SetTooltip("%s", ToString(kind));
        }
    }

    void DrawLink(const MaterialLink& link) {
        const MaterialNode* from = graph_.FindNode(link.fromNode);
        const MaterialNode* to = graph_.FindNode(link.toNode);
        if (from == nullptr || to == nullptr) {
            return;
        }
        const std::size_t fromIndex = PinIndexIn(*from, link.fromPin);
        const std::size_t toIndex = PinIndexIn(*to, link.toPin);
        if (fromIndex == kNoPin || toIndex == kNoPin) {
            return;
        }
        const ImVec4 color = ColorOf(types_.Of(graph_, *from, link.fromPin));
        canvas_.Link(IdOf(link.guid), PinId(IdOf(from->guid), fromIndex),
                     PinId(IdOf(to->guid), toIndex), ToVec4(color), 2.0f);
    }

    // --- koneksi & penghapusan ---------------------------------------------

    void HandleCreate() {
        if (!canvas_.BeginCreate()) {
            canvas_.EndCreate();
            return;
        }
        uint64_t fromPinId = 0;
        uint64_t toPinId = 0;
        // SEKALI, bukan di dalam while: percobaan koneksi bukan antrean, dan
        // menguraskan membekukan editor begitu kedua ujungnya sah. Lihat
        // catatan di NodeGraph.h.
        if (canvas_.QueryNewLink(fromPinId, toPinId)) {
            TryConnect(fromPinId, toPinId);
        }
        canvas_.EndCreate();
    }

    void TryConnect(uint64_t fromPinId, uint64_t toPinId) {
        MaterialPin fromPin;
        MaterialPin toPin;
        const MaterialNode* fromNode = ResolvePin(fromPinId, fromPin);
        const MaterialNode* toNode = ResolvePin(toPinId, toPin);
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
        // Tipe sumbernya yang sudah disimpulkan, bukan yang dideklarasikan —
        // penyimpulan yang sama dipakai validasi dan kompiler, sehingga kanvas
        // tidak pernah menerima sambungan yang kemudian ditolak keduanya.
        const ValueKind sourceKind = types_.Of(graph_, *fromNode, fromPin.name);
        if (fromPin.direction != PinDirection::Output ||
            toPin.direction != PinDirection::Input || fromNode == toNode ||
            !Accepts(toPin.kind, sourceKind)) {
            canvas_.RejectLink();
            return;
        }
        if (!canvas_.AcceptLink()) {
            return;
        }

        // Pin masukan hanya boleh punya satu sumber: yang lama diputus.
        const Uuid toGuid = toNode->guid;
        const std::string toName = toPin.name;
        graph_.links.erase(std::remove_if(graph_.links.begin(), graph_.links.end(),
                                          [&](const MaterialLink& link) {
                                              return link.toNode == toGuid &&
                                                     link.toPin == toName;
                                          }),
                           graph_.links.end());

        MaterialLink link;
        link.guid = Uuid::Generate();
        link.fromNode = fromNode->guid;
        link.fromPin = fromPin.name;
        link.toNode = toGuid;
        link.toPin = toName;
        graph_.links.push_back(std::move(link));
        Touch();
    }

    void HandleDelete() {
        if (!canvas_.BeginDelete()) {
            canvas_.EndDelete();
            return;
        }
        // Antrean, dan memang harus dikuras — kebalikan dari QueryNewLink.
        uint64_t id = 0;
        while (canvas_.QueryDeletedLink(id)) {
            if (!canvas_.AcceptDeletion()) {
                continue;
            }
            if (const Uuid* guid = GuidOf(id)) {
                const Uuid target = *guid;
                graph_.links.erase(
                    std::remove_if(graph_.links.begin(), graph_.links.end(),
                                   [&target](const MaterialLink& link) {
                                       return link.guid == target;
                                   }),
                    graph_.links.end());
                Touch();
            }
        }
        while (canvas_.QueryDeletedNode(id)) {
            const Uuid* guid = GuidOf(id);
            const MaterialNode* node = guid != nullptr ? graph_.FindNode(*guid) : nullptr;
            // Node keluaran tidak boleh hilang: material tanpanya tidak punya
            // arti, dan menyisakan kanvas yang tidak bisa dikompilasi tanpa cara
            // jelas memperbaikinya lebih buruk daripada menolak penghapusannya.
            if (node == nullptr || node->type == kSurfaceOutputType) {
                canvas_.RejectDeletion();
                continue;
            }
            if (!canvas_.AcceptDeletion()) {
                continue;
            }
            graph_.RemoveNode(*guid);
            Touch();
        }
        canvas_.EndDelete();
    }

    // --- menu konteks & palet ----------------------------------------------

    void HandleContextMenus(EditorContext& context) {
        canvas_.Suspend();
        if (canvas_.RequestedBackgroundMenu()) {
            spawnPosition_ = Vec2(ImGui::GetMousePos().x, ImGui::GetMousePos().y);
            ImGui::OpenPopup("##palette");
            paletteFilter_.clear();
        }
        if (ImGui::BeginPopup("##palette")) {
            DrawPalette(context);
            ImGui::EndPopup();
        }
        canvas_.Resume();
    }

    void DrawPalette(EditorContext& /*context*/) {
        ImGui::TextColored(kHintColor, "Add node");
        ImGui::Separator();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::InputTextWithHint("##filter", "Search", &paletteFilter_);

        std::string category;
        for (const MaterialNodeType& type : MaterialNodeCatalog::Get().All()) {
            // Node keluaran tidak ada di palet: tepat satu boleh ada, dan ia
            // sudah dibuat bersama materialnya.
            if (type.key == kSurfaceOutputType) {
                continue;
            }
            if (!paletteFilter_.empty() && !Matches(type, paletteFilter_)) {
                continue;
            }
            if (type.category != category) {
                category = type.category;
                ImGui::Separator();
                ImGui::TextColored(kHintColor, "%s", category.c_str());
            }
            if (ImGui::Selectable(type.label.c_str())) {
                AddNode(type.key);
                ImGui::CloseCurrentPopup();
            }
            if (!type.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                ImGui::SetTooltip("%s", type.tooltip.c_str());
            }
        }
    }

    static bool Matches(const MaterialNodeType& type, const std::string& filter) {
        const auto contains = [&filter](const std::string& text) {
            return std::search(text.begin(), text.end(), filter.begin(), filter.end(),
                               [](char a, char b) {
                                   return std::tolower(static_cast<unsigned char>(a)) ==
                                          std::tolower(static_cast<unsigned char>(b));
                               }) != text.end();
        };
        return contains(type.label) || contains(type.key) || contains(type.category);
    }

    void AddNode(const std::string& type) {
        MaterialNode node;
        node.guid = Uuid::Generate();
        node.type = type;
        node.position = spawnPosition_;
        if (type == "input.texture") {
            node.settings["name"] = "Texture";
        } else if (type == "input.constant") {
            node.settings["kind"] = "float";
            node.settings["value"] = "0.0";
        }
        graph_.nodes.push_back(std::move(node));
        Touch();
    }

    // --- panel samping ------------------------------------------------------

    void DrawSidePanel() {
        if (!ImGui::BeginTabBar("##side")) {
            return;
        }
        if (ImGui::BeginTabItem("Details")) {
            DrawDetails();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Compiled Slang")) {
            DrawCompiledSlang();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    void DrawDetails() {
        ImGui::TextColored(kHintColor, "Parameters (exposed to instances)");
        if (ImGui::Button("Add Parameter")) {
            MaterialParameter parameter;
            parameter.name = "param" + std::to_string(graph_.parameters.size() + 1);
            parameter.kind = ValueKind::Float;
            parameter.defaultValue = "0.0";
            graph_.parameters.push_back(std::move(parameter));
            Touch();
        }

        for (std::size_t i = 0; i < graph_.parameters.size();) {
            MaterialParameter& parameter = graph_.parameters[i];
            ImGui::PushID(static_cast<int>(i));

            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
            if (ImGui::InputText("##name", &parameter.name)) {
                Touch();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
            if (DrawKindCombo("##kind", parameter.kind)) {
                Touch();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
            if (ImGui::InputText("##default", &parameter.defaultValue)) {
                Touch();
            }
            ImGui::SameLine();
            const bool remove = ImGui::SmallButton("x");
            ImGui::PopID();

            if (remove) {
                graph_.parameters.erase(graph_.parameters.begin() +
                                        static_cast<std::ptrdiff_t>(i));
                Touch();
            } else {
                ++i;
            }
        }

        ImGui::Separator();
        DrawSelectedNode();
    }

    /// Setelan node yang sedang dipilih. Satu node saja: menyunting setelan
    /// beberapa node sekaligus menjanjikan sesuatu yang maknanya berbeda-beda
    /// per jenis node.
    void DrawSelectedNode() {
        const std::vector<uint64_t> selected = canvas_.SelectedNodes();
        if (selected.size() != 1) {
            ImGui::TextColored(kHintColor, "Select a single node to edit it.");
            return;
        }
        const Uuid* guid = GuidOf(selected.front());
        MaterialNode* node = guid != nullptr ? FindNode(*guid) : nullptr;
        if (node == nullptr) {
            return;
        }
        const MaterialNodeType* type = MaterialNodeCatalog::Get().Find(node->type);
        ImGui::TextColored(kHintColor, "%s",
                           type != nullptr ? type->label.c_str() : node->type.c_str());

        if (node->type == "input.texture") {
            DrawSetting(*node, "name", "Name");
        } else if (node->type == "input.constant") {
            ValueKind kind = ValueKindFromString(node->Setting("kind", "float"));
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
            if (DrawKindCombo("Type", kind)) {
                node->settings["kind"] = ToString(kind);
                Touch();
            }
            DrawSetting(*node, "value", "Value");
        } else if (node->type == "param.get") {
            DrawParameterCombo(*node);
        } else if (node->type == "comment") {
            DrawSetting(*node, "text", "Text");
        }

        // Nilai literal untuk pin masukan yang tidak tersambung. Sama sahnya
        // dengan kabel, dan jauh lebih cepat untuk sebuah angka.
        ImGui::Spacing();
        ImGui::TextColored(kHintColor, "Unconnected inputs");
        for (const MaterialPin& pin : PinsOf(graph_, *node)) {
            if (pin.direction != PinDirection::Input ||
                graph_.LinkInto(node->guid, pin.name) != nullptr) {
                continue;
            }
            std::string value = node->pinValues.count(pin.name) != 0
                                    ? node->pinValues.at(pin.name)
                                    : pin.defaultValue;
            ImGui::PushID(pin.name.c_str());
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
            if (ImGui::InputText(pin.label.empty() ? pin.name.c_str() : pin.label.c_str(),
                                 &value)) {
                node->pinValues[pin.name] = value;
                Touch();
            }
            ImGui::PopID();
        }
    }

    void DrawSetting(MaterialNode& node, const char* key, const char* label) {
        std::string value = node.Setting(key);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        if (ImGui::InputText(label, &value)) {
            node.settings[key] = value;
            Touch();
        }
    }

    void DrawParameterCombo(MaterialNode& node) {
        const std::string current = node.Setting("parameter", "(none)");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        if (!ImGui::BeginCombo("Parameter", current.c_str())) {
            return;
        }
        for (const MaterialParameter& parameter : graph_.parameters) {
            if (ImGui::Selectable(parameter.name.c_str(), parameter.name == current)) {
                node.settings["parameter"] = parameter.name;
                Touch();
            }
        }
        ImGui::EndCombo();
    }

    static bool DrawKindCombo(const char* label, ValueKind& kind) {
        static constexpr ValueKind kChoices[] = {ValueKind::Float, ValueKind::Float2,
                                                 ValueKind::Float3, ValueKind::Float4,
                                                 ValueKind::Bool};
        bool changed = false;
        if (!ImGui::BeginCombo(label, ToString(kind))) {
            return false;
        }
        for (const ValueKind choice : kChoices) {
            if (ImGui::Selectable(ToString(choice), choice == kind)) {
                kind = choice;
                changed = true;
            }
        }
        ImGui::EndCombo();
        return changed;
    }

    void DrawCompiledSlang() {
        if (!compiled_.errors.empty()) {
            for (const MaterialIssue& issue : compiled_.errors) {
                ImGui::TextColored(kErrorColor, "%s  %s", icons::kLogError,
                                   issue.message.c_str());
                // Pesan yang bisa diklik: menemukan node penyebabnya di kanvas
                // yang besar adalah pekerjaan yang tidak perlu dilakukan
                // pengguna ketika editor sudah tahu jawabannya.
                if (issue.node.IsValid() && ImGui::IsItemClicked()) {
                    canvas_.CenterOnNode(IdOf(issue.node));
                }
            }
            ImGui::Separator();
        }
        if (compiled_.slang.empty()) {
            ImGui::TextColored(kHintColor, "Nothing compiled yet.");
            return;
        }
        ImGui::TextUnformatted(compiled_.slang.c_str());
    }

    // --- berkas -------------------------------------------------------------

    void CreateMaterial(EditorContext& context) {
        const std::filesystem::path folder =
            std::filesystem::path(context.assets->Root()) / "Materials";
        std::filesystem::path path = folder / "NewMaterial.simmat";
        int suffix = 0;
        while (std::filesystem::exists(path)) {
            path = folder / ("NewMaterial" + std::to_string(++suffix) + ".simmat");
        }

        MaterialGraph fresh;
        MaterialNode output;
        output.guid = Uuid::Generate();
        output.type = std::string(kSurfaceOutputType);
        output.position = Vec2(240.0f, 80.0f);
        fresh.nodes.push_back(std::move(output));

        if (!SaveMaterialToFile(fresh, path).ok) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Cannot create " + path.filename().string());
            }
            return;
        }
        context.assets->ScanNow();
        if (const assets::AssetRecord* record =
                context.assets->FindByRelativePath("Materials/" + path.filename().string())) {
            Open(context, record->guid);
        }
    }

    void Open(EditorContext& context, const Uuid& guid) {
        const assets::AssetRecord* record = context.assets->Find(guid);
        if (record == nullptr) {
            return;
        }
        const std::filesystem::path path = context.assets->AbsolutePath(*record);
        if (path.extension() == ".simmatinst") {
            OpenInstance(context, guid, *record, path);
            return;
        }

        MaterialGraph loaded;
        const MaterialIoResult result = LoadMaterialFromFile(loaded, path);
        if (!result.ok) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Cannot open " + record->name + ": " + result.error);
            }
            return;
        }
        graph_ = std::move(loaded);
        instance_ = MaterialInstance{};
        instanceMode_ = false;
        parentName_.clear();
        openGuid_ = guid;
        openName_ = record->name;
        openPath_ = context.assets->AbsolutePath(*record);
        dirty_ = false;
        placed_.clear();
        sized_.clear();
        groupInset_.clear();
        ids_.clear();
        guids_.clear();
        pendingFit_ = true;
        Recompile();
    }

    /// Memuat instance beserta graph induknya.
    ///
    /// Graph induk dimuat apa adanya ke `graph_` — bukan disalin untuk disunting,
    /// melainkan supaya daftar parameter dan hasil kompilasinya bisa ditampilkan.
    /// Mode instance mematikan setiap jalur yang menyuntingnya.
    void OpenInstance(EditorContext& context, const Uuid& guid,
                      const assets::AssetRecord& record, const std::filesystem::path& path) {
        MaterialInstance loaded;
        const MaterialIoResult result = LoadInstanceFromFile(loaded, path);
        if (!result.ok) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Cannot open " + record.name + ": " + result.error);
            }
            return;
        }
        const assets::AssetRecord* parent = context.assets->Find(loaded.parent);
        if (parent == nullptr) {
            if (context.notifications != nullptr) {
                context.notifications->Error(record.name + " points at a material that is gone");
            }
            return;
        }
        MaterialGraph parentGraph;
        const MaterialIoResult parentResult =
            LoadMaterialFromFile(parentGraph, context.assets->AbsolutePath(*parent));
        if (!parentResult.ok) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Cannot open parent " + parent->name);
            }
            return;
        }

        graph_ = std::move(parentGraph);
        instance_ = std::move(loaded);
        instanceMode_ = true;
        parentName_ = parent->name;
        openGuid_ = guid;
        openName_ = record.name;
        openPath_ = path;
        dirty_ = false;
        placed_.clear();
        ids_.clear();
        guids_.clear();
        Recompile();
    }

    void CreateInstance(EditorContext& context) {
        const std::filesystem::path folder =
            std::filesystem::path(context.assets->Root()) / "Materials";
        const std::string stem = openPath_.stem().string();
        std::filesystem::path path = folder / (stem + "Instance.simmatinst");
        int suffix = 0;
        while (std::filesystem::exists(path)) {
            path = folder / (stem + "Instance" + std::to_string(++suffix) + ".simmatinst");
        }

        MaterialInstance fresh;
        fresh.parent = openGuid_;
        // Tanpa satu timpaan pun. Instance baru harus terlihat persis seperti
        // induknya — perbedaan yang muncul sebelum ada yang menyentuhnya akan
        // membuat pengguna mencari sebab yang tidak ada.
        if (!SaveInstanceToFile(fresh, path).ok) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Cannot create " + path.filename().string());
            }
            return;
        }
        context.assets->ScanNow();
        if (const assets::AssetRecord* record =
                context.assets->FindByRelativePath("Materials/" + path.filename().string())) {
            Open(context, record->guid);
        }
    }

    void Save(EditorContext& context) {
        if (!openGuid_.IsValid()) {
            return;
        }
        if (instanceMode_) {
            SaveInstance(context);
            return;
        }
        if (!SaveMaterialToFile(graph_, openPath_).ok) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Save failed: " + openName_);
            }
            return;
        }
        dirty_ = false;
        if (context.notifications != nullptr) {
            context.notifications->Success("Saved " + openName_);
        }
    }

    void SaveInstance(EditorContext& context) {
        if (!SaveInstanceToFile(instance_, openPath_).ok) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Save failed: " + openName_);
            }
            return;
        }
        dirty_ = false;
        if (context.notifications != nullptr) {
            context.notifications->Success("Saved " + openName_);
        }
    }

    /// Menandai graph berubah DAN mengompilasi ulang.
    ///
    /// Kompilasi di setiap suntingan, bukan atas permintaan: yang membuat editor
    /// node berguna adalah melihat akibat sebuah sambungan langsung, dan graph
    /// material berukuran wajar dikompilasi dalam waktu yang tidak terasa.
    void Touch() {
        dirty_ = true;
        Recompile();
    }

    void Recompile() {
        MaterialCompileOptions options;
        // Yang dikompilasi selalu graph INDUK, jadi yang disebut komentar kepala
        // juga harus induknya — bukan nama instance yang kebetulan sedang
        // dibuka. Kode yang menyebut berkas yang tidak memuatnya akan menyesatkan
        // tepat ketika seseorang menelusuri balik dari shader ke sumbernya.
        const std::string& source = instanceMode_ ? parentName_ : openName_;
        options.moduleName = source.empty() ? "material.simmat" : source;
        compiled_ = CompileMaterial(graph_, options);
        types_ = MaterialTypes::Infer(graph_);

        errorNodes_.clear();
        for (const MaterialIssue& issue : compiled_.errors) {
            if (issue.node.IsValid()) {
                errorNodes_.push_back(issue.node);
            }
        }
    }

    // --- pemetaan id --------------------------------------------------------

    static constexpr std::size_t kNoPin = static_cast<std::size_t>(-1);

    uint64_t IdOf(const Uuid& guid) {
        const auto it = ids_.find(guid);
        if (it != ids_.end()) {
            return it->second;
        }
        // Mulai dari 1: pustaka memakai nol sebagai "tidak ada".
        const uint64_t id = static_cast<uint64_t>(ids_.size()) + 1;
        ids_.emplace(guid, id);
        guids_.emplace(id, guid);
        return id;
    }

    const Uuid* GuidOf(uint64_t id) const {
        const auto it = guids_.find(id);
        return it == guids_.end() ? nullptr : &it->second;
    }

    MaterialNode* FindNode(const Uuid& guid) {
        const auto it = std::find_if(graph_.nodes.begin(), graph_.nodes.end(),
                                     [&guid](const MaterialNode& node) {
                                         return node.guid == guid;
                                     });
        return it == graph_.nodes.end() ? nullptr : &*it;
    }

    std::size_t PinIndexIn(const MaterialNode& node, std::string_view pin) {
        const std::vector<MaterialPin> pins = PinsOf(graph_, node);
        for (std::size_t i = 0; i < pins.size(); ++i) {
            if (pins[i].name == pin) {
                return i;
            }
        }
        return kNoPin;
    }

    /// Pin dikembalikan BY VALUE, bukan lewat pointer ke penyangga bersama —
    /// jebakan yang sudah kena sekali di Graph Editor: panggilan kedua menimpa
    /// isi yang ditunjuk hasil panggilan pertama.
    const MaterialNode* ResolvePin(uint64_t pinId, MaterialPin& outPin) {
        const Uuid* guid = GuidOf(NodeOfPin(pinId));
        const MaterialNode* node = guid != nullptr ? graph_.FindNode(*guid) : nullptr;
        if (node == nullptr) {
            return nullptr;
        }
        const std::vector<MaterialPin> pins = PinsOf(graph_, *node);
        const std::size_t index = PinIndexOf(pinId);
        if (index >= pins.size()) {
            return nullptr;
        }
        outPin = pins[index];
        return node;
    }

    NodeCanvas canvas_;

    MaterialGraph graph_;
    /// Terisi hanya di mode instance; `graph_` lalu berisi graph INDUKNYA.
    MaterialInstance instance_;
    bool instanceMode_ = false;
    std::string parentName_;
    MaterialTypes types_;
    MaterialCompileResult compiled_;
    std::vector<Uuid> errorNodes_;

    Uuid openGuid_;
    std::string openName_;
    std::filesystem::path openPath_;
    bool dirty_ = false;
    bool pendingFit_ = false;
    float sideWidth_ = 0.0f;
    Vec2 spawnPosition_{0.0f, 0.0f};
    std::string paletteFilter_;

    /// Grup yang ukurannya sudah dipasang ke kanvas, dan selisih ukur-balik
    /// masing-masing. Lihat DrawGroupNode.
    std::unordered_set<Uuid> sized_;
    std::unordered_map<Uuid, Vec2> groupInset_;
    Uuid renamingNode_;
    std::string renameBuffer_;
    bool focusRename_ = false;

    std::unordered_map<Uuid, uint64_t> ids_;
    std::unordered_map<uint64_t, Uuid> guids_;
    std::unordered_set<Uuid> placed_;
};

}  // namespace

SIM_REGISTER_PANEL(MaterialEditorPanel, 27)

}  // namespace sim::editor
