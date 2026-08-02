#pragma once

#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Core/TaskPool.h"
#include "Sim/Render/ThumbnailCache.h"
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

        // Dimiliki pemanggil, bukan EditorApp. Keduanya perlu dibuat di tempat
        // yang boleh melihat RHI, dan EditorFramework bukan tempat itu.
        TaskPool* tasks = nullptr;
        render::IThumbnailCache* thumbnails = nullptr;
        script::ScriptRuntime* scripts = nullptr;
    };

    bool Initialize(const Config& config);
    void Shutdown();

    /// Menggambar seluruh UI editor untuk satu frame. Dipanggil di antara
    /// ImGui NewFrame dan Render.
    void DrawFrame(float deltaSeconds);

    bool WantsExit() const { return wantsExit_; }

    /// Meminta editor berhenti. Bila ada perubahan yang belum disimpan, sebuah
    /// dialog muncul lebih dulu dan WantsExit() tetap false sampai pengguna
    /// memilih. Inilah satu-satunya jalur keluar, termasuk untuk tombol tutup
    /// jendela — kalau ada jalur lain, akan ada cara kehilangan pekerjaan.
    void RequestExit() { exitRequested_ = true; }

    /// Judul jendela, ikut menandai perubahan yang belum disimpan.
    std::string WindowTitle() const;

    void SetFrameLock(float hz, std::string reason);

    scene::World& GetWorld() { return world_; }

    /// Membuat level contoh saat editor dibuka tanpa berkas apa pun.
    /// Menjalankan skrip. Keadaan scene dicuplik lebih dulu supaya Stop bisa
    /// mengembalikannya persis seperti sebelum Play — tanpa itu, mencoba sesuatu
    /// berarti kehilangan penataan level yang sedang dikerjakan.
    void Play();
    void Stop();
    bool IsPlaying() const { return playing_; }

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

    /// Memuat ulang skrip yang berkasnya berubah sejak frame lalu.
    void ReloadChangedScripts();

    void InstallCrashHandler();
    void CreateEntityAction();
    void DeleteSelectionAction();
    void DuplicateSelectionAction();
    void PasteAction(bool asChild);

    /// GUID entity terpilih yang leluhurnya tidak ikut terpilih.
    std::vector<Uuid> SelectedRoots() const;

    // Urutan deklarasi menentukan urutan penghancuran (terbalik). panels_
    // dideklarasikan sebelum history_ supaya history — yang command-nya bisa
    // menunjuk data milik panel — dihancurkan lebih dulu.
    PanelManager panels_;
    scene::World world_;
    assets::AssetDatabase assets_;
    CommandHistory history_;
    Selection selection_;
    ActionRegistry actions_;
    Notifications notifications_;
    EditorShell shell_;
    EditorContext context_;

    /// Sub-pohon hasil Copy, dalam format teks level. Disimpan di sini dan
    /// bukan di papan klip sistem: papan klip sistem berisi teks, dan menempel
    /// level ke dalam editor teks lain (atau sebaliknya) bukan alur yang
    /// diinginkan siapa pun.
    std::vector<std::string> clipboard_;

    enum class Dialog { None, SaveAs, Open };

    void DrawLevelDialogs();
    void DrawExitPrompt();
    void UpdateAutosave(float deltaSeconds);

    std::string saveAsName_;
    Dialog pendingDialog_ = Dialog::None;
    bool focusSaveAsField_ = false;

    std::filesystem::path configDir_;
    std::filesystem::path levelPath_;
    /// Cuplikan level sesaat sebelum Play, dipakai Stop untuk mengembalikannya.
    std::string playSnapshot_;
    bool playing_ = false;
    float autosaveTimer_ = 0.0f;
    bool exitRequested_ = false;
    bool wantsExit_ = false;
    bool initialized_ = false;
};

}  // namespace sim::editor
