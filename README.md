# SimEngine

Game engine 3D dengan inti C++20 dan Lua 5.4 sebagai bahasa runtime. Editornya
dibangun di atas Dear ImGui (branch docking) dengan multi-viewport — untuk
sementara dimatikan, lihat catatan di bawah — sehingga
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
| E4 | Level editor: gizmo, picking, box-select, outliner, multi-edit | ✅ |
| E5 | Asset database (GUID stabil), Asset Browser, thumbnail, drag-drop | ✅ |
| E6 | Lua + visual scripting (graph dikompilasi ke Lua) | ✅ |
| E7 | Editor khusus: material, particle, terrain, vegetation, animation | ⏳ |
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
| `~/.simengine/layout.ini` | susunan dock dan posisi panel mengambang |
| `~/.simengine/panels.json` | panel mana yang terbuka |
| `~/.simengine/shortcuts.json` | pintasan yang diubah dari bawaannya |
| `~/.simengine/Logs/editor.log` | log lengkap sesi terakhir |
| `~/.simengine/Assets/` | folder aset yang dipantau editor |
| `~/.simengine/ThumbnailCache/` | thumbnail hasil dekode, berkunci hash isi berkas |

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
| Focus ke seleksi | **F** |
| Pilih objek | **klik kiri** |
| Tambah / kurangi seleksi | **Ctrl** atau **Shift** + klik |
| Seleksi kotak | **drag kiri** di ruang kosong |
| Alat: pilih / pindah / putar / skala | **Q** / **W** / **E** / **R** |
| Ruang world ↔ local | **X** |
| Snapping | tombol magnet di bilah kiri; **klik kanan** padanya untuk mengatur kelipatan |

Menyeret gizmo — berapa pun frame yang dilaluinya — menghasilkan **tepat satu
entri undo**. Dengan snapping menyala, yang dibulatkan adalah nilai akhirnya,
bukan selisih seretan, sehingga hasilnya selalu kelipatan persis dan galat tidak
pernah menumpuk. Hanya sumbu yang benar-benar digerakkan yang dibulatkan.

### Menyunting banyak objek sekaligus

Inspector menampilkan komponen yang dimiliki **seluruh** seleksi. Field yang
nilainya berbeda antar objek ditandai `—`; menyuntingnya berlaku untuk semuanya,
tetapi hanya field yang benar-benar disentuh yang ikut berubah — sepuluh lampu
berwarna berbeda tetap berbeda warnanya ketika intensitasnya diseragamkan.

Viewport terkunci di dockspace dan tidak bisa dilepas menjadi jendela tersendiri.
Panel yang dilepas mendapat swapchain dan present sendiri; untuk gambar seukuran
viewport penuh biayanya jauh di atas panel berisi teks.

> **Panel tidak bisa keluar menjadi jendela OS untuk sementara.** Panel yang
> mendapat viewport sendiri terlihat menggantung, dan gejalanya tidak selalu
> muncul — dua hal yang bersama-sama membuatnya sulit dilacak sementara editor
> tetap dipakai bekerja. Sampai penyebabnya ketemu, `ImGuiConfigFlags_ViewportsEnable`
> dimatikan: panel masih bisa dilepas dari dock dan mengambang, tapi tetap di
> dalam jendela editor. Untuk menguji perbaikannya, jalankan dengan
> `SIM_ENABLE_VIEWPORTS=1` — tanpa membangun ulang.

### Aset

Setiap berkas di `~/.simengine/Assets` mendapat berkas `.meta` di sebelahnya
berisi **GUID**. Level menyimpan GUID itu, bukan nama berkas — sehingga mengganti
nama atau memindahkan aset tidak menyentuh satu pun level. Berkas `.meta` wajib
ikut masuk kontrol versi bersama asetnya; kehilangannya memberi aset identitas
baru dan memutus semua yang merujuknya.

Perubahan dari luar editor terdeteksi lewat pemantau sistem berkas (inotify di
Linux, `ReadDirectoryChangesW` di Windows) dan muncul di bawah dua detik tanpa
restart — termasuk ketika berkas ditimpa, yang ikut menyegarkan thumbnail-nya.

Menyeret aset: ke **viewport** membuat entity, ke **field Inspector** menetapkan
rujukannya (bisa di-undo), ke **folder** memindahkan berkasnya.

Proyek yang belum punya folder `Assets/Meshes` mendapat **aset contoh** disemai
ke sana dari `Resources/Meshes` sekali saja: model shader ball, berikut berkas
`.meta` dan lisensinya. Syaratnya keberadaan foldernya, bukan berkasnya satu per
satu — aset contoh yang sengaja dihapus tidak boleh muncul lagi setiap editor
dijalankan. Level contoh lalu memasang `Meshes/shaderBall.fbx` pada entity
**Shader Ball**, dicari lewat jalur; tidak ada berarti rujukannya kosong, bukan
kesalahan.

Modelnya public domain (Unlicense), dibuat Mat Makin untuk
[derkreature/ShaderBall](https://github.com/derkreature/ShaderBall).

Sampai renderer mesh datang di E8, viewport tetap menggambarnya sebagai kotak
kawat satuan: tidak ada importer yang membaca geometrinya, dan `SceneView`
memakai AABB tetap untuk setiap mesh. Yang sudah benar sekarang adalah
rujukannya — tersimpan sebagai GUID di berkas level, dan siap dipakai begitu
renderer-nya ada.

### Skrip

`ScriptComponent` merujuk sebuah `.lua` yang mengembalikan tabel berisi
`OnStart(self)` dan `OnUpdate(self, dt)`. **F5** menjalankannya, **Shift+F5**
berhenti — dan scene kembali persis seperti sebelum Play, karena keadaannya
dicuplik sebelum satu baris skrip pun berjalan.

Menyimpan berkas skrip selagi Play berlangsung memuat ulang skrip itu saja,
**tanpa menghentikan permainan**. Tabel `self.state` tiap instance dipertahankan;
kalau ikut hilang, memuat ulang skrip tidak ada bedanya dengan memulai ulang.
Terukur 22 ms dari berkas ditulis sampai kode barunya berjalan.

Komponen dijangkau lewat namanya — `sim.get_component(entity, "Transform")` —
dan daftarnya dibangkitkan dari reflection, jadi komponen baru ikut terjangkau
tanpa menambah satu baris pun di sisi Lua. Panel **Lua Console** menyediakan REPL
di state yang sama, lengkap dengan riwayat, pelengkapan **Tab**, dan traceback.

`sim.vec3` dan `sim.quat` bekerja pada **bentuk tabel yang sama** dengan yang
dikembalikan `get_component`, jadi nilai yang dibaca dari komponen bisa langsung
dihitung dan langsung ditulis kembali — tidak ada tipe kedua yang harus
dikonversi bolak-balik di perbatasan.

```lua
local Spin = {}

-- Muncul di Inspector. Tipenya disimpulkan dari nilai bawaannya, jadi tidak
-- ada tipe yang dituliskan dua kali dan bisa bertentangan.
Spin.properties = { speed = 1.5, clockwise = true, label = "spinner" }

function Spin:OnUpdate(dt)
    local t = sim.get_component(self.entity, "Transform")
    t.rotation = sim.axis_angle(sim.up(), sim.time() * self.props.speed)
    sim.set_component(self.entity, "Transform", t)
end

return Spin
```

Yang tersimpan di entity hanyalah properti yang **benar-benar disunting**;
sisanya membaca bawaan dari berkas skrip. Karena itu mengubah bawaan di skrip
ikut berlaku untuk entity yang belum pernah disentuh, tanpa menyentuh yang sudah
disesuaikan. Menyuntingnya bisa di-undo, berlaku untuk seluruh seleksi, dan ikut
tersimpan ke berkas level.

Panel **Script Editor** menyunting berkasnya di dalam editor: **Tab** melengkapi
nama dari state Lua yang sungguh berjalan — bukan daftar yang ditulis tangan,
jadi ia tidak pernah bisa berbohong tentang apa yang ada — dan sintaksnya
diperiksa di setiap ketikan dengan *memuat* berkasnya, tidak menjalankannya.

### Skrip editor

Berkas `.lua` di `Assets/Editor` memperluas editornya sendiri. Folder terpisah
dari `Assets/Scripts` bukan sekadar kerapian: yang di sini berjalan di dalam
editor dengan akses ke riwayat undo dan panel, yang di sana berjalan saat Play.
Mencampurnya berarti skrip gameplay bisa menambah menu, dan skrip editor ikut
terbawa ke build permainan.

```lua
sim.editor.menu("Turunkan semua", function()
    local sebelum = ambil_posisi()
    -- Lewat riwayat yang sama dengan panel C++, jadi Ctrl+Z membatalkannya.
    sim.editor.command("Turunkan semua",
        function() pasang_posisi(sebelum - 1) end,
        function() pasang_posisi(sebelum) end)
end)

sim.editor.panel("Catatan", function()
    sim.ui.text("terpilih: " .. sim.editor.selection_count())
    if sim.ui.button("Kerjakan") then ... end
end)
```

`sim.editor.command` **wajib** lewat `CommandHistory` yang sama dengan panel
C++ — satu jalur tulis yang lolos dari undo sudah cukup membuat Ctrl+Z tidak
bisa dipercaya, dan pengguna tidak punya cara tahu perubahan mana yang aman.

Pemrosesan aset secara batch memakai indeks yang sama dengan Asset Browser:

```lua
for _, aset in ipairs(sim.editor.assets("Texture")) do
    sim.editor.rename_asset(aset.path, aset.name:gsub("^tmp_", ""))
end
```

Mengganti nama dan memindahkan aset sengaja **tidak** lewat undo — sama seperti
di Asset Browser. Yang berubah adalah berkas di disk, dan riwayat undo hanya
menjanjikan pembatalan yang tidak bisa ditepatinya begitu berkas itu disentuh
dari luar editor. GUID-nya tidak ikut berubah, jadi tidak ada level yang putus.

`sim.ui.*` hanya sah di dalam callback panel; di luar itu ia melempar kesalahan
Lua alih-alih menggambar ke jendela sembarang. Menyimpan berkasnya memuat ulang
seluruh skrip editor tanpa restart: item menu yang dihapus dari berkasnya ikut
hilang, sementara panel dengan judul yang sama dipakai ulang beserta posisi
dock dan keadaan buka/tutupnya.

### Visual scripting

`GraphComponent` merujuk sebuah `.simgraph` — daftar node, pin, dan koneksi
dalam JSON berversi seperti `.simlevel`. Yang berjalan saat Play bukan graph-nya,
melainkan **Lua hasil kompilasinya**.

Itu pilihan yang menentukan. Alternatifnya mesin graph yang menelusuri node satu
per satu saat runtime: dua jalur eksekusi yang harus dijaga sama perilakunya, dan
yang kedua selalu lebih lambat sekaligus lebih sulit di-debug. Dengan
mengompilasi, yang berjalan hanya satu runtime — graph adalah *penulis kode*,
bukan penafsir. Profiler, traceback, hot reload, sandbox, dan properti-di-
Inspector yang sudah ada langsung berlaku, tanpa satu baris pun kode khusus.

Katalog node-nya **dibangkitkan dari reflection**: setiap komponen terdaftar
menghasilkan sepasang node Get/Set dengan satu pin per field. Komponen baru — atau
field baru pada komponen lama — langsung muncul di palet tanpa pekerjaan tambahan.

Keluarannya sengaja layak dibaca. Graph "putar entity saat OnUpdate" di atas
menjadi:

```lua
function Graph:OnUpdate(dt)
    self:__ensure()
    -- node a3f19c (Set Transform)
    sim.set_component(self.entity, "Transform",
        { rotation = sim.axis_angle(sim.up(), (sim.time() * self.state.speed)) })
end
```

Bukan kemewahan: itulah yang membuat graph bisa di-debug dengan alat yang sama
seperti skrip biasa, dan membuat pengguna bisa lulus dari visual scripting ke Lua
tanpa jurang. Peta sumber node ↔ baris membuat error runtime menyorot node
penyebabnya di kanvas, bukan menyerahkan nomor baris di berkas yang tidak pernah
ia lihat.

Memuat level yang memakai graph **tidak mengompilasi apa pun** — yang dipakai
`.lua` yang sudah ada di cache. Menyunting graph di editor langsung berlaku saat
Play berikutnya, tanpa langkah build manual. Keduanya memanggil compiler yang
sama persis; hasil yang berbeda antara editor dan runtime adalah kelas bug yang
tidak boleh dibuka.

**Graph bisa dipakai ulang sebagai template.** Beri sebuah graph daftar Input
dan Output, dan graph lain bisa memakainya sebagai satu node. Compiler
menyisipkannya sebagai fungsi Lua:

```lua
-- Subgraph: Skala
local function sub_skala_1(self, amount)
    return (amount * 2)
end

function Graph:OnUpdate(dt)
    local scaled_1 = sub_skala_1(self, self.state.speed)
    ...
end
```

Satu definisi berapa pun kali dipanggil — dan karena yang tersimpan adalah
rujukan, bukan salinan, memperbaiki template memperbaiki setiap pemakainya.
Menyunting template membuat hasil kompilasi pemakainya usang, jadi Play
berikutnya menjalankan yang baru tanpa ada yang perlu diingat.

Siklus pada pin data dan lingkar pada pin exec ditolak saat kompilasi dengan
pesan yang menunjuk node penyebabnya — dan kompilernya **kembali**, bukan
menggantung membawa serta editor.

### Kunci laju frame

Editor mengunci laju frame ke **refresh rate terendah** di antara semua monitor
yang terpasang — 60 Hz + 100 Hz menjadi 60, 100 Hz + 144 Hz menjadi 100.
Alasannya multi-viewport: sebuah panel bisa berada di monitor mana pun dan ikut
digambar pada frame yang sama, dan menyamakan ke monitor terlambat membuat semua
panel bergerak konsisten alih-alih sebagian patah-patah. Aturannya dipertahankan
selagi multi-viewport dimatikan — jendela editor sendiri tetap bisa dipindahkan
ke monitor mana pun. Laju dihitung ulang otomatis
kalau monitor ditambah, dicabut, atau mode-nya berubah, dan nilainya beserta
monitor penyebabnya ditampilkan di status bar.

## Struktur

```
Code/          modul engine (Core, Reflect, Scene, Assets, Platform, RHI,
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
- [RENDER-OPENPBR.md](docs/RENDER-OPENPBR.md) — lobe OpenPBR yang belum ada (subsurface, transmission, thin film) dan cara menjalankannya real time
- [PLAN-MATERIALX.md](docs/PLAN-MATERIALX.md) — impor material dari 3ds Max lewat dokumen MaterialX, dan blok parameter Max di dalam FBX sebagai cadangannya
- [EDITOR-PANELS.md](docs/EDITOR-PANELS.md) — spesifikasi tiap panel
- [DEPENDENCIES.md](docs/DEPENDENCIES.md) — versi terkunci dan alasannya
