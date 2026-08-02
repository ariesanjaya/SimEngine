# Plan Implementasi Editor (E0 → E7)

Setiap milestone punya: **tujuan**, **pekerjaan**, dan **kriteria terima** yang bisa
diperiksa manual. Milestone dianggap selesai hanya kalau semua kriteria terima lulus.

Perkiraan waktu memakai satuan "sesi kerja" (≈ setengah hari fokus), bukan tanggal.

---

## E0 — Fondasi build · ~2 sesi · ✅ SELESAI (2 Agustus 2026)

**Tujuan.** `cmake --preset linux-clang-debug && cmake --build --preset ...`
menghasilkan binari kosong yang jalan, dengan semua dependensi terkunci versinya.

**Pekerjaan**

- `CMakeLists.txt` root: C++20, `CMAKE_EXPORT_COMPILE_COMMANDS`, larangan build
  in-source, opsi `SIM_BUILD_TESTS` / `SIM_BUILD_EDITOR` / `SIM_WERROR` / `SIM_WITH_TRACY`.
- `CMakePresets.json`: `linux-clang-debug`, `linux-clang-release`, `linux-clang-asan`,
  `linux-clang-tsan`. Generator Ninja, output `build/<preset>`.
- `cmake/SimTargets.cmake`: helper `sim_add_library(nama ...)` yang menyeragamkan
  warning (`-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor` dan
  kawan-kawannya), standar bahasa, properti folder, dan alias `Sim::Nama`.
  `-Wconversion`/`-Wold-style-cast` dipisah di balik `SIM_STRICT_WARNINGS`
  (bawaan OFF) — keduanya sangat berisik di kode yang berurusan dengan ImGui dan
  Vulkan, dan menyalakannya sejak awal hanya melatih kebiasaan mengabaikan
  peringatan.
- `cmake/SimDeps.cmake`: FetchContent untuk seluruh dependensi (lihat
  [DEPENDENCIES.md](DEPENDENCIES.md)), semua dengan `GIT_TAG` commit/tag pasti dan
  `FETCHCONTENT_UPDATES_DISCONNECTED ON` supaya build kedua tidak menyentuh jaringan.
- `cmake/SimVulkan.cmake`: `find_package(Vulkan)`, volk sebagai loader, VMA.
- `cmake/SimShaders.cmake`: fungsi `sim_compile_shaders()` — GLSL/Slang → SPIR-V lewat
  `glslc`/`slangc` dari SDK, ikut dependency tracking sehingga edit shader memicu rebuild.
- `.clang-format` (basis LLVM, kolom 100), `.clang-tidy`, `.gitignore`, `git init`.
- `Tests/` dengan doctest dan satu test sanity supaya `ctest` hijau sejak awal.

**Kriteria terima**

1. ✅ Konfigurasi bersih dari nol (`rm -rf build/`) berhasil tanpa intervensi manual.
2. ✅ Build kedua tanpa perubahan tidak mengunduh apa pun (offline-capable).
3. ✅ `ctest --preset linux-clang-debug` hijau.
4. ✅ `compile_commands.json` ada dan dikenali clangd.

**Yang berbeda dari rencana**

- SDL memperlakukan sebagian dependensi opsional (XScrnSaver, ALSA, PulseAudio)
  sebagai wajib dan menggagalkan konfigurasi kalau paket dev-nya tidak ada.
  `SimDeps.cmake` kini memprobe sendiri lewat pkg-config dan mematikan yang
  tidak tersedia, supaya `cmake --preset` jalan di mesin bersih tanpa perlu
  `apt install` lebih dulu. Backend audio baru dibutuhkan di E9.
- `imgui` dipecah jadi dua target: `ImGui::ImGui` (inti, tanpa Vulkan/SDL) dan
  `ImGui::Backend` (backend SDL3+Vulkan). Dengan begitu aturan "Editor tidak
  boleh melihat Vulkan" ditegakkan build system, bukan disiplin.
- `glslc` menolak kombinasi `-o` dengan `-MF`, dan memperlakukan argumen kosong
  hasil generator expression yang tidak aktif sebagai berkas masukan kedua.
  `SimShaders.cmake` memakai `-MD` tanpa `-MF` dan `$<IF:...>` alih-alih dua
  genex terpisah.
- **Preset `linux-clang-asan` dan `-tsan` semula tidak bisa di-build sama sekali.**
  Clang hanya menautkan runtime sanitizer secara statis ke *executable*, tidak ke
  shared library, sedangkan SDL menautkan dirinya dengan `--no-undefined` —
  hasilnya seluruh simbol `__asan_*`/`__ubsan_*` menjadi undefined. Diperbaiki
  dengan opsi `SIM_SDL_SHARED` yang dimatikan pada kedua preset sanitizer,
  sehingga SDL ditautkan statis di sana. Menambah `CMAKE_SHARED_LINKER_FLAGS`
  saja tidak cukup.

---

## E1 — Platform + RHI + shell ImGui · ~4 sesi · ✅ SELESAI (2 Agustus 2026)

**Tujuan.** Jendela editor terbuka dengan dockspace penuh, panel bisa ditarik keluar
ke monitor kedua, font tajam di tiap DPI, dan tema gelap terpasang.

**Pekerjaan**

- `Core`: `Log` (spdlog, sink ke file + sink ke ring buffer untuk panel Console),
  `SIM_ASSERT`, `Uuid`, `Path`, `Time`, `SmallVector`, math (`glm` di-alias sebagai
  `sim::Vec3` dst supaya bisa diganti belakangan), dan `MainThreadQueue` —
  antrian yang dipakai job system, file watcher, dan nanti server MCP untuk
  mengeksekusi pekerjaan di main thread (seam #5 di ARCHITECTURE.md).
- `Platform`: `Window` (SDL3), `DisplayInfo` (enumerasi monitor, skala konten, bounds,
  refresh rate), `LowestRefreshRate()`, `Input` (state keyboard/mouse yang tidak
  bergantung ImGui), `SystemDialog`.
- `Core::FrameLimiter` + **kunci laju frame ke refresh rate terendah** di antara
  monitor yang terpasang. Vsync saja tidak cukup: ia mengunci ke laju monitor
  yang sedang menampilkan jendela, jadi editor akan berjalan 144 fps di satu
  layar dan 60 fps di layar lain. Dengan multi-viewport, panel di kedua monitor
  digambar pada frame yang sama, sehingga laju harus satu dan mengikuti yang
  terlambat. Dihitung ulang saat monitor ditambah/dicabut/berubah mode.
- `RHI`: refactor `sdl3_vulkan.cpp` jadi kelas — `Instance`, `PhysicalDevice`, `Device`,
  `Swapchain`, `FrameContext` (command pool + fence + semaphore per frame in-flight),
  `Image`/`Buffer` di atas VMA, `RenderTarget` (color+depth offscreen), `DescriptorAllocator`.
  Validation layer aktif di Debug lewat `VK_EXT_debug_utils` (bukan `debug_report` yang
  sudah usang seperti di contoh acuan).
- `ImGuiIntegration`: `ImGuiLayer` (init/shutdown/new frame/render),
  `ImGuiConfigFlags_DockingEnable | ViewportsEnable`, `ConfigDpiScaleFonts`,
  `ConfigDpiScaleViewports`; `Theme` (palet gelap mirip acuan O3DE, rounding, spacing);
  font UI **Inter** yang dibundel di `Resources/Fonts` pada 13 px, dengan font
  sistem sebagai cadangan (font bawaan ImGui hanya ASCII, dan font sistem
  berbeda antar-distribusi sehingga tata letak panel bisa bergeser);
  `TextureBridge` (lihat ARCHITECTURE.md seam #2, dengan antrian rilis tertunda).
- `Render`: header `IViewportRenderer` + `StubRenderer` (clear + grid tanah
  prosedural lewat segitiga penuh layar, dengan sumbu X/Z berwarna).
- `EditorFramework` + `Editor`: shell (menu bar, toolbar, dockspace, status bar),
  `PanelManager`, layout dock bawaan meniru acuan, dan panel Outliner /
  Inspector / Asset Browser / Viewport / Console / Statistics.

**Kriteria terima** — hasil audit 2 Agustus 2026, diverifikasi lewat input sungguhan
(XTEST) dan pengukuran piksel, bukan pengamatan sekilas.

1. ✅ `SimEditor` terbuka dengan dockspace memenuhi jendela; menu bar dan status bar tampak.
2. ✅ Panel diseret keluar jendela utama menjadi jendela OS terpisah dan bisa berada
   di monitor kedua. Terverifikasi: menyeret tab Entity Inspector menghasilkan
   jendela X11 baru di (1473, 90) — monitor atas — dengan ukuran 320×715.
3. ✅ Layout tersimpan dan dipulihkan, termasuk posisi jendela di monitor kedua.
   Diuji satu siklus penuh: lepas panel ke monitor atas → tutup bersih → buka
   lagi → jendela kembali di (1473, 90) berukuran 320×715, dan siklus kedua
   tanpa menyentuh `layout.ini` menghasilkan hasil yang sama. Sempat rusak;
   lihat "Penempatan viewport" di bawah.
4. ⚠️ Nol error validation dari kode SimEngine. Yang tersisa berasal dari Dear ImGui
   sendiri: `ImGui_ImplVulkanH_CreateOrResizeWindow` mentransisikan layout semua
   image swapchain viewport sekunder tanpa meng-*acquire*-nya lebih dulu,
   menghasilkan 1 error per image (4 error) setiap kali panel dilepas jadi jendela
   sendiri. Bukan sesuatu yang bisa kita perbaiki tanpa mem-patch ImGui.
5. ✅ Panel Viewport menampilkan tekstur `StubRenderer`, resize tidak lagi
   mengalokasi ulang tiap frame (terukur: 1336 gerakan seret → 1 alokasi ulang),
   dan **ASan bersih**. Diuji dengan build `linux-clang-asan` selama satu sesi
   penuh berisi 1347 gerakan seret pemisah dock lalu penutupan bersih:
   nol error memori, nol pelanggaran UBSan, dan nol kebocoran yang dialokasikan
   kode SimEngine. Yang dilaporkan LeakSanitizer seluruhnya berasal dari dalam
   pustaka — ~50 KB di `SDL_Init` (cache XRandR/xsettings milik X11) dan ~1,9 KB
   di loader/layer Vulkan; pada semuanya, frame kode kita hanyalah pemanggil
   terluar, tidak ada satu pun alokasi yang berasal dari kita.
6. ✅ Laju frame terkunci ke refresh rate terendah dari monitor yang terpasang,
   dan nilainya beserta monitor penyebabnya terlihat di status bar.

**Belum dikerjakan dari daftar pekerjaan E1**

Bagian ini jujur dicatat karena semuanya baru dibutuhkan di E3 ke atas, bukan karena
sudah selesai:

| Modul | Belum ada | Dibutuhkan mulai |
|---|---|---|
| `Core` | `Uuid`, `Path`, `Time`, `SmallVector` | E3 (GUID aset & serialisasi) |
| `Platform` | `Input` (state independen ImGui), `SystemDialog` | E4 (pintasan viewport), E5 (impor aset) |
| `RHI` | `Buffer`/`Image` generik di atas VMA, `DescriptorAllocator` | E8 |
| `ImGuiIntegration` | `FontManager` sebagai kelas tersendiri | — fungsinya sudah ada di `ImGuiLayer::LoadFonts` |

**Penempatan viewport — cacat yang sudah diperbaiki**

Panel yang tersimpan di monitor kedua selalu kembali ke jendela utama saat editor
dijalankan lagi. Dugaan awal — penyimpanan layout rusak, atau urutan pembuatan
viewport salah — keduanya keliru. Yang sebenarnya terjadi, terukur frame demi
frame:

| Frame | Keadaan |
|---|---|
| 1–2 | ImGui memulihkan posisi **dengan benar**: viewport sendiri di (1473, 90), monitor 1, jendela platform belum dibuat |
| 2 | `UpdatePlatformWindows()` membuat jendela SDL, meminta posisi (1473, 90); SDL melaporkan balik **(32, 1112)** — window manager menolak posisi itu dan menempatkannya di pojok jendela utama |
| 3 | ImGui menyinkronkan diri ke posisi WM, viewport pindah ke monitor 0 |
| 4 | Karena kini bertumpang tindih dengan viewport utama, ImGui menggabungkannya kembali — panel hilang |

Perbaikannya di `PanelManager::EnforceViewportPlacement()`: posisi yang diinginkan
ImGui direkam selama jendela platform belum ada, lalu dipaksakan kembali paling
banyak 8 frame setelah jendela itu muncul, dan hanya bila selisihnya melebihi
16 px. Penegakan dilepas begitu WM menerima posisinya atau begitu pengguna mulai
menyeret jendela.

Dua hal yang **tidak** memperbaikinya, dicatat supaya tidak dicoba lagi:

- `ImGuiWindowClass::ParentViewportId = 0` (meminta backend tidak menjadikan
  jendela panel sebagai transient-for jendela utama). Tidak mengubah apa pun
  dalam pengukuran, dan biayanya nyata — panel berhenti ikut minimize bersama
  jendela utama — jadi tidak dipakai.
- Memindahkan `ImGui::Render()` dan `UpdatePlatformWindows()` keluar dari cabang
  sukses swapchain. Ini **tetap dipertahankan** karena memang pemakaian ImGui
  yang benar (melewatkannya satu frame bisa membuat viewport dibuang), tapi
  bukan penyebab cacat ini.

**Cacat terbuka**

1. Validation error dari ImGui saat viewport sekunder dibuat (butir 4 di atas).

---

## E2 — Editor framework · ~4 sesi · ✅ SELESAI (2 Agustus 2026)

**Tujuan.** Kerangka yang membuat menambah panel baru jadi murah, dan setiap
perubahan data bisa di-undo.

**Pekerjaan**

- `EditorApp`: siklus hidup, loop, autosave, penanganan crash (tulis log + layout).
- `PanelManager`: registrasi panel (`SIM_REGISTER_PANEL`), buka/tutup, fokus, banyak
  instance untuk panel tertentu (mis. dua viewport), menu **Window** terisi otomatis.
- `DockLayout`: layout bawaan dibangun lewat `ImGui::DockBuilder` meniru acuan —
  kiri-atas Outliner, kiri-bawah Asset Browser, tengah Viewport, kanan Inspector,
  bawah Console. Menu **View → Reset Layout**. Beberapa preset workspace
  (Level, Material, Animation) yang bisa dipilih.
- `CommandHistory`: stack undo/redo, penggabungan command beruntun (drag gizmo →
  satu entri), batas memori, panel **History** untuk melihat/melompat.
- `Selection`: himpunan entity terpilih, sinyal perubahan, konsep "konteks aktif"
  supaya Inspector tahu harus menampilkan apa.
- `ActionRegistry` + `ShortcutMap`: aksi bernama (`level.save`, `edit.undo`,
  `gizmo.translate`) dengan pintasan yang bisa diubah, disimpan ke config, dan
  ditampilkan di menu.
- `Notifications`: toast pojok kanan-bawah, progress bar untuk pekerjaan latar.
- `ConsolePanel`: menyerap sink log, filter level, filter teks, klik baris log yang
  merujuk aset → menyorot aset itu di Asset Browser.
- Pustaka widget dasar di `EditorFramework/Widgets`: `PropertyGrid`, `SearchField`,
  `SplitButton`, `IconButton`, `DragVec3` (dengan label X/Y/Z berwarna seperti acuan),
  `Toolbar`, `TreeView` (dengan drag-drop dan rename inline).

**Kriteria terima** — semuanya diverifikasi lewat input sungguhan (XTEST) dan
sanitizer, bukan pengamatan sekilas.

1. ✅ Menambah panel baru = satu berkas, tanpa menyentuh berkas lain sama sekali.
   Diuji: menaruh satu `ProbePanel.cpp` di `Code/Editor/src/` lalu build ulang
   menaikkan jumlah panel 8 → 9, dan menghapusnya mengembalikannya ke 8.
2. ✅ **View → Reset Layout** mengembalikan susunan seperti gambar acuan, dan
   menu **View → Workspace** menyediakan tiga preset (Level, Authoring, Debug).
3. ✅ Undo/redo termasuk penggabungan. Diuji: menyeret satu field sejauh 120 px
   mengubah nilai 1.000 → 2.200, **satu** Ctrl+Z mengembalikannya ke 1.000, dan
   Ctrl+Shift+Z memulihkannya. Unit test menegaskan 50 perubahan berturut-turut
   menghasilkan satu entri history.
4. ✅ Pintasan diubah lewat panel Preferences (Undo → Ctrl+Alt+U), editor
   ditutup, dijalankan lagi, dan bindingnya masih Ctrl+Alt+U.
5. ✅ Nol peringatan ThreadSanitizer. Diuji dengan stres 4 thread penulis dan
   1 thread pembaca pada `LogRing` — persis pola yang dipakai panel Console.

**Yang berbeda dari rencana**

- `EditorApp` **tidak** memiliki loop utama, jendela, atau device seperti tertulis
  di daftar pekerjaan. Loop tetap di `Apps/SimEditor` karena di sanalah platform
  dan RHI boleh dilihat; memindahkannya ke EditorFramework akan memaksa modul itu
  ikut melihat keduanya dan meruntuhkan aturan modul di ARCHITECTURE.md.
  `EditorApp` memiliki layanan bersama (history, seleksi, aksi, notifikasi) dan
  menggambar satu frame UI.
- Panel ditemukan lewat `file(GLOB ... CONFIGURE_DEPENDS)` atas pola `*Panel*.cpp`.
  Globbing biasanya dihindari, tapi kriteria terima nomor 1 secara harfiah
  meminta "tanpa mengubah berkas lain" — dan daftar sumber CMake adalah berkas
  lain. Ruang lingkupnya dibatasi satu folder dan satu pola nama.
- `Sim::EditorPanels` ditautkan dengan `WHOLE_ARCHIVE`. Panel mendaftarkan diri
  lewat inisialisasi statik, dan tanpa itu linker membuang berkas objek yang
  simbolnya tidak pernah dirujuk — panel hilang tanpa satu pun pesan error.
- **Keadaan buka/tutup panel ikut disimpan** (`~/.simengine/panels.json`). Tidak
  ada di rencana, tapi ketahuan saat pengujian: ImGui menyimpan posisi dan ukuran
  jendela, tapi tidak keadaan buka/tutup, sehingga panel yang sengaja ditutup
  pengguna muncul lagi setiap editor dijalankan.
- `TreeView` dengan drag-drop dan rename inline **belum** dibuat. Widget itu baru
  bermakna setelah ada hierarki sungguhan, jadi dikerjakan bersama Outliner di E4.
  Widget yang sudah ada: `DragVec3`, `SearchField`, `IconButton`, `ToolbarButton`,
  `PropertyLabel`, `ComponentHeader`.
- Autosave belum ada isinya: belum ada dokumen untuk disimpan sampai E3.

---

## E3 — Reflection + scene model · ~4 sesi · ✅ SELESAI (2 Agustus 2026)

**Tujuan.** Ada model data scene yang bisa diserialisasi, dan Inspector bisa
menggambarnya tanpa kode khusus per komponen.

**Pekerjaan**

- `Reflect`: `TypeRegistry`, `TypeDesc`, `FieldDesc`, `Attribute` (Range, Step, Tooltip,
  Category, ReadOnly, HideInInspector, AssetRef<T>, EnumNames), akses field lewat
  pointer-to-member dengan type erasure, dukungan tipe container (`std::vector<T>`)
  dan tipe bersarang.
- `Scene`: `World` (pembungkus `entt::registry`), `EntityId` stabil + GUID untuk
  serialisasi, `HierarchyComponent` (parent/children/urutan), `TransformComponent`
  (lokal + world cache + dirty propagation), `NameComponent`, `VisibilityComponent`,
  `StaticFlagComponent`.
- Komponen awal yang cukup untuk menguji Inspector: `MeshRendererComponent`,
  `LightComponent` (directional/point/spot), `CameraComponent`.
- Serialisasi: `.simlevel` (JSON, nlohmann) dengan versi skema + jalur migrasi.
- `Prefab` v0: simpan sub-tree jadi `.simprefab`, instansiasi kembali, catatan override
  per-field (override baru diselesaikan penuh di E4).
- `Project`: `project.simproj` (nama, path aset, level startup, setting), pembukaan
  project dari CLI dan dari dialog.

**Kriteria terima** — 12 test di `Tests/SceneTests.cpp`, plus verifikasi UI.

1. ✅ Round-trip byte-per-byte. Diuji dengan **10.920 entity** ber-hierarki
   (melebihi 5.000 yang diminta): simpan → muat → simpan menghasilkan teks yang
   sama persis, termasuk GUID, urutan entity, urutan komponen, dan presisi angka.
2. ✅ Field baru otomatis muncul dan tersimpan. Diuji dengan menambahkan
   `lodBias` ke `MeshRendererComponent`: satu field di struct dan satu baris
   `.Field<...>()` di pendaftaran. "LOD Bias" langsung muncul di Inspector
   sebagai slider (karena diberi `Range`), dan `"lodBias": 0.0` muncul di berkas
   level — tanpa satu baris pun berubah di kode Inspector maupun serialisasi.
3. ✅ Berkas skema versi 1 (rotasi Euler derajat) dimigrasikan ke versi 2
   (quaternion) dengan peringatan di Console, dan menyimpannya kembali memakai
   skema terbaru. Berkas dari editor yang lebih baru ditolak dengan pesan jelas,
   bukan dimuat separuh.
4. ✅ Transform hierarki (posisi dan skala induk merambat ke anak), siklus parent
   ditolak tanpa merusak hierarki yang ada, dan menghapus induk ikut menghapus
   keturunannya beserta entri indeks GUID-nya.

**Yang berbeda dari rencana**

- Pendaftaran komponen **eksplisit** (`RegisterCoreComponents()` dipanggil
  konstruktor `World`), bukan lewat makro inisialisasi statik seperti panel.
  Alasannya: urutan pendaftaran menentukan urutan komponen di berkas level, dan
  urutan inisialisasi statik antar-TU tidak ditentukan bahasa. Berkas level
  harus byte-per-byte sama setiap kali disimpan.
- `TypeKey` memakai `std::type_index`, bukan hash nama. Yang perlu stabil
  lintas-sesi adalah nama tipe dan nama field yang tertulis di berkas, bukan
  pengenal di memori — dan type_index menghilangkan seluruh kelas bug tabrakan
  hash tanpa biaya.
- Undo untuk penyuntingan komponen bersifat **generik**: cuplikan komponen
  disimpan sebagai JSON sebelum dan sesudah. Salinan byte tidak bisa dipakai
  karena komponen memuat `std::string`. Hasilnya nol command khusus per tipe —
  komponen baru langsung bisa di-undo.
- Enum diserialisasi sebagai **nama**, bukan angka, supaya menyisipkan nilai
  baru di tengah daftar tidak mengubah arti berkas yang sudah ada.
- **Prefab v0** dibuat sebagai potongan level dengan format yang sama persis
  (`.simprefab`), dengan GUID ditukar baru saat instansiasi. Catatan override
  per-field belum ada — sesuai rencana, itu diselesaikan di E4.
- Penyuntingan multi-seleksi di Inspector belum ada; ikut E4 bersama gizmo,
  karena keduanya butuh keputusan yang sama soal nilai campuran.

---

## E4 — Level Editor · ~6 sesi · ✅ SELESAI (2 Agustus 2026)

**Tujuan.** Menyusun level nyata: pilih, pindah, putar, skala, parent, duplikat,
hapus — semuanya bisa di-undo dan tersimpan.

**Pekerjaan**

- **Entity Outliner** (posisi B pada acuan): tree hierarki, cari + filter,
  rename inline (klik-ganda atau F2), drag untuk mengubah parent termasuk lepas
  ke ruang kosong untuk menjadikan akar, multi-select (Ctrl per baris, Shift
  untuk rentang), tombol visibility & lock per baris, menu konteks (Create
  Child, Rename, Duplicate, Save as Prefab, Move to Root, Delete), ikon per
  tipe entity yang diturunkan dari komponennya.
- **Entity Inspector** (posisi E): header nama + Entity ID yang bisa disalin,
  tombol **Add Component**, komponen dalam header yang bisa dilipat, menu
  per-komponen (Copy, Paste, Reset, Remove), PropertyGrid dari reflection,
  penanganan multi-select (nilai berbeda ditampilkan sebagai "—").
- **Viewport** (posisi D): kamera fly (WASD + klik kanan) dan orbit (Alt+drag),
  focus ke seleksi (F), grid adaptif, wireframe per objek, ikon entity tanpa
  geometri, gizmo translate/rotate/scale dengan pilihan world/local (overlay
  D1), snapping (grid/sudut/skala) dengan kelipatan yang bisa diatur, tombol
  perspective/ortho dan Lit/Wireframe (overlay D2), picking klik dan seleksi
  kotak, statistik pojok.
- Operasi level: New/Open/Save/Save As, dirty marker di judul jendela, prompt
  saat keluar, autosave berkala ke lokasi terpisah.
- Copy/paste/duplicate (Ctrl+C/V/D), "paste as child" (Ctrl+Shift+V).
- Prefab: instansiasi dan "Save as Prefab". Override per-field dipindahkan ke
  E5 — lihat catatan di bawah.

**Kriteria terima** — 15 test di `Tests/LevelEditorTests.cpp`, plus verifikasi UI
lewat input sintetis.

1. ✅ Level 17 entity dengan hierarki 3 tingkat: simpan → muat → simpan
   menghasilkan teks yang sama persis. Diuji juga lewat UI — box-select seluruh
   viewport lalu Delete menyisakan tepat entity yang berada di luar kotak, dan
   satu undo mengembalikan kelimanya beserta hierarkinya.
2. ✅ Satu seretan gizmo = **satu entri undo**, berapa pun frame yang dilaluinya.
   Diuji dengan 40 perintah berturut-turut yang menyatu jadi satu entri, dan
   lewat UI: menyeret sumbu Y dari 1,0 ke 2,76 lalu satu Ctrl+Z mengembalikannya
   persis ke 1,0. Seretan atas seleksi yang berbeda sengaja **tidak** digabung —
   kalau digabung, "sebelum" milik objek pertama akan dipakai membatalkan
   perpindahan objek kedua.
3. ✅ 120 frame × 100 entity memakan **0,162 ms per frame** — 1% dari anggaran
   16,6 ms pada 60 Hz. Ambang testnya dipatok 1 ms, bukan 16,6 ms: anggaran satu
   frame harus dibagi dengan menggambar seluruh UI dan scene, jadi batas yang
   longgar akan lulus bahkan ketika kinerjanya sudah belasan kali lebih buruk.
4. ✅ Picking diuji terhadap AABB dalam **ruang lokal** objek, bukan AABB dunia:
   kubus yang diputar 45° tetap bisa diklik tepat pada bentuknya, dan sinar pada
   x = 0,9 meleset seperti seharusnya. Dua objek segaris pandang memilih yang
   terdepan, dan pemenangnya berbalik ketika sinar datang dari arah lain.
   Box-select memilih apa yang di dalam kotak dan melewatkan yang di luar.
5. ✅ Snapping menghasilkan kelipatan persis: menyeret sumbu X mendaratkan objek
   di 2,5 tepat, sementara Y dan Z tidak tersentuh sama sekali. Seratus langkah
   berturut-turut tetap kelipatan persis — tidak pernah muncul 2,4999998.

**Yang berbeda dari rencana**

- **Ikon entity tidak digambar renderer.** Rencana awal menaruh `BillboardIcon`
  di `ViewportScene`. Itu memaksa renderer mengenali tipe komponen untuk memilih
  gambar — "entity ini lampu, jadi gambarkan bohlam" adalah pengetahuan editor.
  Ikon sekarang digambar panel sebagai glyph dari font ikon yang sudah dimuat:
  tajam di skala DPI mana pun, nol aset tambahan, dan ikonnya sama persis dengan
  yang dipakai Outliner untuk entity yang sama. `BillboardIcon` dihapus dari
  antarmuka renderer.
- **Snapping tidak diserahkan ke ImGuizmo.** Pustaka itu membulatkan *selisih*
  seretan, sehingga objek yang posisi awalnya bukan kelipatan akan tetap bukan
  kelipatan selamanya dan sisa pecahannya menumpuk. Pembungkus kita membulatkan
  nilai akhirnya, dan hanya pada sumbu yang benar-benar digerakkan — membulatkan
  ketiganya akan melompatkan Y dan Z ketika pengguna hanya menggeser X.
- **ImGuizmo dipatok ke commit, bukan tag.** Rilis bertag terakhirnya (1.83)
  jauh lebih tua daripada ImGui 1.92 yang kita pakai. Commit yang dipilih sudah
  diuji kompilasi bersih tanpa satu pun API ImGui yang sudah dihapus.
- **Semua perintah scene menyimpan GUID, bukan handle entity.** Undo
  menghidupkan kembali entity yang sudah dihapus, dan entt boleh memakai ulang
  nomor entity yang sama untuk objek yang sama sekali berbeda. GUID adalah
  satu-satunya identitas yang bertahan melewati siklus hapus–undo.
- **Open dan Save As memakai daftar berkas sendiri, bukan dialog OS.** Menambah
  pustaka dialog berkas sekarang berarti satu dependensi untuk sesuatu yang akan
  digantikan Asset Browser di E5. Sementara ini level dipilih dari folder editor.
- **Override prefab per-field dipindahkan ke E5.** Penanda override dan "revert
  to prefab" perlu membandingkan instance terhadap sumbernya, dan itu menuntut
  aset dengan identitas yang stabil — yang baru lahir bersama AssetDatabase.
  Mengerjakannya sekarang berarti membangunnya dua kali.

**Tiga bug yang ditemukan saat pengujian**, dicatat karena penyebabnya bukan hal
yang jelas dari membaca kode:

1. **Editor menggantung saat resize.** `StubRenderer` menyimpan `VkFence` milik
   `Device` untuk ditunggu belakangan, padahal `Device` mendaur ulang fence itu
   begitu submit-nya selesai. Yang ditunggu bisa jadi submit yang sedang direkam
   saat itu juga — deadlock yang pasti terjadi, dan resize hanya mempercepatnya.
   `SubmitTransient` sekarang mengembalikan nomor submit yang tidak pernah
   dipakai ulang, dan `WaitTransient` langsung kembali bila nomornya sudah tidak
   ada (yang berarti pekerjaannya memang sudah selesai).
2. **Sumbu Y gizmo menunjuk ke bawah.** Proyeksi kita membalik `[1][1]` untuk
   Vulkan, ImGuizmo mengasumsikan konvensi OpenGL dan membaliknya sekali lagi.
   Pembalikannya dikembalikan khusus untuk ImGuizmo; matriks hasilnya tidak
   terpengaruh karena yang berubah hanya cara memetakan dunia ke piksel.
3. **Klik tombol overlay viewport menghapus seleksi, dan tombolnya tidak
   bereaksi.** Status mouse dibaca sebelum tombolnya diajukan ke ImGui, sehingga
   ImGui belum tahu ada tombol di bawah kursor. Urutan pengajuan item sekarang
   yang menentukan siapa berhak atas sebuah klik: overlay diajukan lebih dulu,
   permukaan viewport menjadi `InvisibleButton` sesudahnya. `ImGuizmo::Enable()`
   dimatikan saat kursor di atas overlay, karena gizmo membaca mouse langsung
   dan tidak tahu apa pun tentang item ImGui.

---

## E5 — Asset Browser + Asset Database · ~5 sesi

**Tujuan.** Aset punya identitas stabil, bisa dicari, dilihat thumbnail-nya, dan
diseret ke level.

**Pekerjaan**

- `AssetDatabase`: pemindaian direktori aset, file `.meta` berisi GUID + setting
  import, indeks GUID → path, deteksi perubahan lewat `FileWatcher`, reimport
  otomatis, graf ketergantungan antar-aset.
- `Importer` registry: satu importer per ekstensi, berjalan di job system,
  menghasilkan aset "compiled" di cache. Untuk fase editor cukup: tekstur (stb_image),
  teks/Lua, JSON, dan pass-through untuk mesh (metadata saja; parsing penuh di E8).
- **Asset Browser** (posisi C pada acuan): panel kiri pohon folder, panel kanan grid
  thumbnail dengan slider ukuran, mode daftar/grid, breadcrumb, pencarian dengan
  filter tipe, favorit, panel detail (nama, ukuran, dimensi, format, GUID) seperti acuan.
- Thumbnail: cache di disk berbasis hash isi, dibuat di thread latar, placeholder
  saat belum siap.
- Drag & drop: aset → viewport (buat entity), aset → field Inspector (assign
  referensi), aset → Asset Browser (pindah file).
- Operasi file yang aman: rename/move memperbarui `.meta` dan tidak memutus referensi
  (karena referensi memakai GUID); delete memperingatkan pemakaian yang ada.
- Panel **Asset References**: siapa memakai aset ini, aset ini memakai siapa.

**Kriteria terima**

1. Menambah file ke folder aset dari luar editor → muncul di browser < 2 detik tanpa restart.
2. Rename aset yang direferensikan level → level tetap utuh setelah dimuat ulang.
3. Folder berisi 10.000 aset tetap bisa di-scroll mulus (thumbnail dimuat malas).
4. Menyeret tekstur ke field material di Inspector menetapkan referensinya, dan bisa di-undo.
5. Hapus aset yang masih dipakai → dialog peringatan berisi daftar pemakai.

---

## E6 — Runtime Lua + visual scripting + editor scripting · ~6 sesi

**Tujuan.** Lua jadi bahasa gameplay, dan editor bisa diperluas dengan Lua.

**Pekerjaan**

- `Script`: `LuaState` (sandbox per-project, batas memori, error handler dengan
  traceback), integrasi sol2, `ScriptComponent` (path script + properti terekspos
  yang muncul di Inspector).
- Binding engine: math (`vec3`, `quat`, `mat4`), `Entity`/`World` (cari, buat, hapus,
  ambil/pasang komponen), input, waktu, log, event. Binding dibangkitkan dari
  `Reflect` supaya komponen baru otomatis terjangkau dari Lua.
- Hot reload: file watcher memicu reload script; state per-entity dipertahankan
  lewat serialisasi tabel `state` sebelum reload.
- **Lua Console** panel: REPL dengan riwayat, autocomplete sederhana, cetak tabel
  yang bisa dilipat.
- **Script Editor** panel: editor teks dengan pewarnaan sintaks Lua, tanda error
  di gutter, jalankan-pilihan.
- **Editor scripting API**: Lua bisa menambah item menu, panel kustom (ImGui
  ter-bind terbatas: `sim.ui.button`, `sim.ui.text`, dst), memproses aset secara
  batch, dan mendaftarkan command yang ikut sistem undo.

### E6.5 — Visual scripting yang dikompilasi ke Lua

**Kenapa dikompilasi, bukan ditafsirkan.** Alternatifnya adalah mesin graph yang
menelusuri node satu per satu saat runtime. Itu berarti dua jalur eksekusi yang
harus dijaga sama perilakunya — satu untuk Lua tulis-tangan, satu untuk graph —
dan yang kedua selalu lebih lambat sekaligus lebih sulit di-debug. Dengan
mengompilasi graph menjadi sumber Lua, yang berjalan hanya satu runtime: graph
adalah *penulis kode*, bukan penafsir. Efek sampingnya besar dan gratis: profiler,
traceback, hot reload, dan sandbox yang sudah ada langsung berlaku untuk graph.

- **Format `.simgraph`** (JSON, berversi seperti `.simlevel`): daftar node, pin,
  dan koneksi. Node menyimpan GUID sendiri sehingga koneksi tidak putus ketika
  node dipindah atau diberi nama baru.
- **Katalog node dibangkitkan dari `Reflect`**, bukan didaftarkan tangan. Setiap
  komponen dan fungsi yang sudah terjangkau Lua otomatis muncul sebagai node —
  satu sumber kebenaran, dan komponen baru tidak menuntut pekerjaan tambahan.
  Node inti: event (`OnStart`, `OnUpdate`, `OnCollision`), alur (branch, loop,
  sequence), variabel (get/set, lokal & graph), matematika, panggilan fungsi,
  dan komentar.
- **Compiler graph → Lua** (`Code/Script/GraphCompiler`):
  - urutan eksekusi dari penelusuran topologis pin *exec*; pin *data* ditarik
    malas saat dibutuhkan, seperti pemanggilan fungsi biasa;
  - siklus pada pin data ditolak dengan pesan yang menunjuk node penyebabnya,
    bukan menggantung;
  - keluarannya Lua yang **bisa dibaca manusia**, dengan komentar `-- node <id>`
    di tiap blok. Ini bukan kemewahan: ia yang membuat graph bisa di-debug
    dengan alat yang sama seperti script biasa, dan membuat pengguna bisa
    lulus dari visual scripting ke Lua tanpa jurang.
  - **peta sumber** node ↔ baris Lua, supaya error runtime menyorot node yang
    salah di editor, bukan baris di berkas yang tidak pernah dilihat pengguna.
- **Kompilasi saat runtime maupun saat impor.** Importer `.simgraph` menghasilkan
  `.lua` di cache aset (jalur normal, nol biaya saat memuat level), sementara
  editor mengompilasi ulang di memori setiap graph disunting sehingga Play bisa
  ditekan tanpa langkah build. Keduanya memanggil compiler yang sama persis —
  hasil yang berbeda antara editor dan runtime adalah kelas bug yang tidak boleh
  dibuka.
- **Panel Graph Editor** memakai `imgui-node-editor` (dependensi yang sudah
  direncanakan untuk E7.1, ditarik lebih awal ke sini): kanvas dengan pan/zoom,
  node dari katalog lewat pencarian, koneksi bertipe yang menolak sambungan tak
  masuk akal, breakpoint per node, dan panel "Compiled Lua" berdampingan yang
  memperlihatkan hasil kompilasinya secara langsung.
- **`GraphComponent`** merujuk aset `.simgraph` lewat `AssetRef`, sejajar dengan
  `ScriptComponent`. Properti yang diekspos graph muncul di Inspector lewat jalur
  reflection yang sama.

**Kriteria terima**

1. Script Lua yang memutar sebuah entity berjalan saat **Play** ditekan, berhenti
   saat **Stop**, dan scene kembali ke keadaan sebelum Play.
2. Mengedit script saat editor berjalan → efeknya terlihat < 1 detik tanpa restart.
3. Error di Lua muncul di Console lengkap dengan traceback dan nomor baris, tidak
   membuat editor crash.
4. Script editor Lua bisa membuat sebuah panel baru dan menambah menu item, dan
   perubahan yang dilakukannya bisa di-undo dari sistem undo utama.
5. Properti yang diekspos script muncul di Inspector dan tersimpan bersama level.
6. Graph "putar entity saat OnUpdate" menghasilkan Lua yang bisa dibaca, dan
   perilakunya **identik** dengan script tulis-tangan yang setara — dibandingkan
   dengan menjalankan keduanya dan mencocokkan transform tiap frame.
7. Error runtime di dalam graph menyorot node penyebabnya di Graph Editor, bukan
   sekadar mencetak nomor baris berkas yang tidak pernah dilihat pengguna.
8. Graph dengan siklus pada pin data ditolak saat kompilasi dengan pesan yang
   menunjuk node penyebabnya; editor tidak menggantung dan tidak crash.
9. Level yang memakai graph dimuat tanpa mengompilasi apa pun — `.lua` hasil
   impor yang dipakai — dan menyunting graph di editor langsung berlaku saat
   Play ditekan tanpa langkah build manual.

---

## E7 — Editor khusus

Semua editor di bawah ini **mengarang data**. Preview visualnya memakai `StubRenderer`
dan baru menjadi akurat setelah E8. Kriteria terima di fase ini menekankan keutuhan
data dan alur kerja, bukan kualitas gambar.

### E7.1 — Material Editor · ~5 sesi

- Node graph (imgui-node-editor): node input (texture sample, constant, UV, vertex
  color, time), node matematika, node utilitas (lerp, fresnel, normal blend), node
  output (base color, metallic, roughness, normal, emissive, opacity, AO).
- Pustaka node dari palet yang bisa dicari; komentar/frame untuk merapikan graph.
- Validasi graph: tipe port, deteksi siklus, port wajib yang belum tersambung.
- Aset material `.simmat` (JSON graph) + material instance `.simmatinst` yang hanya
  menyimpan override parameter dari material induk.
- Panel parameter: parameter yang diekspos graph muncul sebagai UI untuk instance.
- Preview: sphere/kubus/plane/mesh kustom, pilihan environment, rotasi lampu.
- **Terima:** graph 30+ node disimpan/dimuat identik; menyambungkan port bertipe salah
  ditolak dengan pesan jelas; membuat instance dari material lalu mengubah satu
  parameter tidak mengubah induknya; hapus node yang tersambung membersihkan link.

### E7.2 — Particle Editor · ~5 sesi

- Sistem berbasis modul: Spawn (rate/burst), Shape (point/sphere/box/cone/mesh),
  Initial (velocity, size, color, rotation, lifetime), Over-Lifetime (curve untuk
  size/color/velocity/rotation), Force (gravity, drag, vortex, noise, point attractor),
  Collision, Sub-emitter, Renderer (billboard/stretched/mesh/ribbon, material, sorting).
- Widget khusus: **CurveEditor** (bezier, banyak kurva, preset ease) dan
  **GradientEditor** (color stop + alpha stop) — dipakai ulang oleh Animation dan Terrain.
- Timeline preview: play/pause/step/loop, scrub waktu, kontrol kecepatan, restart.
- Statistik langsung: partikel aktif, spawn/detik, peringatan anggaran.
- Aset `.simfx`.
- **Terima:** efek dengan 5 modul + 3 kurva disimpan/dimuat identik; scrub timeline
  bersifat deterministik (waktu yang sama → keadaan yang sama, karena RNG di-seed
  per waktu); menonaktifkan modul tidak menghapus datanya; preview berjalan pada
  100k partikel tanpa membekukan UI (simulasi CPU dulu, dibatasi anggaran).

### E7.3 — Terrain Editor · ~5 sesi

- Data terrain: heightmap ubin (tile) + layer material + peta bobot (splat) + peta
  hole, disimpan sebagai `.simterrain` + tekstur pendamping.
- Alat sculpt: raise/lower, flatten, smooth, noise, ramp, erosion sederhana; parameter
  brush (ukuran, kekuatan, jatuh-tempo/falloff, brush kustom dari tekstur).
- Alat paint: paint layer material dengan bobot, paint hole, paint atribut (mis. berjalan/tidak).
- Import/export heightmap (PNG 16-bit, RAW).
- Pengaturan LOD & tile, ukuran dunia, resolusi heightmap per tile.
- Undo untuk operasi brush: simpan patch region sebelum/sesudah, bukan seluruh heightmap.
- **Terima:** sculpt di batas dua tile menghasilkan permukaan menyatu (tidak ada retakan
  data); undo satu goresan brush mengembalikan heightmap persis; terrain 4×4 km dengan
  heightmap 2048² per tile tetap bisa diedit tanpa lonjakan memori; import/export PNG
  16-bit round-trip tanpa kehilangan presisi.

### E7.4 — Vegetation Editor · ~4 sesi

- Layer vegetasi: aset mesh/prefab, kepadatan, skala acak (min/maks), rotasi acak,
  offset, penyelarasan ke normal permukaan, jarak LOD & billboard.
- Aturan penempatan: rentang ketinggian, rentang kemiringan, layer terrain tertentu,
  mask tekstur, jarak minimum antar-instance (Poisson disk), seed deterministik.
- Alat kuas: paint kepadatan, hapus, ratakan; sebar otomatis ke seluruh terrain
  berdasarkan aturan.
- Penyimpanan instance: prosedural (aturan + seed, dihitung ulang) dengan lapisan
  edit manual (tambah/hapus per-instance) — supaya file tidak membengkak.
- **Terima:** menyebar 1 juta instance dari aturan selesai < 10 detik dan file
  tersimpan < 5 MB; seed yang sama menghasilkan sebaran yang sama persis di mesin
  berbeda; mengubah terrain di bawah vegetasi memicu penyesuaian ketinggian instance;
  edit manual bertahan setelah aturan diubah.

### E7.5 — Animation Editor · ~6 sesi

- **Skeleton view**: pohon bone, bind pose, retarget mapping ke rig standar.
- **Timeline / Dope Sheet**: track per-bone/per-properti, keyframe (pindah, salin,
  hapus, skala waktu), tipe interpolasi per-key, marker/event yang bisa memanggil Lua.
- **Curve Editor**: memakai ulang widget dari E7.2, dengan tangent handle.
- **State Machine graph**: state, transisi dengan kondisi (parameter bool/float/trigger),
  blend tree 1D/2D, layer dengan mask bone, IK setting.
- Preview: pemutaran pada mesh skinned, kontrol kecepatan, loop, perbandingan
  before/after retarget.
- Aset: `.simanim` (klip), `.simskel` (skeleton), `.simanimgraph` (state machine).
- **Terima:** klip 60 detik pada rig 100 bone bisa di-scrub mulus; memindahkan
  keyframe bisa di-undo; kondisi transisi tersimpan/dimuat identik; event pada
  timeline memanggil fungsi Lua saat preview mencapai frame tersebut; blend tree 2D
  menghasilkan bobot yang benar pada titik uji.

---

## Ringkasan urutan & ketergantungan

```
E0 ─► E1 ─► E2 ─► E3 ─► E4 ─┬─► E5 ─┬─► E6 ─► E7.1 Material
                            │       │
                            │       └─────► E7.3 Terrain ──► E7.4 Vegetation
                            │
                            └─────────────► E7.2 Particle
                                            E7.5 Animation  (butuh E5 untuk aset rig)
```

E7.4 (Vegetation) bergantung pada E7.3 (Terrain) karena aturan penempatannya membaca
heightmap dan layer terrain. Sisanya bisa dikerjakan paralel setelah E5/E6.

**Track AI (A0..A4)** bercabang dari E2 dan berjalan paralel — lihat
[PLAN-AI.md](PLAN-AI.md). Menjalankannya lebih awal justru menguntungkan: sejak A0,
agen sudah bisa memeriksa editor lewat screenshot dan menjalankan aksi menu, yang
mempercepat pengerjaan E3 ke atas.

## Risiko khusus fase editor

| Risiko | Dampak | Penanganan |
|---|---|---|
| ImGuizmo / imgui-node-editor tidak kompatibel dengan ImGui 1.92 docking | E4/E7.1 tertahan | Keduanya dibungkus antarmuka sendiri (`Sim::Gizmo`, `Sim::NodeGraph`). Kalau pecah, ganti implementasi tanpa mengubah panel. Gizmo sendiri ≈ 600 baris kalau harus ditulis ulang. |
| Rebuild atlas font saat pindah monitor merilis tekstur yang masih dipakai frame in-flight | Crash saat drag ke monitor lain | Semua rilis tekstur lewat antrian tunda `TextureBridge` (N frame). Diuji dengan ASan di kriteria terima E1. |
| Reflection kurang ekspresif untuk kasus E7 (kurva, gradient, graph) | Inspector jadi penuh kode khusus | Sejak E3, `Attribute` mendukung "custom drawer" per-tipe; kurva/gradient/graph didaftarkan sebagai drawer, bukan pengecualian. |
| Undo untuk operasi besar (brush terrain, sebar vegetasi) memakan memori | Editor kehabisan RAM | Command menyimpan patch/delta, bukan snapshot penuh; `CommandHistory` punya batas memori dan membuang entri terlama. |
| Preview stub terlalu jauh dari hasil akhir sehingga penulis aset salah menilai | Rework di E8 | Preview stub selalu diberi label "Preview (unlit)". Material Editor menampilkan nilai channel apa adanya, bukan mencoba meniru PBR. |
