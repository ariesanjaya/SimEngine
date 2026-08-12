#pragma once

#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Core/TaskPool.h"
#include "Sim/Render/ThumbnailCache.h"
#include "Sim/Editor/Actions.h"
#include "Sim/Editor/Command.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/EditorScripting.h"
#include "Sim/Editor/EditorShell.h"
#include "Sim/Editor/Notifications.h"
#include "Sim/Editor/PanelManager.h"
#include "Sim/Editor/ProjectLibrary.h"
#include "Sim/Editor/Selection.h"
#include "Sim/Editor/SkinnedPreview.h"
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
        /// Tempat pintasan, layout, log, dan cache turunan disimpan. **Milik
        /// editor, bukan milik project**: yang di sini berlaku untuk seluruh
        /// project yang pernah dibuka orang ini.
        std::filesystem::path configDir;
        /// Tempat project baru dibuat secara bawaan. Boleh kosong — dialog
        /// project baru lalu meminta lokasinya sendiri.
        std::filesystem::path projectsRoot;
        /// Folder `Resources` di sebelah executable. Kosong berarti aset contoh
        /// tidak disemai — editor tetap berjalan, level contohnya saja yang
        /// kehilangan modelnya.
        std::filesystem::path resourceDir;
        /// Folder `Shaders` di sebelah executable. Preview material membacanya
        /// untuk mengambil `openpbr.slang`.
        std::filesystem::path shaderDir;
        render::IViewportRenderer* viewportRenderer = nullptr;
        render::IMaterialPreview* materialPreview = nullptr;
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

    // --- project --------------------------------------------------------------

    /// Project yang sedang dibuka. `root` kosong berarti belum ada.
    const scene::Project& CurrentProject() const { return project_; }
    bool HasProject() const { return !project_.root.empty(); }

    /// Membuka sebuah project dan menyiapkan seluruh yang bergantung padanya:
    /// indeks aset, folder level dan prefab, runtime skrip, lalu level awalnya.
    bool OpenProject(const std::filesystem::path& path);
    /// Membuat project baru di `parent/<nama>` lalu membukanya.
    bool CreateProject(const std::filesystem::path& parent, const std::string& name);
    /// Menutup project yang sedang dibuka dan kembali ke project manager.
    void CloseProject();

    ProjectLibrary& Projects() { return projects_; }

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

    /// Memuat ulang skrip yang berkasnya berubah sejak frame lalu — instance
    /// gameplay lewat ScriptRuntime, skrip editor lewat EditorScripting.
    void ReloadChangedScripts();

    /// Pemakai sebuah aset yang berada di luar indeks aset: scene yang sedang
    /// dibuka, dan berkas level di folder editor.
    std::vector<std::string> FindExternalAssetUsers(const Uuid& guid) const;

    void InstallCrashHandler();
    void DrawProjectManager();
    void PickFolder(const std::filesystem::path& start,
                    std::function<void(const std::filesystem::path&)> accept);
    /// Folder isi project yang sedang dibuka. Kosong bila belum ada project.
    std::filesystem::path AssetsDirectory() const;
    std::filesystem::path LevelsDirectory() const;
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
    /// Indeks isi bawaan editor, berakar di folder `Resources` di sebelah
    /// executable. **Dibuka sekali saat start dan tidak pernah ditutup**: ia
    /// tidak bergantung project mana pun, dan itulah seluruh gunanya — sebuah
    /// pustaka yang isinya sama pada setiap project yang dibuka.
    assets::AssetDatabase builtinAssets_;
    CommandHistory history_;
    Selection selection_;
    ActionRegistry actions_;
    Notifications notifications_;
    /// Pemutar klip animasi untuk mesh ber-rig di viewport.
    SkinnedPreview animation_;
    EditorShell shell_;
    EditorScripting scripting_;
    EditorContext context_;

    /// Sub-pohon hasil Copy, dalam format teks level. Disimpan di sini dan
    /// bukan di papan klip sistem: papan klip sistem berisi teks, dan menempel
    /// level ke dalam editor teks lain (atau sebaliknya) bukan alur yang
    /// diinginkan siapa pun.
    std::vector<std::string> clipboard_;

    enum class Dialog { None, SaveAs, Open };

    void ApplyTimeOfDay(float deltaSeconds);
    void DrawLevelDialogs();
    void DrawExitPrompt();
    void UpdateAutosave(float deltaSeconds);

    std::string saveAsName_;
    Dialog pendingDialog_ = Dialog::None;
    bool focusSaveAsField_ = false;

    std::filesystem::path configDir_;
    std::filesystem::path projectsRoot_;
    std::filesystem::path resourceDir_;
    std::filesystem::path levelPath_;
    scene::Project project_;
    ProjectLibrary projects_;
    /// Isian dialog project baru. Disimpan di sini, bukan `static` di dalam
    /// fungsi gambarnya: yang `static` bertahan melewati penutupan dialog, dan
    /// dialog yang terbuka dengan sisa ketikan sesi sebelumnya adalah dialog
    /// yang membuat orang tidak sengaja membuat project bernama salah.
    std::string newProjectName_;
    std::string openProjectPath_;
    /// Lokasi tempat project baru dibuat. Dimulai dari `projectsRoot_`, lalu
    /// mengikuti apa pun yang terakhir dipilih lewat dialog.
    std::filesystem::path newProjectRoot_;
    /// Sebuah dialog sistem sedang terbuka. Tombolnya dimatikan selama itu.
    bool dialogOpen_ = false;
    std::string projectError_;
    /// Kolam tugas dan folder cache graph, disimpan karena keduanya dibutuhkan
    /// lagi setiap kali sebuah project dibuka — bukan hanya sekali saat start.
    TaskPool* tasks_ = nullptr;
    std::filesystem::path graphCacheDir_;
    /// Cuplikan level sesaat sebelum Play, dipakai Stop untuk mengembalikannya.
    std::string playSnapshot_;
    /// Seleksi sesaat sebelum Play, disimpan sebagai GUID.
    ///
    /// Bukan `SelectionId`: memulihkan cuplikan membangun ulang scene dari nol,
    /// jadi handle entity yang lama tidak menunjuk apa pun sesudahnya. GUID
    /// justru bertahan — ia yang tertulis di berkas level.
    std::vector<Uuid> playSelection_;
    bool playing_ = false;
    /// Play sedang tertahan sebuah breakpoint graph. Scene tetap tergambar,
    /// yang berhenti hanyalah OnUpdate.
    bool pausedAtBreakpoint_ = false;
    float autosaveTimer_ = 0.0f;
    bool exitRequested_ = false;
    bool wantsExit_ = false;
    bool scriptingReady_ = false;
    bool initialized_ = false;
};

}  // namespace sim::editor
