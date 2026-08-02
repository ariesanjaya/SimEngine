#pragma once

#include "Sim/Editor/Panel.h"

#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

namespace sim::editor {

/// Pemilik seluruh panel: menggambarnya, mengisi menu Window, dan menyimpan
/// keadaan buka/tutupnya.
class PanelManager {
public:
    Panel* Add(std::unique_ptr<Panel> panel);
    Panel* Find(std::string_view id);

    void Update(EditorContext& context);
    void Draw(EditorContext& context);

    /// Isi menu Window. Dipanggil dari dalam BeginMenu("Window").
    void DrawWindowMenu();

    /// Menyimpan dan memulihkan keadaan buka/tutup panel.
    ///
    /// ImGui menyimpan posisi dan ukuran jendela di imgui.ini, tapi tidak
    /// keadaan buka/tutupnya — itu milik kita (p_open). Tanpa ini, panel yang
    /// sengaja ditutup pengguna akan muncul lagi setiap editor dijalankan.
    bool SaveState(const std::filesystem::path& path) const;
    bool LoadState(const std::filesystem::path& path);

    const std::vector<std::unique_ptr<Panel>>& Panels() const { return panels_; }

private:
    /// Posisi yang diinginkan sebuah viewport panel, dipakai untuk menegakkan
    /// kembali penempatan saat window manager mengabaikannya.
    struct ViewportPlacement {
        float wantedX = 0.0f;
        float wantedY = 0.0f;
        int framesRemaining = 0;
    };

    void EnforceViewportPlacement();

    std::vector<std::unique_ptr<Panel>> panels_;
    /// Kunci: ImGuiID viewport. Entri dihapus begitu penempatannya stabil.
    std::unordered_map<unsigned int, ViewportPlacement> placements_;
};

}  // namespace sim::editor
