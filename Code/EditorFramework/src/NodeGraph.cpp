#include "Sim/Editor/NodeGraph.h"

#include <algorithm>

#include <imgui.h>
#include <imgui_node_editor.h>

#include <vector>

namespace sim::editor {
namespace {

namespace ed = ax::NodeEditor;

ImVec2 ToImGui(const Vec2& v) {
    return ImVec2(v.x, v.y);
}

ImVec4 ToImGui(const Vec4& v) {
    return ImVec4(v.x, v.y, v.z, v.w);
}

}  // namespace

struct NodeCanvas::Impl {
    ed::EditorContext* context = nullptr;
    /// Konteks yang aktif sebelum Begin(), dikembalikan di End().
    ed::EditorContext* previous = nullptr;
};

namespace {

/// Menjadikan sebuah kanvas "yang sedang aktif" selama satu panggilan.
///
/// imgui-node-editor menyimpan konteks aktif di satu variabel global, dan
/// beberapa fungsinya — NavigateToContent, GetNodePosition — melakukan
/// dereferensi tanpa memeriksanya. Memanggilnya di luar Begin()/End() karena itu
/// bukan menghasilkan hasil yang salah melainkan segfault.
///
/// Membebankan aturan itu ke pemanggil berarti setiap panel harus mengingat
/// sesuatu yang tidak terlihat di antarmuka ini. Guard ini yang menanggungnya,
/// sekaligus mengembalikan konteks sebelumnya — supaya dua kanvas yang bersarang
/// (kelak: graph material di dalam panel yang juga punya kanvas) tidak saling
/// mencuri.
class ScopedEditor {
public:
    explicit ScopedEditor(ed::EditorContext* context) : previous_(ed::GetCurrentEditor()) {
        ed::SetCurrentEditor(context);
    }
    ~ScopedEditor() { ed::SetCurrentEditor(previous_); }

    ScopedEditor(const ScopedEditor&) = delete;
    ScopedEditor& operator=(const ScopedEditor&) = delete;

private:
    ed::EditorContext* previous_ = nullptr;
};

}  // namespace

NodeCanvas::NodeCanvas() : impl_(std::make_unique<Impl>()) {}

NodeCanvas::~NodeCanvas() {
    Shutdown();
}

void NodeCanvas::Initialize() {
    if (impl_->context != nullptr) {
        return;
    }
    ed::Config config;
    // SettingsFile null: posisi node adalah bagian dari berkas `.simgraph` dan
    // ikut dibagikan bersama graph-nya. Membiarkan pustaka menyimpannya sendiri
    // akan membuat graph yang sama tampak berbeda di dua mesin, dan perubahan
    // tata letak tidak muncul di diff.
    config.SettingsFile = nullptr;
    config.EnableSmoothZoom = true;
    // Geser kanvas dengan tombol TENGAH, sama seperti pan di viewport 3D.
    // Bawaan pustaka adalah tombol kanan, dan dua panel yang memakai tombol
    // berbeda untuk gerakan yang sama adalah hal yang harus diingat pengguna
    // setiap kali ia berpindah panel.
    //
    // Tombol kanan lalu berarti satu hal saja di sini — membuka palet node.
    // Sebelumnya ia berarti dua hal tergantung apakah tangan pengguna bergeser
    // beberapa piksel, dan yang seperti itu sulit dilakukan dengan sengaja.
    config.NavigateButtonIndex = 2;
    impl_->context = ed::CreateEditor(&config);
}

void NodeCanvas::Shutdown() {
    if (impl_->context == nullptr) {
        return;
    }
    ed::DestroyEditor(impl_->context);
    impl_->context = nullptr;
}

void NodeCanvas::Begin(const char* id, const Vec2& size) {
    impl_->previous = ed::GetCurrentEditor();
    ed::SetCurrentEditor(impl_->context);
    ed::Begin(id, ToImGui(size));
}

void NodeCanvas::End() {
    ed::End();
    ed::SetCurrentEditor(impl_->previous);
    impl_->previous = nullptr;
}

void NodeCanvas::BeginNode(uint64_t id) {
    ed::BeginNode(ed::NodeId(id));
}

void NodeCanvas::EndNode() {
    ed::EndNode();
}

void NodeCanvas::BeginGroupNode(uint64_t id, const Vec4& color) {
    // Latar dan bingkainya dibuat tembus pandang. Pustaka menggambar grup
    // sebelum node biasa, jadi ia memang berada di belakang — tapi latar yang
    // pekat tetap membuat kanvas terbaca sebagai "kotak besar berisi kotak
    // kecil" alih-alih sebagai penanda wilayah.
    ImVec4 fill = ToImGui(color);
    fill.w *= 0.25f;
    ed::PushStyleColor(ed::StyleColor_NodeBg, fill);
    ed::PushStyleColor(ed::StyleColor_NodeBorder, ToImGui(color));
    ed::BeginNode(ed::NodeId(id));
}

void NodeCanvas::GroupArea(const Vec2& size) {
    ed::Group(ToImGui(size));
}

void NodeCanvas::EndGroupNode() {
    ed::EndNode();
    ed::PopStyleColor(2);
}

Vec2 NodeCanvas::GetNodeSize(uint64_t id) const {
    const ScopedEditor scoped(impl_->context);
    const ImVec2 size = ed::GetNodeSize(ed::NodeId(id));
    return Vec2(size.x, size.y);
}

void NodeCanvas::SetGroupSize(uint64_t id, const Vec2& size) {
    const ScopedEditor scoped(impl_->context);
    ed::SetGroupSize(ed::NodeId(id), ToImGui(size));
}

namespace {

/// Titik tambat kabel, sebagai pecahan dari kotak pin: (0,0) kiri-atas,
/// (1,1) kanan-bawah.
ImVec2 PivotOf(NodeCanvas::PinSide side) {
    switch (side) {
        case NodeCanvas::PinSide::Left: return ImVec2(0.0f, 0.5f);
        case NodeCanvas::PinSide::Right: return ImVec2(1.0f, 0.5f);
        case NodeCanvas::PinSide::Top: return ImVec2(0.5f, 0.0f);
        case NodeCanvas::PinSide::Bottom: return ImVec2(0.5f, 1.0f);
    }
    return ImVec2(0.0f, 0.5f);
}

/// Berapa jauh kabel keluar lurus sebelum melengkung, sebagai ukuran kotak
/// tambatnya. Nol berarti kabel berangkat tepat dari titik tambatnya.
void PushPivot(NodeCanvas::PinSide side) {
    ed::PushStyleVar(ed::StyleVar_PivotAlignment, PivotOf(side));
    ed::PushStyleVar(ed::StyleVar_PivotSize, ImVec2(0.0f, 0.0f));
}

}  // namespace

void NodeCanvas::BeginInputPin(uint64_t id, PinSide side) {
    PushPivot(side);
    ed::BeginPin(ed::PinId(id), ed::PinKind::Input);
}

void NodeCanvas::BeginOutputPin(uint64_t id, PinSide side) {
    PushPivot(side);
    ed::BeginPin(ed::PinId(id), ed::PinKind::Output);
}

void NodeCanvas::EndPin() {
    ed::EndPin();
    // Dilepas SESUDAH EndPin(): pustaka membaca pivotnya saat pin ditutup, dan
    // yang melepasnya lebih dulu menambatkan setiap kabel di kiri.
    ed::PopStyleVar(2);
}

void NodeCanvas::Link(uint64_t id, uint64_t fromPin, uint64_t toPin, const Vec4& color,
                      float thickness) {
    ed::Link(ed::LinkId(id), ed::PinId(fromPin), ed::PinId(toPin), ToImGui(color), thickness);
}

bool NodeCanvas::BeginCreate() {
    return ed::BeginCreate();
}

bool NodeCanvas::QueryNewLink(uint64_t& fromPin, uint64_t& toPin) {
    ed::PinId from;
    ed::PinId to;
    if (!ed::QueryNewLink(&from, &to)) {
        return false;
    }
    fromPin = from.Get();
    toPin = to.Get();
    // Keduanya baru terisi setelah pengguna menahan koneksinya di atas pin
    // tujuan. Sebelum itu `to` masih nol, dan pemanggil tidak punya cukup
    // informasi untuk memutuskan apa pun.
    return fromPin != 0 && toPin != 0;
}

bool NodeCanvas::AcceptLink() {
    return ed::AcceptNewItem();
}

void NodeCanvas::RejectLink() {
    // Merah dan lebih tebal: pengguna melihat penolakannya selagi masih menahan
    // mouse, bukan setelah melepas dan mendapati tidak ada yang terjadi.
    ed::RejectNewItem(ImVec4(0.94f, 0.35f, 0.32f, 1.0f), 2.0f);
}

void NodeCanvas::EndCreate() {
    ed::EndCreate();
}

bool NodeCanvas::BeginDelete() {
    return ed::BeginDelete();
}

bool NodeCanvas::QueryDeletedLink(uint64_t& id) {
    ed::LinkId link;
    if (!ed::QueryDeletedLink(&link)) {
        return false;
    }
    id = link.Get();
    return true;
}

bool NodeCanvas::QueryDeletedNode(uint64_t& id) {
    ed::NodeId node;
    if (!ed::QueryDeletedNode(&node)) {
        return false;
    }
    id = node.Get();
    return true;
}

bool NodeCanvas::AcceptDeletion() {
    return ed::AcceptDeletedItem();
}

void NodeCanvas::RejectDeletion() {
    ed::RejectDeletedItem();
}

void NodeCanvas::EndDelete() {
    ed::EndDelete();
}

void NodeCanvas::SetNodePosition(uint64_t id, const Vec2& position) {
    const ScopedEditor scoped(impl_->context);
    ed::SetNodePosition(ed::NodeId(id), ToImGui(position));
}

Vec2 NodeCanvas::GetNodePosition(uint64_t id) const {
    const ScopedEditor scoped(impl_->context);
    const ImVec2 position = ed::GetNodePosition(ed::NodeId(id));
    return Vec2(position.x, position.y);
}

std::vector<uint64_t> NodeCanvas::SelectedNodes() const {
    const ScopedEditor scoped(impl_->context);
    const int count = ed::GetSelectedObjectCount();
    if (count <= 0) {
        return {};
    }
    std::vector<ed::NodeId> raw(static_cast<std::size_t>(count));
    const int found = ed::GetSelectedNodes(raw.data(), count);
    std::vector<uint64_t> result;
    result.reserve(static_cast<std::size_t>(found));
    for (int i = 0; i < found; ++i) {
        result.push_back(raw[static_cast<std::size_t>(i)].Get());
    }
    return result;
}

std::vector<uint64_t> NodeCanvas::SelectedLinks() const {
    const ScopedEditor scoped(impl_->context);
    const int count = ed::GetSelectedObjectCount();
    if (count <= 0) {
        return {};
    }
    std::vector<ed::LinkId> raw(static_cast<std::size_t>(count));
    const int found = ed::GetSelectedLinks(raw.data(), count);
    std::vector<uint64_t> result;
    result.reserve(static_cast<std::size_t>(found));
    for (int i = 0; i < found; ++i) {
        result.push_back(raw[static_cast<std::size_t>(i)].Get());
    }
    return result;
}

void NodeCanvas::Select(uint64_t nodeId, bool append) {
    const ScopedEditor scoped(impl_->context);
    ed::SelectNode(ed::NodeId(nodeId), append);
}

void NodeCanvas::ClearSelection() {
    const ScopedEditor scoped(impl_->context);
    ed::ClearSelection();
}

void NodeCanvas::FitToContent() {
    const ScopedEditor scoped(impl_->context);
    ed::NavigateToContent();
}

void NodeCanvas::CenterOnNode(uint64_t id) {
    const ScopedEditor scoped(impl_->context);
    // Yang digeser adalah PANDANGAN, bukan node-nya.
    //
    // Pustaka juga punya CenterNodeOnScreen(), dan namanya menjanjikan persis
    // hal ini — tapi ia MEMINDAHKAN node ke tengah layar, bukan membawa layar
    // ke node. Memakainya untuk "fokus" mengubah tata letak graph dan menandai
    // berkasnya kotor, padahal pengguna hanya ingin melihat.
    //
    // Wajib dipanggil di luar Begin()/End(): perubahan seleksi baru berlaku
    // setelah kanvas ditutup, jadi menavigasi di dalamnya akan menuju seleksi
    // yang LAMA.
    ed::SelectNode(ed::NodeId(id), false);
    ed::NavigateToSelection();
}

uint64_t NodeCanvas::DoubleClickedLink() const {
    const ScopedEditor scoped(impl_->context);
    return ed::GetDoubleClickedLink().Get();
}

uint64_t NodeCanvas::DoubleClickedNode() const {
    const ScopedEditor scoped(impl_->context);
    return ed::GetDoubleClickedNode().Get();
}

bool NodeCanvas::RequestedBackgroundMenu() {
    return ed::ShowBackgroundContextMenu();
}

void NodeCanvas::Suspend() {
    ed::Suspend();
}

void NodeCanvas::Resume() {
    ed::Resume();
}

void NodeCanvas::DrawNodeBackground(uint64_t id, const Vec4& color, float thickness) {
    const ScopedEditor scoped(impl_->context);
    ImDrawList* draw = ed::GetNodeBackgroundDrawList(ed::NodeId(id));
    if (draw == nullptr) {
        return;
    }
    const ImVec2 min = ed::GetNodePosition(ed::NodeId(id));
    const ImVec2 size = ed::GetNodeSize(ed::NodeId(id));
    draw->AddRect(ImVec2(min.x - 1.0f, min.y - 1.0f),
                  ImVec2(min.x + size.x + 1.0f, min.y + size.y + 1.0f),
                  ImGui::GetColorU32(ToImGui(color)), ed::GetStyle().NodeRounding, 0, thickness);
}

void NodeCanvas::DrawNodeHeader(uint64_t id, float height, const Vec4& color) {
    const ScopedEditor scoped(impl_->context);
    ImDrawList* draw = ed::GetNodeBackgroundDrawList(ed::NodeId(id));
    if (draw == nullptr || height <= 0.0f) {
        return;
    }
    const ImVec2 min = ed::GetNodePosition(ed::NodeId(id));
    const ImVec2 size = ed::GetNodeSize(ed::NodeId(id));
    if (size.x <= 0.0f) {
        return;
    }
    const float rounding = ed::GetStyle().NodeRounding;
    const float border = ed::GetStyle().NodeBorderWidth;
    // Ditarik masuk selebar garis batasnya supaya pita tidak menutupi tepi
    // node — yang tertutup akan terlihat sebagai node tanpa batas di bagian
    // atas saja.
    const ImVec2 headerMin(min.x + border, min.y + border);
    const ImVec2 headerMax(min.x + size.x - border, min.y + std::min(height, size.y));

    const ImVec4 top = ToImGui(color);
    const ImVec4 bottom(top.x * 0.55f, top.y * 0.55f, top.z * 0.55f, top.w);

    // **Dua bagian, dan pembagiannya tepat di garis membulatnya.** Sudut
    // membulat hanya bisa digambar `AddRectFilled`, yang satu warna; gradasi
    // hanya bisa digambar `AddRectFilledMultiColor`, yang tidak bisa membulat.
    // Membagi di garis itu membuat keduanya bertemu tanpa celah dan tanpa warna
    // yang menyembul di luar sudutnya.
    const float split = std::min(headerMin.y + rounding, headerMax.y);
    draw->AddRectFilled(headerMin, ImVec2(headerMax.x, split), ImGui::GetColorU32(top), rounding,
                        ImDrawFlags_RoundCornersTop);
    if (split < headerMax.y) {
        draw->AddRectFilledMultiColor(ImVec2(headerMin.x, split), headerMax,
                                      ImGui::GetColorU32(top), ImGui::GetColorU32(top),
                                      ImGui::GetColorU32(bottom), ImGui::GetColorU32(bottom));
    }
    // Garis pemisah tipis di kaki pita: gradasi sendirian membuat batasnya
    // kabur tepat ketika node berlatar terang.
    draw->AddLine(ImVec2(headerMin.x, headerMax.y), ImVec2(headerMax.x, headerMax.y),
                  ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.35f)), 1.0f);
}

void NodeCanvas::PushNodeStyle(const Vec4& background, const Vec4& border, const Vec4& padding,
                               float rounding) {
    const ScopedEditor scoped(impl_->context);
    ed::PushStyleColor(ed::StyleColor_NodeBg, ToImGui(background));
    ed::PushStyleColor(ed::StyleColor_NodeBorder, ToImGui(border));
    ed::PushStyleVar(ed::StyleVar_NodePadding,
                     ImVec4(padding.x, padding.y, padding.z, padding.w));
    ed::PushStyleVar(ed::StyleVar_NodeRounding, rounding);
}

void NodeCanvas::PopNodeStyle() {
    const ScopedEditor scoped(impl_->context);
    ed::PopStyleVar(2);
    ed::PopStyleColor(2);
}

void NodeCanvas::PushLinkDirection(const Vec2& source, const Vec2& target, float strength) {
    const ScopedEditor scoped(impl_->context);
    ed::PushStyleVar(ed::StyleVar_SourceDirection, ImVec2(source.x, source.y));
    ed::PushStyleVar(ed::StyleVar_TargetDirection, ImVec2(target.x, target.y));
    ed::PushStyleVar(ed::StyleVar_LinkStrength, strength);
}

void NodeCanvas::PopLinkDirection() {
    const ScopedEditor scoped(impl_->context);
    ed::PopStyleVar(3);
}

void NodeCanvas::SetPinRect(const Vec2& min, const Vec2& max) {
    ed::PinPivotRect(ImVec2(min.x, min.y), ImVec2(max.x, max.y));
    ed::PinRect(ImVec2(min.x, min.y), ImVec2(max.x, max.y));
}

void NodeCanvas::DrawNodeBand(uint64_t id, float top, float bottom, const Vec4& color,
                              float rounding, float inset) {
    const ScopedEditor scoped(impl_->context);
    ImDrawList* draw = ed::GetNodeBackgroundDrawList(ed::NodeId(id));
    if (draw == nullptr || bottom <= top) {
        return;
    }
    const ImVec2 min = ed::GetNodePosition(ed::NodeId(id));
    const ImVec2 size = ed::GetNodeSize(ed::NodeId(id));
    if (size.x <= inset * 2.0f) {
        return;
    }
    draw->AddRectFilled(ImVec2(min.x + inset, top), ImVec2(min.x + size.x - inset, bottom),
                        ImGui::GetColorU32(ToImGui(color)), rounding);
}

void NodeCanvas::SetStyle(const NodeCanvasStyle& style) {
    if (impl_->context == nullptr) {
        return;
    }
    const ScopedEditor scoped(impl_->context);
    ed::Style& target = ed::GetStyle();
    target.NodeRounding = style.nodeRounding;
    target.NodeBorderWidth = style.nodeBorderWidth;
    target.NodePadding = ImVec4(style.nodePadding.x, style.nodePadding.y, style.nodePadding.z,
                                style.nodePadding.w);
    target.PinRounding = style.pinRounding;
    target.LinkStrength = style.linkStrength;
    target.Colors[ed::StyleColor_NodeBg] = ToImGui(style.nodeBackground);
    target.Colors[ed::StyleColor_NodeBorder] = ToImGui(style.nodeBorder);
}

}  // namespace sim::editor
