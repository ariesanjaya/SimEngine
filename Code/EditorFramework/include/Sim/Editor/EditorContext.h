#pragma once

#include "Sim/Render/IMaterialPreview.h"
#include "Sim/Render/IViewportRenderer.h"
#include "Sim/Render/TimeOfDay.h"
#include "Sim/Render/ThumbnailCache.h"
#include "Sim/Scene/World.h"

#include <functional>
#include <string>
#include <vector>

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
    /// Preview material untuk Material Editor. Target rendernya sendiri, karena
    /// panel Viewport dan preview keduanya menggambar `ImGui::Image()` dan satu
    /// target berarti yang belakangan menimpa yang duluan. Null bila perangkat
    /// tidak mendukungnya — panel wajib memeriksa.
    render::IMaterialPreview* materialPreview = nullptr;
    /// Pratinjau aset untuk Asset Browser. Dimiliki pemanggil EditorApp.
    render::IThumbnailCache* thumbnails = nullptr;
    const FrameLimiter* frameLimiter = nullptr;

    /// Pengaturan global illumination.
    ///
    /// Di sini, bukan di panel Viewport, karena dua panel menyentuhnya: Viewport
    /// meneruskannya ke renderer, Statistics menyuntingnya. Pengaturan yang
    /// dimiliki salah satu panel akan hilang begitu panel itu ditutup.
    render::GiSettings gi;

    /// Pengaturan post-process. Di sini karena alasan yang sama dengan `gi`:
    /// dua tempat menyentuhnya — panel yang menyuntingnya dan viewport yang
    /// mengalirkannya ke renderer.
    render::PostProcessSettings post;

    /// Pengaturan langit atmosferik. Di sini karena alasan yang sama dengan
    /// `gi` dan `post`.
    struct SkySettings {
        bool enabled = true;
        float intensity = 20.0f;
        float cameraHeightKm = 0.5f;
        bool aerialPerspective = true;
        float aerialHaze = 1.0f;

        /// Sumber langit, dan berkas HDR-nya bila sumbernya peta.
        render::SkySource source = render::SkySource::Atmosphere;
        std::string hdriPath;
        float hdriRotation = 0.0f;
        float hdriIntensity = 1.0f;
    } sky;

    /// Awan volumetrik. Terpisah dari `sky` karena biayanya berbeda satu orde,
    /// dan karena itu sakelarnya perlu berdiri sendiri.
    render::CloudSettings clouds;

    /// Waktu-hari: preset kurva, jam siklus, dan tempat mataharinya.
    ///
    /// Di sini karena alasan yang sama dengan `gi`: dua hal menyentuhnya. Panel
    /// Time of Day menyuntingnya, dan `EditorApp` menerapkannya ke lampu
    /// matahari tiap frame — termasuk saat panelnya tertutup, karena siklus
    /// siang-malam yang berhenti begitu panelnya ditutup bukan siklus.
    render::TimeOfDayPreset timeOfDayPreset = render::TimeOfDayPreset::Default();
    render::TimeOfDayClock timeOfDayClock;
    render::SunPlacement sunPlacement;
    /// False berarti lampu matahari di scene dibiarkan apa adanya. Editor yang
    /// diam-diam menimpa nilai yang baru saja disetel tangan adalah editor yang
    /// tidak bisa dipakai menyetel apa pun.
    bool timeOfDayEnabled = false;

    /// Laju frame yang sedang dikunci, dan alasannya (monitor terlambat).
    float lockedFps = 60.0f;
    std::string frameLockReason;

    std::string projectPath;
    std::string levelName = "untitled";

    /// Folder tempat "Save as Prefab" menulis. Diisi EditorApp; sampai ada
    /// konsep proyek yang sesungguhnya (E5), isinya folder konfigurasi editor.
    std::string prefabDir;

    /// Folder `Shaders` di sebelah executable — berisi `.spv` hasil build dan
    /// `openpbr.slang` yang ditanam ke setiap modul material. Diisi EditorApp.
    std::string shaderDir;

    /// Cache SPIR-V hasil kompilasi shader material. Diisi EditorApp.
    ///
    /// Di folder konfigurasi editor, bukan di folder proyek: isinya diturunkan
    /// sepenuhnya dari graph dan bisa dibuang kapan saja, jadi ia tidak boleh
    /// ikut masuk version control proyek orang. Kosong berarti tanpa lapisan
    /// disk — kompilasinya tetap jalan, hanya tidak tersimpan antar-jalan.
    std::string shaderCacheDir;

    /// Waktu frame terakhir, dipakai widget yang beranimasi.
    float deltaSeconds = 0.0f;

    /// Diisi shell; panel memanggilnya untuk meminta editor berhenti.
    std::function<void()> requestExit;
    /// Diisi shell; meminta layout dock dikembalikan ke bawaan.
    std::function<void()> requestResetLayout;
    std::function<void()> requestPlay;
    std::function<void()> requestStop;
    /// Isi menu Scripts, diisi EditorApp dari registrasi Lua. Null bila editor
    /// dibangun tanpa Lua.
    std::function<void()> drawScriptMenu;

    /// Pemakai sebuah aset yang **tidak terindeks sebagai aset**, dalam bentuk
    /// deskripsi siap tampil.
    ///
    /// `AssetDatabase::UsersOf` hanya tahu tentang isi folder aset, sedangkan
    /// dua pemakai terpenting berada di luarnya: scene yang sedang dibuka —
    /// yang bahkan belum tentu ada di disk — dan berkas level milik editor.
    /// Tanpa keduanya, peringatan "aset ini masih dipakai" bisa berkata "tidak
    /// ada yang memakai" tepat ketika yang memakainya adalah pekerjaan yang
    /// sedang dibuka pengguna.
    std::function<std::vector<std::string>(const Uuid&)> findExternalAssetUsers;

    // E5: AssetDatabase* assets
};

}  // namespace sim::editor
