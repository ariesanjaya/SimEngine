#include "Sim/Editor/DockLayout.h"

#include "Sim/Core/Log.h"
#include "Sim/Editor/PanelIds.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <string>

namespace sim::editor {
namespace {

/// ImHashStr me-reset hash-nya saat menemukan "###", jadi "###id" menghasilkan
/// ImGuiID yang sama dengan "Judul Apa Pun###id". Berkat itu layout tidak perlu


/// tahu judul panel — hanya id-nya, yang memang tidak pernah berubah.
void Dock(const char* panelId, ImGuiID node) {
    const std::string key = std::string("###") + panelId;
    ImGui::DockBuilderDockWindow(key.c_str(), node);
}

/// Editor yang membuka sebuah aset menempel di node yang sama dengan Viewport.
///
/// **Sebagai tab di sebelah viewport, bukan panel tersendiri di pinggir.** Yang
/// dibuka editor ini adalah dokumen — satu mesh, satu material, satu klip — dan
/// dokumen menempati ruang utama, bergantian dengan adegan yang sedang disusun.
/// Menaruhnya di kolom sempit berarti pratinjau 3D-nya selebar daftar berkas.
void DockDocumentEditors(ImGuiID center) {
    Dock(panel_id::kMeshEditor, center);
    Dock(panel_id::kMaterialEditor, center);
    Dock(panel_id::kAnimationEditor, center);
    Dock(panel_id::kParticleEditor, center);
}

ImGuiID ResetRoot(ImGuiID dockspaceId) {
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);
    return dockspaceId;
}

void BuildLevelLayout(ImGuiID dockspaceId) {
    ImGuiID center = ResetRoot(dockspaceId);

    // Rasio diukur terhadap sisa ruang pada saat pembelahan, bukan terhadap
    // jendela penuh — karena itu urutan pembelahannya penting dan angkanya
    // tidak sama dengan proporsi akhir yang terlihat.
    //
    // Console dipisah lebih dulu dari akar supaya ia melebar penuh di dasar
    // jendela, seperti pada tata letak acuan. Kalau dipisah setelah kolom kiri
    // dan kanan, ia hanya akan selebar area tengah.
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.24f, nullptr, &center);
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25f, nullptr, &center);
    ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.55f, nullptr, &left);

    Dock(panel_id::kOutliner, left);
    Dock(panel_id::kAssetBrowser, leftBottom);
    Dock(panel_id::kInspector, right);
    Dock(panel_id::kConsole, bottom);
    Dock(panel_id::kLuaConsole, bottom);
    Dock(panel_id::kStatistics, bottom);
    Dock(panel_id::kHistory, right);
    Dock(panel_id::kPreferences, right);
    Dock(panel_id::kViewport, center);
    DockDocumentEditors(center);

    ImGui::DockBuilderFinish(dockspaceId);
}

/// Mengarang aset: viewport diperbesar, properti tetap di kanan, dan daftar
/// aset naik jadi kolom penuh karena di sinilah bahan bakunya dipilih.
void BuildAuthoringLayout(ImGuiID dockspaceId) {
    ImGuiID center = ResetRoot(dockspaceId);

    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.18f, nullptr, &center);

    Dock(panel_id::kAssetBrowser, left);
    Dock(panel_id::kOutliner, left);
    Dock(panel_id::kInspector, right);
    Dock(panel_id::kHistory, right);
    Dock(panel_id::kConsole, bottom);
    Dock(panel_id::kLuaConsole, bottom);
    Dock(panel_id::kViewport, center);
    DockDocumentEditors(center);

    ImGui::DockBuilderFinish(dockspaceId);
}

/// Menelusuri masalah: Console mendapat separuh layar, Statistics dan History
/// berdampingan dengannya, viewport dikecilkan tapi tetap terlihat.
void BuildDebugLayout(ImGuiID dockspaceId) {
    ImGuiID center = ResetRoot(dockspaceId);

    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.5f, nullptr, &center);
    ImGuiID bottomRight =
        ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.3f, nullptr, &bottom);
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.22f, nullptr, &center);

    Dock(panel_id::kOutliner, left);
    Dock(panel_id::kAssetBrowser, left);
    Dock(panel_id::kConsole, bottom);
    Dock(panel_id::kLuaConsole, bottom);
    Dock(panel_id::kStatistics, bottomRight);
    Dock(panel_id::kHistory, bottomRight);
    Dock(panel_id::kPreferences, bottomRight);
    Dock(panel_id::kInspector, center);
    Dock(panel_id::kViewport, center);
    DockDocumentEditors(center);

    ImGui::DockBuilderFinish(dockspaceId);
}

}  // namespace

const char* WorkspaceName(Workspace workspace) {
    switch (workspace) {
        case Workspace::Level: return "Level";
        case Workspace::Authoring: return "Authoring";
        case Workspace::Debug: return "Debug";
    }
    return "Level";
}

void BuildLayout(ImGuiID dockspaceId, Workspace workspace) {
    switch (workspace) {
        case Workspace::Authoring: BuildAuthoringLayout(dockspaceId); break;
        case Workspace::Debug: BuildDebugLayout(dockspaceId); break;
        case Workspace::Level:
        default: BuildLevelLayout(dockspaceId); break;
    }
    SIM_DEBUG_LOG("Editor", "Dock layout rebuilt for workspace '{}'", WorkspaceName(workspace));
}

}  // namespace sim::editor
