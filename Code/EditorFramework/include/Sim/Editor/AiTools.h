#pragma once

namespace sim::ai {
class ToolRegistry;
}

namespace sim::editor {

class EditorApp;

/// Mendaftarkan tool MCP yang dilayani editor.
///
/// **Arahnya satu, dan itu disengaja.** EditorFramework melihat AIBridge;
/// AIBridge tidak tahu apa-apa tentang editor. Itulah yang membuat modul yang
/// sama nanti bisa dipakai `SimHeadless`, yang mendaftarkan himpunan tool yang
/// berbeda karena tidak punya panel maupun viewport.
///
/// `app` harus hidup lebih lama daripada `tools`: handler-nya memegangnya lewat
/// referensi, karena tool yang menyalin keadaan editor akan menjawab agen dengan
/// keadaan pada saat pendaftaran, bukan pada saat ditanya.
void RegisterEditorTools(ai::ToolRegistry& tools, EditorApp& app);

}  // namespace sim::editor
