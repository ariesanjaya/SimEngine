# SimEngine

Game engine 3D dengan inti C++20 dan Lua 5.4 sebagai bahasa runtime. Editornya
dibangun di atas Dear ImGui (branch docking) dengan multi-viewport, sehingga
panel bisa ditarik keluar jendela utama ke monitor lain.

Urutan pengerjaan: **editor dulu, rendering kemudian**. Alasan dan
konsekuensinya ada di [docs/ROADMAP.md](docs/ROADMAP.md).

## Status

| Milestone | Isi | Status |
|---|---|---|
| E0 | Build system CMake + clang + dependensi terkunci | ✅ |
| E1 | Platform SDL3, RHI Vulkan, shell ImGui docking/multi-monitor | ✅ |
| E2 | Editor framework: command/undo, seleksi, shortcut, widget | ✅ |
| E3 | Reflection, scene EnTT, serialisasi `.simlevel`, prefab, project | ✅ |
| E4–E7 | Level editor, asset browser, Lua, editor khusus | ⏳ |
| A0–A4 | Engine sebagai MCP server untuk agentic AI | ⏳ |
| E8–E9 | Renderer PBR, runtime, packaging | ⏳ |

## Membangun

Prasyarat: clang 18+, CMake 3.25+, Ninja, dan Vulkan SDK.

```sh
source /home/arie/SDK/vulkan-sdk-1.4.350.1/setup-env.sh

cmake --preset linux-clang-debug        # konfigurasi pertama mengunduh dependensi
cmake --build --preset linux-clang-debug
ctest --preset linux-clang-debug

./build/linux-clang-debug/bin/SimEditor
```

Preset lain: `linux-clang-release`, `linux-clang-asan`, `linux-clang-tsan`.

Konfigurasi kedua dan seterusnya tidak menyentuh jaringan. Untuk memaksa build
sepenuhnya offline: tambahkan `-DFETCHCONTENT_FULLY_DISCONNECTED=ON`.

Semua dependensi diambil lewat `FetchContent` dengan versi terkunci — daftar
lengkapnya di [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md). Satu-satunya yang
diambil dari sistem adalah Vulkan SDK.

## Menjalankan editor

Berkas yang ditulis editor saat berjalan:

| Path | Isi |
|---|---|
| `~/.simengine/layout.ini` | susunan dock, termasuk posisi jendela di monitor kedua |
| `~/.simengine/panels.json` | panel mana yang terbuka |
| `~/.simengine/shortcuts.json` | pintasan yang diubah dari bawaannya |
| `~/.simengine/Logs/editor.log` | log lengkap sesi terakhir |

Hapus `layout.ini` untuk kembali ke tata letak bawaan (atau **View → Reset Layout**).

### Kontrol viewport

| Aksi | Kontrol |
|---|---|
| Orbit | **Alt + drag kiri** |
| Geser (pan) | **drag tengah** |
| Zoom | **roda** |
| Terbang | **tahan klik kanan** lalu **W/A/S/D**, **Q/E** turun-naik |
| Kecepatan terbang | **roda** sambil klik kanan ditahan |
| Cepat / pelan | **Shift** (4×) / **Ctrl** (¼×) sambil terbang |
| Kembali ke titik asal | **F** |

Viewport terkunci di dockspace dan tidak bisa dilepas menjadi jendela tersendiri.
Panel yang dilepas mendapat swapchain dan present sendiri; untuk gambar seukuran
viewport penuh biayanya jauh di atas panel berisi teks.

### Kunci laju frame

Editor mengunci laju frame ke **refresh rate terendah** di antara semua monitor
yang terpasang — 60 Hz + 100 Hz menjadi 60, 100 Hz + 144 Hz menjadi 100. Dengan
multi-viewport, sebuah panel bisa berada di monitor mana pun dan ikut digambar
pada frame yang sama; menyamakan ke monitor terlambat membuat semua panel
bergerak konsisten alih-alih sebagian patah-patah. Laju dihitung ulang otomatis
kalau monitor ditambah, dicabut, atau mode-nya berubah, dan nilainya beserta
monitor penyebabnya ditampilkan di status bar.

## Struktur

```
Code/          modul engine (Core, Reflect, Scene, Platform, RHI,
               ImGuiIntegration, Render, EditorFramework, Editor, Script)
Apps/          entry point (SimEditor)
Shaders/       sumber GLSL/Slang, dikompilasi ke SPIR-V saat build
Resources/     font dan aset editor, disalin ke folder keluaran
Tests/         unit test doctest
docs/          rencana dan arsitektur
```

Aturan modul yang ditegakkan build system: `Editor` dan `EditorFramework`
me-link inti Dear ImGui saja, tanpa header Vulkan. Semua rendering diakses lewat
antarmuka `IViewportRenderer`. Rinciannya di
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Dokumentasi

- [ROADMAP.md](docs/ROADMAP.md) — peta besar dan prinsip yang menentukan urutan kerja
- [ARCHITECTURE.md](docs/ARCHITECTURE.md) — modul, aturan dependensi, lima seam utama
- [PLAN-EDITOR.md](docs/PLAN-EDITOR.md) — E0..E7 dengan kriteria terima
- [PLAN-AI.md](docs/PLAN-AI.md) — A0..A4, engine sebagai MCP server
- [PLAN-RENDER.md](docs/PLAN-RENDER.md) — E8..E9
- [EDITOR-PANELS.md](docs/EDITOR-PANELS.md) — spesifikasi tiap panel
- [DEPENDENCIES.md](docs/DEPENDENCIES.md) — versi terkunci dan alasannya
