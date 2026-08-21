#pragma once

#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Core/TaskPool.h"
#include "Sim/Render/ThumbnailCache.h"
#include "Sim/Editor/Actions.h"
#include "Sim/Editor/Command.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Physics/PhysicsScene.h"
#include "Sim/Editor/EditorScripting.h"
#include "Sim/Editor/EditorShell.h"
#include "Sim/Editor/Notifications.h"
#include "Sim/Editor/PanelManager.h"
#include "Sim/Editor/ProjectLibrary.h"
#include "Sim/Editor/Selection.h"
#include "Sim/Editor/ToolApproval.h"
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
    EditorApp();
    /// Dideklarasikan dan didefinisikan di `.cpp`, bukan dibiarkan implisit:
    /// `textureBakery_` sebuah `unique_ptr` ke tipe yang hanya
    /// dideklarasi-maju di header ini, dan destructor implisit menuntut tipe
    /// lengkapnya di setiap TU yang menyertakan header ini.
    ~EditorApp();

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
        /// Perender kedua untuk Mesh Editor. Boleh null.
        render::IViewportRenderer* meshPreview = nullptr;
        const FrameLimiter* frameLimiter = nullptr;
        float lockedFps = 60.0f;
        std::string frameLockReason;

        // Dimiliki pemanggil, bukan EditorApp. Keduanya perlu dibuat di tempat
        // yang boleh melihat RHI, dan EditorFramework bukan tempat itu.
        TaskPool* tasks = nullptr;
        render::IThumbnailCache* thumbnails = nullptr;
        script::ScriptRuntime* scripts = nullptr;

        /// Tidak ada jendela, tidak ada ImGui, tidak ada panel.
        ///
        /// Yang dimatikannya adalah **skrip editor**: yang di folder itu
        /// menambah menu dan panel, dan keduanya memanggil ImGui yang tidak ada.
        /// Skrip gameplay tidak terpengaruh — ia berjalan lewat Play, dengan
        /// runtime yang sama.
        bool headless = false;
    };

    bool Initialize(const Config& config);
    void Shutdown();

    /// Menggambar seluruh UI editor untuk satu frame. Dipanggil di antara
    /// ImGui NewFrame dan Render.
    void DrawFrame(float deltaSeconds);

    /// Bagian frame yang tidak menggambar apa pun: pemindaian aset, skrip,
    /// animasi, fisika. `DrawFrame` memanggilnya sendiri.
    ///
    /// **Ada supaya SimHeadless bisa memutar editor tanpa jendela.** Tanpa
    /// pemisahan ini, satu-satunya cara memajukan keadaan editor adalah
    /// memanggil sesuatu yang menyentuh ImGui.
    void Tick(float deltaSeconds);

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

    /// Simulasi yang sedang berjalan. Kosong dan tidak valid di luar Play.
    ///
    /// Terbuka supaya panel bisa menampilkan jumlah benda dan sebab kegagalan,
    /// dan supaya uji bisa melangkahkannya tanpa melalui `DrawFrame` — yang
    /// menuntut konteks ImGui yang tidak ada di luar editor.
    physics::PhysicsScene& GetPhysics() { return physics_; }

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

    /// Folder isi project yang sedang dibuka. Kosong bila belum ada project.
    ///
    /// **Publik sejak A1.** Tool MCP menyusun jalur level dari sini, dan
    /// menyalin aturannya ke sana berarti dua tempat yang menghitung folder yang
    /// sama — yang akan berselisih pada project yang memakai tata letak sendiri,
    /// yaitu justru yang `Project::levelsPath` ada untuk mengizinkan.
    std::filesystem::path AssetsDirectory() const;
    std::filesystem::path LevelsDirectory() const;

    /// True selama layar pemilih level menutupi editor.
    ///
    /// **Terbaca dari luar supaya bisa diuji.** `LoadLevel` sempat tidak pernah
    /// menurunkan layar ini, jadi memuat level dari mana pun selain pemilihnya
    /// sendiri menukar isi dunia diam-diam sementara editor tetap menampilkan
    /// pemilih — dan tidak ada satu pun cara mengetahuinya dari luar.
    /// Gerbang persetujuan mode `ask`. Composition root memasangnya ke
    /// `McpServer::SetApprover`; `EditorApp` yang menggambar dialognya, karena
    /// dialog itu harus muncul walau panel AI Bridge tertutup.
    ToolApprovalGate& Approvals() { return approvals_; }

    bool IsAwaitingLevelChoice() const { return awaitingLevelChoice_; }

    /// Membuat berkas level kosong bernama `name` lalu membukanya.
    ///
    /// Publik dengan alasan yang sama dengan kedua folder di atas: `level.new`
    /// milik track AI memanggilnya, dan menyalin aturan pembuatannya ke sana
    /// berarti dua tempat yang harus sepakat soal bentuk level kosong.
    bool CreateLevelFile(const std::string& name);

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
    /// Layar pemilih level, digambar sesudah project dipilih dan sebelum shell.
    void DrawLevelPicker();
    /// Membuat level baru bernama `name`, langsung menuliskannya ke folder
    /// Levels. **Ditulis, bukan hanya dibangun di memori** — level yang hanya
    /// hidup di RAM sampai seseorang menekan Save adalah level yang hilang
    /// begitu editor ditutup.
    /// Menempatkan satu template prefab bawaan ke dalam dunia.
    ///
    /// Mengembalikan entity akarnya, atau `kNullEntity` bila templatenya tidak
    /// ada — level contoh yang kehilangan satu bagian lebih baik daripada
    /// editor yang menolak membuka project karena sebuah berkas Resources
    /// hilang.
    scene::Entity PlaceTemplate(const char* group, const char* name, scene::Entity parent,
                                const char* renameTo = nullptr);

    /// Mencatat level yang barusan dibuka ke berkas project.
    ///
    /// Dipakai layar pemilih sebagai sorotan "terakhir dibuka" — **bukan** untuk
    /// memuatnya otomatis di sesi berikutnya.
    void RememberStartupLevel(const std::filesystem::path& path);
    void PickFolder(const std::filesystem::path& start,
                    std::function<void(const std::filesystem::path&)> accept);
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
    /// Baker tekstur. **Dimiliki di sini, bukan di `main`**, karena berbeda dari
    /// ThumbnailCache ia tidak butuh device sama sekali — ia mendekode, memampat,
    /// dan menulis berkas, semuanya di CPU.
    std::unique_ptr<assets::TextureBakery> textureBakery_;
    /// Baker medan jarak mesh, dengan alasan yang sama: ia bekerja di CPU, di
    /// `TaskPool`, dan hasilnya diserahkan ke renderer alih-alih dihitung di
    /// dalamnya.
    std::unique_ptr<assets::MeshSdfBakery> meshSdfBakery_;
    /// Penjaga shader material, dengan alasan yang sama: ia bekerja di CPU dan
    /// di `TaskPool`, dan yang menyentuh GPU hanyalah langkah terakhirnya di
    /// main thread.
    std::unique_ptr<MaterialPrograms> materialPrograms_;
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

    /// True antara "project terpilih" dan "level terpilih".
    ///
    /// **Levelnya dipilih tiap kali editor dibuka**, bukan dimuat diam-diam dari
    /// `startupLevel`. Project yang berisi banyak level tidak punya satu level
    /// yang "benar", dan editor yang selalu membuka yang terakhir memaksa orang
    /// menutup lalu membuka lagi hanya untuk berpindah.
    bool headless_ = false;
    bool awaitingLevelChoice_ = false;
    /// Nama untuk level baru, dipegang layar pemilih.
    std::string newLevelName_;

    void ApplyTimeOfDay(float deltaSeconds);
    ToolApprovalGate approvals_;

    void DrawLevelDialogs();
    void DrawApprovalPrompt();

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
    /// Membangun simulasi saat Play, dan melaporkan sebabnya bila tidak bisa.
    void StartPhysics();

    std::string playSnapshot_;
    /// Seleksi sesaat sebelum Play, disimpan sebagai GUID.
    ///
    /// Bukan `SelectionId`: memulihkan cuplikan membangun ulang scene dari nol,
    /// jadi handle entity yang lama tidak menunjuk apa pun sesudahnya. GUID
    /// justru bertahan — ia yang tertulis di berkas level.
    std::vector<Uuid> playSelection_;
    /// Simulasi yang berjalan selama Play, dibangun ulang tiap kali ditekan.
    ///
    /// Selalu ada sebagai anggota, juga di build tanpa PhysX: yang di dalamnya
    /// menolak dengan pesan, dan itu jauh lebih berguna daripada pointer null
    /// yang harus diperiksa di setiap titik panggil.
    physics::PhysicsScene physics_;
    /// Whitebox yang sedang terbuka, dibagi panel dan viewport.
    WhiteboxStore whiteboxes_;
    TerrainStore terrains_;
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
