#include "Sim/Editor/EditorApp.h"

#include "Sim/Core/Log.h"
#include "Sim/Editor/Gizmo.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Editor/PropertyGrid.h"
#include "Sim/Editor/SceneCommands.h"
#include "Sim/Scene/Serialization.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <vector>

namespace sim::editor {
namespace {

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

bool EditorApp::Initialize(const Config& config) {
    configDir_ = config.configDir;

    context_.history = &history_;
    context_.selection = &selection_;
    context_.actions = &actions_;
    context_.notifications = &notifications_;
    context_.world = &world_;
    context_.viewportRenderer = config.viewportRenderer;
    context_.thumbnails = config.thumbnails;
    context_.frameLimiter = config.frameLimiter;
    context_.lockedFps = config.lockedFps;
    context_.frameLockReason = config.frameLockReason;
    context_.prefabDir = (configDir_ / "Prefabs").string();

    // Folder aset untuk sementara berada di folder konfigurasi editor. Begitu
    // konsep proyek matang, akarnya pindah mengikuti project.simproj.
    assets_.Initialize({configDir_ / "Assets", config.tasks, 1.0f});
    context_.assets = &assets_;

    // PropertyGrid menampilkan nama aset, bukan GUID mentah, lewat kait ini.
    SetAssetNameResolver([this](const Uuid& guid) -> std::string {
        const assets::AssetRecord* record = assets_.Find(guid);
        return record == nullptr ? std::string{} : record->name;
    });

    context_.requestExit = [this]() { RequestExit(); };
    context_.requestResetLayout = [this]() { shell_.RequestResetLayout(); };

    history_.SetMemoryBudget(64u * 1024u * 1024u);

    RegisterCoreActions();
    if (actions_.Load(configDir_ / "shortcuts.json")) {
        SIM_INFO("Editor", "Shortcuts loaded from {}",
                 (configDir_ / "shortcuts.json").string());
    }

    CreateStarterLevel();

    PanelRegistry::Get().InstantiateAll(panels_);
    panels_.LoadState(configDir_ / "panels.json");
    SIM_INFO("Editor", "{} panels registered", panels_.Panels().size());

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
                                               ? configDir_ / "Levels" / "untitled.simlevel"
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
class CreateEntityCommand final : public ICommand {
public:
    CreateEntityCommand(scene::World* world, Selection* selection, scene::Entity parent)
        : world_(world), selection_(selection), parent_(parent), guid_(Uuid::Generate()) {}

    void Do() override {
        entity_ = world_->CreateWithGuid(guid_, "Entity", parent_);
        if (selection_ != nullptr) {
            selection_->SelectOnly(ToSelectionId(entity_));
        }
    }
    void Undo() override {
        world_->Destroy(entity_);
        if (selection_ != nullptr) {
            selection_->Clear();
        }
    }
    std::string Name() const override { return "Create Entity"; }

private:
    scene::World* world_;
    Selection* selection_;
    scene::Entity parent_;
    Uuid guid_;
    scene::Entity entity_ = scene::kNullEntity;
};


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

void EditorApp::CreateStarterLevel() {
    // Level contoh, bukan level kosong. Editor yang dibuka dengan layar hampa
    // tidak memberi tahu apa pun tentang cara memakainya; beberapa objek dengan
    // komponen berbeda langsung memperlihatkan Outliner, Inspector, dan undo
    // bekerja.
    world_.Clear();
    const scene::Entity environment = world_.Create("Environment");

    const scene::Entity ground = world_.Create("Ground", environment);
    world_.Add<scene::MeshRendererComponent>(
        ground, scene::MeshRendererComponent{{}, {}, false, true});
    world_.Add<scene::StaticFlagComponent>(ground, scene::StaticFlagComponent{true});

    const scene::Entity ball = world_.Create("Shader Ball", environment);
    world_.TryGet<scene::TransformComponent>(ball)->position = Vec3(0.0f, 1.0f, 0.0f);
    world_.Add<scene::MeshRendererComponent>(
        ball, scene::MeshRendererComponent{{}, {}, true, true});

    const scene::Entity sun = world_.Create("Sun", environment);
    auto& sunLight = world_.Add<scene::LightComponent>(sun);
    sunLight.type = scene::LightType::Directional;
    sunLight.color = Vec3(1.0f, 0.96f, 0.88f);
    sunLight.intensity = 3.0f;
    world_.TryGet<scene::TransformComponent>(sun)->rotation =
        Quat(Vec3(-0.9f, 0.6f, 0.0f));

    const scene::Entity fill = world_.Create("Fill Light", environment);
    world_.TryGet<scene::TransformComponent>(fill)->position = Vec3(-3.0f, 2.5f, 2.0f);
    auto& fillLight = world_.Add<scene::LightComponent>(fill);
    fillLight.color = Vec3(0.55f, 0.65f, 1.0f);
    fillLight.intensity = 1.5f;

    const scene::Entity camera = world_.Create("Camera");
    world_.TryGet<scene::TransformComponent>(camera)->position = Vec3(0.0f, 2.0f, 8.0f);
    world_.Add<scene::CameraComponent>(camera);

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
    SIM_INFO("Editor", "Loaded {} entities from {}", result.entityCount, path.string());
    if (result.migrated) {
        notifications_.Warning("Level migrated from schema " +
                               std::to_string(result.sourceVersion));
    } else {
        notifications_.Success("Loaded " + std::to_string(result.entityCount) + " entities");
    }
    return true;
}

void EditorApp::DrawFrame(float deltaSeconds) {
    context_.deltaSeconds = deltaSeconds;

    // Mendahului panel: hasil pemindaian latar diterapkan di sini, sehingga
    // seluruh panel dalam frame ini melihat daftar aset yang sama.
    assets_.Update(deltaSeconds);
    if (context_.thumbnails != nullptr) {
        context_.thumbnails->Update();
    }

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

void EditorApp::DrawLevelDialogs() {
    // Belum ada dialog berkas sistem, dan menambahkannya sekarang berarti satu
    // dependensi lagi untuk sesuatu yang akan digantikan Asset Browser di E5.
    // Sampai saat itu, level dipilih dari folder editor lewat daftar sederhana —
    // cukup untuk menyimpan dengan nama lain dan membuka lagi.
    const std::filesystem::path levelsDir = configDir_ / "Levels";

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
            levelPath_.empty() ? configDir_ / "Levels" / "untitled.simlevel" : levelPath_;
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
    if (!history_.IsDirty()) {
        autosaveTimer_ = 0.0f;
        return;
    }
    autosaveTimer_ += deltaSeconds;
    if (autosaveTimer_ < kIntervalSeconds) {
        return;
    }
    autosaveTimer_ = 0.0f;

    const std::filesystem::path path = configDir_ / "Levels" / "autosave.simlevel";
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const scene::LevelIoResult result = scene::SaveLevelToFile(world_, path);
    if (result.ok) {
        SIM_INFO("Editor", "Autosaved {} entities to {}", result.entityCount, path.string());
    } else {
        SIM_WARN("Editor", "Autosave failed: {}", result.error);
    }
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
    actions_.Save(configDir_ / "shortcuts.json");
    panels_.SaveState(configDir_ / "panels.json");
    initialized_ = false;
}

}  // namespace sim::editor
