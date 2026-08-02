#pragma once

#include "Sim/Render/IViewportRenderer.h"
#include "Sim/Render/ThumbnailCache.h"
#include "Sim/Scene/World.h"

#include <functional>
#include <string>

namespace sim {
class FrameLimiter;
}

namespace sim::assets {
class AssetDatabase;
}

namespace sim::script {
class ScriptRuntime;
}

namespace sim::editor {

/// Menjembatani Entity dan SelectionId.
///
/// Digeser satu supaya nilai 0 tetap berarti "tidak ada": entity pertama entt
/// bernilai 0, dan tanpa pergeseran ini objek pertama di scene akan selalu
/// tampak tidak terpilih.
inline uint64_t ToSelectionId(scene::Entity entity) {
    return static_cast<uint64_t>(entity) + 1;
}
inline scene::Entity ToEntity(uint64_t id) {
    return id == 0 ? scene::kNullEntity : static_cast<scene::Entity>(id - 1);
}

class ActionRegistry;
class CommandHistory;
class Notifications;
class Selection;

/// Segala sesuatu yang dibutuhkan panel dari luar dirinya.
///
/// Panel tidak boleh mencari dependensinya lewat singleton; semuanya lewat
/// struct ini. Efeknya terasa di E4 ke atas: panel jadi bisa diuji dengan
/// konteks palsu, dan di A4 sebuah agen bisa menjalankan panel di mode headless
/// dengan renderer offscreen tanpa mengubah kode panel.
struct EditorContext {
    // --- layanan editor (dimiliki EditorApp) ---
    CommandHistory* history = nullptr;
    Selection* selection = nullptr;
    ActionRegistry* actions = nullptr;
    Notifications* notifications = nullptr;

    /// Dunia yang sedang disunting. Dimiliki EditorApp.
    scene::World* world = nullptr;

    /// Indeks aset. Dimiliki EditorApp. Referensinya hanya sah dalam satu
    /// frame: isinya ditukar utuh saat pemindaian latar selesai.
    assets::AssetDatabase* assets = nullptr;

    /// Runtime Lua. Null bila editor dibangun tanpa Lua — panel yang
    /// memakainya wajib memeriksa.
    script::ScriptRuntime* scripts = nullptr;
    /// True selama Play berjalan. Panel memakainya untuk menonaktifkan
    /// penyuntingan yang akan hilang begitu Stop ditekan.
    bool playing = false;

    render::IViewportRenderer* viewportRenderer = nullptr;
    /// Pratinjau aset untuk Asset Browser. Dimiliki pemanggil EditorApp.
    render::IThumbnailCache* thumbnails = nullptr;
    const FrameLimiter* frameLimiter = nullptr;

    /// Laju frame yang sedang dikunci, dan alasannya (monitor terlambat).
    float lockedFps = 60.0f;
    std::string frameLockReason;

    std::string projectPath;
    std::string levelName = "untitled";

    /// Folder tempat "Save as Prefab" menulis. Diisi EditorApp; sampai ada
    /// konsep proyek yang sesungguhnya (E5), isinya folder konfigurasi editor.
    std::string prefabDir;

    /// Waktu frame terakhir, dipakai widget yang beranimasi.
    float deltaSeconds = 0.0f;

    /// Diisi shell; panel memanggilnya untuk meminta editor berhenti.
    std::function<void()> requestExit;
    /// Diisi shell; meminta layout dock dikembalikan ke bawaan.
    std::function<void()> requestResetLayout;
    std::function<void()> requestPlay;
    std::function<void()> requestStop;

    // E5: AssetDatabase* assets
};

}  // namespace sim::editor
