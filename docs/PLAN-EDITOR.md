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

   ⚠️ **Kriteria 2 dan 3 tidak lagi berlaku sejak multi-viewport dimatikan**
   (3 Agustus 2026): panel yang mendapat viewport sendiri menggantung. Keduanya
   pernah terverifikasi dan jalurnya masih ada — `SIM_ENABLE_VIEWPORTS=1`
   menyalakannya kembali — tapi jangan dibaca sebagai keadaan editor sekarang.
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
3. ✅ 120 frame × 100 entity memakan **0,162 ms per frame** di build Debug —
   1% dari anggaran 16,6 ms pada 60 Hz. Build Release: **0,0083 ms per frame**.
   Angka tanpa menyebut build-nya menyesatkan; keduanya dicatat di sini supaya
   pengukuran berikutnya membandingkan hal yang sama. Ambang testnya dipatok
   1 ms, bukan 16,6 ms: anggaran satu frame harus dibagi dengan menggambar
   seluruh UI dan scene, jadi batas yang longgar akan lulus bahkan ketika
   kinerjanya sudah belasan kali lebih buruk.
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

   **Perbaikan itu sendiri mematikan gizmo, dan baru ketahuan di E6** — lihat
   catatan pemeriksaan ulang di bawah.

**Diperiksa ulang 3 Agustus 2026**, setelah E6 selesai, lewat input sungguhan
(XTEST) di editor yang berjalan — bukan dengan membaca kode.

Kriteria 1, 2, 4, dan 5 masih berlaku apa adanya: seleksi kotak atas enam entity
lalu Delete mengosongkan outliner dan satu Ctrl+Z mengembalikan keenamnya
lengkap dengan hierarkinya; menyeret panah X memberi Translate X = 2,821 dengan
Y dan Z tetap 0; cincin Y memberi Rotate Y = 72,69°; dan dengan snapping menyala
hasilnya 3,000 persis.

Kriteria 2 dan 5 sempat **tidak berlaku sama sekali** di antara E4 dan
pemeriksaan ini: gizmo tidak bisa menggeser apa pun. Dua sebab, keduanya
berpangkal pada permukaan viewport yang diperkenalkan oleh perbaikan nomor 3 di
atas — sebuah item ImGui yang menutupi seluruh area gizmo, sementara ImGuizmo
membaca keadaan ImGui yang *global*:

- `ImGui::IsAnyItemHovered()` ikut memeriksa hover frame sebelumnya
  (`HoveredIdPreviousFrame`). Begitu kursor masuk ke viewport, permukaan itu
  membuat jawabannya selalu "ya", gizmo menerima `interactive = false`, dan
  ImGuizmo menggambarnya abu-abu.
- `ImGuizmo::CanActivate()` mensyaratkan `!IsAnyItemHovered() && !IsAnyItemActive()`.
  Selama permukaan itu diajukan, gizmo tidak akan pernah bisa memulai manipulasi.

Pelajarannya bukan "ImGuizmo rewel" melainkan bahwa **verifikasi UI punya masa
berlaku**: klaim nomor 2 dan 5 memang benar saat ditulis, dan yang
membatalkannya adalah perubahan di berkas yang sama beberapa baris di atasnya.
Test tidak menangkapnya karena yang rusak bukan logikanya melainkan perantara
antara ImGui, ImGuizmo, dan panel.

---

## E5 — Asset Browser + Asset Database · ~5 sesi · ✅ SELESAI (2 Agustus 2026)

**Tujuan.** Aset punya identitas stabil, bisa dicari, dilihat thumbnail-nya, dan
diseret ke level.

**Pekerjaan**

- `AssetDatabase`: pemindaian direktori, berkas `.meta` berisi GUID, indeks
  GUID → path, deteksi perubahan lewat `FileWatcher`, impor ulang otomatis, dan
  graf ketergantungan antar-aset dua arah.
- `Core::FileWatcher`: inotify di Linux, `ReadDirectoryChangesW` di Windows.
- `Core::TaskPool`: kolam thread untuk impor dan thumbnail.
- `Importer` registry per tipe aset. Tekstur hanya membaca kepala berkas;
  dokumen (level/prefab/material) dipindai untuk GUID yang dirujuknya; mesh,
  skrip, dan teks pass-through — mesh baru benar-benar diurai di E8.
- **Asset Browser**: pohon folder, breadcrumb yang tiap ruasnya bisa diklik,
  mode grid/daftar, slider ukuran thumbnail, pencarian, filter tipe, favorit,
  dan panel detail (nama, tipe, path, ukuran, dimensi, GUID).
- Thumbnail: cache disk berbasis hash isi, dibuat di thread latar, ikon tipe
  sebagai pengganti selama belum siap, dan unggah GPU dibatasi empat per frame.
- Drag & drop: aset → viewport (membuat entity di titik tembus bidang tanah),
  aset → field Inspector (menetapkan `AssetRef`), aset → folder (memindahkan
  berkas).
- Operasi file yang aman: rename/move memindahkan `.meta` bersama asetnya
  sehingga GUID bertahan; delete memperingatkan dengan daftar pemakainya.
- **Rujukan aset memakai GUID**: `MeshRendererComponent::mesh` dan `::material`
  berubah dari `std::string` berisi nama menjadi `AssetRef`. Skema level naik
  ke versi 3.

**Kriteria terima** — 15 test di `Tests/AssetTests.cpp`, plus verifikasi UI.

1. ✅ Berkas yang ditulis dari luar editor muncul di bawah dua detik tanpa
   restart, lengkap dengan `.meta`-nya. Diuji di editor sungguhan maupun di test.
2. ✅ Mengganti nama aset yang dirujuk level tidak memutus apa pun.
   `DamagedHelmet.glb` diseret ke field Mesh Asset, lalu diganti nama jadi
   `helmet-v2.glb` lewat panel; GUID di berkas level tetap sama persis dengan
   GUID di `.meta` yang sudah berpindah nama. Berlaku sama untuk pemindahan
   antar folder.
3. ✅ Folder berisi 10.044 aset tidak lagi berbiaya apa pun saat diam. CPU
   editor per dua detik: **123 jiffies** (polling + pemindaian penuh) → **80-113**
   (pemantau berkas) → **46-47** (pemantau + cache tampilan), sama dengan editor
   yang hanya berisi 44 aset.
4. ✅ Menyeret tekstur ke field Inspector menetapkan GUID-nya — cocok persis
   dengan isi `.meta` — dan satu Ctrl+Z mengosongkannya, Ctrl+Shift+Z
   mengembalikannya.
5. ✅ Menghapus aset yang masih dipakai memunculkan dialog berisi **daftar**
   pemakainya (`Levels/arena.simlevel`, `Levels/lobby.simlevel`), bukan sekadar
   jumlahnya, beserta akibatnya. Cancel menahan berkasnya; Delete membuangnya
   bersama `.meta`.

**Diperiksa ulang 3 Agustus 2026**, setelah E6 selesai, lewat input sungguhan
(XTEST) di editor yang berjalan. Kelimanya masih berlaku:

1. Berkas yang disalin dari luar muncul di Asset Browser dalam **0,19 detik**
   lengkap dengan thumbnail dan `.meta` — kriterianya meminta di bawah dua detik.
3. 10.000 aset (20.000 berkas dengan `.meta`-nya) tidak berbiaya apa pun saat
   diam: **28–30 jiffies per dua detik**, sama dengan editor berisi 8 aset
   (28–29). Yang sesekali naik ke 37 adalah pemindaian penuh berjeda 30 detik
   yang memang tetap ada sebagai jalur pemulihan.
4. Menyeret tekstur ke field Material menetapkan GUID yang **cocok persis**
   dengan isi `.meta`-nya; Ctrl+Z mengosongkannya, Ctrl+Shift+Z mengembalikannya.
5. Dialognya menyebut kedua pemakainya per nama beserta akibatnya; Cancel
   menahan berkasnya, Delete membuang berkas dan `.meta`-nya.

**Satu celah yang baru terlihat saat memeriksa ulang nomor 5, dan sudah
ditutup.** Peringatan itu semula hanya menghitung pemakai yang **terindeks
sebagai aset**, sedangkan `SaveLevel` menulis ke `~/.simengine/Levels` — di luar
akar aset `~/.simengine/Assets`. Dua pemakai terpenting karena itu tidak pernah
disebut: berkas level milik editor, dan **scene yang sedang dibuka** — yang
bahkan belum tentu ada di disk. Menghapus tekstur yang baru saja dipasang ke
sebuah entity lolos tanpa peringatan sama sekali. Verifikasi E5 dahulu memakai
level yang kebetulan berada di dalam folder aset, jadi celahnya tidak terlihat.

Perbaikannya tidak dengan memindahkan folder level — itu keputusan tentang tata
letak proyek, dan akarnya memang ditandai sementara sampai `project.simproj`
matang. Yang dibetulkan adalah pertanyaannya: "siapa memakai aset ini" punya
lebih dari satu sumber jawaban, dan Asset Browser tidak boleh menganggap indeks
aset satu-satunya. `EditorContext::findExternalAssetUsers` menambahkan dua
sumber, diisi `EditorApp`:

- **Scene yang sedang dibuka**, ditelusuri `scene::EntitiesUsingAsset()` yang
  dituntun reflection — setiap field `AssetRef`, termasuk yang bersarang di
  dalam struct dan vektor. Daftar komponen yang ditulis tangan akan diam-diam
  ketinggalan, dan yang bergantung padanya adalah sebuah peringatan keselamatan.
- **Berkas `.simlevel`** di folder level editor, dicocokkan sebagai teks — sama
  seperti importer dokumen, karena GUID memang ditulis sebagai string.

Berkas level yang sedang dibuka dilewati hanya bila scene di memori juga
memakainya; di situ ia cuma pengulangan. Kalau scene sudah tidak memakainya
sementara berkasnya masih — misalnya setelah undo yang belum disimpan — keduanya
memang berbeda, dan yang di disk tetap akan rusak. Melewatkannya dengan alasan
"yang di memori lebih benar" justru menyembunyikan satu-satunya pemakai yang
tersisa.

Diverifikasi lewat XTEST untuk kedua sumber secara terpisah: tekstur yang hanya
dipakai scene yang belum disimpan memunculkan `Scene "untitled" (open): Ground`,
dan tekstur yang hanya dipakai berkas level memunculkan
`Levels/untitled.simlevel`. Keduanya sebelumnya tidak memunculkan apa pun.

**Yang berbeda dari rencana**

- **Panel Asset References tidak dibuat terpisah.** Fungsinya — "siapa memakai
  aset ini, aset ini memakai siapa" — ada di panel detail Asset Browser, tempat
  informasi itu justru dibutuhkan: saat memutuskan menghapus atau memindahkan.
  Panel tersendiri berarti dua tempat yang menampilkan hal sama.
- **`FileWatcher` ada di `Core`, bukan `Platform`**, walau isinya kode khusus
  sistem operasi. `Platform` mengekspor SDL3 secara publik dan pemantau ini
  tidak menyentuh SDL sama sekali; menaruhnya di sana akan menyeret SDL ke modul
  Assets beserta seluruh binari test-nya.
- **Pemindaian penuh tidak dihapus, hanya turun peran.** Kedua platform bisa
  kehilangan event (`IN_Q_OVERFLOW`, `ERROR_NOTIFY_ENUM_DIR`), jadi `Poll()`
  mengembalikan bool dan pemindaian berjeda 30 detik tetap ada sebagai jalur
  pemulihan.
- **Jalur Windows belum pernah dijalankan.** Mesin pengembangan hanya Linux. Ia
  ditulis mengikuti dokumentasi dan ditandai di kodenya sebagai wajib diuji
  sebelum build Windows E9 dianggap selesai.
- **Migrasi skema 2 → 3 kehilangan data**, dan itu tidak terhindarkan: versi 2
  menyimpan nama seperti `"shaderball_default_1m"`, dan tidak ada cara
  memetakannya ke GUID tanpa daftar aset dari zaman berkas itu ditulis — yang
  memang tidak pernah ada. Nama yang dilepas dicatat di Console agar bisa
  dipasang ulang.
- **Impor mesh masih pass-through.** Dimensi dan isi geometri baru dibaca di E8;
  yang diindeks sekarang hanya identitas, ukuran, dan waktu ubahnya.

**Tiga temuan kinerja, semuanya lewat pengukuran dan bukan tebakan**

1. **`std::filesystem::relative()` memakan 95% waktu pemindaian.** Pada pohon
   10.000 berkas: jalan-jalan direktori saja 15 ms, ditambah `relative()` jadi
   284 ms, ditambah pemotongan prefiks string 14 ms. Membagi versi lama ke empat
   thread hanya akan sampai ~70 ms — masih lima kali lebih lambat daripada satu
   thread yang memanggil fungsi yang benar. Ini yang membuat "pemindaian
   multi-thread" jadi jawaban yang salah untuk masalah ini.
2. **Sebuah optimasi dibuang lagi setelah diukur.** Sidik jari isi folder untuk
   memutus lebih awal ketika tidak ada yang berubah: 134 vs 123 jiffies, di
   dalam batas derau. Biaya yang tersisa ada di *mengumpulkan* datanya, bukan di
   apa yang terjadi sesudahnya. Kode yang ditambahkan demi kinerja tapi tidak
   terbukti mempercepat apa pun tidak layak disimpan.
3. **Penyebab dominan berpindah tempat setelah tiap perbaikan.** Setelah
   pemindaian murah, yang menonjol adalah panel memanggil `InFolder()` tiap
   frame — 10.000 pemeriksaan × 60 fps untuk daftar yang sama persis. Diperbaiki
   dengan menyusun ulang daftar hanya saat `AssetDatabase::Version()`, folder,
   pencarian, atau filter berubah.

**Satu bug yang hanya muncul saat ditanya**

Menimpa tekstur dari luar editor memperbarui metadata tapi **tidak**
thumbnail-nya: mengganti gambar merah 256×256 dengan biru 512×128 membuat
bentuknya berubah jadi memanjang sementara warnanya tetap merah. Separuh benar,
sehingga tampak berfungsi. Penyebabnya cache thumbnail berkunci GUID saja — dan
GUID memang sengaja tidak berubah saat berkas ditimpa, karena justru itu inti
seluruh sistem aset. `Request()` kini menerima `contentTag` dari ukuran dan waktu
ubah berkas, dengan handle lama tetap dipakai sampai gambar baru siap.

---

## E6 — Runtime Lua + visual scripting + editor scripting · ~6 sesi · ✅ SELESAI (2 Agustus 2026)

**Tujuan.** Lua jadi bahasa gameplay, dan editor bisa diperluas dengan Lua.

**Pekerjaan**

- `LuaVM` (sandbox per-project — `io` dan `os` dibuang, error handler dengan
  traceback), integrasi sol2, `ScriptComponent` (path skrip + properti terekspos
  yang muncul di Inspector).
- Binding engine: math (`vec3`, `quat`, `axis_angle`), `sim.get_component` /
  `sim.set_component` yang **dituntun reflection** sehingga komponen baru
  otomatis terjangkau, waktu, dan log.
- Hot reload: `AssetDatabase` melaporkan GUID mana yang isinya berganti; tabel
  `state` tiap instance dipertahankan menyeberangi reload.
- **Lua Console**: REPL dengan riwayat, pelengkapan Tab, cetak tabel yang bisa
  dilipat.
- **Script Editor**: daftar `.lua`, pemeriksaan sintaks per ketikan (memuat,
  tidak menjalankan), pelengkapan dari state Lua yang sungguh berjalan.
- **Editor scripting API**: `sim.editor.menu/panel/command`, `sim.ui.*` terbatas,
  dan pemrosesan aset batch (`assets`, `rename_asset`, `move_asset`).
- **E6.5 visual scripting**: format `.simgraph`, katalog node dari `Reflect`,
  compiler graph → Lua beserta peta sumber, `GraphCache`, `GraphComponent`, dan
  panel **Graph Editor** di atas imgui-node-editor.

**Kriteria terima** — 27 test di `Tests/GraphTests.cpp`, 12 di `ScriptTests.cpp`,
5 di `EditorScriptingTests.cpp`, plus verifikasi di editor sungguhan.

1. ✅ Skrip yang memutar entity berjalan saat **Play**, berhenti saat **Stop**,
   dan scene kembali persis ke keadaan sebelum Play — cuplikannya diambil
   sebelum satu baris skrip pun berjalan. Seleksi ikut kembali, dipetakan lewat
   GUID: handle entity tidak bertahan melewati pembangunan ulang scene, tapi
   GUID bertahan karena ialah yang tertulis di berkas level.
2. ✅ Menyunting skrip saat editor berjalan berlaku dalam **22 ms** dari berkas
   ditulis sampai kode barunya jalan; kriterianya meminta di bawah satu detik.
   Berlaku juga untuk skrip editor di `Assets/Editor`, yang dimuat ulang
   seluruhnya karena registrasinya tidak menyebut berkas asalnya.
3. ✅ Kesalahan Lua muncul di Console lengkap dengan traceback dan nomor baris.
   Instance yang gagal dimatikan supaya tidak membanjiri Console enam puluh kali
   per detik dan menenggelamkan pesan pertama — satu-satunya yang berguna.
4. ✅ Skrip editor bisa menambah item menu dan panel, dan perubahannya masuk
   `CommandHistory` yang sama dengan panel C++ sehingga Ctrl+Z membatalkannya.
   Diuji lewat `sim.editor.command` yang naik-turunkan sebuah nilai, lalu
   Undo/Redo dari riwayat utama.
5. ✅ Properti yang diekspos skrip muncul di Inspector, bisa disunting untuk
   seluruh seleksi sebagai satu entri undo, dan ikut tersimpan ke berkas level.
   Yang tersimpan di entity hanyalah yang benar-benar disunting.
   Berlaku sama untuk variabel graph yang ditandai *Exposed*: keduanya
   menjalankan Lua dan menyimpan nilainya per-entity, jadi Inspector melayani
   keduanya lewat kode yang sama — yang membedakan hanya aset yang dirujuk.
6. ✅ Graph "putar entity saat OnUpdate" menghasilkan Lua yang bisa dibaca, dan
   perilakunya identik dengan skrip tulis tangan yang setara — keduanya
   dijalankan berdampingan dan transform-nya dicocokkan **tiap frame** selama 30
   frame, bukan hanya di akhir: dua rotasi bisa berpapasan di nilai yang sama
   pada satu titik waktu.
7. ✅ Kesalahan runtime menyorot node penyebabnya. `ScriptRuntime` menyimpan
   kegagalan terakhir per aset beserta nomor barisnya, dan peta sumber
   menerjemahkannya kembali menjadi node. Seluruh node yang ikut menghasilkan
   baris itu disorot — satu baris memang bisa memuat beberapa, karena node murni
   disisipkan sebagai ekspresi ke dalam pernyataan yang memakainya.
8. ✅ Siklus pada pin data ditolak saat kompilasi dengan pesan yang menunjuk
   node penyebabnya, dan kompilernya **kembali** alih-alih menggantung. Berlaku
   juga untuk lingkar pada pin exec, yang akan membuat penelusurannya tidak
   pernah berhenti.
9. ✅ Memuat level yang memakai graph tidak mengompilasi apa pun selama hasil
   kompilasinya masih lebih baru daripada berkas graph-nya. Menyunting graph
   membuat sumbernya lebih baru, dan Play berikutnya memakai hasil baru tanpa
   langkah build manual. Keduanya memanggil `CompileGraph` yang sama persis.

**Yang berbeda dari rencana**

- **Node `OnCollision` tidak dibuat.** Fisika baru datang di E9, dan node yang
  tidak pernah bisa menyala adalah janji yang tidak bisa ditepati katalog.
- **Kompilasi graph tinggal di modul `Script`, bukan sebagai importer aset.**
  `GraphCompiler` ada di `Script`, sedangkan `Script` bergantung pada `Assets`;
  menaruhnya sebagai importer akan membalik arah ketergantungan itu dan menutup
  jalan bagi runtime memakai `Assets` tanpa Lua. `GraphCache` menggantikannya,
  dengan sifat yang diminta kriteria 9 tetap utuh.
- **Suntingan graph tidak melewati `CommandHistory`**, sama seperti Script
  Editor yang menyunting teks. Riwayat undo utama menjanjikan pembatalan
  perubahan *scene*; menaruh suntingan dokumen di sana membuat Ctrl+Z melompat
  bolak-balik antara dua hal yang tidak berhubungan.
- **Breakpoint menahan frame berikutnya, bukan tumpukan panggilan.** Frame yang
  sedang berjalan tetap diselesaikan; menghentikan Lua di tengah tumpukan
  menuntut debug hook yang belum ada. Batas itu dinyatakan di tooltip-nya.
- **`imgui-node-editor` dipatok ke `master`, bukan `develop`.** Keduanya
  mendahului ImGui 1.92, tapi hanya `master` yang memuat pengganti
  `ImRect::Floor()` dan `ImGui::GetKeyIndex()`. Dua patch di `cmake/patches/`:
  `operator*(float, ImVec2)` yang kini disediakan ImGui sendiri — dengan penjaga
  berupa makro yang ditetapkan ImGui persis ketika ia menyediakan operator itu,
  sehingga patch tidak menebak nomor versi dan tetap benar setelah ImGui
  dinaikkan — dan area klik latar kanvas yang melewatkan tombol menu konteks,
  cacat yang tersembunyi selama tombol pan dan tombol menu kebetulan sama.

**Panel Material Editor yang tidak menanggapi klik — tidak lagi terjadi, tanpa
akar masalah yang terbukti**

Dicatat karena ia bisa kembali, dan karena apa yang SUDAH digugurkan menghemat
waktu kalau itu terjadi.

Gejalanya: daftar aset di panel tidak menanggapi klik sama sekali, sementara menu
dan panel lain menanggapi klik yang sama. Sekarang tidak bisa direproduksi lagi —
juga tidak dengan `SIM_ENABLE_VIEWPORTS=1`, sehingga **multi-viewport bukan
penyebabnya**, meski itu dugaan pertama yang paling masuk akal.

Yang paling mungkin tersisa: `ClampFloatingWindow()`. Saat gejalanya muncul,
panel Graph Editor punya ukuran tersimpan 1920×1080 dan, sebagai jendela
mengambang di dalam viewport utama, menutupi seluruh jendela editor. Panel
mengambang seukuran viewport yang berada di atas panel lain dalam urutan jendela
ImGui memang menelan klik yang ditujukan ke panel di bawahnya — dan clamp itu
membuat keadaan tersebut mustahil. Masuk akal, tapi tidak terbukti: gejalanya
hilang sebelum sempat diinstrumentasi.

Sudah digugurkan: `NodeCanvas::Initialize()` yang dipanggil sekali alih-alih tiap
frame (fungsinya early-return bila konteksnya sudah ada, jadi keduanya setara),
`BeginDisabled`/`BeginChild` yang tidak berpasangan (jumlahnya seimbang), dan
multi-viewport.

Kalau kembali: catat `ImGui::GetCurrentContext()->HoveredWindow->Name` dari dalam
panel. Itu langsung menjawab jendela mana yang sebenarnya menerima kursornya,
yang seharusnya dilakukan lebih dulu.

**Temuan yang mengubah keputusan**

1. **Pin setter komponen tidak boleh punya nilai bawaan.** `sim.set_component`
   hanya menulis field yang ada di tabelnya. Kalau pin yang tidak tersambung
   ikut mengirim nilai netral, menyetel posisi lewat graph akan diam-diam
   mengembalikan rotasi dan skala entity — kelas bug yang sangat sulit dilacak
   karena penyebabnya adalah field yang pengguna tidak sentuh sama sekali.
2. **"Murni" berarti tanpa efek samping, bukan tetap.** Cache hasil node murni
   harus dibuang setiap kali node yang punya efek samping berjalan; sebuah
   `get_component` setelah `set_component` harus membaca yang baru.
3. **Panel yang lahir setelah `LoadState` tidak pernah memulihkan keadaannya.**
   Panel Lua selalu begitu — ia baru ada setelah berkas skripnya dijalankan —
   sehingga panel yang sengaja ditutup pengguna muncul lagi setiap editor
   dijalankan. `PanelManager` kini menyimpan keadaan yang dibacanya dan
   menerapkannya juga pada panel yang didaftarkan belakangan.
4. **Konteks global imgui-node-editor menjatuhkan editor, bukan sekadar salah.**
   Beberapa fungsinya melakukan dereferensi tanpa memeriksa konteks aktif, jadi
   memanggil "Fit" di luar `Begin()`/`End()` berarti segfault. Pembungkus
   `Sim::NodeGraph` yang menanggungnya, bukan setiap panel yang memakainya.
5. **Dua koneksi ke satu pin input** bisa muncul di berkas hasil suntingan
   tangan, dan yang kedua diam-diam dikalahkan yang pertama — perilaku yang
   ditentukan urutan penyimpanan dan tidak terlihat sama sekali di kanvas.
   Sekarang dibuang saat dimuat, sejalan dengan koneksi yang menunjuk node yang
   sudah tidak ada.
6. **Kolom yang meregang di dalam node membuat node tidak bisa digeser.** Node
   menentukan ukurannya sendiri dari isinya, sementara kolom tabel yang meregang
   mengambil selebar ruang yang tersedia — dan di dalam node, "ruang yang
   tersedia" adalah selebar kanvas. Keduanya saling memberi makan: setiap node
   melebar sampai tepi kanvas, dan menyeretnya hanya melebarkannya lagi.
   Tabel pin sekarang memakai `SizingFixedFit` + `NoHostExtendX`; yang kedua
   perlu karena tanpa itu lebar *luar* tabel tetap mengambil ruang yang tersedia
   walau kolomnya sudah menyesuaikan isi.
7. **Menanyakan koneksi baru di dalam `while` membekukan editor.** Percobaan
   koneksi bukan antrean: pustaka melaporkan calon yang SAMA setiap kali
   ditanya, jadi loop yang mengurasnya berputar selamanya begitu kedua ujung
   kabel sah. Gejalanya jauh dari penyebabnya — yang terlihat pengguna adalah
   editor yang membeku persis saat kabel dijatuhkan ke sebuah pin, yang mudah
   dibaca sebagai "kabel tidak bisa digambar". Penghapusan justru kebalikannya:
   ia memang antrean dan memang harus dikuras. Asimetri itu kini tertulis di
   header pembungkusnya, karena tidak ada apa pun pada nama fungsinya yang
   memberi petunjuk.
8. **Menggeser node tidak menandai graph kotor**, sehingga tata letak yang baru
   diatur tidak bisa disimpan sama sekali — tombol Save tetap padam. Posisi kini
   mengalir balik dari kanvas ke model begitu node digeser, dengan ambang
   setengah piksel supaya pembulatan pustaka tidak menandai kotor pada frame
   pertama setelah graph dibuka. Pembacaan posisi di dalam Save() ikut dihapus:
   dua mekanisme untuk satu hal berarti yang kedua diam-diam menang setiap kali
   keduanya tidak sepakat.

---

## E7 — Editor khusus

Semua editor di bawah ini **mengarang data**. Preview visualnya memakai `StubRenderer`
dan baru menjadi akurat setelah E8. Kriteria terima di fase ini menekankan keutuhan
data dan alur kerja, bukan kualitas gambar.

### E7.1 — Material Editor · ~5 sesi · 🔨 model data selesai

**Keluarannya OpenPBR Surface v1.1, bukan base-color/metallic/roughness.**
Perubahan dari rencana awal, dan alasannya bukan kelengkapan demi kelengkapan:
shader yang akan menjalankannya sudah ditulis terhadap spesifikasi itu
(`/home/arie/SDK/openpbr.slang`, berikut generator LUT split-sum-nya di
`openpbr_dfg.slang`). Graph yang mengekspos himpunan parameter berbeda menuntut
lapisan penerjemah di antara keduanya — tempat yang paling mungkin membuat
material terlihat berbeda antara preview dan hasil akhir, dengan selisih yang
tidak akan pernah dicari orang karena tidak ada yang mengaku salah.

Nilai bawaan tiap pin disalin dari `OpenPBRSurface::defaults()`. Duplikasi yang
disengaja, dijaga sebuah test: material yang dibiarkan apa adanya harus terlihat
sama di editor dan di shader.

Tiga pin di luar spesifikasi itu — `normal`, `emissive`, `opacity` — memang bukan
bagian BRDF-nya. Normal membelokkan bingkai shading sebelum lobe dievaluasi,
emissive ditambahkan sesudahnya, dan opacity dipakai saat menggabungkan ke
target.

- Node graph (imgui-node-editor): node input (texture sample, constant, UV, vertex
  color, time, world normal, view direction), node matematika, node utilitas
  (lerp, clamp, fresnel, normal blend, combine, split), node output OpenPBR.
- Pustaka node dari palet yang bisa dicari; komentar/frame untuk merapikan graph.
- Validasi graph: tipe port, deteksi siklus, port wajib yang belum tersambung.
- Aset material `.simmat` (JSON graph) + material instance `.simmatinst` yang hanya
  menyimpan override parameter dari material induk.
- Panel parameter: parameter yang diekspos graph muncul sebagai UI untuk instance.
- Preview: sphere/kubus/plane/mesh kustom, pilihan environment, rotasi lampu.
- **Terima:** graph 30+ node disimpan/dimuat identik; menyambungkan port bertipe salah
  ditolak dengan pesan jelas; membuat instance dari material lalu mengubah satu
  parameter tidak mengubah induknya; hapus node yang tersambung membersihkan link.

**Sudah ada** (25 test di `Tests/MaterialTests.cpp`): modul `Sim::Material` berisi
model graph `.simmat` beserta I/O JSON-nya, katalog node, validasi, dan kompiler
graph → Slang. Tiga kriteria terima yang tidak menuntut UI sudah terpenuhi dan
terkunci test — graph 33 node bolak-balik byte-per-byte identik, koneksi bertipe
salah ditolak dengan pesan yang menyebut kedua tipenya, dan menghapus node
membersihkan kabelnya.

Aturan tipe mengikuti Slang: skalar melebar ke vektor apa pun (`0.5` sah untuk
base color), arah sebaliknya tidak — memilihkan komponen mana yang dipakai adalah
keputusan yang harus ditulis pengguna lewat node Split. Tekstur dan bool berdiri
sendiri.

Pin node matematika bertipe `Numeric` — "skalar atau vektor float apa pun".
Tipe hasilnya disimpulkan dari yang paling lebar di antara masukannya, dan
**penyimpulan itu satu, dipakai bersama** validasi, kompiler, dan nanti panel.
Kalau masing-masing menyimpulkan sendiri, kanvas bisa menerima sambungan yang
kemudian ditolak kompiler, dan pengguna tidak punya cara menebak siapa yang
benar. `Dot` didaftarkan tersendiri karena hasilnya selalu skalar apa pun
masukannya.

**Yang dihasilkan kompiler mengisi `OpenPBRSurface`, bukan menghitung cahaya.**
Model shading-nya sudah ada dan sudah diuji di `openpbr.slang`; tugas graph hanya
menjawab "berapa nilai tiap parameter permukaan di titik ini". Memisahkan
keduanya berarti mengubah model shading tidak menyentuh satu pun material, dan
mengubah material tidak bisa merusak model shading.

Fungsinya diawali `OpenPBRSurface::defaults()` lalu hanya menimpa pin yang
benar-benar dikemudikan, sehingga nilai bawaan runtime tinggal di satu tempat —
shader itu sendiri — dan kode yang keluar tetap pendek untuk material yang hanya
menyentuh dua-tiga kanal, yaitu sebagian besarnya.

**Panelnya ada**, di atas `Sim::NodeGraph` yang sama dengan Graph Editor —
pembungkus imgui-node-editor dari E6.5 yang sudah menanggung seluruh jebakan
pustakanya. Bentuknya sengaja sama pula: daftar aset di kiri, kanvas di tengah,
Details dan hasil kompilasi sebagai tab di kanan, pemisah yang bisa digeser.
Keduanya editor node, dan pengguna yang berpindah di antara keduanya tidak
seharusnya perlu belajar dua kali.

Warna pin memakai tipe yang **disimpulkan**, bukan yang dideklarasikan: sebuah
Multiply yang diberi float3 memang menghasilkan float3, dan pin abu-abu yang
tidak pernah berubah warna menyembunyikan justru informasi yang paling berguna
saat menyambung. Lebar vektor dibedakan gradasi, bukan warna yang berjauhan —
keempatnya saling bisa disambung lewat pelebaran skalar.

Node keluaran tidak ada di palet dan tidak bisa dihapus: tepat satu boleh ada,
dan material tanpanya tidak punya arti.

**Material instance (`.simmatinst`) ada.** Instance tidak menyalin graph — ia
menyimpan GUID induk ditambah daftar parameter yang benar-benar diubah. Dua
akibat yang keduanya diinginkan: memperbaiki induk memperbaiki seluruh
instance-nya sekaligus, dan berkas instance tetap kecil berapa pun besar graph
induknya. Yang tidak ditimpa tidak ditulis, sehingga mengubah nilai bawaan di
induk mengalir ke instance yang tidak pernah menyentuh parameter itu — perilaku
yang sama dengan properti skrip di Inspector.

Nilai parameter disimpan **terurai menjadi angka**, bukan sebagai literal Slang.
`MaterialParameter::defaultValue` tetap teks karena `.simmat` harus ramah dibaca,
tapi nilai itu tidak pernah masuk kode yang dihasilkan — kompiler hanya menulis
nama variabelnya. Jadi yang dibutuhkan instance dan panel adalah angka, dan
menguraikannya sekali di satu tempat (`ParseValue`) lebih baik daripada di setiap
pemakainya. Penguraiannya tidak bergantung locale: `strtof` akan membaca "0.8"
sebagai 0 di locale yang memakai koma desimal, dan berkas yang isinya berubah
arti menurut setelan sistem adalah kelas bug yang tidak boleh dibuka.

Dengan ini **kriteria terima keempat terpenuhi** dan terkunci test: membuat
instance lalu mengubah satu parameter meninggalkan berkas induknya byte-per-byte
sama.

**Frame/grup dan komentar ada di kanvas material**, disalin dari Graph Editor
beserta seluruh jebakannya — jebakan itu milik pustaka kanvas, bukan milik salah
satu panel. Yang paling mahal: yang DISETEL adalah luas kotaknya, yang DIBACA
BALIK adalah ukuran node berikut judul dan bingkainya, dan menyimpan yang kedua
ke tempat yang pertama membuat grup tumbuh sedikit tiap putaran simpan-buka.
Diverifikasi tidak terjadi di sini: `[257, 145]` sebelum dan sesudah putaran
penuh simpan → tutup → buka → simpan. Grup dibuat dari seleksi lewat tombol
toolbar maupun **Ctrl+G**, dan judulnya diganti dengan klik ganda di tempat.

**Belum ada: preview — dan ia terhalang, bukan sekadar belum dikerjakan.**
`IViewportRenderer` adalah satu instance dengan satu target render, dipakai
bersama panel Viewport. Preview material yang memanggil `Render()` akan menimpa
gambar Viewport pada frame yang sama, karena keduanya menggambar
`ImGui::Image(ColorTarget())` dari tekstur yang sama.

Jalan keluarnya kecil dan sudah jelas: `render::CreateStubRenderer()` adalah
pabrik, jadi composition root bisa membuat instance KEDUA khusus preview dan
mengirimkannya lewat `EditorContext`. Itu juga bentuk yang benar untuk E8 —
preview material memang "satu view lagi", bukan kasus khusus.

Yang menahan bukan itu, melainkan gunanya: `StubRenderer` hanya menggambar grid
dan wireframe AABB, sehingga preview sekarang tidak akan memperlihatkan apa pun
tentang materialnya. Menuliskan evaluator OpenPBR kedua di CPU untuk mengisi
kekosongan itu justru pelanggaran terhadap alasan yang dipegang seluruh E7.1 —
model shading hanya boleh punya satu implementasi. Karena itu preview menunggu
E8.2, yang memang menyebutnya sebagai titik sambungnya.

Mode instance di panel terverifikasi lewat XTEST: material dibuat, parameter
ditambahkan dan disimpan, **New Instance** membuat `.simmatinst` yang membuka
dengan daftar parameter induknya, menimpa satu nilai memunculkan penanda `*`
beserta tombol *revert* dan menyalakan Save, berkas tersimpan hanya memuat
timpaannya, induknya tidak tersentuh, dan *revert* mengembalikan nilai induk.

**Tiga kelompok parameter OpenPBR sengaja belum ada di node keluaran** —
`subsurface_*`, `transmission_*`, `thin_film_*`. Ketiganya bukan dilupakan:
implementasi acuan Adobe menjalankannya lewat sampling dan integrasi volumetrik
yang tidak berbentuk benar untuk rasterizer, jadi masing-masing menuntut teknik
real-time tersendiri. Tekniknya sudah dipilih dan ditulis di
[`docs/RENDER-OPENPBR.md`](RENDER-OPENPBR.md), berikut cara membayarnya hanya
ketika dipakai. Satu langkah dari sana jatuh di E7.1 dan tidak menunggu E8:
**topeng fitur di `MaterialCompileResult`**, yang memungkinkan permutasi shader
dibangkitkan dari graph alih-alih cabang runtime.

### E7.2 — Particle Editor · ~5 sesi · 🔨 model, simulasi, dan panel selesai

- Sistem berbasis modul: Spawn (rate/burst), Shape (point/sphere/box/cone/mesh),
  Initial (velocity, size, color, rotation, lifetime), Over-Lifetime (curve untuk
  size/color/velocity/rotation), Force (gravity, drag, vortex, noise, point attractor),
  Collision, Sub-emitter, Renderer (billboard/stretched/mesh/ribbon, material, sorting).
- Satu efek memuat **beberapa emitter** yang berjalan pada satu timeline; tiap
  emitter punya tumpukan modul, benih, dan anggaran partikelnya sendiri.
- Widget khusus: **CurveEditor** (bezier, banyak kurva, preset ease) dan
  **GradientEditor** (color stop + alpha stop) — dipakai ulang oleh Animation dan Terrain.
- Timeline preview: play/pause/step/loop, scrub waktu, kontrol kecepatan, restart.
- Statistik langsung: partikel aktif, spawn/detik, peringatan anggaran.
- Aset `.simfx`.
- **Terima:** efek dengan 5 modul + 3 kurva disimpan/dimuat identik; scrub timeline
  bersifat deterministik (waktu yang sama → keadaan yang sama, karena RNG di-seed
  per waktu); menonaktifkan modul tidak menghapus datanya; preview berjalan pada
  100k partikel tanpa membekukan UI (simulasi CPU dulu, dibatasi anggaran).

**Sudah ada** (19 test di `Tests/ParticleTests.cpp`): `Curve` dan `Gradient` di
`Sim::Core`, modul `Sim::Particle` berisi model efek `.simfx` beserta I/O-nya,
simulasi CPU, dan panelnya. **Keempat kriteria terimanya sudah terpenuhi.**

**Sebuah efek memuat beberapa emitter, bukan satu tumpukan modul.** Ini bentuk
yang dipakai editor partikel mapan mana pun, dan alasannya bukan konvensi: api
yang sungguhan adalah nyala inti, percikan yang melompat, asap yang naik pelan,
dan bara yang jatuh — empat perilaku dengan bentuk, umur, dan gaya yang
berbeda-beda. Memaksa keempatnya ke dalam satu tumpukan modul berarti setiap
parameter harus bisa bercabang, dan tidak ada satu pun yang bisa disetel tanpa
mengganggu tiga yang lain. Keragaman datang dari menggabungkan emitter, bukan
dari menumpuk modul sejenis.

Tiga hal yang membuat penggabungan itu aman, dan ketiganya dikunci test:

1. **Menambah emitter kedua tidak menggeser satu partikel pun milik yang
   pertama.** Kalau bisa, menyusun efek berlapis berubah jadi menebak: setiap
   penambahan merusak apa yang sudah disetel sebelumnya.
2. **Benihnya milik emitter, bukan turunan dari posisinya di daftar.** Menyusun
   ulang emitter tidak boleh mengubah efek yang sudah jadi.
3. **Anggaran partikel dihitung per emitter.** Anggaran bersama membuat emitter
   yang lahir lebih dulu memakan seluruh jatah, dan emitter di bawahnya diam
   tanpa alasan yang terlihat di panel — sementara pengaturannya sendiri terlihat
   benar.

Seluruh emitter berjalan pada **satu jam yang sama**. Itu yang membedakannya dari
sekadar beberapa efek yang dijalankan berdampingan: ledakan yang kilatannya
menyala pada 0,0 dtk, pecahannya melompat pada 0,05 dtk, dan asapnya membubung
sesudahnya hanya terbaca sebagai satu kejadian kalau ketiganya membagi satu
timeline — termasuk saat timeline itu digeser.

Partikelnya membawa nomor emitter asalnya, bukan disimpan sebagai daftar terpisah
per emitter, karena penggambaran harus mengurutkan seluruh partikel menurut jarak
**lintas emitter**. Partikel yang dikelompokkan per emitter akan digambar
berkelompok, dan asap dari emitter kedua akan selalu menutupi api dari emitter
pertama walau letaknya di belakang.

Skema `.simfx` naik ke **v2**: modul dibungkus di dalam daftar `emitters`. Berkas
v1 — satu tumpukan modul di akar — tetap terbaca sebagai efek dengan satu emitter.
Berkas yang sudah ada di cakram tidak boleh hilang hanya karena bentuk
penyimpanannya berkembang.

**Determinisme scrub datang dari dua keputusan, bukan dari satu.** Pertama, angka
acak sebuah partikel diturunkan dari *nomor urutnya* lewat hash, bukan diambil
dari aliran RNG yang berjalan; aliran yang berjalan membuat partikel ke-100
bergantung pada berapa kali RNG dipanggil sebelumnya, sehingga menggeser timeline
mundur lalu maju menghasilkan efek yang berbeda dan penulisnya kehilangan
kepercayaan pada apa yang dilihatnya. Kedua, langkah waktunya tetap (1/60) dan
dihitung dari nol, karena melangkah 0→1 detik sekaligus tidak sama dengan enam
puluh langkah kecil. Menggeser maju melanjutkan; menggeser mundur memulai ulang;
keduanya mendarat di keadaan yang sama karena melewati batas langkah yang sama.

**Anggaran 100k partikel terukur 2,6 ms per langkah di Release**, 37 ms di Debug.
Test-nya karena itu memakai anggaran berbeda per build: 8 ms di Release — separuh
frame 60 Hz, menyisakan ruang untuk menggambarnya — dan 120 ms di Debug, longgar
tapi tetap menangkap kemunduran algoritmik.

`Curve` dan `Gradient` sengaja di `Sim::Core`, bukan di `Sim::Particle`: Terrain
(E7.3) dan Animation (E7.5) memakai keduanya, dan runtime harus bisa
mengevaluasinya tanpa menyeret ImGui. Serialisasinya untuk sementara masih di
modul Particle — memindahkannya ke Core sekarang menuntut Core membocorkan
nlohmann ke header publiknya, dan bentuk API yang benar baru terlihat ketika ada
pemakai kedua yang nyata.

**Panelnya ada**, berikut widget `CurveEditor` dan `GradientEditor` di
`EditorFramework/Widgets` — keduanya tidak tahu apa pun tentang partikel, karena
Terrain (E7.3) dan Animation (E7.5) akan memakainya juga. Yang masuk hanya sebuah
`Curve` atau `Gradient` dan ukuran kotaknya.

Kurva dan pita gradient **digambar dengan mencuplik evaluatornya**, bukan dengan
menghitung ulang bezier di widget. Itu yang menjamin apa yang terlihat sama
dengan apa yang dievaluasi simulasi — dua rumus untuk satu kurva adalah dua rumus
yang akan berbeda. Pegangan tangen hanya muncul untuk kunci yang terpilih: sepuluh
kunci dengan seluruh pegangannya sekaligus menjadi rimbun garis yang justru
menyembunyikan bentuk kurvanya. Alpha gradient ditampilkan sebagai tinggi terisi,
bukan papan catur — papan catur memberitahu ADA transparansi, tingginya
memberitahu BERAPA, dan itulah yang sedang disunting.

**Preview-nya digambar dengan draw list ImGui, bukan lewat `IViewportRenderer`.**
Kendalanya sama dengan preview material — satu instance, satu target render,
sudah dipakai panel Viewport. Bedanya, untuk partikel gambar 2D justru cukup
jujur: sebuah partikel adalah titik dengan ukuran dan warna, jadi yang terlihat
benar-benar memperlihatkan bentuk semburan, gerak, kurva ukuran, dan gradient
warnanya. Yang belum ada: billboard bertekstur dan urutan tembus pandang, yang
memang milik E8. Partikel jauh digambar lebih dulu — tanpa pengurutan, gumpalan
partikel terlihat berlubang.

Simulasinya milik `Sim::Particle`, bukan panel: panel hanya menentukan waktu mana
yang ingin dilihat. Itu yang membuat preview dijamin sama dengan yang dijalankan
runtime nanti.

Daftar emitter disusun sebagai **baris bertumpuk ke bawah**, bukan kolom
berdampingan. Alasannya bukan selera: tumpukan modul di bawahnya panjang, dan
daftar yang tumbuh ke arah yang sama dengan isinya bisa berbagi satu batang gulir
— sementara kolom berdampingan memaksa memilih antara nama emitter yang terpotong
atau lebar panel yang habis sebelum modulnya terlihat.

**Isi modul mendapat lingkup ID sendiri** (`PushID("body")`). Tanpa itu, sebuah
widget yang labelnya sama dengan nama modulnya — combo `"Shape"` di dalam modul
`"Shape"` — berbagi ID dengan header di atasnya, dan ImGui mengirim kliknya ke
salah satu saja: combonya tidak pernah terbuka, headernya yang menutup. Lingkup
terpisah memperbaiki seluruh golongan bug itu, bukan satu kejadiannya.

**Belum ada:** modul Sub-emitter, dan penetapan material/mesh pada modul Render —
keduanya menunggu sesuatu yang bisa menggambarnya.

### E7.3 — Terrain Editor · ~5 sesi · 🔨 semuanya kecuali LOD dan sculpt di viewport 3D

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

**Sudah ada** (46 test di `Tests/TerrainTests.cpp`): modul `Sim::Terrain` berisi
penyimpanan heightmap berubin, layer material dengan peta bobot splat, peta hole,
brush sculpt dan paint, undo per goresan untuk ketiga peta, dan I/O `.simterrain`
+ PNG 16-bit/8-bit + RAW, berikut panelnya. **Keempat kriteria terimanya sudah
terpenuhi** — yang tersisa di E7.3 adalah pengaturan LOD dan penyambungan ke
viewport 3D, keduanya menunggu E8.

Keempat kriteria itu bukan sekadar diuji belakangan; masing-masing memaksa satu
keputusan bentuk, dan itu sebabnya semuanya bisa dipenuhi tanpa renderer.

**Retakan di batas tile dicegah oleh bentuk penyimpanannya, bukan oleh
kedisiplinan.** Sampel dialamati secara global dan setiap sampel hanya tinggal di
satu tile: tile (tx,ty) memiliki rentang setengah terbuka `[tx·S, (tx+1)·S)`.
Tidak ada baris tepi yang disalin ke tetangganya, jadi tidak ada dua salinan yang
bisa berbeda. Engine yang menyimpan baris tepi di kedua tile harus menulis
keduanya setiap kali, dan satu jalur kode yang lupa menghasilkan celah yang baru
terlihat setelah di-render. Ongkosnya pindah ke pembuatan mesh — quad yang
menyeberangi batas perlu satu baris dari tetangga, jadi mesh tile membaca
`RawAt` alih-alih hanya array-nya sendiri. Itu pertukaran yang benar: membaca
tetangga adalah operasi terlokalisasi, sedangkan menjaga dua salinan tetap sama
adalah kewajiban yang tersebar ke setiap penulis. Test-nya menguncinya dengan
pernyataan yang lebih kuat daripada "tidak ada lompatan di jahitan": **terrain
4×4 ubin dan terrain satu ubin menghasilkan heightmap yang sama byte-per-byte
untuk goresan yang sama** — pengubinan tidak terlihat sama sekali.

**Undo menyalin blok 64², bukan tile.** Menyalin seluruh tile 2048² berarti 8 MB
untuk satu sentuhan brush selebar sepuluh meter — persis lonjakan memori yang
dilarang kriteria ketiga. Blok disalin sekali per goresan pada sentuhan pertama
yang mengenainya, jadi menyeret brush bolak-balik di tempat yang sama tidak
menyalin apa pun lagi. Satuan undo-nya **goresan**, bukan sentuhan: menyeret
menghasilkan puluhan sentuhan, dan riwayat yang mencatat tiap sentuhan menuntut
puluhan kali Ctrl+Z untuk membatalkan satu sapuan. Salinannya juga hanya satu
arah — undo *menukar* isi blok dengan yang hidup, sehingga redo gratis dan
memorinya separuh dari menyimpan sebelum-sesudah.

**Tile dialokasikan saat pertama ditulis.** Terrain 4 km dengan ubin 2048² pada
jarak sampel 0,25 m adalah 8×8 ubin = 512 MB kalau seluruhnya penghuni memori;
yang dibayar hanyalah yang benar-benar disunting. `BytesResident()` melaporkannya
apa adanya, jadi test menguji angka yang sama dengan yang akan dilihat panel —
bukan RSS proses, yang bergoyang karena sebab yang tidak ada hubungannya.

**PNG 16-bit round-trip tanpa kehilangan presisi adalah sifat penyimpanannya,
bukan sifat importernya.** Dengan tinggi float32 di dalam, tidak ada importer
yang bisa memenuhinya — setiap ekspor membulatkan. Karena itu tingginya `uint16`
di dalam rentang `[minHeight, maxHeight]`, dan ekspor maupun impor sekadar
menyalin. Konversi meter→sampel **membulatkan ke terdekat, bukan memotong**:
dengan pemotongan, membaca lalu menulis kembali nilai yang sama menurunkannya
satu langkah tiap kali, dan brush berkekuatan nol pun perlahan meratakan terrain.

**Enkoder PNG-nya ditulis sendiri, bukan lewat dependensi baru.**
`stbi_write_png` hanya menulis 8 bit — 256 tingkat tinggi, yaitu langkah empat
meter pada terrain setinggi kilometer. Yang kurang darinya bukan kompresornya
melainkan wadahnya: `stb_image_write` sudah memuat deflate lengkap, jadi yang
perlu ditambahkan hanya header, satu CRC per chunk, dan satu filter per baris
(±150 baris). Filternya adaptif per baris dengan heuristik yang dianjurkan
spesifikasi PNG; tanpa filter, heightmap 16-bit nyaris tidak terkompresi dan PNG
seukuran RAW tidak ada gunanya karena RAW sudah tersedia. Test menguncinya:
berkas PNG harus lebih kecil dari RAW yang setara.

Pembacaannya lewat `stbi_load_16`, **bukan dekoder tandingan buatan sendiri**.
Menulis dan membaca dengan implementasi yang sama akan membuat round-trip lulus
walaupun berkasnya bukan PNG yang sah — yang teruji hanya konsistensi dengan
diri sendiri. Berkas keluarannya juga sudah diperiksa dengan Pillow di luar
build: mode `I`, seluruh sampel cocok, seluruh CRC chunk sah.

Implementasi stb kini dikompilasi **tepat sekali** di `Third-Party/stb/stb_impl.cpp`
(target `Stb::Impl`). Selama hanya Assets yang memakai gambar, "tepat sekali"
bisa dijaga dengan menaruh `#define ..._IMPLEMENTATION` di modul itu; begitu
Terrain ikut memakainya, keduanya membawa definisi yang sama dan penautan gagal.
`stbi_zlib_compress` dibungkus `stb_impl::Deflate` di TU yang sama, sehingga
buffer hasil `malloc`-nya tidak pernah keluar dari kode yang tahu cara
membebaskannya.

Heightmap tinggal di **berkas pendamping**, bukan di dalam JSON `.simterrain`.
Sebuah heightmap 4096² adalah 33 MB angka; sebagai teks JSON ia menjadi ratusan
megabyte yang tidak bisa dibaca manusia, tidak berguna untuk di-diff, dan lambat
diurai. Yang ada di `.simterrain` adalah yang memang ingin dibaca dan diubah
orang: ukuran ubin, skala, rentang tinggi.

Impor yang ukurannya tidak cocok **ditolak dengan pesan yang menyebut kedua
ukuran**, bukan diskala diam-diam. Menskala ulang adalah cara paling halus untuk
merusak peta seseorang: hasilnya terlihat masuk akal dan tetap salah.
`ReadHeightmapPng` tetap tersedia untuk membaca ukurannya, supaya panel bisa
menawarkan menyesuaikan terrain alih-alih sekadar menolak.

#### Panelnya

**Preview-nya peta 2D dari atas, bukan viewport 3D.** Di viewport 3D-lah tempat
memahat yang sebenarnya, dan ke situ panel ini akan tersambung begitu E8 bisa
menggambar terrain — tapi sampai itu ada, kursor brush akan melayang di atas
ruang kosong, dan alat yang tidak bisa dilihat hasilnya tidak bisa diuji maupun
dipakai. Peta dari atas juga bukan sekadar penambal: untuk membaca *bentuk*
terrain — di mana lembahnya, seberapa lebar punggungannya, apakah jalan yang
dipahat lurus — pandangan dari atas lebih jujur daripada perspektif, dan itu
sebabnya alat terrain mana pun menyediakannya berdampingan dengan viewport-nya.

**Digambar sebagai kisi bervertex warna lewat draw list, bukan tekstur.** Panel
tidak punya jalan untuk mengunggah buffer piksel: `EditorContext` tidak membuka
`rhi::Device`, `Sim::RHI` adalah dependensi PRIVATE milik `Sim::Render`, dan
satu-satunya penghasil `ImTextureID` yang terjangkau panel adalah
`IViewportRenderer::ColorTarget()` dan `IThumbnailCache::Request()` — yang
menerima *path berkas*, bukan buffer. Batas itu benar dan tidak layak ditembus
demi sebuah preview. Kisi ber-shading digambar langsung dari terrain, jadi ia
selalu sinkron dan tidak ada yang perlu diunggah ulang setiap sentuhan brush.

**Gradasi warna dan bayangannya diskalakan ke rentang tinggi yang terlihat,
bukan ke `minHeight`/`maxHeight` yang dikonfigurasi.** Ini ditemukan lewat
pengujian XTEST, bukan dari membaca kode: terrain baru berentang seribu meter
sementara goresan pertama baru setinggi dua meter, dan diukur terhadap rentang
konfigurasi seluruh peta tetap satu warna rata — goresannya terjadi, tapi
kesan pertamanya adalah alat yang rusak. Lerengnya pun dinyatakan dalam piksel
layar, bukan meter, sehingga relief tetap terbaca pada peta dua kilometer maupun
pada zoom sepuluh meter tanpa slider "vertical exaggeration" yang harus disetel
ulang tiap berpindah skala. Yang dibayar adalah arti warnanya berubah saat
digeser; itu dibayar balik dengan legenda yang menyebutkan meternya.

**Sentuhan brush dipancarkan pada langkah waktu tetap DAN jarak tetap.**
Mengalikan kekuatan brush dengan `dt` frame terdengar benar tapi tidak: pada
mesin 144 Hz sebuah goresan menerima lebih dari empat kali lipat sentuhan
dibanding mesin 30 Hz, dan karena tiap sentuhan dibulatkan ke sampel 16-bit,
bukit yang sama digores dengan cara yang sama menjadi bukit yang berbeda.
Langkah tetapnya sengaja angka yang sama dengan simulasi partikel.

Langkah waktu saja ternyata belum cukup — juga terlihat lewat XTEST. Seretan
cepat memindahkan kursor lebih jauh daripada satu jari-jari di antara dua
sentuhan, dan yang tertinggal adalah rangkaian manik-manik alih-alih garis.
Jumlah sentuhan karena itu diambil dari yang lebih besar antara langkah waktu
dan langkah jarak, lalu **jatah waktunya dibagi rata** ke seluruhnya: menyeret
cepat menyebarkan material yang sama di lintasan yang lebih panjang, bukan
menumpuk lebih banyak.

Batas susulannya membatasi **jumlah waktu**, bukan jumlah sentuhan. Versi
pertamanya membatasi jumlah sentuhan, dan test memperlihatkan akibatnya: goresan
biasa pada mesin lambat kehilangan seperlima materialnya diam-diam. Yang dibatasi
harus yang memang ingin dibatasi — seperempat detik material, dibagi ke sebanyak
apa pun sentuhan yang dibutuhkan untuk menutup lintasannya.

**Memuat heightmap tidak mewujudkan ubin yang seluruhnya datar.** Juga temuan
dari menjalankan panelnya: berkas heightmap memuat seluruh peta, jadi tanpa
penyaringan ini membuka terrain membatalkan seluruh guna alokasi malas — terrain
4×4 km langsung menghuni memori sepenuhnya, padahal bagian yang datar tidak
menyimpan apa pun yang belum diketahui. Memindai lebih murah daripada
mengalokasi, dan jauh lebih murah daripada menahannya.

**Suntingan tidak melewati `CommandHistory`**, sama seperti Material, Graph, dan
Script Editor. `Command.h` sempat merencanakan sebaliknya — patch heightmap
disebut namanya di sana sebagai calon peng-override `MemoryCost()`. Ternyata
tidak bisa: terrain adalah dokumen yang dibuka dan ditutup, sedangkan
`CommandHistory` tidak punya cakupan dokumen, jadi membuka terrain lain akan
meninggalkan command yang membatalkan goresan pada heightmap yang sama sekali
berbeda. Catatannya di `Command.h` sudah dikoreksi. Ctrl+Z panel hanya berlaku
saat panel ini fokus — dua pemilik untuk satu pintasan berarti satu di antaranya
diam-diam kalah.

Impor heightmap **mengosongkan riwayat goresan**. Impor bukan goresan, jadi
riwayatnya tidak lagi cocok dengan apa yang ada di peta; membiarkannya berarti
satu Ctrl+Z memasang kembali potongan heightmap lama di atas yang baru diimpor.

#### Layer material dan peta bobot

**Bobot layer dasar tidak disimpan. Ia sisa: `255 − Σ(bobot layer lain)`.**

Itu yang membuat "total bobot tiap sampel selalu 255" menjadi sifat bentuk
penyimpanannya, bukan kewajiban yang harus diingat setiap penulis — argumen yang
sama dengan kepemilikan sampel setengah terbuka pada heightmap-nya. Dan invarian
itulah yang menentukan apakah hasil paint bisa diduga: kalau totalnya bebas,
sebuah sampel bisa berakhir bertotal 40, dan perender harus memilih di antara dua
kesalahan. Menormalkan saat menggambar berarti menghapus semua layer tidak
menghapus apa pun, karena sisa sekecil apa pun diregangkan kembali menjadi penuh.
Tidak menormalkan berarti ada bercak gelap yang tidak dicat siapa pun. Dengan
layer dasar sebagai sisa, tidak ada sampel yang bisa kehilangan seluruh
materialnya. Konsekuensinya layer 0 tidak bisa dihapus maupun dipindah, dan
mengecat layer 0 berarti menghapus layer di atasnya — yang memang persis arti
"mengecat kembali ke dasar".

**Deskripsi layer tinggal di dalam `Terrain`, bersama peta bobot yang
ditunjuknya**, bukan di `TerrainDocument`. Peta bobot layer ke-2 tidak berarti
apa-apa tanpa tahu layer ke-2 itu apa, jadi menghapus atau memindahkan layer
harus menggeser keduanya. Disimpan di dua tempat, setiap pemanggil wajib
melakukan dua hal — dan yang lupa menghasilkan cat yang berpindah ke material
yang salah tanpa satu pun tanda. `SaveDocumentToString` karena itu menerima
daftar layer sebagai parameter terpisah; ia serialisasi, bukan salinan hidup.

**Peta bobot ikut berubin dan ikut dialokasikan malas**, dengan satu tambahan:
menulis nol ke ubin yang belum ada tidak melakukan apa-apa. Tanpa pintasan itu,
sekali sapu penghapus di atas terrain 4×4 km sudah cukup untuk mewujudkan seluruh
peta bobotnya — kebalikan dari yang diminta. Dan karena menyusutkan layer lain
hanya menulis ketika totalnya benar-benar terlampaui, mengecat layer pertama di
atas terrain kosong tidak menyentuh peta mana pun kecuali miliknya sendiri.

**`WriteWeights` sengaja tidak menjaga invariannya, dan pemuat berkas memanggil
`NormalizeWeights()` setelah seluruh layer masuk.** Dinormalkan per berkas, layer
yang dibaca lebih dulu akan menggerus layer berikutnya hanya karena urutan
bacanya. Normalisasinya sendiri ada karena peta bobot datang dari berkas yang
bisa disunting di luar editor: invarian yang hanya dijaga jalur paint adalah
invarian yang batal begitu ada jalan masuk kedua.

**Sentuhan cat dibulatkan menjauhi nilai sekarang, bukan ke terdekat.** Dengan
pembulatan ke terdekat, setiap langkah yang lebih kecil dari setengah tingkat
membulat kembali ke tempatnya, dan itu terjadi di dua tempat: pada sentuhan
terakhir sebelum penuh — sehingga "cat sampai penuh" menjadi mustahil — dan di
pinggir kuas, di mana bobotnya kecil sehingga sampel di sana tidak pernah
tersentuh sama sekali. Yang kedua lebih buruk: jari-jari yang sebenarnya menjadi
lebih kecil daripada lingkaran yang digambar kursor, dan tidak ada angka di panel
yang menyebutkannya. Yang dibayar adalah pita tipis di pinggir kuas terisi lebih
cepat daripada janji profilnya, terbatas satu tingkat per sentuhan; bentuk
profilnya sendiri tetap terbaca, dan itu yang dikunci sebuah test tersendiri.

#### Peta hole

**Hole adalah sifat quad, bukan sifat sampel.** Yang dihapus perender adalah segi
empat di antara empat sampel, bukan sampelnya. Disimpan per sampel ada dua
pilihan dan keduanya salah: quad dihapus bila salah satu sudutnya ditandai — maka
lubang yang tergambar selalu lebih besar daripada yang dicat dan lubang selebar
satu quad mustahil dibuat; atau quad dihapus hanya bila keempat sudutnya ditandai
— maka lubang kecil tidak pernah muncul sama sekali. Disimpan per quad, yang
dicat dan yang tergambar adalah benda yang sama. Kolom dan baris terakhir peta
karena itu tidak memiliki quad, dan `HoleAt` di sana selalu false.

**Hole biner, jadi `falloff` pada kuasnya bekerja sebagai ambang, bukan
gradasi** — separuh quad yang berlubang bukan sesuatu yang bisa digambar perender
mana pun. Kursornya menggambar lingkaran pada ambang setengah bobot, bukan pada
jari-jari penuh: kursor yang menggambar jari-jari penuh untuk alat yang memotong
sampai setengahnya berbohong tepat pada satu-satunya hal yang ditanyakan orang
kepadanya.

**Disimpan satu byte per quad, bukan satu bit.** Bit delapan kali lebih hemat,
tapi yang dibayar untuk kemasan itu bukan hanya kode pengemasnya melainkan blok
jurnal undo yang tidak lagi sejajar dengan batas kata pada ukuran ubin yang bukan
kelipatan 64. Satu byte membuat peta hole memakai jalur salin, jurnal, dan
alokasi yang sama persis dengan peta bobot. Hole jarang, dan ubin yang tidak
berlubang tidak dialokasikan sama sekali — jadi yang dihemat pengemasan adalah
delapan per sembilan dari sesuatu yang sudah mendekati nol.

**Jumlah lubang dijaga bertahap, termasuk saat undo menukar isi blok.** Panel
menampilkannya tiap frame; menghitungnya saat ditanya berarti membaca puluhan
megabyte demi satu angka, dan menghitung ulang setelah tiap undo membuat Ctrl+Z
pada terrain besar tersendat karena pekerjaan yang tidak ada hubungannya dengan
besar goresannya.

#### Yang mengikat ketiganya

**Satu goresan mencakup ketiga peta.** Bukan karena satu goresan pernah menyentuh
ketiganya — tidak pernah — melainkan karena riwayat yang terpisah per peta berarti
Ctrl+Z yang artinya bergantung pada tab mana yang sedang terbuka, dan itu tidak
bisa ditebak siapa pun. `RemoveLayer` dan `MoveLayer` sebaliknya **membuang**
riwayat: jurnalnya menunjuk layer lewat indeks, dan indeks yang sama sesudahnya
menunjuk layer yang berbeda — satu Ctrl+Z akan memasang bobot layer yang dihapus
ke atas layer yang bukan pemiliknya, kerusakan senyap yang lebih buruk daripada
undo yang hilang.

**Peta yang tidak menyimpan apa pun tidak ditulis, dan namanya tidak dicatat.**
Satu aturan untuk heightmap, peta bobot, dan peta hole sekaligus: tidak ada
berkas nol byte yang harus dijelaskan, dan tidak ada nama di JSON yang menunjuk
berkas yang tidak ada. Nama berkasnya tetap dicatat di `.simterrain` alih-alih
ditebak dari nama dokumennya, karena pemuat yang menebak akan diam-diam mengambil
peta milik terrain lain yang kebetulan senama, dan tidak punya cara membedakan
"berkasnya belum ada" dari "layernya memang belum pernah dicat".

Enkoder PNG-nya yang sama melayani 8 bit maupun 16 bit. Bukan karena
`stbi_write_png` tidak mampu menulis 8 bit — ia mampu — melainkan supaya seluruh
berkas pendamping sebuah terrain dihasilkan satu penulis: dua penulis berarti dua
perilaku yang bisa berbeda dalam hal yang baru terlihat pada berkas orang lain,
yaitu pilihan filter, ukuran keluaran, dan penanganan gambar kosong. Berkas
keluarannya diperiksa di luar build dengan Pillow dan pemeriksa CRC tersendiri:
mode `L`, seluruh 4096 sampel bobot cocok, peta hole hanya berisi 0/255.

**Peta 2D-nya punya tiga tampilan** — Relief, Layers, dan Weight — dan tampilan
itu berganti mengikuti tab alat yang dipilih, tapi hanya pada saat tabnya
berganti. Berpindah tab adalah tindakan yang disengaja, jadi pantas mengubah
tampilan; memaksakannya tiap frame akan membuat combo tampilan di bawahnya tidak
bisa dipakai sama sekali. Ketiganya memakai bayangan lereng yang sama: peta bobot
yang kehilangan bayangannya menjadi bercak abu-abu yang tidak bisa dicocokkan
dengan bentuk terrain di bawahnya, padahal justru itu yang ingin dilihat saat
mengecat.

**Lubang digambar sebagai persegi quad-nya sendiri, bukan dicuplik di simpul kisi
seperti tingginya.** Tinggi boleh dicuplik: puncak yang hilang pada zoom jauh
tetap menyisakan lerengnya, jadi petanya tetap benar walaupun kasar. Lubang tidak
punya lereng — ia ada atau tidak ada — dan lubang yang tidak tergambar terbaca
sebagai lubang yang tidak ada. Peta hole karena itu dipindai pada kisi quad-nya
sendiri, dengan langkah yang melebar mengikuti zoom supaya ongkosnya terbatas,
dan sel sebaris digabung menjadi satu persegi supaya jumlah perintah gambarnya
tidak ikut melebar.

**Belum ada:** pengaturan LOD, dan sculpt maupun paint langsung di viewport 3D.
Keduanya baru berarti ketika ada yang menggambar terrainnya di E8. Alat brush-nya
sendiri tidak perlu ditulis ulang saat itu — penjadwal sentuhan `BrushStroke`
hanya menerima posisi dunia dan sebuah callback, dan dari mana posisi itu datang
bukan urusannya. Sculpt dan paint sudah berbagi penjadwal yang sama persis itu,
karena masalahnya memang sama persis: laju frame yang tidak boleh mengubah hasil,
dan seretan cepat yang tidak boleh meninggalkan manik-manik.

### E7.4 — Vegetation Editor · ~4 sesi · 🔨 semuanya kecuali menggambar mesh-nya

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

**Sudah ada:** modul `Sim::Vegetation` (`Code/Vegetation`) berisi layer beserta
aturan penempatannya, sebaran Poisson disk deterministik, peta kepadatan yang
dicat, suntingan tangan yang bertahan, undo per goresan, dan format `.simveg`
beserta PNG kepadatan pendampingnya; panel Vegetation Editor dengan peta 2D,
tiga tab alat, impor/ekspor mask, dan tombol Scatter. 38 test (241.982
assertion) — termasuk keempat kriteria terimanya. Keempatnya terukur, bukan diperkirakan: 1.118.891 instance
dalam **416 ms** (Release; 2.137 ms pada build Debug) dan berkas tersimpan
**979 byte**.

**Yang menentukan seluruh rancangan: instance tidak disimpan, aturannya yang
disimpan.** Sejuta instance adalah 36 MB di memori dan puluhan megabyte di
berkas apa pun yang menuliskannya satu per satu — dan seluruhnya bisa dihitung
ulang dari beberapa ratus byte. Kriteria "berkas < 5 MB" karena itu bukan target
kompresi melainkan akibat: yang ditulis hanya aturan, benih, peta kepadatan yang
dicat, dan suntingan tangan. Yang tersisa dari kriteria itu tinggal memastikan
suntingan tangan tidak tumbuh tak terbatas, dan itu memang dibatasi tangan yang
membuatnya.

**Sebaran ditulis sendiri sampai ke bit terakhirnya, bukan memakai
`std::mt19937` beserta distribusinya.** Mesin Mersenne-nya memang menghasilkan
barisan bit yang sama di mana pun, tapi standar tidak menetapkan bagaimana
sebuah `std::uniform_real_distribution` mengubah bit menjadi angka — dua pustaka
standar boleh berbeda dari benih yang sama. Kriteria "benih yang sama, sebaran
yang sama di mesin berbeda" mustahil dipenuhi dengan pinjaman itu. Yang dipakai
splitmix64 (perkalian, geser, XOR 64-bit) dengan pecahan dibuat sebagai
`(u32 >> 8) * 2⁻²⁴` — perkalian pangkat dua yang tidak pernah membulatkan di FPU
IEEE-754 mana pun. Satu-satunya libm di jalur penerimaan kandidat adalah kosinus
ambang kemiringan, dan ia dihitung sekali per sebaran dalam double lalu
dikuantisasi ke kisi 1/65536 — selisih satu ULP antar-libm hilang jauh sebelum
sampai ke perbandingan. Test mengunci hasilnya pada satu nilai hash tetap; ia
tidak bisa membuktikan mesin lain setuju, tapi ia menutup satu-satunya cara
sifat itu hilang dalam praktik, yaitu seseorang menukar aliran acaknya dengan
yang "setara".

**Susunan titiknya diputuskan sebelum aturan mana pun dilihat.** Ini yang paling
mudah salah, dan versi pertamanya memang salah: menguji jarak minimum *setelah*
aturan tinggi dan kemiringan terdengar hemat — buat apa menghitung jarak untuk
kandidat yang toh ditolak. Test yang menuntut "mengubah aturan hanya menyaring"
langsung menangkapnya. Kandidat yang ditolak aturan tidak menempati slot dalam
pemadatan, jadi tetangga yang tadinya terhalang olehnya menjadi diterima:
mengetatkan sebuah aturan bukan menipiskan hutan melainkan **menyusunnya
ulang**, dan setiap penghapusan tangan yang menunjuk posisi lama menunjuk ke
tempat yang sudah tidak ada isinya. Dengan pemadatan diputuskan lebih dulu,
posisi hanya ditentukan benih, jarak minimum, dan ukuran dunia; setiap aturan
menjadi murni pengurang. Kerapatan di dalam daerah yang lolos tidak ikut
berkurang, karena susunannya memang menutupi seluruh dunia sejak awal.

**Penghapusan tangan dikunci pada posisi, bukan nomor urut.** Nomor urut bergeser
begitu sebuah aturan diubah, dan penghapusan yang bergeser menghapus pohon yang
salah tanpa ada yang menyadarinya. Kuncinya XZ yang dibulatkan ke milimeter, dan
ia unik menurut konstruksi: sebaran menjamin jarak antar-instance tidak kurang
dari `minDistance`, yang dijepit jauh di atas satu milimeter. Satu-satunya
perubahan yang membatalkan daftar hapus adalah mengubah jarak minimum atau
benihnya — keduanya memang memindahkan semuanya, dan panel menyebutkannya di
tooltip slidernya. Instance yang dihapus **tetap memegang tempatnya** dalam
pemadatan; kalau ia dilepas, menghapus satu pohon akan menumbuhkan pohon lain di
sebelahnya.

**Kisi Poisson-nya hanya menyimpan dua baris sel.** Uji jarak minimum berjangkauan
satu sel, dan kisi dipindai baris demi baris, jadi yang harus diingat hanyalah
baris sekarang dan baris sebelumnya — baris berikutnya masih kosong. Tanpa itu,
terrain empat kilometer dengan jarak minimum satu meter menuntut setengah
gigabyte kisi untuk sebuah uji berjangkauan satu meter. Kapasitas empat instance
per sel bukan tebakan: sel berukuran `minDistance` persegi bisa dibagi empat
sub-persegi yang diagonalnya `minDistance/√2`, jadi dua instance tidak mungkin
berbagi sub-persegi — dan empat sudut sel bisa terisi seluruhnya, jadi empat juga
tidak bisa dikurangi.

**Memahat di bawah hutan menempelkan ulang, bukan menyebar ulang.** Menyebar
ulang berarti setiap goresan brush mengocok seluruh hutan: pohon berpindah,
hilang, dan muncul di tempat lain sementara yang diminta hanyalah tanah di
bawahnya naik. `RefreshHeights` menjaga XZ setiap instance dan hanya memperbarui
tinggi dan normalnya. Aturan penempatan sengaja tidak diperiksa ulang di sana —
instance yang lerengnya menjadi terlalu curam tetap berdiri sampai disebar ulang,
karena pohon yang lenyap di bawah kuas sculpt tanpa ada yang menghapusnya lebih
buruk daripada pohon yang berdiri di tempat yang tidak semestinya.

**Sebaran ulang selalu utuh, tidak pernah sepotong.** Pemadatan Poisson
bergantung pada urutan, jadi menyebar ulang sebuah persegi akan diam-diam
berbeda dari yang dijanjikan benihnya di sepanjang tepi persegi itu — dan
"diam-diam berbeda" adalah kebalikan dari seluruh guna benih. Akibatnya mengecat
kepadatan tidak langsung menumbuhkan atau mencabut apa pun; panel menyebutkan itu
di baris status ("Density edited — Scatter to apply") alih-alih menyembunyikannya
dengan sebaran sepotong.

**Mask yang diimpor dan peta kepadatan yang dicat adalah benda yang sama.**
Aturan penempatan menyebut "mask tekstur", dan mask yang hanya bisa dibuat dengan
kuas di panel ini bukan itu — mask yang berguna datang dari peta yang sudah ada
(sebaran hujan, keteduhan, zona terlarang), dan tidak satu pun digambar tangan.
Menjadikannya masukan kedua yang dikalikan dengan yang dicat terdengar lebih
luwes, tapi itu membuat pertanyaan "kenapa di sini kosong" punya dua tempat untuk
dijawab, dan kuas tidak bisa memperbaiki apa yang datang dari berkas. Satu peta,
dua cara mengisinya.

Impor **mencuplik ulang** gambar berukuran berapa pun, sedangkan pemuat berkas
pendamping menolak ukuran yang tidak cocok. Keduanya benar dan bukan
ketidakkonsistenan: jumlah sampel heightmap *adalah* identitas terrainnya,
sedangkan resolusi kisi kepadatan semata pilihan internal editor
(`densityCellSize`) yang tidak bisa ditebak orang yang menyiapkan gambarnya di
Substance. Menolak 1024² hanya karena kisinya kebetulan 1025² berarti menyuruh
orang membalik rekayasa angka yang bukan urusannya. Yang dibayar — mask yang
lebih halus daripada kisinya kehilangan detail — disebutkan di notifikasinya,
karena detail yang hilang diam-diam terbaca sebagai kuas yang tidak bekerja.

**Menanam tangan adalah klik, menghapus adalah seretan.** Kuas yang menanam
sambil diseret harus tahu apakah sudah ada sesuatu di dekat sentuhannya, dan
menjawab itu pada daftar berisi sejuta instance menuntut indeks spasial yang
jalur prosedural sama sekali tidak membutuhkannya — kisinya hanya hidup selama
penyebaran. Menanam tanpa pemeriksaan itu menumpuk puluhan pohon di satu titik
pada seretan paling pelan sekalipun.

**Peta 2D-nya digambar abu-abu, bukan dengan gradasi topografi seperti di
penyunting terrain.** Bukan penyederhanaan: warna di panel ini sudah punya
pekerjaan lain, yaitu membedakan layer vegetasi, dan latar berwarna membuat titik
hijau di atas lereng hijau tidak terlihat sama sekali. Titiknya digambar dengan
langkah yang melebar mengikuti **perkiraan jumlah yang terlihat**, bukan jumlah
seluruhnya — dengan langkah tetap, memperbesar ke satu rumpun tetap membuang 49
dari 50 pohonnya walaupun yang tersisa di layar tinggal seratus. Berapa yang
dilewati disebutkan di baris status, karena peta yang menggambar sebagian dan
diam soal itu berbohong tepat tentang kerapatan — satu-satunya hal yang dinilai
orang di sini.

**Belum ada:** menggambar mesh vegetasinya sendiri, LOD dan billboard yang
benar-benar berlaku, dan menanam/menghapus langsung di viewport 3D. Ketiganya
menunggu E8; jarak LOD, billboard, dan cull sudah tersimpan di `.simveg` dan
tinggal dibaca perendernya.

### E7.5 — Animation Editor · ~6 sesi · 🔨 seluruh runtime; panelnya belum

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

**Sudah ada:** modul `Sim::Animation` (`Code/Animation`) — rangka berurutan
topologis dengan bind pose dan kamus retarget, klip berbasis `Curve` yang sama
dengan Particle dan Terrain, pencampuran pose beserta bone mask dan layer aditif,
event, penanda fase, root motion, blend tree 1D/2D, state machine berlapis
dengan transisi berkondisi, dan format `.simskel`/`.simanim`/`.simanimgraph`.
49 test.

Dua kriteria terima sudah terpenuhi dan terukur:

- *"klip 60 detik pada rig 100 bone bisa di-scrub mulus"* — rig 100 bone, 900
  track, 162 ribu kunci, posisi acak: **0,090 ms per frame** (Release; 0,57 ms
  pada Debug), berbanding anggaran 16,7 ms satu frame 60 Hz.
- *"kondisi transisi tersimpan/dimuat identik"* — bolak-balik lewat berkas,
  lengkap dengan tiga tipe parameter dan pembanding yang berbeda-beda, lalu
  serialisasi ulangnya dibandingkan byte demi byte.
- *"blend tree 2D menghasilkan bobot yang benar pada titik uji"* — tepat di
  simpul bobotnya persis 1, setengah jalan antar-simpul berbagi rata, jumlahnya
  selalu 1, dan di luar sebaran ia jatuh ke simpul terluar ke arah itu.

**Rotasi ditulis sebagai tiga kurva Euler dan dipakai sebagai kuaternion.** Ini
pemisahan yang disengaja antara bentuk penulisan dan bentuk pemakaian. Menulis
menuntut kanal skalar — dope sheet, tangen, dan "geser kunci ini tiga frame"
hanya punya arti pada angka tunggal terhadap waktu, dan tidak ada penyunting
kurva yang bisa menampilkan kuaternion; setiap DCC memperlihatkan Euler kepada
animator karena alasan yang sama. Memakai menuntut kuaternion, karena dua sudut
Euler yang dicampur komponen demi komponen tidak menghasilkan rotasi di
antaranya. Kurvanya karena itu dicuplik menjadi kuaternion saat sampling, dan
seluruh pencampuran terjadi sesudah itu. Yang dibayar: gimbal lock menjadi sifat
kurva yang ditulis — terlihat dan bisa dikendalikan penulisnya, sama seperti di
DCC mana pun.

**Track menunjuk bone lewat nama, bukan indeks.** Indeks bergeser begitu ada bone
disisipkan, dan track yang bergeser menganimasikan tulang yang salah tanpa ada
yang menyadarinya. Nama juga yang membuat retargeting mungkin sama sekali:
pemetaannya dihitung sekali ke dalam `ClipBinding`, terpisah dari klipnya —
karena satu klip bisa dipasang ke lebih dari satu rangka, dan itu persis arti
retargeting. Kamus dua rig disusun lewat nama rig standar yang dicatat
masing-masing, jadi N rig menuntut N kamus, bukan N².

**Dua hal masuk lingkup yang tidak disebut daftar di atas**, keduanya dari membaca
Esoterica (lihat `docs/DEPENDENCIES.md`):

- **Penanda fase.** Dicampur pada waktu ternormalisasi yang sama, klip jalan dan
  klip lari berada pada fase langkah yang berbeda — kaki kiri menapak di satu
  klip sementara di klip lain sedang terangkat, dan hasilnya kaki yang menggeser
  di tanah. Penanda fase memetakan waktu satu klip ke waktu berfase sama pada
  klip lain, jadi yang dicampur selalu menapak dengan menapak.
- **Root motion.** Tanpanya karakter berjalan di tempat sementara kapsul
  fisikanya diam, atau meluncur karena kecepatan kapsulnya tidak pernah cocok
  dengan langkah kakinya.

**Event memakai selang setengah terbuka `[from, to)`.** Itu yang membuat sebuah
event menyala tepat sekali per lintasan: dengan selang tertutup ia menyala dua
kali di batas frame, dan dengan selang terbuka event pada waktu 0 tidak pernah
menyala. Frame yang tersendat dan melompati lima event menyalakan kelimanya —
event yang hilang di mesin lambat adalah bug yang hanya muncul di mesin lambat.

**Blend tree 2D memakai interpolasi gradient band**, bukan "simpul terdekat"
maupun jarak terbalik. Yang terdekat melompat di batas antar-simpul; jarak
terbalik membuat setiap simpul menyumbang di mana-mana, jadi berdiri tepat di
atas simpul "lari" tetap mencampurkan sedikit "mundur". Gradient band memberi
keduanya sekaligus: tepat di sebuah simpul bobotnya persis 1, di sepanjang ruas
antar-simpul ia linear, dan peralihannya mulus di mana pun. Blend 1D justru
tidak memakainya — di satu dimensi yang diharapkan orang hanya dua simpul
mengapit yang aktif, karena pada kecepatan 3 m/s tidak ada alasan klip "diam"
ikut menyumbang.

**Trigger padam sesudah seluruh lapis mengevaluasi transisinya, bukan per lapis.**
Dipadamkan per lapis, satu trigger hanya terlihat lapis pertama dan lapis atas
yang seharusnya ikut bereaksi diam saja. Bool tidak padam sendiri: ia keadaan
yang dimiliki gameplay, sedangkan trigger kejadian yang dinyalakan sekali —
tanpa pemadaman itu, satu tombol lompat membuat karakter melompat selamanya.

**Kondisi menunjuk parameter lewat nama, dan pembandingnya ditulis sebagai nama
juga.** Indeks tidak bisa memenuhi kriteria "tersimpan/dimuat identik":
menyisipkan parameter menggeser seluruh indeks sesudahnya, jadi berkas yang sama
berarti kondisi yang berbeda setelah daftar parameternya disunting. Angka enum
menghadapi hal yang sama pada perubahan kode.

**`Blend` boleh menulis ke pose yang sama dengan salah satu masukannya**, dan
runtime graph mengandalkannya — ia mencampur lapis demi lapis ke dalam satu pose
hasil, dan pose antara per lapis berarti satu alokasi per lapis per frame. Test
menangkap versi pertamanya yang tidak aman: `Pose::Resize` memakai `assign`,
jadi masukannya terhapus sebelum sempat dibaca dan hasilnya pose kosong.

**Durasi state adalah rata-rata durasi klipnya yang ditimbang bobot**, bukan
durasi klip pertama. Blend tree yang mencampur jalan (1,2 s) dengan lari (0,7 s)
harus mempercepat langkahnya secara mulus saat bobotnya bergeser; memakai salah
satu durasi saja membuat langkahnya melompat tepat ketika bobotnya berpindah
dominan.

**Blend tree tidak bisa disarangkan di dalam blend tree.** Batas yang disengaja:
pohon bersarang menuntut evaluasi rekursif beserta bobot berjenjang, dan hampir
seluruh gunanya sudah tercakup blend tree 2D — yang memang alasan 2D ada.

**Belum ada:** IK, panel penyuntingnya (pohon skeleton, dope sheet, penyunting
kurva, kanvas state machine), dan penyaluran event ke Lua. Dua kriteria terima
menempel di sana: memindahkan keyframe bisa di-undo, dan event memanggil fungsi
Lua saat preview mencapai frame tersebut — `GraphInstance::Events()` sudah
melaporkan nama event beserta bobot lapisnya, tinggal disalurkan. Preview pada
mesh skinned menunggu E8; `Pose::ComputeSkinning` sudah menghasilkan matriks yang
tinggal diunggah.

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
heightmap dan layer terrain. Keduanya sudah ada — `HeightAtWorld` dan
`WeightAt(layer, x, y)` — jadi ketergantungan itu sudah terbayar. Sisanya bisa
dikerjakan paralel setelah E5/E6.

**Track AI (A0..A4)** bercabang dari E2 dan berjalan paralel — lihat
[PLAN-AI.md](PLAN-AI.md). Menjalankannya lebih awal justru menguntungkan: sejak A0,
agen sudah bisa memeriksa editor lewat screenshot dan menjalankan aksi menu, yang
mempercepat pengerjaan E3 ke atas.

## Risiko khusus fase editor

| Risiko | Dampak | Penanganan |
|---|---|---|
| ImGuizmo / imgui-node-editor tidak kompatibel dengan ImGui 1.92 docking | E4/E7.1 tertahan | **Terjadi, dan sudah ditangani.** imgui-node-editor memang tidak bisa dikompilasi terhadap ImGui 1.92: patokannya dipindah ke `master` dan satu tabrakan sisa ditutup patch di `cmake/patches/` (lihat E6.5). Keduanya tetap dibungkus antarmuka sendiri (`Sim::Gizmo`, `Sim::NodeGraph`), jadi kalau pecah lagi yang berubah satu berkas — bukan setiap panel. Gizmo sendiri ≈ 600 baris kalau harus ditulis ulang. |
| Rebuild atlas font saat pindah monitor merilis tekstur yang masih dipakai frame in-flight | Crash saat drag ke monitor lain | Semua rilis tekstur lewat antrian tunda `TextureBridge` (N frame). Diuji dengan ASan di kriteria terima E1. |
| Reflection kurang ekspresif untuk kasus E7 (kurva, gradient, graph) | Inspector jadi penuh kode khusus | Sejak E3, `Attribute` mendukung "custom drawer" per-tipe; kurva/gradient/graph didaftarkan sebagai drawer, bukan pengecualian. |
| Undo untuk operasi besar (brush terrain, sebar vegetasi) memakan memori | Editor kehabisan RAM | Command menyimpan patch/delta, bukan snapshot penuh; `CommandHistory` punya batas memori dan membuang entri terlama. |
| Preview stub terlalu jauh dari hasil akhir sehingga penulis aset salah menilai | Rework di E8 | Preview stub selalu diberi label "Preview (unlit)". Material Editor menampilkan nilai channel apa adanya, bukan mencoba meniru PBR. |
| Mengubah ukuran jendela terasa lambat | Editor terasa berat justru saat pengguna menata ruang kerjanya | **Diselidiki dan diukur.** Biayanya hampir seluruhnya `vkCreateSwapchainKHR`: 25–50 ms per pemanggilan di NVIDIA 580/X11, di Debug maupun Release, dan X11 mengirim satu perubahan ukuran per langkah seretan. Yang bisa dikendalikan hanya seberapa sering ia dipanggil, jadi dua permintaan yang tidak mengubah apa pun dibuang: peristiwa milik jendela panel yang mengambang (dulu membangun ulang swapchain jendela utama pada ukuran yang sama persis — 11 dan 27 ms terukur) dan ukuran yang sudah cocok. Tiga pendekatan lain dicoba dan **ditolak karena diukur tidak membantu**: membatasi laju bangun-ulang selama seretan justru membekukan isi jendela (21 dari 27 frame per 500 ms hilang, dibanding 2 dari 26) karena driver ini menjawab OUT_OF_DATE, bukan SUBOPTIMAL; membuang `oldSwapchain` malah lebih lambat (39 ms vs 29 ms); dan MAILBOX menggantikan FIFO tidak berpengaruh. Sisa keterlambatan bingkai jendela terhadap kursor (±27 px, ±35 ms) milik window manager — SDL menonaktifkan `_NET_WM_SYNC_REQUEST` secara bawaan, jadi WM tidak sedang menunggu editor. |
