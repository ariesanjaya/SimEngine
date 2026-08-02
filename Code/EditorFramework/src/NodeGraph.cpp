#include "Sim/Editor/NodeGraph.h"

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

void NodeCanvas::BeginInputPin(uint64_t id) {
    ed::BeginPin(ed::PinId(id), ed::PinKind::Input);
}

void NodeCanvas::BeginOutputPin(uint64_t id) {
    ed::BeginPin(ed::PinId(id), ed::PinKind::Output);
}

void NodeCanvas::EndPin() {
    ed::EndPin();
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
    ed::SelectNode(ed::NodeId(id), false);
    ed::NavigateToSelection();
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

}  // namespace sim::editor
