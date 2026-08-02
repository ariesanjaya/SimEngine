#pragma once

#include "Sim/Editor/Actions.h"
#include "Sim/Editor/Command.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/EditorShell.h"
#include "Sim/Editor/Notifications.h"
#include "Sim/Editor/PanelManager.h"
#include "Sim/Editor/Selection.h"
#include "Sim/Scene/World.h"

#include <filesystem>

namespace sim::editor {

/// Siklus hidup editor: memiliki layanan bersama, mendaftarkan aksi inti, dan
/// menggambar satu frame UI.
///
/// Sengaja TIDAK memiliki loop utama, jendela, atau device Vulkan. Loop tetap
/// di Apps/SimEditor karena di sanalah platform dan RHI boleh dilihat; kalau
/// dipindah ke sini, EditorFramework harus ikut melihat keduanya dan aturan
/// modul di docs/ARCHITECTURE.md runtuh.
class EditorApp {
public:
    struct Config {
        /// Tempat pintasan, layout, dan log disimpan.
        std::filesystem::path configDir;
        render::IViewportRenderer* viewportRenderer = nullptr;
        const FrameLimiter* frameLimiter = nullptr;
        float lockedFps = 60.0f;
        std::string frameLockReason;
    };

    bool Initialize(const Config& config);
    void Shutdown();

    /// Menggambar seluruh UI editor untuk satu frame. Dipanggil di antara
    /// ImGui NewFrame dan Render.
    void DrawFrame(float deltaSeconds);

    bool WantsExit() const { return wantsExit_; }
    void RequestExit() { wantsExit_ = true; }

    /// Judul jendela, ikut menandai perubahan yang belum disimpan.
    std::string WindowTitle() const;

    void SetFrameLock(float hz, std::string reason);

    scene::World& GetWorld() { return world_; }

    /// Membuat level contoh saat editor dibuka tanpa berkas apa pun.
    void CreateStarterLevel();
    bool SaveLevel(const std::filesystem::path& path);
    bool LoadLevel(const std::filesystem::path& path);

    CommandHistory& History() { return history_; }
    Selection& GetSelection() { return selection_; }
    ActionRegistry& Actions() { return actions_; }
    Notifications& GetNotifications() { return notifications_; }
    PanelManager& Panels() { return panels_; }
    EditorContext& Context() { return context_; }

private:
    void RegisterCoreActions();
    void InstallCrashHandler();
    void CreateEntityAction();
    void DeleteSelectionAction();

    // Urutan deklarasi menentukan urutan penghancuran (terbalik). panels_
    // dideklarasikan sebelum history_ supaya history — yang command-nya bisa
    // menunjuk data milik panel — dihancurkan lebih dulu.
    PanelManager panels_;
    scene::World world_;
    CommandHistory history_;
    Selection selection_;
    ActionRegistry actions_;
    Notifications notifications_;
    EditorShell shell_;
    EditorContext context_;

    std::filesystem::path configDir_;
    std::filesystem::path levelPath_;
    bool wantsExit_ = false;
    bool initialized_ = false;
};

}  // namespace sim::editor
