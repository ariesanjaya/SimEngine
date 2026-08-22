#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Core/Log.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/NodeGraph.h"
#include "Sim/Editor/Widgets.h"
#include "Sim/Editor/Notifications.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Material/MaterialCompiler.h"
#include "Sim/Material/MaterialGraph.h"
#include "Sim/Material/MaterialInstance.h"
#include "Sim/Material/MaterialNodeCatalog.h"
#include "Sim/Assets/TextureBakery.h"
#include "Sim/Material/MaterialParameterBlock.h"
#include "Sim/Material/MaterialShaderModule.h"
#include "Sim/Material/MaterialValidation.h"
#include "Sim/Material/ShaderCache.h"
#include "Sim/Render/IMaterialPreview.h"

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
/// Daftar override kosong, untuk material yang bukan instance. Sebuah static
/// alih-alih sementara: `Fill` menerima referensi, dan sementara yang dibuat di
/// argumen akan mati sebelum sempat dibaca kalau suatu saat pemanggilannya
/// dipecah.
const std::vector<ParameterOverride> kNoOverrides;

/// Melipat sudut ke (-pi, pi].
///
/// Yaw cahaya diseret, dan seretan tidak punya ujung. Menjepitnya alih-alih
/// melipat membuat cahaya berhenti di belakang objek — tepat pada sudut yang
/// paling dicari orang saat memeriksa rim light.
float WrapAngle(float radians) {
    const float wrapped = std::fmod(radians + kPi, kTwoPi);
    return wrapped < 0.0f ? wrapped + kPi : wrapped - kPi;
}

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

/// Warna pita kepala menurut kategori node.
///
/// **Kategori, bukan tipe node.** Sebuah graph material berisi puluhan jenis
/// node, dan warna per jenis hanya menghasilkan pelangi yang tidak menyatakan
/// apa pun. Kategori memisahkan yang benar-benar berbeda perannya: dari mana
/// data masuk, apa yang menghitungnya, dan ke mana ia keluar.
ImVec4 HeaderColorOf(std::string_view category) {
    if (category == "Output") {
        return ImVec4(0.62f, 0.24f, 0.24f, 1.0f);
    }
    if (category == "Input") {
        return ImVec4(0.24f, 0.44f, 0.62f, 1.0f);
    }
    if (category == "Math") {
        return ImVec4(0.28f, 0.52f, 0.36f, 1.0f);
    }
    return ImVec4(0.40f, 0.38f, 0.46f, 1.0f);
}

/// Jarak isi node ke tepinya. Dipakai dua kali — sebagai gaya kanvas, dan
/// sebagai tinggi tambahan pita kepala — jadi keduanya tidak boleh berbeda.
constexpr float kNodePadding = 7.0f;

/// Bentuk ikon pin menurut tipe nilainya.
widgets::PinShape ShapeOf(ValueKind kind) {
    // Tekstur adalah sesuatu yang dirujuk, bukan dihitung — dan itu perbedaan
    // yang paling sering salah disambung di graph material.
    return kind == ValueKind::Texture ? widgets::PinShape::Square : widgets::PinShape::Circle;
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
        // Gaya dipasang tiap frame bersama Initialize(): keduanya bekerja pada
        // konteks kanvas yang sedang aktif, dan panel node lain memasang
        // gayanya sendiri pada konteksnya sendiri.
        NodeCanvasStyle style;
        style.nodePadding = Vec4(kNodePadding, kNodePadding, kNodePadding, kNodePadding);
        canvas_.SetStyle(style);

        DrawToolbar(context);
        ImGui::Separator();

        // Lihat catatan yang sama di Particle dan Mesh Editor: penelusuran aset
        // milik Asset Browser, bukan milik tiap editor.
        if (!openGuid_.IsValid()) {
            ImGui::TextColored(kHintColor,
                               "Klik ganda sebuah material di Asset Browser, atau buat yang baru.");
        } else if (instanceMode_) {
            // Instance tidak punya graph sendiri, jadi tidak ada kanvas untuk
            // digambar. Menampilkan graph induknya di sini akan mengundang
            // suntingan yang tidak akan tersimpan ke mana-mana.
            DrawInstanceAndSide(context);
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
        // Sesudah `EndChild`, child itulah item terakhir — jadi di sinilah
        // sasaran jatuhnya dipasang, bukan di dalam `DrawCanvas`. Di dalam sana
        // item terakhirnya adalah node yang kebetulan digambar paling akhir.
        HandleTextureDrop(context);

        ImGui::SameLine(0.0f, 0.0f);
        DrawSideSplitter(handle);
        ImGui::SameLine(0.0f, 0.0f);

        if (ImGui::BeginChild("##side", ImVec2(0.0f, 0.0f))) {
            DrawSidePanel(context, /*withDetails=*/true);
        }
        ImGui::EndChild();
    }

    /// Instance: daftar parameter di kiri, hasil kompilasi induk di kanan.
    ///
    /// Hasil kompilasinya tetap ditampilkan, dan itu bukan sisa dari mode graph:
    /// yang ingin diketahui pemakai instance adalah parameter mana yang
    /// benar-benar dipakai shader — sesuatu yang hanya terjawab dengan melihat
    /// kodenya.
    void DrawInstanceAndSide(EditorContext& context) {
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
            // Tanpa tab Details: parameter yang diekspos milik graph induk, dan
            // menyuntingnya dari sebuah instance akan mengubah setiap instance
            // lain diam-diam.
            DrawSidePanel(context, /*withDetails=*/false);
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

        // Pin mana yang tersambung, dihitung sekali per frame. Menanyakannya
        // per pin berarti menelusuri seluruh daftar link untuk setiap pin di
        // kanvas, dan itu berlipat dua kali terhadap ukuran graph.
        connectedPins_.clear();
        for (const MaterialLink& link : graph_.links) {
            const MaterialNode* from = graph_.FindNode(link.fromNode);
            const MaterialNode* to = graph_.FindNode(link.toNode);
            if (from != nullptr) {
                const std::vector<MaterialPin> pins = PinsOf(graph_, *from);
                for (std::size_t i = 0; i < pins.size(); ++i) {
                    if (pins[i].name == link.fromPin) {
                        connectedPins_.insert(PinId(IdOf(from->guid), i));
                    }
                }
            }
            if (to != nullptr) {
                const std::vector<MaterialPin> pins = PinsOf(graph_, *to);
                for (std::size_t i = 0; i < pins.size(); ++i) {
                    if (pins[i].name == link.toPin) {
                        connectedPins_.insert(PinId(IdOf(to->guid), i));
                    }
                }
            }
        }

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

        // Tinggi pita kepala diukur di sini dan digambar sesudah EndNode():
        // lebar node baru diketahui setelah seluruh isinya ditata.
        const float headerTop = ImGui::GetCursorScreenPos().y;
        ImGui::TextUnformatted(type != nullptr ? type->label.c_str() : node.type.c_str());
        DrawNodeSubtitle(node);
        const float headerBottom = ImGui::GetCursorScreenPos().y;
        ImGui::Dummy(ImVec2(0.0f, 3.0f));

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

        // Pita kepala setinggi isinya ditambah padding node di atas dan bawah.
        canvas_.DrawNodeHeader(id, (headerBottom - headerTop) + kNodePadding * 2.0f,
                               ToVec4(HeaderColorOf(type != nullptr ? type->category : "")));

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

        // Bentuk dari tipe yang DIDEKLARASIKAN, warna dari tipe yang
        // DISIMPULKAN. Bentuk menyatakan apa yang boleh disambungkan ke sana
        // dan tidak berubah-ubah; warna menyatakan apa yang sebenarnya
        // mengalir, dan itu memang bergantung pada apa yang sudah tersambung.
        const widgets::PinShape shape = ShapeOf(pin.kind);
        const bool connected = connectedPins_.count(pinId) != 0;

        if (input) {
            canvas_.BeginInputPin(pinId);
            widgets::PinIcon(shape, connected, color);
            ImGui::SameLine();
            ImGui::TextUnformatted(label.c_str());
        } else {
            canvas_.BeginOutputPin(pinId);
            ImGui::TextUnformatted(label.c_str());
            ImGui::SameLine();
            widgets::PinIcon(shape, connected, color);
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

    // --- menjatuhkan tekstur ------------------------------------------------

    /// Menerima tekstur yang diseret dari Asset Browser dan membuat node untuknya.
    ///
    /// **Hanya tekstur.** Menjatuhkan mesh atau material di sini tidak punya
    /// arti yang jelas — dan yang lebih buruk daripada tidak melakukan apa-apa
    /// adalah melakukan sesuatu yang tidak diminta, jadi yang lain ditolak
    /// beserta sebabnya.
    void HandleTextureDrop(EditorContext& context) {
        if (!ImGui::BeginDragDropTarget()) {
            return;
        }
        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SIM_ASSET");
        if (payload == nullptr || payload->DataSize != sizeof(Uuid)) {
            ImGui::EndDragDropTarget();
            return;
        }
        const Uuid guid = *static_cast<const Uuid*>(payload->Data);
        // Posisinya diambil sebelum `EndDragDropTarget`, dari mouse — sama
        // dengan yang dipakai palet node, jadi node yang dijatuhkan mendarat di
        // tempat yang sama dengan node yang dibuat lewat menu.
        const ImVec2 mouse = ImGui::GetMousePos();
        ImGui::EndDragDropTarget();

        const assets::AssetRecord* record =
            context.assets != nullptr ? context.assets->Find(guid) : nullptr;
        if (record == nullptr) {
            return;
        }
        if (record->type != assets::AssetType::Texture) {
            context.notifications->Warning(std::string("Only textures can be dropped here; ") +
                                           record->name + " is a " +
                                           assets::ToString(record->type));
            return;
        }
        if (!openGuid_.IsValid() || instanceMode_) {
            return;
        }

        // **Namanya dihitung sebelum node-nya ada.** Menghitungnya sesudah
        // berarti node yang baru saja dimasukkan ikut dihitung sebagai pemakai
        // namanya sendiri, dan tekstur pertama yang dijatuhkan sudah langsung
        // bernama "Texture2".
        const std::string name = UniqueTextureName(TextureNodeName(record->name));

        // **Dua node, bukan satu — dan itu yang membuat hasilnya bisa dipakai.**
        // Keluaran `input.texture` bertipe `Texture`: ia sebuah binding, bukan
        // warna, dan satu-satunya yang menerimanya adalah masukan `texture`
        // milik `Sample Texture`. Menjatuhkan node tekstur sendirian karena itu
        // menghasilkan sesuatu yang tidak bisa disambungkan ke `baseColor` sama
        // sekali — yang tampak seperti kabelnya menolak, padahal yang kurang
        // adalah node di antaranya.
        //
        // Yang dijatuhkan orang berarti "saya ingin gambar ini terlihat", dan
        // pasangan inilah bentuk terpendek dari kalimat itu.
        MaterialNode texture;
        texture.guid = Uuid::Generate();
        texture.type = "input.texture";
        texture.position = Vec2(mouse.x, mouse.y);
        texture.settings["name"] = name;
        texture.settings["texture"] = guid.ToString();

        MaterialNode sample;
        sample.guid = Uuid::Generate();
        sample.type = "input.sample";
        // Di sebelah kanannya, sejarak lebar sebuah node. Menumpuk keduanya di
        // satu titik membuat yang di bawah tidak terlihat sama sekali, dan yang
        // pertama dilakukan orang adalah menyeretnya untuk mencari yang hilang.
        sample.position = Vec2(mouse.x + ImGui::GetFontSize() * 12.0f, mouse.y);

        MaterialLink link;
        link.guid = Uuid::Generate();
        link.fromNode = texture.guid;
        link.fromPin = "texture";
        link.toNode = sample.guid;
        link.toPin = "texture";

        const std::string sampleName = name;
        graph_.nodes.push_back(std::move(texture));
        graph_.nodes.push_back(std::move(sample));
        graph_.links.push_back(std::move(link));
        Touch();
        context.notifications->Info("Dropped " + sampleName +
                                    " — connect its RGB to Base Color");
    }

    /// Nama berkas menjadi nama node: tanpa ekstensi, dan tanpa yang kosong.
    static std::string TextureNodeName(const std::string& assetName) {
        const std::size_t dot = assetName.rfind('.');
        std::string stem = dot == std::string::npos ? assetName : assetName.substr(0, dot);
        return stem.empty() ? std::string("Texture") : stem;
    }

    /// Nama yang belum dipakai node tekstur lain di graph ini.
    ///
    /// **Wajib, bukan kerapian.** Nama node menjadi nama binding di kode yang
    /// dihasilkan (`tAlbedo`), jadi dua node bernama sama menghasilkan dua
    /// deklarasi bernama sama — dan yang muncul adalah galat slangc yang
    /// menyebut simbol, bukan node. Menjatuhkan tekstur yang sama dua kali
    /// adalah hal yang paling wajar dilakukan orang.
    std::string UniqueTextureName(const std::string& base) const {
        const auto taken = [this](const std::string& candidate) {
            for (const MaterialNode& node : graph_.nodes) {
                if (node.type != "input.texture") {
                    continue;
                }
                if (node.Setting("name") == candidate) {
                    return true;
                }
            }
            return false;
        };
        if (!taken(base)) {
            return base;
        }
        for (int suffix = 2; suffix < 1000; ++suffix) {
            const std::string candidate = base + std::to_string(suffix);
            if (!taken(candidate)) {
                return candidate;
            }
        }
        return base;
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

    /// Sisi kanan: tab di atas, preview **tetap** di bawah.
    ///
    /// Preview sengaja bukan tab. Sebagai tab ia hilang tepat ketika ia paling
    /// berguna — saat orang menyunting parameter di Details atau menelusuri
    /// galat di Compiled Slang — dan menyunting material tanpa melihat akibatnya
    /// adalah bolak-balik yang tidak perlu dilakukan siapa pun.
    void DrawSidePanel(EditorContext& context, bool withDetails) {
        const float available = ImGui::GetContentRegionAvail().y;
        const float previewHeight = PreviewHeight(available);
        // Tab mendapat sisa ruangnya, preview mendapat bagian bawah yang tetap.
        // Urutannya penting: child pertama harus tahu tingginya, dan itu hanya
        // bisa dihitung dari tinggi preview yang sudah ditentukan lebih dulu.
        if (ImGui::BeginChild("##sidetabs", ImVec2(0.0f, available - previewHeight))) {
            DrawSideTabs(withDetails);
        }
        ImGui::EndChild();

        ImGui::Separator();
        DrawPreview(context);
    }

    /// Tinggi area preview: persegi mengikuti lebar panel, tapi tidak boleh
    /// memakan lebih dari separuh tingginya — panel yang diseret pendek harus
    /// tetap menyisakan ruang untuk tab di atasnya.
    float PreviewHeight(float available) const {
        const float width = ImGui::GetContentRegionAvail().x;
        const float controls = ImGui::GetFrameHeightWithSpacing() * 2.0f;
        return std::clamp(width + controls, ImGui::GetFontSize() * 8.0f,
                          std::max(available * 0.5f, ImGui::GetFontSize() * 8.0f));
    }

    void DrawSideTabs(bool withDetails) {
        if (!ImGui::BeginTabBar("##side")) {
            return;
        }
        if (withDetails && ImGui::BeginTabItem("Details")) {
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
                TouchDeferred();
            }
            RecompileOnCommit();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
            if (DrawKindCombo("##kind", parameter.kind)) {
                Touch();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
            if (ImGui::InputText("##default", &parameter.defaultValue)) {
                TouchDeferred();
            }
            RecompileOnCommit();
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
                // Modelnya tetap diperbarui tiap ketukan — yang ditunda hanya
                // kompilasinya. Menahan modelnya juga berarti teks yang diketik
                // hilang begitu panel digambar ulang karena sebab lain.
                node->pinValues[pin.name] = value;
                TouchDeferred();
            }
            RecompileOnCommit();
            ImGui::PopID();
        }
    }

    void DrawSetting(MaterialNode& node, const char* key, const char* label) {
        std::string value = node.Setting(key);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        if (ImGui::InputText(label, &value)) {
            node.settings[key] = value;
            TouchDeferred();
        }
        RecompileOnCommit();
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


    // --- preview ------------------------------------------------------------

    /// Menggambar preview PBR sungguhan: shader yang sama persis dengan yang
    /// nanti dipakai renderer, dikompilasi lewat `slangc` dan di-cache.
    ///
    /// **Panel tidak mengevaluasi shading sendiri sedikit pun.** Itu aturan yang
    /// dipegang seluruh E7.1 — model shading hanya boleh punya satu implementasi
    /// — dan preview yang "kira-kira mirip" justru bentuk pelanggaran yang
    /// paling mahal: selisihnya tidak akan pernah dicari orang karena tidak ada
    /// yang mengaku salah.
    void DrawPreview(EditorContext& context) {
        // Keadaan pratinjau diterbitkan di **setiap** jalan keluar fungsi ini,
        // bukan hanya di jalur yang berhasil. `material.preview` menunggu
        // `ready`, dan keadaan yang tertinggal dari material sebelumnya membuat
        // ia memotret material yang salah tanpa satu pun tanda.
        EditorContext::DocumentPreviewState& published = context.materialPreviewState;
        published.asset = openGuid_;
        published.ready = false;
        published.rect = {};

        render::IMaterialPreview* preview = context.materialPreview;
        if (preview == nullptr) {
            published.status = "This device has no material preview.";
            ImGui::TextColored(kHintColor,
                               "Preview is unavailable on this device. Editing the graph "
                               "does not require it.");
            return;
        }
        if (!compiled_.ok) {
            published.status = "The graph does not compile; there is nothing to preview.";
            ImGui::TextColored(kErrorColor, "%s  Fix the errors first — nothing to preview.",
                               icons::kLogError);
            return;
        }

        EnsurePreviewShaders(context, *preview);
        if (!previewError_.empty()) {
            published.status = previewError_;
            ImGui::TextColored(kErrorColor, "%s  %s", icons::kLogError, previewError_.c_str());
            // Sumbernya tetap ditampilkan: galat slangc menyebut nomor baris,
            // dan nomor baris tanpa kodenya tidak bisa dipakai siapa pun.
            ImGui::TextColored(kHintColor, "See the Compiled Slang tab for the source.");
            return;
        }
        if (!preview->HasMaterial()) {
            published.status = "Still compiling.";
            ImGui::TextColored(kHintColor, "Compiling…");
            return;
        }

        DrawPreviewControls();
        UpdatePreviewTextures(context, *preview);
        UploadPreviewParameters(*preview);

        const float width = std::max(ImGui::GetContentRegionAvail().x, 32.0f);
        const float height = std::max(std::min(width, ImGui::GetContentRegionAvail().y), 32.0f);

        render::MaterialPreviewDesc desc;
        desc.width = static_cast<uint32_t>(width);
        desc.height = static_cast<uint32_t>(height);
        desc.shape = previewShape_;
        desc.cameraYaw = previewYaw_;
        desc.cameraPitch = previewPitch_;
        desc.distance = previewDistance_;
        const float cosPitch = std::cos(previewLightPitch_);
        desc.lightDirection = Vec3(std::sin(previewLightYaw_) * cosPitch,
                                   std::sin(previewLightPitch_),
                                   std::cos(previewLightYaw_) * cosPitch);
        desc.lightRadiance = Vec3(previewExposure_);
        desc.time = previewTime_;
        previewTime_ += context.deltaSeconds;
        preview->Render(desc);

        const render::TextureHandle texture = preview->ColorTarget();
        if (texture == render::kInvalidTexture) {
            return;
        }
        const Vec2 uv = preview->ColorTargetUvMax();

        // **Tombol tak terlihat dulu, gambarnya menyusul.** `ImGui::Image` bukan
        // item yang bisa diaktifkan — ia tidak pernah menjadi `IsItemActive()`,
        // jadi seret di atasnya tidak pernah terbaca. Ini hanya ketahuan dengan
        // menjalankannya: kodenya terbaca benar, slider di sebelahnya bekerja,
        // dan yang gagal diam-diam justru satu-satunya interaksi di sini.
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 size(width, height);
        // Relatif terhadap titik asal viewport utama, satuan logis — bentuk yang
        // sama dengan `viewportRect`, karena yang memotong keduanya sama.
        if (const ImGuiViewport* main = ImGui::GetMainViewport()) {
            published.rect.position = Vec2(origin.x - main->Pos.x, origin.y - main->Pos.y);
            published.rect.size = Vec2(size.x, size.y);
            published.rect.mainSize = Vec2(main->Size.x, main->Size.y);
            published.ready = true;
            published.status.clear();
        }
        ImGui::InvisibleButton("##view", size,
                               ImGuiButtonFlags_MouseButtonLeft |
                                   ImGuiButtonFlags_MouseButtonRight);
        const bool active = ImGui::IsItemActive();
        const bool hovered = ImGui::IsItemHovered();
        ImGui::GetWindowDrawList()->AddImage(
            static_cast<ImTextureID>(texture), origin,
            ImVec2(origin.x + size.x, origin.y + size.y), ImVec2(0.0f, 0.0f),
            ImVec2(uv.x, uv.y));

        if (hovered) {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                previewDistance_ = std::clamp(previewDistance_ - wheel * 0.25f, 1.6f, 12.0f);
            }
        }

        // **Kiri menggerakkan cahaya, kanan menggerakkan kamera.** Keduanya
        // dipisah karena keduanya menjawab pertanyaan yang berbeda: memindahkan
        // cahaya memperlihatkan bentuk sorotan sebuah material, sedangkan
        // mengorbit memperlihatkan bagaimana rupanya berubah terhadap sudut
        // pandang — dan untuk material anisotropik atau ber-coat, yang kedua
        // itulah yang tidak bisa disimpulkan dari yang pertama.
        //
        // Objeknya sendiri tidak pernah diputar. Memutar objek terlihat sama
        // dengan mengorbit kamera, tapi ia menggeser bingkai tangent bersamanya
        // — jadi arah anisotropi ikut berputar dan justru menyembunyikan hal
        // yang sedang diperiksa.
        if (active) {
            const ImVec2 drag = ImGui::GetIO().MouseDelta;
            if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                previewYaw_ -= drag.x * 0.01f;
                // Dibatasi sedikit di bawah kutub: tepat di kutub arah "atas"
                // dan arah pandang sejajar, dan LookAt menghasilkan matriks yang
                // tidak terdefinisi — preview berkedip hitam alih-alih memberi
                // pesan.
                previewPitch_ = std::clamp(previewPitch_ + drag.y * 0.01f, -1.5f, 1.5f);
            } else {
                // Cahaya mengikuti kursor, bukan berlawanan dengannya. Kedua
                // tandanya sempat terbalik — seret ke kiri memindahkan sorotan
                // ke kanan — dan itu bukan kesalahan yang bisa dilihat dari
                // kode: yang menentukan arahnya adalah letak kamera, bukan
                // rumus di baris ini.
                //
                // Berbeda dengan orbit di atas, yang memang berlawanan: di sana
                // yang diseret adalah objeknya, jadi kamera bergerak ke arah
                // sebaliknya.
                previewLightYaw_ = WrapAngle(previewLightYaw_ + drag.x * 0.01f);
                previewLightPitch_ =
                    std::clamp(previewLightPitch_ - drag.y * 0.01f, -1.45f, 1.45f);
            }
        }
    }

    /// Dua baris, bukan satu.
    ///
    /// Panel samping ini bisa diseret sempit, dan beberapa kontrol berlabel
    /// dalam satu baris membuat label terakhir terpotong tepat pada lebar yang
    /// dipakai orang.
    void DrawPreviewControls() {
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
        const char* shapes[] = {"Sphere", "Cube", "Plane"};
        int shape = static_cast<int>(previewShape_);
        if (ImGui::Combo("##shape", &shape, shapes, IM_ARRAYSIZE(shapes))) {
            previewShape_ = static_cast<render::PreviewShape>(shape);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset")) {
            ResetPreviewView();
        }
        ImGui::SameLine();
        // Pembagian tombol mouse ditulis di tempat, bukan disembunyikan di
        // tooltip: kontrol yang harus ditemukan lebih dulu dengan menebak
        // adalah kontrol yang sebagian orang tidak akan pernah temukan.
        ImGui::TextColored(kHintColor, "drag: light · right-drag: orbit");

        ImGui::SetNextItemWidth(std::max(
            ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Exposure").x - spacing * 2.0f,
            ImGui::GetFontSize() * 4.0f));
        ImGui::SliderFloat("Exposure", &previewExposure_, 0.5f, 8.0f, "%.1f");
    }

    void ResetPreviewView() {
        previewYaw_ = 0.0f;
        previewPitch_ = 0.2f;
        previewDistance_ = 3.2f;
        previewLightYaw_ = 0.7f;
        previewLightPitch_ = 0.6f;
    }

    /// Mengompilasi ulang shader preview hanya ketika sumbernya benar-benar
    /// berubah.
    ///
    /// Dibandingkan terhadap teks Slang-nya, bukan terhadap `dirty_`: memindahkan
    /// sebuah node menandai dokumen kotor tanpa mengubah satu karakter pun kode
    /// yang dihasilkan, dan membangun ulang pipeline karenanya akan membuat
    /// menyeret node terasa tersendat.
    void EnsurePreviewShaders(EditorContext& context, render::IMaterialPreview& preview) {
        if (compiled_.slang == previewSource_ && (preview.HasMaterial() || !previewError_.empty())) {
            return;
        }
        previewSource_ = compiled_.slang;
        previewError_.clear();

        if (!cacheReady_) {
            cacheReady_ = true;
            const std::string identity = material::SlangCompilerIdentity();
            if (identity.empty()) {
                cacheUsable_ = false;
            } else {
                cache_.Configure(context.shaderCacheDir, identity);
                cache_.SetCompiler(material::MakeSlangCompiler());
                prelude_ = material::LoadOpenPbrPrelude(context.shaderDir);
                cacheUsable_ = !prelude_.empty();
                if (!cacheUsable_) {
                    SIM_WARN("Editor", "openpbr.slang not found in {}", context.shaderDir);
                }
            }
        }
        if (!cacheUsable_) {
            previewError_ = prelude_.empty() && !cache_.Statistics().compiles
                                ? "Shader toolchain unavailable (slangc or openpbr.slang missing)."
                                : "Shader toolchain unavailable.";
            preview.ClearMaterial();
            return;
        }

        material::MaterialModuleOptions options;
        options.prelude = prelude_;

        // Lapisan yang tidak mungkin dipakai dimatikan sebelum `slangc`
        // melihatnya. Tanpa baris ini, preview mengompilasi model shading utuh
        // untuk material yang coat dan fuzz-nya nol — dan yang diukur panel
        // menjadi biaya yang tidak pernah dibayar saat menggambar.
        options.lobes = compiled_.lobes;
        const material::CompileOutput vertex = cache_.Get(
            material::MakeMaterialRequest(compiled_.slang, material::ShaderStage::Vertex, options));
        const material::CompileOutput fragment =
            cache_.Get(material::MakeMaterialRequest(compiled_.slang,
                                                     material::ShaderStage::Fragment, options));
        if (!vertex.ok || !fragment.ok) {
            previewError_ = vertex.ok ? fragment.error : vertex.error;
            if (previewError_.empty()) {
                previewError_ = "slangc failed without a message.";
            }
            preview.ClearMaterial();
            return;
        }

        block_.Build(graph_.parameters);
        // Jalur tekstur pratinjau ikut dilupakan: modul yang baru punya daftar
        // slot yang lain, dan yang lama akan menunjuk slot yang sudah bergeser.
        previewTexturePaths_.clear();
        render::MaterialPreviewShaders shaders;
        shaders.vertexSpirv = vertex.spirv;
        shaders.fragmentSpirv = fragment.spirv;
        shaders.parameterBytes = block_.Bytes();
        shaders.textureCount = static_cast<uint32_t>(compiled_.textures.size());
        if (!preview.SetMaterial(shaders)) {
            previewError_ = preview.LastError();
        }
    }

    /// Memasang gambar tiap slot tekstur pratinjau.
    ///
    /// **Sebelum ini ada, pratinjau selalu memakai putih 1×1 untuk setiap
    /// slot** — ia menyatakan jumlah teksturnya dan tidak pernah menerima satu
    /// pun gambar, jadi material bertekstur tampak persis seperti material
    /// polos. Itu yang membuat tekstur yang dijatuhkan ke kanvas "tidak keluar".
    ///
    /// Yang diserahkan jalur `.ktx2` hasil bake, bukan berkas sumbernya: aturan
    /// yang sama dengan jalur tekstur viewport, dan alasan yang sama — yang
    /// mendekode gambar adalah baker, bukan yang menggambar.
    void UpdatePreviewTextures(EditorContext& context, render::IMaterialPreview& preview) {
        std::vector<std::string> paths;
        paths.reserve(compiled_.textures.size());
        for (const material::TextureBinding& binding : compiled_.textures) {
            Uuid image = binding.texture;
            // Slot yang diisi parameter diambil dari instance-nya; yang tidak,
            // dari node-nya sendiri.
            if (instanceMode_ && !binding.parameter.empty()) {
                const Uuid overridden = instance_.Texture(binding.parameter);
                if (overridden.IsValid()) {
                    image = overridden;
                }
            }
            std::string path;
            if (image.IsValid() && context.assets != nullptr &&
                context.textureBakery != nullptr) {
                if (const assets::AssetRecord* record = context.assets->Find(image)) {
                    const assets::BakedTextureRef baked =
                        context.textureBakery->Request(context.assets->AbsolutePath(*record));
                    if (baked.state == assets::BakeState::Ready) {
                        path = baked.path.string();
                    }
                }
            }
            paths.push_back(std::move(path));
        }
        // Dibandingkan, bukan dipasang tiap frame: memasangnya berarti menunggu
        // device diam dan mengunggah ulang setiap gambar, enam puluh kali per
        // detik. Yang masih di-bake menjadi jalur kosong hari ini dan jalur
        // sungguhan beberapa frame lagi — dan perbandingan inilah yang membuat
        // pergantian itu terjadi tepat sekali.
        if (paths == previewTexturePaths_) {
            return;
        }
        previewTexturePaths_ = std::move(paths);
        preview.SetTextures(previewTexturePaths_);
    }

    /// Menulis blok uniform dari nilai bawaan parameter, ditimpa override
    /// instance bila yang dibuka memang sebuah instance.
    ///
    /// Inilah yang membuat preview instance memperlihatkan nilai instance-nya,
    /// bukan nilai induknya — dan keduanya memakai pipeline yang sama, karena
    /// yang berbeda antara induk dan instance memang hanya isi blok uniformnya.
    void UploadPreviewParameters(render::IMaterialPreview& preview) {
        block_.Fill(graph_.parameters, instanceMode_ ? instance_.overrides : kNoOverrides,
                    parameterBytes_);
        preview.SetParameters(parameterBytes_);
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

    bool OpenAsset(const Uuid& guid, EditorContext& context) override {
        if (context.assets == nullptr) {
            return false;
        }
        const assets::AssetRecord* record = context.assets->Find(guid);
        if (record == nullptr || record->type != assets::AssetType::Material) {
            return false;
        }
        // `.simmat` dan `.simmatinst` sama-sama bertipe Material tapi dibuka
        // lewat jalur yang berbeda: yang kedua tidak punya graph sendiri, ia
        // hanya menimpa parameter induknya.
        if (record->relativePath.size() > 12 &&
            record->relativePath.rfind(".simmatinst") == record->relativePath.size() - 11) {
            OpenInstance(context, guid, *record, context.assets->AbsolutePath(*record));
        } else {
            Open(context, guid);
        }
        return true;
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
    ///
    /// **Untuk suntingan yang selesai dalam satu tindakan** — menyambung kabel,
    /// menambah node, memilih dari combo. Yang diketik huruf demi huruf memakai
    /// pasangan `TouchDeferred`/`RecompileOnCommit` di bawah.
    void Touch() {
        dirty_ = true;
        Recompile();
    }

    /// Menandai berubah **tanpa** mengompilasi ulang.
    ///
    /// Dipakai kotak teks: mengetik "0.75" adalah empat ketukan, dan tiga di
    /// antaranya adalah nilai yang tidak pernah dimaksudkan siapa pun — "0",
    /// "0.", "0.7". Mengompilasi ketiganya berarti menjalankan `slangc` tiga
    /// kali untuk hasil yang langsung dibuang, dan pada graph besar itu terasa
    /// sebagai editor yang tersendat justru saat sedang diketik.
    ///
    /// Penanda "belum tersimpan" tetap dinaikkan tiap ketukan: modelnya memang
    /// sudah berubah, dan tombol Save yang baru menyala setelah pindah fokus
    /// adalah tombol yang berbohong.
    void TouchDeferred() { dirty_ = true; }

    /// Mengompilasi ulang bila widget yang baru saja digambar **selesai**
    /// disunting.
    ///
    /// `IsItemDeactivatedAfterEdit` menjawab tepat pertanyaan itu: ia benar pada
    /// frame ketika fokus berpindah — klik ke tempat lain, Tab, atau Enter — dan
    /// hanya bila isinya memang berubah. Memakai `EnterReturnsTrue` saja tidak
    /// cukup: yang mengetik lalu mengklik kanvas tidak pernah menekan Enter, dan
    /// suntingannya tidak akan pernah terlihat.
    ///
    /// Ia juga menutup seretan slider: satu seretan adalah puluhan frame yang
    /// nilainya berubah, dan yang dikompilasi hanya yang terakhir.
    void RecompileOnCommit() {
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            Recompile();
        }
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
    /// Pin yang tersambung pada frame ini. Diisi ulang tiap frame sebelum node
    /// digambar — ikon pin yang terisi menyatakan tersambung.
    std::unordered_set<uint64_t> connectedPins_;

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
    /// Jalur `.ktx2` yang sedang terpasang di pratinjau, satu per slot.
    std::vector<std::string> previewTexturePaths_;
    std::string paletteFilter_;

    /// Grup yang ukurannya sudah dipasang ke kanvas, dan selisih ukur-balik
    /// masing-masing. Lihat DrawGroupNode.
    std::unordered_set<Uuid> sized_;
    std::unordered_map<Uuid, Vec2> groupInset_;
    Uuid renamingNode_;
    std::string renameBuffer_;
    bool focusRename_ = false;

    // --- preview ---
    material::ShaderCache cache_;
    std::string prelude_;
    bool cacheReady_ = false;
    bool cacheUsable_ = false;
    /// Teks Slang yang shader preview-nya sedang terpasang. Perbandingan
    /// terhadap ini yang memutuskan perlu-tidaknya kompilasi ulang.
    std::string previewSource_;
    std::string previewError_;
    material::MaterialParameterBlock block_;
    std::vector<uint8_t> parameterBytes_;
    render::PreviewShape previewShape_ = render::PreviewShape::Sphere;
    float previewYaw_ = 0.0f;
    float previewPitch_ = 0.2f;
    float previewDistance_ = 3.2f;
    float previewLightYaw_ = 0.7f;
    float previewLightPitch_ = 0.6f;
    float previewExposure_ = 3.0f;
    float previewTime_ = 0.0f;

    std::unordered_map<Uuid, uint64_t> ids_;
    std::unordered_map<uint64_t, Uuid> guids_;
    std::unordered_set<Uuid> placed_;
};

}  // namespace

SIM_REGISTER_PANEL(MaterialEditorPanel, 27)

}  // namespace sim::editor
