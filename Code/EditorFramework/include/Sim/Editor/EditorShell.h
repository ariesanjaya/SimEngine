#pragma once

#include "Sim/Editor/DockLayout.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/PanelManager.h"

namespace sim::editor {

/// Kerangka jendela utama: menu bar, toolbar, dockspace, status bar.
///
/// Shell tidak tahu apa pun tentang panel tertentu — ia hanya menyediakan
/// tempat dan menyerahkan penggambaran ke PanelManager. Itu sebabnya menambah
/// panel baru tidak menyentuh berkas ini sama sekali.
class EditorShell {
public:
    void Draw(EditorContext& context, PanelManager& panels);

    /// Meminta layout dock disusun ulang pada frame berikutnya.
    void RequestResetLayout() { resetLayoutRequested_ = true; }
    void SetWorkspace(Workspace workspace);
    Workspace CurrentWorkspace() const { return workspace_; }

private:
    void DrawMenuBar(EditorContext& context, PanelManager& panels);
    void DrawToolbar(EditorContext& context);
    void DrawStatusBar(EditorContext& context);

    Workspace workspace_ = Workspace::Level;
    bool resetLayoutRequested_ = false;
    bool layoutInitialized_ = false;
    bool showImGuiDemo_ = false;
    bool showStyleEditor_ = false;
};

}  // namespace sim::editor
