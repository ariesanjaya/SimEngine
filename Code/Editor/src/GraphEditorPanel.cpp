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
                PanelCategory::Authoring) {}

    void OnDraw(EditorContext& context) override {
        if (context.assets == nullptr || context.scripts == nullptr) {
            ImGui::TextDisabled("No asset database.");
            return;
        }
        canvas_.Initialize();

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
            const float sideWidth = ImGui::GetContentRegionAvail().x * 0.36f;
            if (ImGui::BeginChild("##canvas", ImVec2(-sideWidth, 0.0f))) {
                DrawCanvas(context);
            }
            ImGui::EndChild();
            ImGui::SameLine();
            if (ImGui::BeginChild("##side", ImVec2(0.0f, 0.0f))) {
                DrawSidePanel(context);
            }
            ImGui::EndChild();
        } else {
            ImGui::TextColored(kHintColor, "Pick a graph on the left, or create one.");
        }
    }

private:
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
        // bertabrakan dengan Save Level.
        if (dirty_ && IsFocused() && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
            Save(context);
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

    void DrawNode(const GraphNode& node) {
        const NodeType* type = NodeCatalog::Get().Find(node.type);
        const uint64_t id = IdOf(node.guid);

        // Posisi berpindah dua arah: dari berkas ke kanvas saat graph dibuka,
        // dan dari kanvas ke berkas saat pengguna menyeret node. Yang menentukan
        // arahnya adalah `placed_` — tanpa itu, menyeret node akan langsung
        // ditimpa balik oleh posisi yang tersimpan di berkas.
        if (!placed_.contains(node.guid)) {
            canvas_.SetNodePosition(id, node.position);
            placed_.insert(node.guid);
        }

        canvas_.BeginNode(id);
        if (node.type == "comment") {
            ImGui::TextColored(kHintColor, "%s", node.Setting("text", "Comment").c_str());
            canvas_.EndNode();
            return;
        }

        ImGui::TextUnformatted(type != nullptr ? type->label.c_str() : node.type.c_str());
        ImGui::Dummy(ImVec2(0.0f, 2.0f));

        const std::vector<GraphPin> pins = PinsOf(graph_, node);
        std::vector<std::size_t> inputs;
        std::vector<std::size_t> outputs;
        for (std::size_t i = 0; i < pins.size(); ++i) {
            (pins[i].direction == PinDirection::Input ? inputs : outputs).push_back(i);
        }

        // Dua kolom: masukan di kiri, keluaran di kanan — bentuk yang dikenali
        // siapa pun yang pernah memakai graph editor lain.
        if (ImGui::BeginTable("pins", 2, ImGuiTableFlags_SizingStretchProp)) {
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
        const std::vector<GraphPin> fromPins = PinsOf(graph_, *from);
        const std::vector<GraphPin> toPins = PinsOf(graph_, *to);
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
        while (canvas_.QueryNewLink(fromPinId, toPinId)) {
            const GraphPin* fromPin = nullptr;
            const GraphPin* toPin = nullptr;
            const GraphNode* fromNode = ResolvePin(fromPinId, fromPin);
            const GraphNode* toNode = ResolvePin(toPinId, toPin);
            if (fromNode == nullptr || toNode == nullptr) {
                canvas_.RejectLink();
                continue;
            }
            // Pengguna boleh menyeret dari arah mana saja; yang disimpan selalu
            // keluaran → masukan.
            if (fromPin->direction == PinDirection::Input) {
                std::swap(fromNode, toNode);
                std::swap(fromPin, toPin);
            }
            if (fromPin->direction != PinDirection::Output ||
                toPin->direction != PinDirection::Input || fromNode == toNode ||
                !PinAccepts(toPin->kind, fromPin->kind)) {
                // Ditolak sebelum terbentuk, bukan dilaporkan setelahnya:
                // koneksi yang tidak masuk akal tidak pernah sempat ada di
                // berkas, jadi tidak ada keadaan rusak yang harus dibereskan.
                canvas_.RejectLink();
                continue;
            }
            if (!canvas_.AcceptLink()) {
                continue;
            }
            // Pin masukan hanya boleh punya satu sumber: yang lama diputus.
            const std::string toPinName = toPin->name;
            const Uuid toGuid = toNode->guid;
            const Uuid fromGuid = fromNode->guid;
            const std::string fromPinName = fromPin->name;
            RemoveLinkInto(toGuid, toPinName);

            GraphLink link;
            link.guid = Uuid::Generate();
            link.fromNode = fromGuid;
            link.fromPin = fromPinName;
            link.toNode = toGuid;
            link.toPin = toPinName;
            graph_.links.push_back(std::move(link));
            Touch();
        }
        canvas_.EndCreate();
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
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::IsWindowHovered()) {
            paletteFilter_.clear();
            ImGui::OpenPopup("##palette");
        }
        DrawPalette(context);
        canvas_.Resume();
    }

    void DrawPalette(EditorContext& /*context*/) {
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

    void DrawSidePanel(EditorContext& context) {
        if (!ImGui::BeginTabBar("##side")) {
            return;
        }
        if (ImGui::BeginTabItem("Compiled Lua")) {
            DrawCompiledLua();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Details")) {
            DrawDetails(context);
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
        DrawVariables();
        ImGui::Separator();
        DrawSelectedNode(context);
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
        // Posisi node dibaca balik dari kanvas tepat sebelum menyimpan. Membaca
        // setiap frame akan menandai graph kotor hanya karena pustaka membulatkan
        // koordinat, dan tombol Save tidak pernah padam.
        for (GraphNode& node : graph_.nodes) {
            node.position = canvas_.GetNodePosition(IdOf(node.guid));
        }
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

    const GraphNode* ResolvePin(uint64_t pinId, const GraphPin*& outPin) {
        const Uuid* guid = GuidOf(NodeOfPin(pinId));
        if (guid == nullptr) {
            return nullptr;
        }
        const GraphNode* node = graph_.FindNode(*guid);
        if (node == nullptr) {
            return nullptr;
        }
        pinScratch_ = PinsOf(graph_, *node);
        const std::size_t index = PinIndexOf(pinId);
        if (index >= pinScratch_.size()) {
            return nullptr;
        }
        outPin = &pinScratch_[index];
        return node;
    }

    NodeCanvas canvas_;
    Graph graph_;
    CompileResult compiled_;

    Uuid openGuid_;
    std::string openName_;
    std::filesystem::path openPath_;
    bool dirty_ = false;
    bool pendingFit_ = false;
    float nextSpawn_ = 40.0f;

    std::vector<Uuid> errorNodes_;
    /// Node yang disorot karena kesalahan RUNTIME, bukan kompilasi.
    std::vector<Uuid> runtimeNodes_;
    std::string runtimeMessage_;
    std::vector<Uuid> breakpoints_;

    std::unordered_map<Uuid, uint64_t> ids_;
    std::unordered_map<uint64_t, Uuid> guids_;
    std::unordered_set<Uuid> placed_;
    uint64_t nextId_ = 1;

    /// Penyangga pin sementara; dipegang supaya pointer yang dikembalikan
    /// ResolvePin tetap sah sampai pemanggilnya selesai.
    std::vector<GraphPin> pinScratch_;

    std::string paletteFilter_;
};

#else

class GraphEditorPanel final : public Panel {
public:
    GraphEditorPanel()
        : Panel(panel_id::kGraphEditor, std::string(icons::kNodeGraph) + "  Graph Editor",
                PanelCategory::Authoring) {}

    void OnDraw(EditorContext& /*context*/) override {
        ImGui::TextDisabled("This build was configured without Lua.");
    }
};

#endif  // SIM_WITH_LUA

}  // namespace

SIM_REGISTER_PANEL(GraphEditorPanel, 26)

}  // namespace sim::editor
