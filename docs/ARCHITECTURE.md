# SimEngine — Arsitektur

## Peta direktori

```
SimEngine/
├── CMakeLists.txt            # root: opsi, modul, urutan add_subdirectory
├── CMakePresets.json         # linux-clang-debug / release / asan / tsan
├── cmake/
│   ├── SimTargets.cmake      # sim_add_library() / sim_add_executable(): warning, C++20, IDE folder
│   ├── SimDeps.cmake         # FetchContent semua dependensi, versi terkunci
│   ├── SimVulkan.cmake       # temukan Vulkan SDK, volk, VMA
│   └── SimShaders.cmake      # kompilasi GLSL/Slang → SPIR-V saat build
├── Code/
│   ├── Core/                 # log, assert, uuid, path, waktu, filewatcher, job system, math
│   ├── Reflect/              # registry tipe, deskriptor field, atribut, konversi any
│   ├── Platform/             # SDL3: window, monitor/DPI, input, clipboard, proses
│   ├── RHI/                  # Vulkan: device, swapchain, buffer/image, VMA, render target
│   ├── ImGuiIntegration/     # konteks ImGui, docking, multi-viewport, tema, font, jembatan tekstur
│   ├── Asset/                # database aset, GUID, .meta, importer, cache thumbnail
│   ├── Scene/                # EnTT world, entity, hierarki transform, komponen, prefab
│   ├── Script/               # Lua 5.4 + sol2, script component, hot reload
│   ├── Material/             # graph `.simmat`, katalog node OpenPBR, kompilasi ke Slang
│   ├── Particle/             # efek `.simfx`, emitter, simulasi CPU deterministik
│   ├── Terrain/              # heightmap berubin, layer splat, peta hole, brush, undo, I/O PNG/RAW
│   ├── Vegetation/           # `.simveg`, sebaran Poisson deterministik, peta kepadatan, suntingan tangan
│   ├── Render/               # IViewportRenderer + StubRenderer, frame graph & frustum culling (E8.1)
│   ├── SceneView/            # `World` → `ViewportScene`: dipakai viewport editor DAN player
│   ├── EditorFramework/      # App, PanelManager, dock layout, Command/Undo, widget
│   ├── AIBridge/             # MCP server (JSON-RPC/HTTP), ToolRegistry, MainThreadQueue, izin
│   └── Editor/               # panel konkrit (Outliner, Inspector, Material, Particle, ...)
├── Apps/
│   ├── SimEditor/            # entry point editor
│   ├── SimHeadless/          # editor tanpa GUI + MCP server, untuk agen di CI (A4)
│   ├── SimRuntime/           # entry point player (E9) — tanpa EditorFramework, tanpa ImGui
│   └── SimCook/              # memangkas aset tak terpakai untuk dikirim (E9)
├── Resources/                # font, ikon, tema, layout dock bawaan
├── Shaders/
├── Scripts/                  # modul Lua: API engine + ekstensi editor
├── Tests/
└── docs/
```

## Aturan dependensi

Panah berarti "boleh link ke". Tidak ada panah balik. Dijaga lewat target CMake —
kalau seseorang menambah `#include` yang melanggar, build gagal karena include
directory-nya memang tidak tersedia.

```
                       ┌──────────────┐
                       │     Core     │  (tidak bergantung apa pun)
                       └──────┬───────┘
             ┌────────────┬───┴────┬─────────────┐
             ▼            ▼        ▼             ▼
        ┌─────────┐ ┌──────────┐ ┌──────┐  ┌──────────┐
        │ Reflect │ │ Platform │ │ RHI  │  │  Asset   │
        └────┬────┘ └────┬─────┘ └──┬───┘  └────┬─────┘
             │           │          │            │
             ▼           │          ▼            │
        ┌─────────┐      │   ┌─────────────┐     │
        │  Scene  │◄─────┼───┤ ImGuiIntegr.│     │
        └────┬────┘      │   └──────┬──────┘     │
             │           │          │            │
             ▼           ▼          ▼            ▼
        ┌────────┐   ┌───────────────────────────────┐
        │ Script │──►│       EditorFramework         │
        └────────┘   └───────────────┬───────────────┘
                          ┌──────────┴──────────┐
                          ▼                     ▼
                   ┌─────────────┐       ┌────────────┐
                   │   Editor    │──────►│  AIBridge  │  (MCP server)
                   └──────┬──────┘       └────────────┘
                          ▼
                   ┌────────────┐
                   │   Render   │  (hanya antarmuka)
                   └────────────┘
```

Aturan keras:

| Modul | Boleh lihat Vulkan? | Catatan |
|---|---|---|
| `Core`, `Reflect`, `Scene`, `Asset`, `Script` | ❌ | murni CPU/data, bisa di-unit-test headless |
| `RHI`, `ImGuiIntegration` | ✅ | satu-satunya tempat `vulkan.h` boleh muncul |
| `Render` | ✅ di `src/`, ❌ di `include/` | header publik hanya antarmuka + POD |
| `SceneView`, `EditorFramework`, `Editor`, `AIBridge` | ❌ | bicara ke `Render` lewat antarmuka |

## Seam utama

Lima seam ini yang membuat "editor dulu, rendering kemudian" berhasil — dan yang
sekaligus membuat track AI hampir gratis. Semuanya harus ada sejak E1/E2 walaupun
implementasinya masih kosong.

### 1. `IViewportRenderer` — batas editor ↔ rendering

```cpp
// Code/Render/include/Sim/Render/IViewportRenderer.h  — TIDAK boleh include vulkan
namespace sim::render {

struct ViewportDesc {
    uint32_t width = 0, height = 0;
    Camera   camera;                 // view/proj, POD dari Core/Math
    DrawMode mode = DrawMode::Material;  // Material / Unlit / Clay / MaterialWireframe / Wireframe
};

// Apa yang harus digambar frame ini. Diisi ulang tiap frame oleh panel,
// tidak menyimpan pointer ke objek scene.
struct ViewportScene {
    std::span<const MeshInstance>  meshes;
    std::span<const LineSegment>   lines;    // grid, bounding box, gizmo bantu
    std::span<const BillboardIcon> icons;    // ikon lampu/kamera di viewport
    EnvironmentDesc                environment;
};

class IViewportRenderer {
public:
    virtual ~IViewportRenderer() = default;
    virtual void Resize(uint32_t w, uint32_t h) = 0;
    virtual void Render(const ViewportDesc&, const ViewportScene&) = 0;
    // Handle buram yang bisa dilempar ke ImGui::Image(). Diterjemahkan
    // jadi VkDescriptorSet di dalam implementasi.
    virtual TextureHandle ColorTarget() const = 0;
    // Picking dibaca dari buffer id; StubRenderer memakai ray-vs-AABB di CPU.
    virtual EntityId      Pick(uint32_t x, uint32_t y) const = 0;
};

} // namespace sim::render
```

`StubRenderer` (E1) mengisi ini dengan: clear color, grid tanah, wireframe AABB per
mesh, ikon billboard, dan picking ray-vs-AABB di CPU. Cukup untuk menjalankan
seluruh E4..E7. `VulkanRenderer` (E8) menggantinya lewat satu baris di factory.

### 2. Jembatan tekstur RHI → ImGui

```cpp
// Code/ImGuiIntegration/include/Sim/ImGuiIntegration/TextureBridge.h
ImTextureID AcquireImGuiTexture(const rhi::Image&, rhi::Sampler);
void        ReleaseImGuiTexture(ImTextureID);   // ditunda sampai frame in-flight selesai
```

Satu-satunya jalan gambar dari GPU masuk ke UI: viewport, thumbnail asset browser,
preview material, preview partikel. Rilis harus tertunda N frame (N = jumlah frame
in-flight), kalau tidak ada use-after-free saat panel ditutup.

### 3. Command — satu-satunya jalan memodifikasi data

```cpp
// Code/EditorFramework/include/Sim/Editor/Command.h
class ICommand {
public:
    virtual ~ICommand() = default;
    virtual void        Do()   = 0;
    virtual void        Undo() = 0;
    virtual const char* Name() const = 0;
    // Untuk drag gizmo/slider: gabungkan langkah beruntun jadi satu entri undo.
    virtual bool        MergeWith(const ICommand&) { return false; }
};
```

Panel tidak pernah menulis `transform.position = x`. Panel memanggil
`history.Execute<SetFieldCommand>(entity, field, newValue)`. Konsekuensinya:
undo/redo, dirty-flag dokumen, checkpoint/rollback untuk agen AI, dan (nanti)
multi-user/replay semuanya jadi otomatis. Aturan ini berlaku sama untuk panel,
untuk script Lua, dan untuk tool MCP — tidak ada pengecualian.

### 4. Reflection — satu deskripsi, tiga pemakai

```cpp
// Pendaftaran tipe (Code/Scene/src/Components.cpp)
SIM_REFLECT(TransformComponent) {
    Type<TransformComponent>("Transform")
        .Field("position", &TransformComponent::position)
        .Field("rotation", &TransformComponent::rotation).Attr(Attr::Degrees{})
        .Field("scale",    &TransformComponent::scale).Attr(Attr::Min{0.0001f});
}
```

Dipakai oleh **empat** konsumen:
- **Inspector** — menggambar widget yang sesuai tipe field + atribut (slider, drag,
  color picker, pemilih aset) tanpa kode per-komponen.
- **Serialisasi** — `ToJson`/`FromJson` generik.
- **Lua** — `entity.transform.position = vec3(1,2,3)` dari satu registry yang sama.
- **MCP (AIBridge)** — JSON Schema untuk tool `entity.modify` dibangkitkan dari
  `TypeDesc`, dan resource `simengine://docs/components` adalah dump registry ini.

Efeknya paling terasa di E7: Particle Editor punya puluhan parameter modul; dengan
reflection, menambah parameter = satu baris, dan UI, simpan/muat, akses Lua, serta
kemampuan agen AI mengaturnya ikut otomatis.

### 5. `MainThreadQueue` — batas thread ↔ state editor

```cpp
// Code/Core/include/Sim/Core/MainThreadQueue.h
template <class T>
std::future<T> Submit(std::function<T()> work);   // dipanggil dari thread mana pun
void           Drain();                            // dipanggil sekali per frame di main thread
```

Semua yang datang dari luar main thread — hasil import aset, event file watcher,
dan **setiap request MCP** — masuk lewat sini dan dieksekusi di titik tetap dalam
frame. Handler MCP yang menyentuh `World`, ImGui, atau Vulkan langsung dari thread
jaringan adalah bug; di build Debug hal itu tertangkap assert pemeriksa thread id.

## Model thread

E0..E7 single-thread untuk logika editor. Empat hal berjalan di luar main thread:

- **Import aset & thumbnail** — thread pool `Core::JobSystem`, hasil di-marshal balik
  ke main thread lewat antrian.
- **File watcher** — thread sendiri, mengirim event ke antrian.
- **Server MCP** — thread jaringan menerima request, tapi tidak pernah menyentuh
  state; semuanya lewat `MainThreadQueue` (seam #5).
- **GPU** — asinkron secara alami; disinkronkan lewat fence per frame in-flight.

Renderer multi-thread (command buffer recording paralel) baru dipertimbangkan di E8.

## Lifecycle satu frame editor

```
SDL_PollEvent          → Platform::Input, ImGui_ImplSDL3_ProcessEvent
MainThreadQueue::Drain → hasil import, event file watcher, hasil job, request MCP
ImGui NewFrame         → per-monitor DPI dicek, font di-rebuild kalau skala berubah
EditorApp::Tick        → shortcut global, command yang tertunda, autosave
PanelManager::Draw     → dockspace, menubar, toolbar, tiap panel menggambar dirinya
  └─ ViewportPanel     → isi ViewportScene, panggil IViewportRenderer::Render,
                          lalu ImGui::Image(renderer.ColorTarget())
ImGui Render           → draw data utama
RHI::Frame             → acquire, record, submit, present
ImGui UpdatePlatformWindows / RenderPlatformWindowsDefault  → jendela multi-viewport
```

Urutan penting: viewport merender ke offscreen target **sebelum** draw list ImGui
disubmit, karena tekstur hasilnya dipakai di draw list yang sama.

## Multi-monitor

Dear ImGui branch docking menyediakan `ImGuiConfigFlags_ViewportsEnable`: panel yang
ditarik keluar dockspace menjadi jendela OS asli, bisa dipindah ke monitor lain.
**Dimatikan untuk sementara** (`ImGuiLayerDesc::enableViewports`): panel yang
mendapat viewport sendiri menggantung. Jalurnya tetap ada dan bisa dinyalakan
lewat `SIM_ENABLE_VIEWPORTS=1` untuk menguji perbaikan.
Yang harus kita tangani:

- **DPI per monitor.** `io.ConfigDpiScaleFonts = true` dan
  `io.ConfigDpiScaleViewports = true`. Saat jendela pindah ke monitor dengan skala
  berbeda, atlas font di-rebuild. Rebuild atlas berarti tekstur font lama harus
  dirilis lewat mekanisme tunda yang sama dengan `TextureBridge`.
- **Satu swapchain per viewport.** `ImGui_ImplVulkan` sudah menanganinya lewat
  `ImGui_ImplVulkanH_Window` per viewport; RHI kita harus mengizinkan pembuatan
  surface tambahan dan tidak berasumsi hanya ada satu swapchain.
- **Simpan posisi jendela.** Layout tersimpan (`imgui.ini`) sudah menyimpan
  posisi/ukuran viewport; kita tambahkan validasi saat startup — kalau monitor yang
  dulu dipakai sudah tidak ada, jendela ditarik kembali ke monitor utama.
