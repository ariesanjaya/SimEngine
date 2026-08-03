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
| ImGuizmo / imgui-node-editor tidak kompatibel dengan ImGui 1.92 docking | E4/E7.1 tertahan | **Terjadi, dan sudah ditangani.** imgui-node-editor memang tidak bisa dikompilasi terhadap ImGui 1.92: patokannya dipindah ke `master` dan satu tabrakan sisa ditutup patch di `cmake/patches/` (lihat E6.5). Keduanya tetap dibungkus antarmuka sendiri (`Sim::Gizmo`, `Sim::NodeGraph`), jadi kalau pecah lagi yang berubah satu berkas — bukan setiap panel. Gizmo sendiri ≈ 600 baris kalau harus ditulis ulang. |
| Rebuild atlas font saat pindah monitor merilis tekstur yang masih dipakai frame in-flight | Crash saat drag ke monitor lain | Semua rilis tekstur lewat antrian tunda `TextureBridge` (N frame). Diuji dengan ASan di kriteria terima E1. |
| Reflection kurang ekspresif untuk kasus E7 (kurva, gradient, graph) | Inspector jadi penuh kode khusus | Sejak E3, `Attribute` mendukung "custom drawer" per-tipe; kurva/gradient/graph didaftarkan sebagai drawer, bukan pengecualian. |
| Undo untuk operasi besar (brush terrain, sebar vegetasi) memakan memori | Editor kehabisan RAM | Command menyimpan patch/delta, bukan snapshot penuh; `CommandHistory` punya batas memori dan membuang entri terlama. |
| Preview stub terlalu jauh dari hasil akhir sehingga penulis aset salah menilai | Rework di E8 | Preview stub selalu diberi label "Preview (unlit)". Material Editor menampilkan nilai channel apa adanya, bukan mencoba meniru PBR. |
| Mengubah ukuran jendela terasa lambat | Editor terasa berat justru saat pengguna menata ruang kerjanya | **Diselidiki dan diukur.** Biayanya hampir seluruhnya `vkCreateSwapchainKHR`: 25–50 ms per pemanggilan di NVIDIA 580/X11, di Debug maupun Release, dan X11 mengirim satu perubahan ukuran per langkah seretan. Yang bisa dikendalikan hanya seberapa sering ia dipanggil, jadi dua permintaan yang tidak mengubah apa pun dibuang: peristiwa milik jendela panel yang mengambang (dulu membangun ulang swapchain jendela utama pada ukuran yang sama persis — 11 dan 27 ms terukur) dan ukuran yang sudah cocok. Tiga pendekatan lain dicoba dan **ditolak karena diukur tidak membantu**: membatasi laju bangun-ulang selama seretan justru membekukan isi jendela (21 dari 27 frame per 500 ms hilang, dibanding 2 dari 26) karena driver ini menjawab OUT_OF_DATE, bukan SUBOPTIMAL; membuang `oldSwapchain` malah lebih lambat (39 ms vs 29 ms); dan MAILBOX menggantikan FIFO tidak berpengaruh. Sisa keterlambatan bingkai jendela terhadap kursor (±27 px, ±35 ms) milik window manager — SDL menonaktifkan `_NET_WM_SYNC_REQUEST` secara bawaan, jadi WM tidak sedang menunggu editor. |
