#include "Sim/Editor/EditorApp.h"
#include "Sim/Editor/ToolApproval.h"

#include "Sim/Assets/MeshSdfBakery.h"
#include "Sim/Assets/TextureBakery.h"
#include "Sim/Editor/MaterialPrograms.h"

#include "Sim/Platform/FileDialog.h"

#include "Sim/Core/Log.h"
#include "Sim/Editor/Gizmo.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Editor/PropertyGrid.h"
#include "Sim/Editor/SceneCommands.h"
#include "Sim/Scene/AssetUsage.h"
#include "Sim/Terrain/Terrain.h"
#include "Sim/Whitebox/Collision.h"
#include "Sim/Scene/Serialization.h"

#if SIM_WITH_LUA
#include "Sim/Script/LuaVM.h"
#include "Sim/Script/ScriptRuntime.h"
#endif

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <csignal>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <vector>

namespace sim::editor {
namespace {

/// Nama folder skrip editor, relatif terhadap folder aset.
constexpr std::string_view kEditorScriptFolder = "Editor";

extern "C" void OnFatalSignal(int signal) {
    // Ini pelanggaran aturan async-signal-safety yang disengaja dan diketahui:
    // menulis log dan menyimpan layout memakai malloc. Alternatifnya adalah
    // kehilangan keduanya setiap kali editor crash, yang jauh lebih merugikan
    // daripada kemungkinan kecil penangan ini ikut menggantung. Setelah selesai
    // sinyalnya diteruskan supaya core dump tetap terbentuk.
    SIM_CRITICAL("Editor", "Fatal signal {} — flushing log and layout", signal);
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);
    }
    ::sim::Log::Shutdown();

    std::signal(signal, SIG_DFL);
    std::raise(signal);
}

}  // namespace

EditorApp::EditorApp() = default;
EditorApp::~EditorApp() = default;

bool EditorApp::Initialize(const Config& config) {
    configDir_ = config.configDir;
    projectsRoot_ = config.projectsRoot;
    newProjectRoot_ = projectsRoot_;
    resourceDir_ = config.resourceDir;
    tasks_ = config.tasks;
    graphCacheDir_ = configDir_ / "GraphCache";

    context_.history = &history_;
    context_.selection = &selection_;
    context_.whiteboxes = &whiteboxes_;
    context_.terrains = &terrains_;
    context_.actions = &actions_;
    context_.notifications = &notifications_;
    context_.world = &world_;
    context_.viewportRenderer = config.viewportRenderer;
    context_.materialPreview = config.materialPreview;
    context_.meshPreview = config.meshPreview;
    context_.thumbnails = config.thumbnails;
    context_.animation = &animation_;
    context_.scripts = config.scripts;
    headless_ = config.headless;
    context_.frameLimiter = config.frameLimiter;
    context_.lockedFps = config.lockedFps;
    context_.frameLockReason = config.frameLockReason;
    // **Cache turunan milik editor, bukan milik project.** Ketiganya berkunci
    // GUID aset atau hash sumber, dan keduanya unik lintas project — jadi satu
    // cache bersama justru yang benar, dan berpindah project tidak berarti
    // mendekode ulang seluruh thumbnail yang sudah pernah dibuat.
    context_.shaderCacheDir = (configDir_ / "ShaderCache").string();
    // Tekstur hasil bake ikut aturan yang sama: kuncinya hash isi berkas, jadi
    // dua project yang memakai tekstur yang sama persis berbagi satu berkas
    // cache alih-alih memampatnya dua kali.
    textureBakery_ =
        std::make_unique<assets::TextureBakery>(configDir_ / "TextureCache", config.tasks);
    context_.textureBakery = textureBakery_.get();
    // Medan jarak mesh ikut aturan yang sama, dan alasan yang sama pula: ia
    // berkunci hash isi berkas, memakan detik per mesh, dan tidak pernah
    // menyentuh GPU. **Sisi voxelnya disamakan dengan kaskade terhalus clipmap
    // GI** — grid yang lebih halus daripada kaskadenya membuang memori pada
    // ketelitian yang hilang saat disampel.
    meshSdfBakery_ = std::make_unique<assets::MeshSdfBakery>(configDir_ / "MeshSdfCache",
                                                            config.tasks,
                                                            assets::MeshSdfSettings{});
    context_.meshSdfBakery = meshSdfBakery_.get();
    // Shader material ikut ke `TaskPool`. Satu panggilan `slangc` adalah detik,
    // dan detik di dalam jalur gambar adalah editor yang membeku tepat pada
    // frame sebuah level dibuka — yaitu frame yang paling terasa.
    materialPrograms_ = std::make_unique<MaterialPrograms>(context_.shaderCacheDir,
                                                           config.shaderDir, config.tasks);
    context_.materialPrograms = materialPrograms_.get();
    context_.shaderDir = config.shaderDir.string();
    context_.builtinDir = config.resourceDir.string();

    // Pustaka bawaan dibuka di sini, bukan di `OpenProject`: isinya sama untuk
    // setiap project, dan membukanya ulang tiap kali project berganti berarti
    // memindai folder yang tidak berubah berkali-kali dalam satu sesi.
    if (!resourceDir_.empty()) {
        builtinAssets_.Initialize({resourceDir_, config.tasks, 30.0f, 60.0f});
        context_.builtinAssets = &builtinAssets_;
    }

    // **Indeks aset belum dibuka di sini.** Akarnya milik project, dan belum ada
    // project sampai seseorang memilihnya — itulah yang membedakan susunan ini
    // dari yang sebelumnya, tempat folder aset editor dan folder aset pekerjaan
    // orang adalah folder yang sama.
    context_.assets = &assets_;

    // PropertyGrid menampilkan nama aset, bukan GUID mentah, lewat kait ini.
    SetAssetNameResolver([this](const Uuid& guid) -> std::string {
        const assets::AssetRecord* record = assets_.Find(guid);
        return record == nullptr ? std::string{} : record->name;
    });

    context_.findExternalAssetUsers = [this](const Uuid& guid) {
        return FindExternalAssetUsers(guid);
    };
    // Panel ditanya berurutan; yang pertama menerima yang menang. Tidak ada
    // tabel "ekstensi ini milik editor itu" di mana pun — editor yang
    // menyatakannya sendiri lewat `Panel::OpenAsset`.
    context_.openAsset = [this](const Uuid& guid) {
        for (const std::unique_ptr<Panel>& panel : panels_.Panels()) {
            if (panel == nullptr || !panel->OpenAsset(guid, context_)) {
                continue;
            }
            panel->SetOpen(true);
            panel->RequestFocus();
            return true;
        }
        return false;
    };
    context_.requestExit = [this]() { RequestExit(); };
    context_.requestResetLayout = [this]() { shell_.RequestResetLayout(); };
    context_.requestPlay = [this]() { Play(); };
    context_.requestStop = [this]() { Stop(); };

    history_.SetMemoryBudget(64u * 1024u * 1024u);

    RegisterCoreActions();
    if (actions_.Load(configDir_ / "shortcuts.json")) {
        SIM_INFO("Editor", "Shortcuts loaded from {}",
                 (configDir_ / "shortcuts.json").string());
    }

    projects_.Load(configDir_ / "projects.json");

    PanelRegistry::Get().InstantiateAll(panels_);
    panels_.LoadState(configDir_ / "panels.json");
    SIM_INFO("Editor", "{} panels registered", panels_.Panels().size());

    // Satu aksi per panel, didaftarkan **sesudah** panelnya ada — `panels_`
    // masih kosong saat `RegisterCoreActions` berjalan.
    //
    // **Menu Window sebelumnya satu-satunya menu yang tidak lewat
    // `ActionRegistry`.** `PanelManager::DrawWindowMenu` menggambarnya
    // langsung, jadi ia tidak punya id, tidak bisa diberi pintasan papan ketik,
    // dan tidak terlihat oleh `editor.execute_action` — yang justru dijanjikan
    // memberi agen akses ke seluruh menu. Sebuah agen yang diminta memeriksa
    // Asset Browser karena itu tidak punya cara membukanya.
    //
    // Membuka saja tidak cukup: panel yang terbuka tapi berada di belakang tab
    // lain di dock yang sama tidak digambar sama sekali. `RequestFocus` yang
    // memilih tab-nya.
    for (const std::unique_ptr<Panel>& panel : panels_.Panels()) {
        if (panel == nullptr) {
            continue;
        }
        const std::string id = panel->Id();
        actions_.Register(Action{"panel." + id,
                                 panel->Title(),
                                 "Window",
                                 {},
                                 ImGuiKey_None,
                                 [this, id]() {
                                     if (Panel* target = panels_.Find(id)) {
                                         target->SetOpen(true);
                                         target->RequestFocus();
                                     }
                                 },
                                 {}});
    }

    InstallCrashHandler();
    initialized_ = true;
    return true;
}

void EditorApp::InstallCrashHandler() {
    for (int signal : {SIGSEGV, SIGABRT, SIGFPE, SIGILL}) {
        std::signal(signal, OnFatalSignal);
    }
}

void EditorApp::RegisterCoreActions() {
    actions_.Register(Action{"edit.undo",
                             "Undo",
                             "Edit",
                             icons::kUndo,
                             ImGuiMod_Ctrl | ImGuiKey_Z,
                             [this]() {
                                 if (history_.Undo()) {
                                     notifications_.Info("Undo");
                                 }
                             },
                             [this]() { return history_.CanUndo(); }});

    actions_.Register(Action{"edit.redo",
                             "Redo",
                             "Edit",
                             icons::kRedo,
                             ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z,
                             [this]() {
                                 if (history_.Redo()) {
                                     notifications_.Info("Redo");
                                 }
                             },
                             [this]() { return history_.CanRedo(); }});

    actions_.Register(Action{"edit.clear_history",
                             "Clear History",
                             "Edit",
                             icons::kDelete,
                             ImGuiKey_None,
                             [this]() { history_.Clear(); },
                             [this]() { return !history_.Entries().empty(); }});

    actions_.Register(Action{"selection.clear",
                             "Deselect All",
                             "Edit",
                             icons::kClose,
                             ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_A,
                             [this]() { selection_.Clear(); },
                             [this]() { return !selection_.Empty(); }});

    actions_.Register(Action{"view.reset_layout",
                             "Reset Layout",
                             "View",
                             icons::kRefresh,
                             ImGuiKey_None,
                             [this]() {
                                 shell_.RequestResetLayout();
                                 notifications_.Info("Layout reset");
                             },
                             {}});

    actions_.Register(Action{"project.close",
                             "Close Project...",
                             "File",
                             icons::kOpen,
                             ImGuiKey_None,
                             [this]() {
                                 // Kembali ke project manager, bukan langsung
                                 // membuka yang lain: memilih project berikutnya
                                 // adalah keputusan yang sama besarnya dengan
                                 // memilih yang pertama.
                                 CloseProject();
                             },
                             [this]() { return HasProject(); }});

    actions_.Register(Action{"editor.exit",
                             "Exit",
                             "File",
                             icons::kClose,
                             ImGuiMod_Alt | ImGuiKey_F4,
                             [this]() { RequestExit(); },
                             {}});

    actions_.Register(Action{"level.save",
                             "Save Level",
                             "File",
                             icons::kSave,
                             ImGuiMod_Ctrl | ImGuiKey_S,
                             [this]() {
                                 SaveLevel(levelPath_.empty()
                                               ? LevelsDirectory() / "untitled.simlevel"
                                               : levelPath_);
                             },
                             {}});

    actions_.Register(Action{"level.save_as",
                             "Save Level As...",
                             "File",
                             icons::kSave,
                             ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S,
                             [this]() {
                                 saveAsName_ = context_.levelName;
                                 focusSaveAsField_ = true;
                                 pendingDialog_ = Dialog::SaveAs;
                             },
                             {}});

    actions_.Register(Action{"level.open",
                             "Open Level...",
                             "File",
                             icons::kOpen,
                             ImGuiMod_Ctrl | ImGuiKey_O,
                             [this]() { pendingDialog_ = Dialog::Open; },
                             {}});

    actions_.Register(Action{"level.reload",
                             "Reload Level",
                             "File",
                             icons::kRefresh,
                             ImGuiKey_None,
                             [this]() { LoadLevel(levelPath_); },
                             [this]() { return !levelPath_.empty(); }});

    actions_.Register(Action{"level.new",
                             "New Level",
                             "File",
                             icons::kAdd,
                             ImGuiMod_Ctrl | ImGuiKey_N,
                             [this]() {
                                 selection_.Clear();
                                 CreateStarterLevel();
                                 notifications_.Info("New level");
                             },
                             {}});

    actions_.Register(Action{"play.start",
                             "Play",
                             "Run",
                             icons::kPlay,
                             ImGuiKey_F5,
                             [this]() { Play(); },
                             // Tidak lagi menuntut `scripts`: Play menjalankan
                             // fisika juga, dan build tanpa Lua tetap berhak
                             // melihat levelnya bergerak.
                             [this]() { return !playing_; }});

    actions_.Register(Action{"play.stop",
                             "Stop",
                             "Run",
                             icons::kStop,
                             ImGuiMod_Shift | ImGuiKey_F5,
                             [this]() { Stop(); },
                             [this]() { return playing_; }});

    actions_.Register(Action{"entity.create",
                             "Create Empty Entity",
                             "Entity",
                             icons::kEntity,
                             ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_N,
                             [this]() { CreateEntityAction(); },
                             {}});

    actions_.Register(Action{"entity.duplicate",
                             "Duplicate",
                             "Entity",
                             icons::kDuplicate,
                             ImGuiMod_Ctrl | ImGuiKey_D,
                             [this]() { DuplicateSelectionAction(); },
                             [this]() { return !selection_.Empty(); }});

    actions_.Register(Action{"entity.copy",
                             "Copy",
                             "Entity",
                             icons::kDuplicate,
                             ImGuiMod_Ctrl | ImGuiKey_C,
                             [this]() { clipboard_ = CopySubtrees(world_, SelectedRoots()); },
                             [this]() { return !selection_.Empty(); }});

    actions_.Register(Action{"entity.paste",
                             "Paste",
                             "Entity",
                             icons::kAdd,
                             ImGuiMod_Ctrl | ImGuiKey_V,
                             [this]() { PasteAction(false); },
                             [this]() { return !clipboard_.empty(); }});

    actions_.Register(Action{"entity.paste_as_child",
                             "Paste as Child",
                             "Entity",
                             icons::kAdd,
                             ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_V,
                             [this]() { PasteAction(true); },
                             [this]() { return !clipboard_.empty() && !selection_.Empty(); }});

    actions_.Register(Action{"entity.delete",
                             "Delete Entity",
                             "Entity",
                             icons::kDelete,
                             ImGuiKey_Delete,
                             [this]() { DeleteSelectionAction(); },
                             [this]() { return !selection_.Empty(); }});

    actions_.Register(Action{"preferences.save_shortcuts",
                             "Save Shortcuts",
                             "Preferences",
                             icons::kSave,
                             ImGuiKey_None,
                             [this]() {
                                 if (actions_.Save(configDir_ / "shortcuts.json")) {
                                     notifications_.Success("Shortcuts saved");
                                 } else {
                                     notifications_.Error("Could not save shortcuts");
                                 }
                             },
                             {}});
}

namespace {

/// Membuat entity, bisa dibatalkan.
///
/// GUID-nya disimpan, bukan dibuat ulang saat redo: kalau berubah, semua yang
/// merujuk entity itu — prefab, referensi antar-komponen nanti — akan menunjuk
/// objek yang sudah tidak ada setelah satu kali undo lalu redo.


}  // namespace

void EditorApp::CreateEntityAction() {
    const scene::Entity parent =
        selection_.Empty() ? scene::kNullEntity : ToEntity(selection_.Primary());
    history_.CloseMergeGroup();
    history_.Execute<CreateEntityCommand>(&world_, &selection_, parent);
}

std::vector<Uuid> EditorApp::SelectedRoots() const {
    // Entity yang leluhurnya ikut terpilih dilewati: sub-pohon leluhurnya sudah
    // memuatnya, dan memprosesnya dua kali menghasilkan salinan ganda saat
    // menyalin dan cuplikan undo kosong saat menghapus.
    std::vector<scene::Entity> entities;
    for (const uint64_t id : selection_.Items()) {
        const scene::Entity entity = ToEntity(id);
        if (world_.IsAlive(entity)) {
            entities.push_back(entity);
        }
    }

    std::vector<Uuid> guids;
    for (const scene::Entity entity : entities) {
        const bool coveredByAncestor =
            std::any_of(entities.begin(), entities.end(), [&](scene::Entity other) {
                return other != entity && world_.IsDescendantOf(entity, other);
            });
        if (!coveredByAncestor) {
            guids.push_back(world_.GuidOf(entity));
        }
    }
    return guids;
}

void EditorApp::DeleteSelectionAction() {
    std::vector<Uuid> guids = SelectedRoots();
    if (guids.empty()) {
        return;
    }
    history_.CloseMergeGroup();
    history_.Execute<DeleteEntitiesCommand>(&world_, &selection_, std::move(guids));
}

void EditorApp::DuplicateSelectionAction() {
    const std::vector<Uuid> guids = SelectedRoots();
    if (guids.empty()) {
        return;
    }
    std::vector<std::string> subtrees = CopySubtrees(world_, guids);
    if (subtrees.empty()) {
        return;
    }
    // Salinan diletakkan sebagai saudara dari aslinya, bukan sebagai akar:
    // menduplikasi sebuah roda mobil harus menghasilkan roda kedua di mobil
    // yang sama, bukan roda yang melayang di akar level.
    const scene::Entity first = world_.FindByGuid(guids.front());
    const Uuid parentGuid = world_.GuidOf(world_.ParentOf(first));

    history_.CloseMergeGroup();
    history_.Execute<PasteEntitiesCommand>(&world_, &selection_, std::move(subtrees), parentGuid,
                                           "Duplicate");
}

void EditorApp::PasteAction(bool asChild) {
    if (clipboard_.empty()) {
        return;
    }
    Uuid parentGuid;
    if (asChild && !selection_.Empty()) {
        const scene::Entity target = ToEntity(selection_.Primary());
        if (world_.IsAlive(target)) {
            parentGuid = world_.GuidOf(target);
        }
    }

    history_.CloseMergeGroup();
    // Salinan papan klip, bukan pindah: menempel dua kali harus menghasilkan
    // dua salinan, dan perintahnya menyimpan GUID hasil penukarannya sendiri.
    history_.Execute<PasteEntitiesCommand>(&world_, &selection_, clipboard_, parentGuid, "Paste");
}

// --- project ------------------------------------------------------------------

std::filesystem::path EditorApp::AssetsDirectory() const {
    return HasProject() ? project_.AssetsDirectory() : std::filesystem::path{};
}

std::filesystem::path EditorApp::LevelsDirectory() const {
    return HasProject() ? project_.LevelsDirectory() : std::filesystem::path{};
}

bool EditorApp::OpenProject(const std::filesystem::path& path) {
    scene::Project project;
    std::string error;
    if (!projects_.Open(path, project, error)) {
        projectError_ = error;
        SIM_WARN("Editor", "Cannot open the project {}: {}", path.string(), error);
        return false;
    }

    // Yang lama ditutup lebih dulu, dan urutannya bukan selera: indeks aset
    // memasang pemantau berkas pada akarnya, dan dua pemantau pada dua akar yang
    // berbeda berarti berkas project lama masih memicu impor ulang di project
    // yang baru.
    if (HasProject()) {
        CloseProject();
    }

    project_ = std::move(project);
    projectError_.clear();

    context_.prefabDir = project_.PrefabsDirectory().string();
    assets_.Initialize({project_.AssetsDirectory(), tasks_, 1.0f});

    // Runtime skrip dipasang ulang: ia memegang pointer ke indeks aset, dan
    // indeks itu baru saja menunjuk akar yang lain.
    if (context_.scripts != nullptr) {
        context_.scripts->Shutdown();
        context_.scripts->Initialize(world_, &assets_, graphCacheDir_);
        scriptingReady_ = false;
    }

    // **Levelnya dipilih, tidak dimuat diam-diam.** `startupLevel` tetap dicatat
    // dan dipakai sebagai sorotan di layar pemilih, tapi yang menentukan tetap
    // orangnya: project berisi banyak level tidak punya satu level yang "benar",
    // dan editor yang selalu membuka yang terakhir memaksa tutup-buka hanya
    // untuk berpindah.
    world_.Clear();
    history_.Clear();
    levelPath_.clear();
    context_.levelName = "untitled";
    awaitingLevelChoice_ = true;
    newLevelName_.clear();
    SIM_INFO("Editor", "Project '{}' open at {} — waiting for a level", project_.name,
             project_.root.string());

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    projects_.Remember(project_, static_cast<int64_t>(now));
    projects_.Save(configDir_ / "projects.json");
    return true;
}

bool EditorApp::CreateProject(const std::filesystem::path& parent, const std::string& name) {
    scene::Project project;
    std::string error;
    if (!projects_.Create(parent, name, project, error)) {
        projectError_ = error;
        SIM_WARN("Editor", "Cannot create the project '{}': {}", name, error);
        return false;
    }
    SIM_INFO("Editor", "Project '{}' created at {}", project.name, project.root.string());
    return OpenProject(project.root);
}

void EditorApp::CloseProject() {
    if (!HasProject()) {
        return;
    }
    if (playing_) {
        Stop();
    }
    // Pemantau berkas dilepas dan indeksnya dikosongkan. Tanpa ini, project
    // berikutnya membuka indeks kedua di atas yang pertama.
    assets_.Shutdown();
    world_.Clear();
    history_.Clear();
    selection_.Clear();
    animation_.Clear();
    // Dokumen yang terbuka ikut dibuang. Bukan kerapian: sebuah terrain
    // berukuran ratusan megabyte, dan membiarkannya berarti membuka project
    // kedua sambil tetap membayar yang pertama — sampai editor ditutup.
    whiteboxes_.Clear();
    terrains_.Clear();
    levelPath_.clear();
    context_.levelName.clear();
    project_ = scene::Project{};
}

/// Membuka dialog folder sistem, lalu menjalankan `accept` di main thread.
///
/// **Dijaga satu dialog pada satu waktu.** SDL akan dengan senang hati membuka
/// dialog kedua di atas yang pertama, dan dua dialog yang keduanya menulis ke
/// medan yang sama akan diselesaikan dalam urutan yang tidak bisa ditebak
/// siapa pun.
void EditorApp::PickFolder(const std::filesystem::path& start,
                           std::function<void(const std::filesystem::path&)> accept) {
    if (dialogOpen_) {
        return;
    }
    dialogOpen_ = true;
    platform::ShowOpenFolderDialog(
        start.string(),
        [this, accept = std::move(accept)](const platform::FileDialogResult& result) {
            dialogOpen_ = false;
            if (!result.error.empty()) {
                // Gagal membuka dialog disebutkan; dibatalkan tidak. Yang
                // pertama adalah editor yang tidak bisa melakukan sesuatu, yang
                // kedua adalah keputusan pengguna — dan memberitahunya bahwa ia
                // baru saja membatalkan sesuatu bukan kabar.
                projectError_ = result.error;
                return;
            }
            if (result.Cancelled()) {
                return;
            }
            accept(result.First());
        });
}

/// Layar pemilih project, digambar sebagai ganti seluruh shell editor.
void EditorApp::RememberStartupLevel(const std::filesystem::path& path) {
    std::error_code code;
    const std::filesystem::path relative = std::filesystem::relative(path, project_.root, code);
    if (code || relative.empty()) {
        return;
    }
    project_.startupLevel = relative.generic_string();
    scene::SaveProject(project_, project_.root / "project.simproj");
}

bool EditorApp::CreateLevelFile(const std::string& name) {
    const std::filesystem::path directory = LevelsDirectory();
    std::error_code code;
    std::filesystem::create_directories(directory, code);

    std::string safe;
    for (const char c : name) {
        // Nama berkas datang dari kotak teks; yang tidak sah disaring di sini
        // alih-alih dibiarkan gagal saat menulis dengan pesan dari OS.
        safe += (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == ' ' || c == '-' ||
                 c == '_')
                    ? c
                    : '_';
    }
    while (!safe.empty() && safe.back() == ' ') {
        safe.pop_back();
    }
    if (safe.empty()) {
        safe = "untitled";
    }

    const std::filesystem::path path = directory / (safe + ".simlevel");
    if (std::filesystem::exists(path)) {
        notifications_.Error("A level named '" + safe + "' already exists");
        return false;
    }

    CreateStarterLevel();
    // Ditulis sekarang, bukan saat Save pertama: level yang hanya hidup di
    // memori tidak bisa dimuat ulang, dan itu persis yang membuat pekerjaan
    // hilang saat editor ditutup tanpa sadar.
    if (!SaveLevel(path)) {
        return false;
    }
    awaitingLevelChoice_ = false;
    return true;
}

void EditorApp::DrawLevelPicker() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##LevelPicker", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();

    // Gaya yang sama dengan layar project: keduanya dibaca sekali, dari jarak
    // yang sama, sebelum ada yang bisa dikerjakan.
    const float baseFont = ImGui::GetFontSize();
    ImGui::PushFont(nullptr, baseFont * 1.25f);
    const float em = ImGui::GetFontSize();
    const ImVec2 origin = viewport->WorkPos;
    const ImVec2 size = viewport->WorkSize;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilledMultiColor(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                                  IM_COL32(22, 24, 62, 255), IM_COL32(38, 22, 78, 255),
                                  IM_COL32(96, 32, 98, 255), IM_COL32(152, 86, 46, 255));

    ImGui::SetCursorPos(ImVec2(em * 3.0f, em * 2.0f));
    ImGui::BeginGroup();
    ImGui::Text("%s", project_.name.c_str());
    ImGui::TextDisabled("Choose a level to open");
    ImGui::Spacing();

    const std::filesystem::path directory = LevelsDirectory();
    std::vector<std::filesystem::path> levels;
    std::error_code code;
    for (const auto& entry : std::filesystem::directory_iterator(directory, code)) {
        if (entry.is_regular_file() && entry.path().extension() == ".simlevel") {
            levels.push_back(entry.path());
        }
    }
    std::sort(levels.begin(), levels.end());

    ImGui::BeginChild("##levels", ImVec2(em * 24.0f, em * 14.0f), ImGuiChildFlags_Borders);
    if (levels.empty()) {
        ImGui::TextDisabled("No levels in this project yet.");
        ImGui::TextDisabled("Create one below to get started.");
    }
    for (const std::filesystem::path& level : levels) {
        const std::string name = level.stem().string();
        // Sorotan pada level terakhir yang dipakai. Ia petunjuk, bukan pilihan
        // otomatis — yang menentukan tetap kliknya.
        const bool wasStartup = !project_.startupLevel.empty() &&
                                project_.root / project_.startupLevel == level;
        if (ImGui::Selectable(name.c_str(), wasStartup) ||
            (wasStartup && ImGui::IsKeyPressed(ImGuiKey_Enter, false))) {
            if (LoadLevel(level)) {
                RememberStartupLevel(level);
                awaitingLevelChoice_ = false;
            }
        }
        if (wasStartup) {
            ImGui::SameLine();
            ImGui::TextDisabled("last opened");
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::SetNextItemWidth(em * 16.0f);
    newLevelName_.resize(128);
    ImGui::InputTextWithHint("##new-level", "New level name...", newLevelName_.data(),
                             newLevelName_.size());
    newLevelName_.resize(std::strlen(newLevelName_.c_str()));
    ImGui::SameLine();
    ImGui::BeginDisabled(newLevelName_.empty());
    if (ImGui::Button("Create")) {
        CreateLevelFile(newLevelName_);
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    if (ImGui::Button("Back to Projects")) {
        // Menutup projectnya, bukan hanya menyembunyikan layar ini: kembali ke
        // daftar project sambil masih memegang project lama adalah keadaan yang
        // tidak bisa dijelaskan ke siapa pun.
        project_ = scene::Project{};
        awaitingLevelChoice_ = false;
    }

    ImGui::EndGroup();
    ImGui::PopFont();
    ImGui::End();
}

void EditorApp::DrawProjectManager() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 origin = viewport->WorkPos;
    const ImVec2 size = viewport->WorkSize;

    ImGui::SetNextWindowPos(origin);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##ProjectManager", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();

    // Layar ini dipandang dari jarak yang berbeda dengan sisa editor: orang
    // membacanya sekali, sebelum ada yang bisa dikerjakan. Ukuran font yang pas
    // untuk panel penuh kontrol terlalu kecil untuk daftar yang harus terbaca
    // sekilas.
    const float baseFont = ImGui::GetFontSize();
    ImGui::PushFont(nullptr, baseFont * 1.25f);
    const float em = ImGui::GetFontSize();
    const float margin = em * 3.0f;
    ImDrawList* draw = ImGui::GetWindowDrawList();

    // Latar gradien, digambar sendiri alih-alih memakai warna tema. Layar ini
    // bukan bagian dari dockspace dan tidak punya panel di belakangnya — tanpa
    // latarnya sendiri yang terlihat hanya warna kosong jendela utama.
    draw->AddRectFilledMultiColor(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                                  IM_COL32(22, 24, 62, 255), IM_COL32(38, 22, 78, 255),
                                  IM_COL32(96, 32, 98, 255), IM_COL32(152, 86, 46, 255));

    const float barHeight = em * 3.2f;
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + barHeight),
                        IM_COL32(16, 16, 19, 255));
    const ImVec2 mark(origin.x + em * 1.7f, origin.y + barHeight * 0.5f);
    draw->AddCircleFilled(mark, em * 0.8f, IM_COL32(64, 72, 104, 255), 32);
    draw->AddCircleFilled(mark, em * 0.34f, IM_COL32(16, 16, 19, 255), 24);

    const ImVec2 tabPos(origin.x + em * 3.6f, origin.y + (barHeight - em * 1.1f) * 0.5f);
    draw->AddText(ImGui::GetFont(), em * 1.1f, tabPos, IM_COL32(124, 180, 255, 255), "Projects");
    const float tabWidth = ImGui::CalcTextSize("Projects").x * 1.1f;
    draw->AddLine(ImVec2(tabPos.x, origin.y + barHeight - em * 0.2f),
                  ImVec2(tabPos.x + tabWidth, origin.y + barHeight - em * 0.2f),
                  IM_COL32(64, 132, 232, 255), em * 0.16f);

    const float headingY = origin.y + barHeight + em * 1.6f;
    ImGui::SetCursorScreenPos(ImVec2(origin.x + margin, headingY));
    ImGui::PushFont(nullptr, baseFont * 2.0f);
    ImGui::TextUnformatted("My Projects");
    ImGui::PopFont();

    // Tombolnya sejajar judul di tepi kanan, dan membuka dialog alih-alih
    // memajang formulirnya. Yang dilakukan orang di layar ini hampir selalu
    // membuka project yang sudah ada; formulir "project baru" yang selalu
    // terbuka menempatkan pekerjaan yang jarang di atas yang sering.
    const float buttonHeight = ImGui::GetFrameHeight();
    const float newWidth = em * 8.4f;
    ImGui::SetCursorScreenPos(
        ImVec2(origin.x + size.x - margin - newWidth - buttonHeight - 1.0f, headingY));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.45f, 0.85f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.55f, 0.95f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.36f, 0.72f, 1.0f));
    ImGui::BeginDisabled(dialogOpen_);
    if (ImGui::Button("New Project...", ImVec2(newWidth, buttonHeight))) {
        ImGui::OpenPopup("##NewProject");
    }
    ImGui::SameLine(0.0f, 1.0f);
    if (ImGui::ArrowButton("##ProjectMore", ImGuiDir_Down)) {
        ImGui::OpenPopup("##ProjectMenu");
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor(3);

    if (ImGui::BeginPopup("##ProjectMenu")) {
        if (ImGui::MenuItem("Open Existing Project...")) {
            PickFolder(newProjectRoot_,
                       [this](const std::filesystem::path& chosen) { OpenProject(chosen); });
        }
        ImGui::Separator();
        // Kotak teksnya tetap ada. Dialog sistem tidak selalu tersedia — mesin
        // tanpa portal desktop, atau sesi jarak jauh — dan jalur yang ditempel
        // tangan adalah satu-satunya jalan masuk di sana.
        ImGui::SetNextItemWidth(em * 18.0f);
        ImGui::InputTextWithHint("##OpenProjectPath", "or paste a path", &openProjectPath_);
        if (ImGui::IsItemDeactivatedAfterEdit() && !openProjectPath_.empty()) {
            OpenProject(std::filesystem::path(openProjectPath_));
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(ImVec2(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("##NewProject", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushFont(nullptr, baseFont * 1.5f);
        ImGui::TextUnformatted("New Project");
        ImGui::PopFont();
        ImGui::Spacing();

        ImGui::SetNextItemWidth(em * 22.0f);
        ImGui::InputTextWithHint("##NewProjectName", "Project name", &newProjectName_);

        // Lokasinya diperlihatkan, tidak hanya diandaikan: "di mana project saya
        // tadi disimpan" adalah pertanyaan yang muncul justru saat editor sudah
        // ditutup, dan jawabannya harus terlihat saat membuatnya.
        ImGui::BeginDisabled(dialogOpen_);
        if (ImGui::Button("Choose location...")) {
            PickFolder(newProjectRoot_,
                       [this](const std::filesystem::path& chosen) { newProjectRoot_ = chosen; });
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", (newProjectRoot_ / ProjectLibrary::SanitizeFolderName(
                                                        newProjectName_.empty() ? "ProjectName"
                                                                                : newProjectName_))
                                      .string()
                                      .c_str());

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        const bool nameUsable = !ProjectLibrary::SanitizeFolderName(newProjectName_).empty();
        ImGui::BeginDisabled(!nameUsable || newProjectRoot_.empty() || dialogOpen_);
        if (ImGui::Button("Create", ImVec2(em * 7.0f, buttonHeight))) {
            CreateProject(newProjectRoot_, newProjectName_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(em * 7.0f, buttonHeight))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    float gridTop = headingY + em * 3.4f;
    if (!projectError_.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(origin.x + margin, gridTop));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.40f, 1.0f));
        ImGui::TextWrapped("%s", projectError_.c_str());
        ImGui::PopStyleColor();
        gridTop += em * 2.0f;
    }

    // Disalin lebih dulu: membuka sebuah project menulis ulang daftar ini, dan
    // menulis ulang wadah yang sedang diiterasi adalah iterator yang menggantung.
    const std::vector<RecentProject> recent = projects_.Recent();
    std::filesystem::path forget;

    ImGui::SetCursorScreenPos(ImVec2(origin.x + margin, gridTop));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild("##ProjectGrid",
                      ImVec2(size.x - margin * 2.0f, origin.y + size.y - gridTop - em * 1.5f));

    if (recent.empty()) {
        ImGui::TextDisabled("No projects yet. Start with \"New Project...\" above.");
    }

    // Teks yang lebih panjang dari kartunya dipotong, bukan dibiarkan meluber:
    // satu nama panjang yang menembus tepi kartu merusak barisan grid-nya, dan
    // grid yang barisnya tidak lurus tidak lebih rapi dari daftar biasa.
    const auto ellipsize = [](const std::string& text, float limit) {
        if (ImGui::CalcTextSize(text.c_str()).x <= limit) {
            return text;
        }
        std::string cut = text;
        while (!cut.empty() && ImGui::CalcTextSize((cut + "...").c_str()).x > limit) {
            cut.pop_back();
        }
        return cut + "...";
    };

    const float cardWidth = em * 10.5f;
    const float cardHeight = em * 13.0f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(1, static_cast<int>((avail + spacing) / (cardWidth + spacing)));

    int column = 0;
    for (const RecentProject& project : recent) {
        if (column > 0) {
            ImGui::SameLine();
        }
        ImGui::PushID(project.path.string().c_str());
        const bool exists = project.Exists();
        const std::string name = project.name.empty() ? "(unnamed)" : project.name;

        // Satu grup per kartu supaya SameLine berikutnya berpatok pada kartunya
        // secara utuh, bukan pada tombol terakhir yang digambar di dalamnya.
        ImGui::BeginGroup();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 p1(p0.x + cardWidth, p0.y + cardHeight);

        ImGui::BeginDisabled(!exists);
        if (ImGui::Selectable("##card", false, ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(cardWidth, cardHeight))) {
            OpenProject(project.path);
        }
        ImGui::EndDisabled();
        // Hover diukur dari kotaknya, bukan dari `IsItemHovered`: begitu tombol
        // "Open" muncul di atas kartu, item itulah yang dianggap ImGui sedang
        // di-hover — dan tombol yang syarat munculnya adalah hover pada item di
        // bawahnya akan berkedip hilang tepat saat kursor sampai padanya.
        const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(p0, p1);

        draw->AddRectFilled(p0, p1, IM_COL32(24, 20, 30, 225), em * 0.25f);
        draw->AddRect(p0, p1,
                      hovered && exists ? IM_COL32(124, 180, 255, 255) : IM_COL32(96, 92, 116, 170),
                      em * 0.25f, 0, em * 0.09f);

        const ImVec2 center(p0.x + cardWidth * 0.5f, p0.y + cardHeight * 0.44f);
        const float ring = em * 2.4f;
        draw->AddCircleFilled(center, ring, IM_COL32(70, 78, 108, 120), 48);
        draw->AddCircle(center, ring, IM_COL32(150, 160, 195, 150), 48, em * 0.26f);
        draw->AddCircleFilled(center, ring * 0.5f, IM_COL32(24, 20, 30, 255), 32);

        // Huruf awalnya, supaya kartu-kartu ini bisa dibedakan dari jauh tanpa
        // membaca namanya — belum ada gambar pratinjau per project untuk dipakai.
        char initial = name[0];
        if (initial >= 'a' && initial <= 'z') {
            initial = static_cast<char>(initial - 32);
        }
        const char letter[2] = {initial, '\0'};
        const float letterSize = em * 1.7f;
        const ImVec2 letterExtent = ImGui::CalcTextSize(letter);
        const float letterScale = letterSize / em;
        draw->AddText(ImGui::GetFont(), letterSize,
                      ImVec2(center.x - letterExtent.x * letterScale * 0.5f,
                             center.y - letterExtent.y * letterScale * 0.5f),
                      IM_COL32(198, 208, 232, 235), letter);

        if (!exists) {
            const ImVec2 warn(p1.x - em * 1.4f, p0.y + em * 1.1f);
            draw->AddTriangleFilled(ImVec2(warn.x, warn.y - em * 0.55f),
                                    ImVec2(warn.x - em * 0.6f, warn.y + em * 0.45f),
                                    ImVec2(warn.x + em * 0.6f, warn.y + em * 0.45f),
                                    IM_COL32(240, 190, 60, 255));
        }

        if (hovered && exists) {
            ImGui::SetCursorScreenPos(ImVec2(p0.x + em * 0.8f, p1.y - em * 2.3f));
            if (ImGui::Button("Open", ImVec2(cardWidth - em * 1.6f, buttonHeight))) {
                OpenProject(project.path);
            }
        }
        if (hovered) {
            ImGui::SetTooltip("%s", project.path.string().c_str());
        }

        ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y + em * 0.5f));
        ImGui::TextUnformatted(ellipsize(name, cardWidth - em * 2.0f).c_str());
        ImGui::SetCursorScreenPos(ImVec2(p1.x - em * 1.7f, p1.y + em * 0.45f));
        if (ImGui::SmallButton("...")) {
            ImGui::OpenPopup("##CardMenu");
        }
        if (ImGui::BeginPopup("##CardMenu")) {
            if (ImGui::MenuItem("Forget")) {
                forget = project.path;
            }
            if (ImGui::MenuItem("Copy path")) {
                ImGui::SetClipboardText(project.path.string().c_str());
            }
            ImGui::EndPopup();
        }

        ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y + em * 1.8f));
        if (exists) {
            // Nama folder induknya saja; jalur lengkapnya ada di tooltip. Jalur
            // penuh di dalam kartu selebar ini selalu terpotong di tengah, dan
            // potongan tengah sebuah jalur tidak memberi tahu apa pun.
            ImGui::TextDisabled("%s", ellipsize(project.path.parent_path().filename().string(),
                                                cardWidth)
                                          .c_str());
        } else {
            // Ditandai, bukan disembunyikan: project yang lenyap dari daftar
            // tanpa penjelasan terbaca sebagai editor yang lupa.
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.40f, 1.0f));
            ImGui::TextUnformatted("not found");
            ImGui::PopStyleColor();
        }

        // Kursornya dikembalikan ke tepi bawah kartu, lalu sebuah item bernilai
        // nol disodorkan di sana. Memindahkan kursor saja tidak cukup: ImGui
        // menghitung batas sebuah grup dari item yang benar-benar diajukan, dan
        // kursor yang melewati item terakhir tanpa item baru adalah persis
        // keadaan yang diadukannya sebagai "extend window boundaries".
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y + em * 3.2f));
        ImGui::Dummy(ImVec2(cardWidth, 0.0f));
        ImGui::EndGroup();
        ImGui::PopID();

        if (++column >= columns) {
            column = 0;
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();

    if (!forget.empty() && projects_.Forget(forget)) {
        projects_.Save(configDir_ / "projects.json");
    }

    ImGui::PopFont();
    ImGui::End();
}

scene::Entity EditorApp::PlaceTemplate(const char* group, const char* name,
                                       scene::Entity parent, const char* renameTo) {
    if (context_.builtinDir.empty()) {
        return scene::kNullEntity;
    }
    const std::filesystem::path path = std::filesystem::path(context_.builtinDir) / "Prefabs" /
                                       group / (std::string(name) + ".simprefab");
    std::ifstream stream(path);
    if (!stream) {
        SIM_WARN("Editor", "starter level: template {} is missing", path.string());
        return scene::kNullEntity;
    }
    const std::string text((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());

    std::string rootGuid;
    const std::string remapped = scene::RemapGuids(text, &rootGuid);
    if (remapped.empty() ||
        !scene::RestoreSubtree(world_, remapped, parent == scene::kNullEntity
                                                     ? Uuid{}
                                                     : world_.GuidOf(parent))) {
        SIM_WARN("Editor", "starter level: template {} could not be placed", path.string());
        return scene::kNullEntity;
    }

    const scene::Entity entity = world_.FindByGuid(Uuid::Parse(rootGuid));
    if (entity != scene::kNullEntity && renameTo != nullptr) {
        world_.SetName(entity, renameTo);
    }
    return entity;
}

void EditorApp::CreateStarterLevel() {
    // **Disusun dari prefab bawaan, bukan ditulis di sini.** Sebelumnya level
    // contoh ini membangun entitynya sendiri satu per satu — jadi ada dua
    // definisi tentang seperti apa sebuah "Shader Ball" atau "Directional
    // Light", dan menyunting templatenya tidak mengubah level baru sama sekali.
    //
    // Sekarang templatenya yang menjadi satu-satunya definisi: memperbaiki
    // prefab memperbaiki setiap level yang dibuat setelahnya.
    world_.Clear();
    const scene::Entity environment = world_.Create("Environment");

    PlaceTemplate("Environment", "Ground", environment);
    PlaceTemplate("Environment", "Sky Dome", environment);
    // Lampu matahari: template directional yang sama, dinamai sesuai perannya.
    // Nama "Sun" itulah yang dicari Time-of-Day saat menggerakkan matahari.
    PlaceTemplate("Lights", "Directional Light", environment, "Sun");
    PlaceTemplate("Actors", "Shader Ball", environment);
    PlaceTemplate("Cameras", "Camera", scene::kNullEntity);

    history_.Clear();
    context_.levelName = "untitled";
    levelPath_.clear();
}

bool EditorApp::SaveLevel(const std::filesystem::path& path) {
    const scene::LevelIoResult result = scene::SaveLevelToFile(world_, path);
    if (!result.ok) {
        SIM_ERROR("Editor", "Save failed: {}", result.error);
        notifications_.Error("Save failed: " + result.error);
        return false;
    }
    levelPath_ = path;
    context_.levelName = path.stem().string();
    history_.MarkSaved();
    SIM_INFO("Editor", "Saved {} entities to {}", result.entityCount, path.string());
    notifications_.Success("Saved " + std::to_string(result.entityCount) + " entities");
    return true;
}

bool EditorApp::LoadLevel(const std::filesystem::path& path) {
    const scene::LevelIoResult result = scene::LoadLevelFromFile(world_, path);
    if (!result.ok) {
        SIM_ERROR("Editor", "Load failed: {}", result.error);
        notifications_.Error("Load failed: " + result.error);
        return false;
    }
    selection_.Clear();
    history_.Clear();
    levelPath_ = path;
    context_.levelName = path.stem().string();
    // **Memuat sebuah level berarti level itu sekarang terbuka.** Tanpa baris
    // ini, hanya dua jalur klik di dalam pemilih level sendiri yang pernah
    // menurunkan layarnya — jadi `LoadLevel` yang dipanggil dari mana pun yang
    // lain menukar isi dunia diam-diam sementara editor tetap menampilkan
    // pemilih. Yang terlihat adalah editor yang mengabaikan permintaan;
    // yang sebenarnya terjadi adalah level yang termuat penuh di balik layar
    // yang menutupinya.
    awaitingLevelChoice_ = false;
    SIM_INFO("Editor", "Loaded {} entities from {}", result.entityCount, path.string());
    if (result.migrated) {
        notifications_.Warning("Level migrated from schema " +
                               std::to_string(result.sourceVersion));
    } else {
        notifications_.Success("Loaded " + std::to_string(result.entityCount) + " entities");
    }
    return true;
}

/// Segala yang harus maju satu frame tapi tidak menggambar apa pun.
///
/// **Dipisah dari `DrawFrame` supaya ada yang bisa dijalankan tanpa jendela.**
/// `SimHeadless` memutar loop yang hanya memanggil ini: tidak ada ImGui, tidak
/// ada panel, tidak ada pintasan papan ketik — tapi indeks aset tetap dipindai,
/// skrip tetap dimuat ulang, animasi tetap maju, dan fisika tetap melangkah.
/// Itulah yang membuat tool MCP menjawab hal yang sama di kedua mode.
///
/// Garis pemisahnya `BeginGizmoFrame()`: dari sana ke bawah semuanya menyentuh
/// ImGui.
void EditorApp::Tick(float deltaSeconds) {
    context_.deltaSeconds = deltaSeconds;
    if (!HasProject()) {
        return;
    }

    // Mendahului panel: hasil pemindaian latar diterapkan di sini, sehingga
    // seluruh panel dalam frame ini melihat daftar aset yang sama.
    assets_.Update(deltaSeconds);
    // Ikut diperbarui walaupun isinya jarang berubah: saat isi bawaan sedang
    // dikerjakan, editor yang tidak melihat perubahannya menuntut restart untuk
    // setiap suntingan.
    builtinAssets_.Update(deltaSeconds);
    ApplyTimeOfDay(deltaSeconds);
    if (context_.thumbnails != nullptr) {
        context_.thumbnails->Update();
    }
    // Mendahului panel juga, dan alasannya sama: panel Viewport membaca palet
    // yang dihasilkannya, jadi memajukan waktu SESUDAH panel digambar berarti
    // yang tergambar selalu pose frame sebelumnya.
    animation_.Update(world_, &assets_, deltaSeconds);
#if SIM_WITH_LUA
    // Dipasang di frame pertama, bukan di Initialize(). Runtime Lua baru
    // di-Initialize SETELAH EditorApp — ia butuh World dan AssetDatabase yang
    // dibuat di dalamnya — sehingga mendaftarkan binding di Initialize() berarti
    // menyentuh state Lua yang belum ada. Menunggu di sini menghapus urutan yang
    // harus diingat pemanggil, alih-alih mendokumentasikannya.
    if (!headless_ && !scriptingReady_ && context_.scripts != nullptr &&
        context_.scripts->VM().State() != nullptr) {
        // Folder terpisah dari skrip gameplay, dan itu bukan sekadar kerapian:
        // yang di sini berjalan di dalam editor dengan akses ke riwayat undo dan
        // panel, sedangkan yang di Scripts berjalan saat Play. Mencampurnya
        // berarti sebuah skrip gameplay bisa menambah menu, dan sebuah skrip
        // editor ikut terbawa ke build permainan.
        scripting_.Initialize(context_.scripts, &context_, &panels_,
                              AssetsDirectory() / kEditorScriptFolder);
        // Breakpoint graph menahan Play, bukan menghentikannya: scene tetap
        // seperti apa adanya saat node itu tercapai, dan Stop tetap satu-satunya
        // yang mengembalikannya. Yang tertahan adalah frame BERIKUTNYA — frame
        // yang sedang berjalan tetap diselesaikan, karena menghentikan Lua di
        // tengah tumpukan panggilan butuh debug hook yang belum ada.
        context_.scripts->SetBreakpointHandler([this](const std::string& node) {
            if (pausedAtBreakpoint_) {
                return;
            }
            pausedAtBreakpoint_ = true;
            notifications_.Info("Paused at graph node " + node.substr(0, 6));
        });
        context_.drawScriptMenu = [this]() { scripting_.DrawMenu(); };
        scriptingReady_ = true;
    }
    // Mendahului panel menggambar: panel yang didaftarkan Lua ditambahkan di
    // sini, di luar penelusuran daftar panel.
    scripting_.FlushPending();
    ReloadChangedScripts();
    if (playing_ && !pausedAtBreakpoint_ && context_.scripts != nullptr) {
        context_.scripts->Update(deltaSeconds);
    }
#endif

    // Fisika melangkah **sesudah** skrip, dan di luar pagar Lua di atas. Skrip
    // yang memindahkan benda kinematik pada frame ini harus terbaca solver pada
    // frame ini juga; urutan sebaliknya membuat setiap gerakan tertinggal satu
    // frame — cukup untuk terlihat sebagai getaran pada platform yang bergerak.
    //
    // `Advance` sendiri yang memutuskan berapa langkah muat di dalam
    // `deltaSeconds`, dan sering jawabannya nol.
    if (playing_ && !pausedAtBreakpoint_) {
        physics_.Advance(world_, deltaSeconds);
    }

}

void EditorApp::DrawFrame(float deltaSeconds) {
    context_.deltaSeconds = deltaSeconds;

    // **Digambar sebelum percabangan layar, bukan sesudahnya.** Kedua cabang di
    // bawah `return` lebih awal, dan agen bisa memanggil tool di layar mana pun
    // — justru `level.open` adalah tool pertama yang dipanggilnya, tepat saat
    // pemilih level sedang tampil. Sebuah dialog persetujuan yang hanya muncul
    // di satu dari tiga layar akan menahan tool call sampai tenggatnya habis,
    // tanpa satu pun tanda tentang kenapa.
    //
    // Ini ketahuan dengan menjalankannya: kodenya terbaca benar di tempatnya
    // yang lama, tepat di sebelah dialog level.
    DrawApprovalPrompt();

    // **Tanpa project, tidak ada yang lain digambar sama sekali.** Bukan sekadar
    // urutan: panel membaca indeks aset, folder level, dan runtime skrip, dan
    // ketiganya baru punya arti sesudah sebuah project dipilih. Menggambar shell
    // lebih dulu dengan semuanya kosong berarti setiap panel harus punya jalur
    // "belum ada project" sendiri-sendiri — dan yang lupa memilikinya akan
    // menulis ke folder yang tidak ada.
    if (!HasProject()) {
        DrawProjectManager();
        return;
    }

    // Alasan yang sama dengan gerbang project di atas: sebelum ada level,
    // Outliner, Inspector, dan viewport semuanya menggambar dunia kosong — dan
    // panel yang harus punya jalur "belum ada level" sendiri-sendiri adalah
    // panel yang salah satunya akan lupa memilikinya.
    if (awaitingLevelChoice_) {
        DrawLevelPicker();
        return;
    }

    Tick(deltaSeconds);

    // Harus mendahului panel mana pun: Viewport memakai gizmo, dan keadaan
    // per-frame-nya hanya direset di sini.
    BeginGizmoFrame();

    // Pintasan diproses sebelum panel menggambar, supaya aksi yang mengubah UI
    // (undo, reset layout) sudah berlaku pada frame yang sama — kalau tidak,
    // pengguna melihat hasilnya terlambat satu frame.
    actions_.ProcessShortcuts();

    panels_.Update(context_);
    shell_.Draw(context_, panels_);
    notifications_.Draw(deltaSeconds);

    DrawLevelDialogs();
    DrawExitPrompt();
    UpdateAutosave(deltaSeconds);
}

void EditorApp::ApplyTimeOfDay(float deltaSeconds) {
    if (!context_.timeOfDayEnabled) {
        return;
    }
    context_.timeOfDayClock.Advance(deltaSeconds);
    context_.sunPlacement.hour = context_.timeOfDayClock.Hour();
    const render::TimeOfDayState state =
        render::EvaluateTimeOfDay(context_.timeOfDayPreset, context_.sunPlacement);

    // **Yang digerakkan adalah lampu matahari di scene, bukan sebuah jalur
    // langit tersendiri.** Arah dan radiance matahari sudah mengalir dari
    // `LightComponent` ke cascade bayangan sejak E8.3, jadi memutar lampunya
    // otomatis memutar bayangannya — dan tidak ada satu pun sistem hilir yang
    // perlu tahu bahwa ada siklus siang-malam sama sekali.
    for (const auto raw : world_.Registry().view<scene::LightComponent>()) {
        const auto entity = static_cast<scene::Entity>(raw);
        auto* light = world_.TryGet<scene::LightComponent>(entity);
        if (light == nullptr || light->type != scene::LightType::Directional) {
            continue;
        }
        auto* transform = world_.TryGet<scene::TransformComponent>(entity);
        if (transform == nullptr) {
            continue;
        }
        // Rotasi yang membuat sumbu −Z entity menunjuk **menjauhi** matahari:
        // lampu directional memancar ke arah hadapnya, sedangkan
        // `SunPosition::direction` menunjuk dari permukaan ke matahari.
        // Membalikkannya tidak menghasilkan galat apa pun — hanya adegan yang
        // tersinari dari arah berlawanan.
        transform->rotation = render::LookRotation(-state.sun.direction);
        light->color = state.sunRadiance;
        // Intensitasnya sudah menyatu ke dalam radiance-nya. Menyimpannya dua
        // kali berarti dua angka yang bisa berselisih, dan yang satu tidak
        // terlihat di panel mana pun.
        light->intensity = 1.0f;
        world_.MarkTransformDirty(entity);
        break;
    }
}

void EditorApp::DrawApprovalPrompt() {
    // **Digambar di sini, bukan di panel AI Bridge.** Panel bisa ditutup, dan
    // mode `ask` yang berhenti bertanya karena sebuah panel ditutup akan
    // menahan setiap tool call agen sampai tenggatnya habis — tanpa satu pun
    // tanda di layar tentang kenapa.
    ToolApprovalGate::Question question;
    if (!approvals_.Pending(question)) {
        return;
    }

    if (!ImGui::IsPopupOpen("##ToolApproval")) {
        ImGui::OpenPopup("##ToolApproval");
    }
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("##ToolApproval", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::Text("The agent wants to run %s", question.tool.c_str());
    ImGui::TextDisabled("%s", question.permission.c_str());
    ImGui::Separator();

    // **Argumennya ditampilkan, dan itu yang membuat ini sebuah persetujuan.**
    // Dialog yang hanya menyebut nama tool meminta orang menyetujui sesuatu yang
    // tidak bisa ia lihat, dan yang tidak bisa dilihat akan disetujui setiap
    // kali. Panjangnya dibatasi: tool batch mengirim ribuan baris, dan dialog
    // setinggi layar sama tidak terbacanya dengan dialog kosong.
    constexpr std::size_t kMaxShown = 2000;
    std::string shown = question.arguments;
    if (shown.size() > kMaxShown) {
        shown.resize(kMaxShown);
        shown += "\n… (" + std::to_string(question.arguments.size() - kMaxShown) +
                 " more characters)";
    }
    ImGui::InputTextMultiline("##arguments", shown.data(), shown.size() + 1,
                              ImVec2(520.0f, 220.0f), ImGuiInputTextFlags_ReadOnly);

    ImGui::Separator();
    if (ImGui::Button("Allow", ImVec2(120.0f, 0.0f))) {
        approvals_.Answer(true);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    // Menolak lebih dulu dalam urutan tab, dan mendapat Escape: yang ragu-ragu
    // menekan Escape, dan yang ragu-ragu sedang tidak menyetujui.
    if (ImGui::Button("Refuse", ImVec2(120.0f, 0.0f)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        approvals_.Answer(false);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void EditorApp::DrawLevelDialogs() {
    // Belum ada dialog berkas sistem, dan menambahkannya sekarang berarti satu
    // dependensi lagi untuk sesuatu yang akan digantikan Asset Browser di E5.
    // Sampai saat itu, level dipilih dari folder editor lewat daftar sederhana —
    // cukup untuk menyimpan dengan nama lain dan membuka lagi.
    const std::filesystem::path levelsDir = LevelsDirectory();

    if (pendingDialog_ == Dialog::SaveAs && !ImGui::IsPopupOpen("Save Level As")) {
        ImGui::OpenPopup("Save Level As");
    }
    if (pendingDialog_ == Dialog::Open && !ImGui::IsPopupOpen("Open Level")) {
        ImGui::OpenPopup("Open Level");
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Save Level As", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("%s", levelsDir.string().c_str());
        ImGui::SetNextItemWidth(320.0f);
        // Fokus diarahkan ke kotak nama saat dialog muncul. Tanpa ini pengguna
        // harus mengklik kotaknya lebih dulu, dan mengetik langsung diikuti
        // Enter — yang wajar dilakukan — tidak menghasilkan apa-apa.
        if (focusSaveAsField_) {
            ImGui::SetKeyboardFocusHere();
            focusSaveAsField_ = false;
        }
        const bool entered = ImGui::InputText("##name", &saveAsName_,
                                              ImGuiInputTextFlags_EnterReturnsTrue |
                                                  ImGuiInputTextFlags_AutoSelectAll);
        ImGui::Spacing();

        const bool valid = !saveAsName_.empty();
        ImGui::BeginDisabled(!valid);
        const bool confirmed = ImGui::Button("Save", ImVec2(120.0f, 0.0f)) || (entered && valid);
        ImGui::EndDisabled();

        if (confirmed) {
            std::error_code error;
            std::filesystem::create_directories(levelsDir, error);
            SaveLevel(levelsDir / (saveAsName_ + ".simlevel"));
            pendingDialog_ = Dialog::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            pendingDialog_ = Dialog::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Open Level", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("%s", levelsDir.string().c_str());
        ImGui::Separator();

        bool anyLevel = false;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(levelsDir, error)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".simlevel") {
                continue;
            }
            anyLevel = true;
            if (ImGui::Selectable(entry.path().filename().string().c_str())) {
                LoadLevel(entry.path());
                pendingDialog_ = Dialog::None;
                ImGui::CloseCurrentPopup();
                break;
            }
        }
        if (!anyLevel) {
            ImGui::TextDisabled("No levels found");
        }

        ImGui::Separator();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            pendingDialog_ = Dialog::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorApp::DrawExitPrompt() {
    if (!exitRequested_) {
        return;
    }
    if (!history_.IsDirty()) {
        exitRequested_ = false;
        wantsExit_ = true;
        return;
    }

    // Dialog dibuka di sini, bukan saat permintaan keluar diterima: ImGui
    // menuntut OpenPopup dipanggil di dalam frame, sedangkan permintaannya bisa
    // datang dari mana saja termasuk penangan event SDL.
    constexpr const char* kTitle = "Unsaved changes";
    if (!ImGui::IsPopupOpen(kTitle)) {
        ImGui::OpenPopup(kTitle);
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::Text("\"%s\" has unsaved changes.", context_.levelName.c_str());
    ImGui::Spacing();

    if (ImGui::Button("Save and Exit", ImVec2(120.0f, 0.0f))) {
        const std::filesystem::path path =
            levelPath_.empty() ? LevelsDirectory() / "untitled.simlevel" : levelPath_;
        if (SaveLevel(path)) {
            wantsExit_ = true;
        }
        // Penyimpanan gagal berarti tidak keluar. Menutup editor setelah gagal
        // menyimpan justru membuang pekerjaan yang baru saja diminta disimpan.
        exitRequested_ = wantsExit_;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard", ImVec2(120.0f, 0.0f))) {
        wantsExit_ = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        exitRequested_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void EditorApp::UpdateAutosave(float deltaSeconds) {
    // Autosave menulis ke berkasnya sendiri, tidak pernah menimpa berkas yang
    // sedang disunting. Menimpanya akan menghapus versi tersimpan terakhir
    // dengan keadaan setengah jadi yang belum tentu diinginkan.
    constexpr float kIntervalSeconds = 120.0f;
    // Tidak menyimpan apa pun selama Play. Yang ada di scene saat itu adalah
    // keadaan permainan yang sedang berjalan — hasil skrip memindahkan dan
    // membuat entity — bukan level yang sedang disunting pengguna. Menuliskannya
    // ke autosave berarti satu-satunya jaring pengaman yang dimiliki pengguna
    // berisi sesuatu yang tidak pernah ia susun. Penghitungnya sengaja tidak
    // direset supaya penyimpanan menyusul segera setelah Stop.
    if (playing_) {
        return;
    }
    if (!history_.IsDirty()) {
        autosaveTimer_ = 0.0f;
        return;
    }
    autosaveTimer_ += deltaSeconds;
    if (autosaveTimer_ < kIntervalSeconds) {
        return;
    }
    autosaveTimer_ = 0.0f;

    const std::filesystem::path path = LevelsDirectory() / "autosave.simlevel";
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const scene::LevelIoResult result = scene::SaveLevelToFile(world_, path);
    if (result.ok) {
        SIM_INFO("Editor", "Autosaved {} entities to {}", result.entityCount, path.string());
    } else {
        SIM_WARN("Editor", "Autosave failed: {}", result.error);
    }
}

std::vector<std::string> EditorApp::FindExternalAssetUsers(const Uuid& guid) const {
    std::vector<std::string> users;
    if (!guid.IsValid()) {
        return users;
    }

    // Scene yang sedang dibuka lebih dulu, dan bukan sekadar karena urutan:
    // ia satu-satunya pemakai yang isinya belum tentu ada di disk. Pengguna
    // yang baru saja memasang tekstur ini ke sebuah entity — dan belum
    // menyimpan — harus tetap diperingatkan.
    const std::vector<scene::Entity> entities = scene::EntitiesUsingAsset(world_, guid);
    const bool usedByOpenScene = !entities.empty();
    if (usedByOpenScene) {
        std::string names;
        // Tiga nama saja. Yang dibutuhkan pengguna adalah tahu bahwa scene-nya
        // ikut terdampak dan kira-kira di mana; menumpahkan lima puluh nama ke
        // dalam dialog justru membuat pesannya tidak terbaca.
        const std::size_t shown = std::min<std::size_t>(entities.size(), 3);
        for (std::size_t i = 0; i < shown; ++i) {
            names += (i == 0 ? "" : ", ") + world_.NameOf(entities[i]);
        }
        if (entities.size() > shown) {
            names += ", +" + std::to_string(entities.size() - shown);
        }
        users.push_back("Scene \"" + context_.levelName + "\" (open): " + names);
    }

    // Berkas level milik editor. Folder ini berada DI LUAR akar aset, jadi
    // AssetDatabase tidak mengindeksnya dan UsersOf() tidak akan pernah
    // menyebutnya — padahal justru di sinilah level pengguna disimpan.
    const std::filesystem::path levels = LevelsDirectory();
    std::error_code error;
    if (!std::filesystem::is_directory(levels, error)) {
        return users;
    }
    const std::string needle = guid.ToString();
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(levels, error)) {
        if (entry.is_regular_file() && entry.path().extension() == ".simlevel") {
            files.push_back(entry.path());
        }
    }
    // Diurutkan supaya daftarnya sama setiap kali dialog dibuka.
    std::sort(files.begin(), files.end());

    for (const std::filesystem::path& file : files) {
        // Berkas level yang sedang dibuka dilewati HANYA bila scene di memori
        // juga memakainya — di situ ia cuma pengulangan.
        //
        // Kalau scene tidak lagi memakainya sementara berkasnya masih, keduanya
        // memang berbeda, dan yang di disk tetap akan rusak selama belum
        // disimpan ulang. Melewatkannya karena "yang di memori lebih benar"
        // berarti menyembunyikan satu-satunya pemakai yang tersisa.
        if (usedByOpenScene && !levelPath_.empty() &&
            std::filesystem::equivalent(file, levelPath_, error)) {
            continue;
        }
        std::ifstream stream(file, std::ios::binary);
        if (!stream) {
            continue;
        }
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        // Pencocokan teks, sama seperti yang dipakai importer dokumen: GUID
        // ditulis sebagai string di berkas level, dan menguraikan seluruh level
        // hanya untuk menjawab "dipakai atau tidak" jauh lebih mahal.
        if (buffer.str().find(needle) != std::string::npos) {
            users.push_back("Levels/" + file.filename().string());
        }
    }
    return users;
}

void EditorApp::ReloadChangedScripts() {
#if SIM_WITH_LUA
    if (context_.scripts == nullptr) {
        return;
    }
    const std::string prefix = std::string(kEditorScriptFolder) + '/';
    bool editorScriptChanged = false;
    for (const Uuid& guid : assets_.ChangedThisUpdate()) {
        const assets::AssetRecord* record = assets_.Find(guid);
        if (record == nullptr) {
            continue;
        }
        // Graph yang berubah dikompilasi ulang lebih dulu, baru instance-nya
        // dimuat ulang lewat jalur yang sama dengan skrip.
        if (record->type == assets::AssetType::Graph) {
            context_.scripts->Graphs().Rebuild(guid, assets_.AbsolutePath(*record));
            if (playing_) {
                context_.scripts->Reload(guid);
                notifications_.Info("Reloaded " + record->name);
            }
            continue;
        }
        if (record->type != assets::AssetType::Script) {
            continue;
        }
        if (record->relativePath.starts_with(prefix)) {
            // Skrip editor dimuat ulang seluruhnya, bukan satu berkas: menu dan
            // panel yang didaftarkan tidak menyebutkan berkas asalnya, jadi
            // tidak ada cara membuang registrasi milik satu berkas saja.
            editorScriptChanged = true;
            continue;
        }
        // Skrip gameplay hanya punya instance selagi bermain. Di luar Play tidak
        // ada yang bisa dimuat ulang, dan berkas yang baru disimpan akan dibaca
        // apa adanya saat Play berikutnya ditekan.
        if (!playing_) {
            continue;
        }
        context_.scripts->Reload(guid);
        notifications_.Info("Reloaded " + record->name);
    }

    if (editorScriptChanged) {
        scripting_.ReloadAll();
        notifications_.Info("Reloaded editor scripts");
    }
#endif
}

void EditorApp::Play() {
    if (playing_) {
        return;
    }
    // **Play tidak lagi menuntut skrip.** Dulu seluruh fungsi ini dipagari
    // `#if SIM_WITH_LUA` dan langsung kembali bila `scripts` null, karena satu-
    // satunya yang berjalan saat Play memang skrip. Sekarang fisika juga
    // berjalan, dan ia tidak ada hubungannya dengan Lua — build tanpa Lua yang
    // tidak bisa menjalankan simulasinya adalah dua fitur opsional yang saling
    // mengunci tanpa alasan.
    //
    // Cuplikan diambil sebelum satu baris skrip pun berjalan. Ini yang membuat
    // Play aman dicoba kapan saja: apa pun yang dilakukan skrip terhadap scene
    // — memindahkan, menghapus, membuat entity — hilang seluruhnya saat Stop.
    playSnapshot_ = scene::SaveLevelToString(world_);

    // Seleksi ikut dicatat, dengan alasan yang sama seperti scene-nya: pengguna
    // menekan Play untuk melihat sesuatu berjalan, bukan untuk kehilangan tempat
    // ia sedang bekerja. Dicatat sebagai GUID karena handle entity tidak
    // bertahan melewati pembangunan ulang scene di Stop.
    //
    // Urutannya dipertahankan: Selection::Primary() adalah yang terakhir, dan
    // banyak operasi editor memakai "yang aktif" sebagai acuan.
    playSelection_.clear();
    playSelection_.reserve(selection_.Count());
    for (const uint64_t id : selection_.Items()) {
        const scene::Entity entity = ToEntity(id);
        if (world_.IsAlive(entity)) {
            playSelection_.push_back(world_.GuidOf(entity));
        }
    }
    // Selama Play seleksinya dikosongkan: skrip boleh menghapus entity, dan
    // panel yang memegang handle mati lebih buruk daripada panel yang kosong.
    selection_.Clear();
    history_.CloseMergeGroup();

#if SIM_WITH_LUA
    if (context_.scripts != nullptr) {
        context_.scripts->Start();
    }
#endif

    StartPhysics();

    playing_ = true;
    pausedAtBreakpoint_ = false;
    context_.playing = true;
    notifications_.Info("Play");
}

void EditorApp::StartPhysics() {
    if (!physics::Available()) {
        // Sekali, saat Play ditekan — bukan tiap frame. Yang perlu diketahui
        // pengguna adalah mengapa tidak ada yang jatuh, dan itu satu kalimat.
        SIM_INFO("Editor", "Play without physics: {}", "this build has no PhysX");
        return;
    }
    // Bentuk whitebox dipasok dari sini, bukan dicari sendiri oleh fisika:
    // yang bisa mengubah GUID menjadi bentuk adalah yang memegang basis aset.
    const auto shapeFromAsset = [this](scene::Entity entity,
                                       physics::ColliderGeometry& out) -> bool {
        if (const auto* terrainComponent = world_.TryGet<scene::TerrainComponent>(entity)) {
            if (!terrainComponent->terrain.IsValid()) {
                return false;
            }
            const assets::AssetRecord* record = assets_.Find(terrainComponent->terrain.guid);
            if (record == nullptr) {
                return false;
            }
            terrain::Terrain* map =
                terrains_.Get(terrainComponent->terrain.guid, assets_.AbsolutePath(*record));
            if (map == nullptr) {
                return false;
            }
            // **Satu kisi untuk seluruh peta.** Batasnya disebut di sini, bukan
            // dibiarkan menjadi alokasi satu gigabyte yang diam-diam: peta 4 km
            // pada jarak sampel 0,25 m adalah 268 juta sampel, dan tidak ada
            // yang memasaknya. Yang melampauinya ditolak beserta angkanya,
            // sampai collider per-ubin ada.
            constexpr std::size_t kMaxSamples = 4096u * 4096u;
            const std::size_t samples = static_cast<std::size_t>(map->SamplesX()) *
                                        static_cast<std::size_t>(map->SamplesY());
            if (samples > kMaxSamples) {
                SIM_WARN("Physics",
                         "terrain '{}' has {} samples, more than the {} a single height field "
                         "collider can hold; give it a coarser sample spacing",
                         record->name, samples, kMaxSamples);
                return false;
            }

            physics::HeightFieldDesc& field = out.heightField;
            map->ReadAll(field.samples);
            map->ReadHoles(field.holes);
            field.width = map->SamplesX();
            field.depth = map->SamplesY();
            field.spacing = map->Desc().sampleSpacing;
            field.minHeight = map->Desc().minHeight;
            field.maxHeight = map->Desc().maxHeight;
            return field.Valid();
        }

        const auto* component = world_.TryGet<scene::WhiteboxComponent>(entity);
        if (component == nullptr || !component->whitebox.IsValid()) {
            return false;
        }
        const assets::AssetRecord* record = assets_.Find(component->whitebox.guid);
        if (record == nullptr) {
            return false;
        }
        whitebox::WhiteboxMesh* box =
            whiteboxes_.Get(component->whitebox.guid, assets_.AbsolutePath(*record));
        if (box == nullptr) {
            return false;
        }
        whitebox::CollisionShape shape = whitebox::BuildCollisionShape(*box);
        out.points = std::move(shape.points);
        out.indices = std::move(shape.indices);
        out.convex = shape.convex;
        return true;
    };

    if (!physics_.Build(world_, {}, shapeFromAsset)) {
        SIM_WARN("Editor", "Physics could not start: {}", physics_.Error());
        notifications_.Warning("Physics could not start: " + physics_.Error());
        return;
    }

    const physics::PhysicsSceneStats& stats = physics_.Stats();
    if (stats.bodies == 0) {
        return;
    }
    // Yang dilewati disebutkan di notifikasi, bukan hanya di log: benda yang
    // punya Rigid Body tanpa Collider jatuh menembus segalanya, dan pengguna
    // yang baru menekan Play sedang menatap viewport, bukan berkas log.
    if (stats.skippedWithoutCollider > 0) {
        notifications_.Warning(std::to_string(stats.skippedWithoutCollider) +
                               " rigid bodies have no collider and are not simulated");
    }
    if (stats.collidersWithoutGeometry > 0) {
        notifications_.Warning(std::to_string(stats.collidersWithoutGeometry) +
                               " whitebox colliders could not be read and fell back to a box");
    }
    if (stats.concaveDynamic > 0) {
        // Bukan galat, tetapi bukan pula yang digambar: benda dinamis hanya
        // boleh memakai selubung cembungnya, dan pada blok cekung selubung itu
        // menutup lekukannya.
        notifications_.Warning(std::to_string(stats.concaveDynamic) +
                               " dynamic whiteboxes are concave, so their hollows are filled in");
    }
    SIM_INFO("Editor", "Physics: {} bodies", stats.bodies);
}

void EditorApp::Stop() {
    if (!playing_) {
        return;
    }
#if SIM_WITH_LUA
    if (context_.scripts != nullptr) {
        context_.scripts->Stop();
    }
#endif
    // Dibuang sebelum scene dipulihkan: benda di dalamnya menunjuk entity yang
    // sebentar lagi tidak ada lagi.
    physics_.Clear();
    selection_.Clear();

    const scene::LevelIoResult result = scene::LoadLevelFromString(world_, playSnapshot_);
    if (!result.ok) {
        // Kegagalan di sini berarti pekerjaan pengguna hilang, jadi ia harus
        // terdengar keras — bukan tercatat diam-diam di log.
        SIM_ERROR("Editor", "Cannot restore scene after Stop: {}", result.error);
        notifications_.Error("Scene could not be restored: " + result.error);
    }
    // Seleksi dipasang kembali lewat GUID. Entity yang tidak ditemukan
    // dilewati alih-alih membatalkan seluruhnya: itu hanya terjadi kalau
    // pemulihan scene-nya sendiri gagal, dan di keadaan itu memulihkan seleksi
    // sebagian tetap lebih berguna daripada tidak sama sekali.
    std::vector<uint64_t> restored;
    restored.reserve(playSelection_.size());
    for (const Uuid& guid : playSelection_) {
        const scene::Entity entity = world_.FindByGuid(guid);
        if (entity != scene::kNullEntity) {
            restored.push_back(ToSelectionId(entity));
        }
    }
    selection_.SetItems(std::move(restored));
    playSelection_.clear();

    playSnapshot_.clear();
    playing_ = false;
    pausedAtBreakpoint_ = false;
    context_.playing = false;

    // Riwayat undo dibersihkan: entri di dalamnya menunjuk keadaan scene sebelum
    // Play, sedangkan scene baru saja dibangun ulang dari nol. Membiarkannya
    // berarti Ctrl+Z menerapkan perubahan ke entity yang sudah tidak sama.
    history_.Clear();
}

std::string EditorApp::WindowTitle() const {
    std::string title = "SimEngine Editor — " + context_.levelName;
    if (history_.IsDirty()) {
        title += " *";
    }
    return title;
}

void EditorApp::SetFrameLock(float hz, std::string reason) {
    context_.lockedFps = hz;
    context_.frameLockReason = std::move(reason);
}

void EditorApp::Shutdown() {
    if (!initialized_) {
        return;
    }
    // History dibersihkan lebih dulu: command-nya bisa menunjuk data milik
    // panel, dan panel dihancurkan setelah ini.
    history_.Clear();
    // Registrasi Lua memegang fungsi milik state Lua. Dilepas sebelum panel
    // dihancurkan, karena panel Lua ikut memegangnya.
    scripting_.Shutdown();
    actions_.Save(configDir_ / "shortcuts.json");
    panels_.SaveState(configDir_ / "panels.json");
    initialized_ = false;
}

}  // namespace sim::editor
