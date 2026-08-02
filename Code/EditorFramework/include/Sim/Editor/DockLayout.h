#pragma once

#include <imgui.h>

namespace sim::editor {

/// Susunan panel bawaan untuk sebuah jenis pekerjaan.
///
/// Workspace bukan sekadar kenyamanan: mengarang material, menyunting animasi,
/// dan menata level membutuhkan panel yang berbeda, dan memindahkannya manual
/// setiap kali berganti pekerjaan adalah pemborosan yang cepat terasa.
enum class Workspace {
    Level,
    Authoring,
    Debug,
};

const char* WorkspaceName(Workspace workspace);

/// Menyusun layout dock untuk sebuah workspace.
///
/// Meniru tata letak acuan (docs/EDITOR-PANELS.md) untuk Workspace::Level:
///   kiri-atas   Entity Outliner      tengah  Viewport
///   kiri-bawah  Asset Browser        kanan   Entity Inspector
///   bawah       Console
///
/// Menghapus isi node yang ada, jadi aman dipanggil untuk "View > Reset Layout".
void BuildLayout(ImGuiID dockspaceId, Workspace workspace);

}  // namespace sim::editor
