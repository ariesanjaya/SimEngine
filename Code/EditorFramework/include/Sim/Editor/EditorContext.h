#pragma once

#include "Sim/Render/ProbeVolume.h"
#include "Sim/SceneView/ProbeBakery.h"
#include "Sim/SceneView/Selection.h"
#include "Sim/SceneView/TerrainStore.h"
#include "Sim/SceneView/WhiteboxStore.h"
#include "Sim/Render/IMaterialPreview.h"
#include "Sim/Render/IViewportRenderer.h"
#include "Sim/Render/TimeOfDay.h"
#include "Sim/Render/ThumbnailCache.h"
#include "Sim/AIBridge/McpServer.h"
#include "Sim/Scene/World.h"

#include <functional>
#include <memory>
#include <filesystem>
#include <string>
#include <vector>

namespace sim {
class FrameLimiter;
}

namespace sim::assets {
class MeshGeometryCache;
class MeshSdfBakery;
class TextureBakery;
class AssetDatabase;
}

namespace sim::script {
class ScriptRuntime;
}

namespace sim::view {
class MaterialPrograms;
class SkinnedPreview;
class SceneView;
}

namespace sim::editor {

// **Milik `Sim::SceneView`, dipakai di sini dengan namanya sendiri.** Terjemahan
// `World` → `ViewportScene` beserta simpanannya bukan lagi milik editor — player
// menjawab pertanyaan yang sama dan berhak memakainya tanpa menyeret ImGui,
// riwayat undo, dan PanelManager. Yang tidak ikut berubah adalah panel: ia
// menulis `Selection` dan `TerrainView` seperti sejak E2.
//
// **Satu arahan, bukan daftar nama.** Daftar `using view::X;` per nama memang
// lebih sempit, dan justru itu masalahnya: setiap tipe baru di `Sim::SceneView`
// harus ditambahkan di sini juga, dan yang lupa menambahkannya tidak mendapat
// galat di modulnya sendiri melainkan di panel yang kebetulan memakainya. Yang
// bisa usang diam-diam lebih buruk daripada yang lebih luas dari perlunya.
//
// Cakupannya tetap `sim::editor` — tidak ada yang bocor ke lingkup global — dan
// tabrakan nama, bila suatu saat ada, menjadi galat kompilasi yang menyebut
// kedua kandidatnya.
using namespace view;  // NOLINT(google-build-using-namespace)

class ActionRegistry;
class CommandHistory;
class Notifications;

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

    /// Whitebox yang sedang terbuka. Panel menyunting, viewport menggambar —
    /// keduanya harus melihat objek yang sama, kalau tidak yang tergambar adalah
    /// bentuk sebelum suntingan terakhir.
    WhiteboxStore* whiteboxes = nullptr;

    /// Terrain yang sedang terbuka, dengan alasan yang sama persis — dan satu
    /// alasan tambahan: sebuah terrain berukuran ratusan megabyte, jadi salinan
    /// kedua bukan sekadar tidak sepakat, ia juga tidak muat.
    TerrainStore* terrains = nullptr;

    /// Runtime Lua. Null bila editor dibangun tanpa Lua — panel yang
    /// memakainya wajib memeriksa.
    script::ScriptRuntime* scripts = nullptr;
    /// True selama Play berjalan. Panel memakainya untuk menonaktifkan
    /// penyuntingan yang akan hilang begitu Stop ditekan.
    bool playing = false;
    /// True selama Play tertahan — oleh Pause, atau oleh breakpoint graph.
    /// Selalu false saat `playing` false.
    bool paused = false;

    render::IViewportRenderer* viewportRenderer = nullptr;
    /// Preview material untuk Material Editor. Target rendernya sendiri, karena
    /// panel Viewport dan preview keduanya menggambar `ImGui::Image()` dan satu
    /// target berarti yang belakangan menimpa yang duluan. Null bila perangkat
    /// tidak mendukungnya — panel wajib memeriksa.
    render::IMaterialPreview* materialPreview = nullptr;
    /// Pratinjau untuk thumbnail **di dalam** node graph material.
    ///
    /// Instance ketiga, dan alasannya sama seperti yang kedua ada: ia memegang
    /// material yang berbeda dari pratinjau besar — satu pin, bukan seluruh
    /// permukaan — dan `SetMaterial` membangun pipeline. Satu instance untuk
    /// keduanya berarti membangun ulang dua pipeline setiap frame, bergantian,
    /// selamanya.
    ///
    /// Kecil dengan sengaja: yang digambar sebesar kotak di dalam node, dan
    /// target 512² untuk kotak 96 px adalah 27 kali piksel yang tidak pernah
    /// terlihat.
    render::IMaterialPreview* materialNodePreview = nullptr;
    /// Pratinjau aset untuk Asset Browser. Dimiliki pemanggil EditorApp.
    render::IThumbnailCache* thumbnails = nullptr;
    /// Baker tekstur: dari berkas sumber ke `.ktx2` di cache. Dimiliki pemanggil
    /// EditorApp.
    ///
    /// Ada di sini, bukan di dalam renderer, karena baker adalah pengondisian
    /// aset dan bukan penggambaran: ia mendekode PNG, membangkitkan mip, dan
    /// menjalankan encoder BC7. Renderer hanya menerima `.ktx2`-nya.
    assets::TextureBakery* textureBakery = nullptr;
    /// Baker medan jarak mesh: dari berkas mesh ke `SdfGrid` yang dipakai
    /// clipmap GI. Dimiliki pemanggil EditorApp, dan ada di sini bukan di dalam
    /// renderer karena yang membakenya OpenVDB — pengondisi aset yang tidak
    /// pernah ikut ke jalur yang dikirim ke pemain.
    assets::MeshSdfBakery* meshSdfBakery = nullptr;
    /// Panggangan cahaya statis: transport ditelusuri path tracer acuan ke
    /// dalam kisi probe (S2 di docs/PLAN-STATIC-GI.md).
    ///
    /// **Null bila editor dibangun tanpa slangc** — `Sim::Reference` dilewati di
    /// sana, dan model shading path tracer-nya dibangkitkan darinya. Panel yang
    /// memakainya wajib memeriksa, aturan yang sama dengan runtime Lua.
    view::ProbeBakery* probeBakery = nullptr;
    /// Kisi yang sudah dipanggang dan sedang dipakai, atau null. Dipegang di
    /// sini supaya panel bisa melaporkannya dan viewport bisa memasangnya ke
    /// renderer — keduanya melihat kisi yang sama.
    std::shared_ptr<const render::ProbeVolume> probeVolume;
    /// Salinan CPU geometri mesh, untuk picking presisi dan query authoring.
    /// Null berarti picking kembali ke jalur kotak batas.
    assets::MeshGeometryCache* meshGeometry = nullptr;
    /// Penjaga shader material untuk pass forward. Dimiliki pemanggil EditorApp,
    /// dan seperti baker tekstur ia mengerjakan pekerjaannya di `TaskPool`.
    MaterialPrograms* materialPrograms = nullptr;
    /// Perender kedua untuk Mesh Editor: target rendernya sendiri, dengan
    /// alasan yang sama seperti pratinjau material — panel Viewport dan panel
    /// ini sama-sama menggambar `ImGui::Image()`, dan satu target berarti yang
    /// belakangan menimpa yang duluan. Null bila perangkat tidak mendukungnya.
    render::IViewportRenderer* meshPreview = nullptr;
    /// Membuka sebuah aset di editor yang menanganinya, lalu memberinya fokus.
    /// Mengembalikan false bila tidak ada panel yang mau menerimanya.
    std::function<bool(const Uuid&)> openAsset;
    /// Indeks isi bawaan editor. Read-only: panel yang menampilkannya harus
    /// mematikan aksi yang menulis — mengganti nama, memindahkan, menghapus —
    /// karena yang ditulis di sana bukan milik pengguna dan akan hilang pada
    /// pemasangan berikutnya.
    assets::AssetDatabase* builtinAssets = nullptr;
    /// Folder `Resources` di sebelah executable: isi bawaan editor, read-only.
    /// **Bukan bagian project mana pun** — ia sama untuk setiap project yang
    /// dibuka, dan menyalinnya ke dalam project adalah pilihan pengguna, bukan
    /// syarat memakainya.
    std::string builtinDir;

    /// Folder setelan pengguna — tempat cache masak tinggal. Dipakai panggangan
    /// cahaya statis untuk menaruh artefak `.simprobe`-nya, sejajar dengan
    /// `MeshSdfCache` dan `ShaderCache`.
    std::filesystem::path configDir;
    /// Pemutar klip animasi untuk mesh ber-rig. Dimiliki EditorApp; panel
    /// Viewport meneruskannya ke `SceneView::Build`. Null berarti mesh ber-rig
    /// digambar pada bind pose-nya.
    SkinnedPreview* animation = nullptr;
    const FrameLimiter* frameLimiter = nullptr;

    /// Pengaturan global illumination.
    ///
    /// Di sini, bukan di panel Viewport, karena dua panel menyentuhnya: Viewport
    /// meneruskannya ke renderer, Statistics menyuntingnya. Pengaturan yang
    /// dimiliki salah satu panel akan hilang begitu panel itu ditutup.
    render::GiSettings gi;

    /// Debug view pemeriksa jalur compute (G3). Di sini karena alasan yang sama
    /// dengan `gi`: panel Statistics yang menyalakannya, panel Viewport yang
    /// meneruskannya ke renderer.
    bool computeGradient = false;

    /// Penetapan lampu ke cluster di GPU (G4). Mematikannya mengembalikan jalur
    /// CPU — yang ada bukan sebagai peninggalan melainkan sebagai pembanding.
    bool gpuClusters = true;

    /// Komposit clipmap SDF di GPU (G4). Sama alasannya.
    bool gpuSdf = true;

    /// Permintaan menempatkan kamera viewport dari luar panel.
    ///
    /// **Sebuah permintaan, bukan nilai kamera.** Kamera itu sendiri milik panel
    /// Viewport — ia yang menyimpan orbit, batas pitch, dan kecepatan terbang —
    /// dan menaruhnya di sini berarti dua tempat yang berhak mengubahnya, dengan
    /// yang satu menimpa yang lain di tengah seretan mouse. Yang lewat sini
    /// hanya "tolong lihat dari sana", dan panel yang memutuskan bagaimana.
    ///
    /// Dipakai `viewport.capture` milik track AI: agen memeriksa hasil kerjanya
    /// dari sudut yang ia pilih sendiri.
    struct ViewportCameraRequest {
        bool pending = false;
        Vec3 from{0.0f, 0.0f, 0.0f};
        Vec3 lookAt{0.0f, 0.0f, 0.0f};
    };
    ViewportCameraRequest cameraRequest;

    /// Tempat gambar viewport berada di dalam jendela utama, frame terakhir.
    ///
    /// **Satuan logis ImGui, bukan piksel.** Yang menulisnya adalah panel, yang
    /// hanya mengenal satuan itu; yang memakainya adalah penangkap layar, yang
    /// hanya mengenal piksel. Skalanya diturunkan di tempat pemakaian dari
    /// perbandingan lebar gambar terhadap `mainSize` — bukan dari faktor DPI
    /// yang harus dibaca dari tempat ketiga dan bisa berbeda per monitor.
    ///
    /// `size` nol berarti panel Viewport tidak digambar pada frame terakhir, dan
    /// itu jawaban yang berbeda dari "digambar di pojok kiri atas".
    struct ViewportRect {
        Vec2 position{0.0f, 0.0f};  ///< relatif terhadap titik asal jendela utama
        Vec2 size{0.0f, 0.0f};
        Vec2 mainSize{0.0f, 0.0f};  ///< ukuran logis jendela utama
    };
    ViewportRect viewportRect;

    /// Keadaan pratinjau sebuah editor dokumen pada frame terakhir.
    ///
    /// **`ready` menjawab pertanyaan yang berbeda dari `rect.size != 0`.** Panel
    /// Material Editor menggambar sesuatu di petak itu jauh sebelum pipeline
    /// materialnya jadi: "Compiling…", pesan galat slangc, atau keterangan bahwa
    /// perangkat ini tidak punya pratinjau. Ketiganya adalah gambar yang sah dan
    /// tak satu pun memperlihatkan materialnya.
    ///
    /// Dipakai `material.preview` dan `particle.preview` milik track AI. Tanpa
    /// `ready`, satu-satunya yang bisa dilakukan tool itu adalah menunggu sekian
    /// ratus milidetik lalu memotret apa pun yang ada — dan mengirimkan gambar
    /// tulisan "Compiling…" dengan keterangan "ini materialmu" adalah kebohongan
    /// yang tidak akan pernah diperiksa agen.
    ///
    /// **Satu instance per editor, bukan satu yang dipakai bergantian.** Material
    /// Editor dan Particle Editor bisa sama-sama tergambar di dock yang berbeda,
    /// dan satu bidang bersama berarti yang belakangan menimpa yang duluan.
    struct DocumentPreviewState {
        ViewportRect rect;
        /// Aset yang sedang terbuka. Tidak sah bila belum ada yang dibuka.
        Uuid asset;
        bool ready = false;
        /// Kenapa belum siap, bila belum. Kosong saat siap.
        std::string status;
    };
    DocumentPreviewState materialPreviewState;
    DocumentPreviewState particlePreviewState;
    DocumentPreviewState animationPreviewState;

    /// Server MCP, bila ada. Nullptr di build atau mode yang tidak
    /// menyalakannya.
    ///
    /// **Panel yang menampilkannya bukan kemewahan.** Server ini memberi siapa
    /// pun yang menyambung kendali atas project yang sedang dibuka, dan
    /// satu-satunya yang membuatnya terlihat adalah panel ini — sebuah server
    /// yang berjalan tanpa ada yang tahu adalah persis keadaan yang tidak boleh
    /// terjadi.
    ai::McpServer* mcpServer = nullptr;

    /// Menyalakan ulang server yang dimatikan dari panel. Disediakan composition
    /// root karena ia yang memegang registry dan konfigurasinya; `Stop` tidak
    /// perlu ini karena server bisa menghentikan dirinya sendiri.
    std::function<bool()> mcpStart;

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

    /// Batas geometri adegan, seperti yang diukur viewport frame lalu.
    ///
    /// **Diukur di satu tempat dan dibaca di tempat lain, karena yang tahu
    /// bentuk dunia adalah yang membangunnya.** `SceneView` sudah memegang
    /// kotak batas tiap mesh beserta matriks dunianya — menghitung ulang di
    /// panel World Settings berarti memuat geometri kedua kalinya, dan
    /// jawabannya akan berbeda selama pemuatan latar belum selesai.
    ///
    /// `valid` false berarti belum ada viewport yang menggambar, atau adegannya
    /// tanpa geometri. Yang membacanya wajib memeriksanya — angka probe atas
    /// kotak kosong adalah angka yang salah, bukan nol.
    struct SceneBounds {
        Vec3 minimum{0.0f};
        Vec3 maximum{0.0f};
        bool valid = false;
    } sceneBounds;

    /// Geometri yang menaungi panggangan, seperti yang dikumpulkan viewport
    /// frame ini (S2).
    ///
    /// **String di dalamnya menunjuk ke penyimpanan `SceneView`**, yang sah
    /// sampai `Build` berikutnya — jadi yang memakainya harus memakainya di
    /// dalam frame yang sama. Itu cukup: yang memakainya tombol Bake, dan
    /// `ProbeBakery::Bake` menyalin geometrinya ke `PickScene` sebelum
    /// mengembalikan kendali.
    std::vector<view::PickItem> bakeItems;

    /// Awan volumetrik. Terpisah dari `sky` karena biayanya berbeda satu orde,
    /// dan karena itu sakelarnya perlu berdiri sendiri.
    render::CloudSettings clouds;

    /// Volume `.vdb` — asap, api, awan yang datang sebagai berkas.
    ///
    /// **Gridnya dimuat di sisi editor, bukan di renderer.** OpenVDB adalah
    /// pengondisi aset; renderer hanya menerima float yang sudah jadi. Aturan
    /// yang sama yang menjaga OpenImageIO di luar jalur runtime.
    struct Volume {
        /// Berkas yang dimuat. Kosong berarti tidak ada volume yang digambar.
        std::string path;
        /// Grid di dalam berkas. Kosong berarti grid float pertama.
        std::string gridName;

        /// Isi yang sudah dimuat, dan revisinya. Renderer mengunggah ulang
        /// hanya ketika revisinya berubah.
        std::shared_ptr<VolumeGrid> grid;
        uint64_t revision = 0;
        /// Pesan dari pemuatan terakhir; ditampilkan panel apa adanya.
        std::string status;

        Vec3 position{0.0f, 1.0f, 0.0f};
        float scale = 1.0f;
        float extinction = 4.0f;
        float stepSize = 0.05f;
        Vec3 albedo{0.8f, 0.8f, 0.85f};
        float lightIntensity = 3.0f;
    } volume;

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
    /// Menahan atau melanjutkan simulasi. Dunia tidak disentuh — lihat
    /// `EditorApp::Pause`.
    std::function<void()> requestTogglePause;
    /// Memajukan simulasi tepat satu langkah tetap, lalu menahannya lagi.
    std::function<void()> requestStep;
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
