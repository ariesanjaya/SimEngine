# SimEngine — Roadmap

Game engine 3D dengan C++20 di inti dan Lua 5.4 sebagai bahasa runtime/gameplay.
Editor dibangun di atas Dear ImGui (branch docking) dengan multi-viewport sehingga
panel bisa ditarik keluar jendela utama ke monitor lain.

Dokumen ini adalah peta besar. Detail per milestone ada di:

- [PLAN-EDITOR.md](PLAN-EDITOR.md) — E0..E7, fase editor (fokus sekarang)
- [PLAN-AI.md](PLAN-AI.md) — A0..A4, engine sebagai MCP server untuk agentic AI (paralel)
- [PLAN-RENDER.md](PLAN-RENDER.md) — E8..E9, fase rendering & runtime
- [ARCHITECTURE.md](ARCHITECTURE.md) — modul, aturan dependensi, seam
- [EDITOR-PANELS.md](EDITOR-PANELS.md) — spesifikasi tiap panel
- [DEPENDENCIES.md](DEPENDENCIES.md) — daftar dependensi + versi terkunci

---

## Prinsip yang menentukan urutan kerja

**Editor dulu, rendering kemudian.** Ini bukan sekadar preferensi; ini keputusan
arsitektur. Konsekuensinya harus dijaga sejak hari pertama:

1. **Tidak ada satu pun panel editor yang boleh menyentuh Vulkan.** Semua panel
   bicara ke `IViewportRenderer` — antarmuka yang menerima deskripsi scene dan
   mengembalikan sebuah tekstur. Sampai E8, implementasinya adalah `StubRenderer`
   (clear color + grid + gizmo garis). Setelah E8, implementasinya diganti tanpa
   satu baris pun panel berubah.
2. **Semua editor mengarang data, bukan piksel.** Material Editor menghasilkan
   graph material yang tersimpan sebagai aset; ia tidak perlu tahu cara meng-compile
   shader. Particle Editor menghasilkan definisi emitter; ia tidak perlu simulasi GPU.
   Preview visual boleh kasar/salah sampai E8 — yang penting datanya benar dan
   round-trip simpan/muat utuh.
3. **Reflection adalah satu sumber kebenaran.** Deskripsi tipe dipakai bertiga:
   menggambar Inspector, serialisasi JSON, dan binding Lua. Menambahkan field baru
   ke komponen berarti satu baris di deskriptor tipe, bukan tiga tempat berbeda.
4. **Setiap perubahan data lewat Command.** Tidak ada panel yang menulis langsung
   ke scene. Undo/redo jadi gratis dan konsisten di semua editor.

Kalau keempat aturan ini dipegang, E8 (rendering) adalah pekerjaan mengisi ruang
kosong yang sudah berbentuk — bukan pembongkaran.

Keempat aturan yang sama juga yang membuat **track AI (A0..A4)** hampir gratis:
agen mengubah data lewat Command yang sama (jadi bisa di-undo), membaca skema tipe
dari reflection yang sama (jadi komponen baru otomatis terjangkau), dan "melihat"
hasil kerjanya lewat `IViewportRenderer` yang sama. Lihat
[PLAN-AI.md](PLAN-AI.md).

---

## Dua fase besar

### Fase 1 — Editor (E0 → E7)

Target akhir: aplikasi editor yang bisa membuka project, menyusun level, mengelola
aset, dan mengarang material/partikel/terrain/vegetasi/animasi — semuanya tersimpan
ke disk dan bisa dimuat ulang persis. Viewport menampilkan grid + wireframe/proxy,
belum PBR.

| Milestone | Isi | Status |
|-----------|-----|--------|
| E0 | Fondasi build: CMake + clang + presets + dependensi terkunci | ✅ |
| E1 | Platform (SDL3) + RHI minimal (Vulkan) + shell ImGui docking/multi-monitor | ✅ |
| E2 | Editor framework: panel, layout, command/undo, seleksi, shortcut, console | ✅ |
| E3 | Reflection + scene model (EnTT) + serialisasi + project system | ✅ |
| E4 | **Level Editor**: outliner, inspector, viewport, gizmo, picking, prefab | ✅ |
| E5 | **Asset Browser** + asset database (GUID, .meta, thumbnail, import) | ⏳ |
| E6 | Runtime Lua: sol2, script component, hot reload, konsol REPL, editor scripting | ⏳ |
| E7 | **Material / Particle / Terrain / Vegetation / Animation Editor** | ⏳ |

### Fase 2 — Rendering & runtime (E8 → E9)

| Milestone | Isi | Status |
|-----------|-----|--------|
| E8 | Renderer nyata: PBR, shadow, sky, post-process, terrain, vegetasi, partikel, skinning | ⏳ |
| E9 | SimRuntime (player), cook/packaging, PhysX, audio | ⏳ |

### Track paralel — AI / MCP (A0 → A4)

Berjalan bersamaan dengan fase editor, tidak menunggu selesai. Detail di
[PLAN-AI.md](PLAN-AI.md).

| Milestone | Isi | Butuh |
|-----------|-----|-------|
| A0 | MCP server (HTTP localhost) + marshaling ke main thread + tool dasar | E2 |
| A1 | Tool scene/entity/viewport-capture + checkpoint & rollback | E3, E4 |
| A2 | Tool aset & project (search, import, create, thumbnail) | E5 |
| A3 | Tool authoring: Lua, material, particle, terrain, vegetation, animation | E6, E7 |
| A4 | Panel AI Assistant (MCP client) + `SimHeadless` untuk agen di CI | A3 |

---

## Kenapa pemisahan ini bisa bekerja

Editor butuh Vulkan hanya untuk dua hal: menggambar UI ImGui, dan menyediakan satu
tekstur untuk panel viewport. Keduanya sudah tercakup oleh `sdl3_vulkan.cpp` yang
jadi acuan — sekitar 250 baris setup yang di E1 kita pecah jadi kelas.

Yang mahal di renderer (material system, shadow, culling, instancing, GPU particle)
tidak dibutuhkan untuk mengarang data. Menunda semuanya ke E8 memberi dua keuntungan:
saat kita akhirnya menulis renderer, format data material/partikel/terrain sudah
stabil dan teruji lewat pemakaian nyata di editor; dan kita tidak menulis renderer
dua kali karena format datanya ternyata salah.

---

## Yang tidak boleh ditunda

Meskipun rendering ditunda, tiga hal ini harus benar sejak awal karena mengubahnya
belakangan mahal:

- **Format aset & GUID.** Referensi antar-aset memakai GUID stabil, bukan path.
  Rename/pindah file tidak boleh memutus referensi. (E5)
- **Command/undo.** Menambahkan undo ke kode yang sudah menulis data langsung
  berarti menulis ulang setiap panel. (E2)
- **Batas modul.** `Code/Editor` tidak boleh `#include <vulkan/vulkan.h>`. Dijaga
  oleh aturan link CMake, bukan oleh disiplin. (E1)

---

## Cara kerja yang disarankan

Satu milestone = satu branch = satu PR. Setiap milestone punya kriteria terima yang
bisa dicek manual dalam < 5 menit (tertulis di PLAN-EDITOR.md). Tidak lanjut ke
milestone berikutnya sebelum kriteria terima yang sekarang lulus, karena setiap
milestone jadi fondasi milestone sesudahnya.

Test otomatis (doctest) wajib untuk: Reflect, serialisasi, command/undo, asset
database, dan binding Lua. UI tidak di-unit-test; diverifikasi lewat kriteria terima
manual.
