# Spesifikasi Panel Editor

Tata letak bawaan meniru gambar acuan (O3DE Editor). Huruf dalam kurung merujuk
penanda pada gambar tersebut.

## Bahasa

**Seluruh teks yang terlihat pengguna memakai bahasa Inggris** — label UI, tooltip,
pesan log yang tampil di Console, dan pesan assert. Alasannya bukan preferensi:
istilah engine (viewport, dockspace, prefab, splat map) tidak punya padanan
Indonesia yang mapan, dan mencampur keduanya justru membuat panel lebih sulit
dibaca daripada memakai satu bahasa saja.

Komentar kode dan dokumen di `docs/` tetap berbahasa Indonesia.

## Ikon

Set ikon: **Lucide** (`Resources/Fonts/lucide.ttf`), di-merge ke font teks
sehingga ikon bisa ditulis di tengah string biasa.

Panel tidak pernah memakai konstanta `ICON_LC_*` langsung, melainkan nama
bermakna di `Sim/Editor/Icons.h` (`icons::kMove`, `icons::kLight`,
`icons::kLogWarn`). Dua alasan: mengganti set ikon hanya menyentuh satu berkas,
dan nama bermakna memaksa konsistensi — tanpa itu "hapus" akan digambar dengan
trash di satu panel dan X di panel lain, dan ketidakcocokannya baru terlihat
setelah tersebar ke mana-mana.

Aturan pemakaian: tombol yang hanya berisi ikon **wajib** punya tooltip. Ikon
yang tidak bisa ditebak dan tidak punya keterangan lebih buruk daripada teks
biasa.

## Tata letak bawaan

```
┌──────────────────────────────────────────────────────────────────────────┐
│ (A) Menu Bar: File  Edit  Entity  Tools  View  Window  Help              │
├──────────────────────────────────────────────────────────────────────────┤
│ (A) Toolbar: [mode] [gizmo] [snap] │ ... │ Play Controls  ▶  ⏸  ⏹        │
├──────────────────┬───────────────────────────────────┬───────────────────┤
│ (B) Entity       │ (D) Viewport — Perspective        │ (E) Entity        │
│     Outliner     │                                   │     Inspector     │
│  ┌────────────┐  │  (D1) overlay alat kiri-atas:     │                   │
│  │ 🔍 Search  │  │       translate / rotate / scale  │  Name  [ ...... ] │
│  └────────────┘  │                                   │  Status  ▼        │
│  ▾ Level         │  (D2) overlay kanan-atas:         │  Entity ID  ....  │
│    ▾ Environment │       kubus orientasi, W/P/L      │  ┌─────────────┐  │
│      • Ground    │                                   │  │Add Component│  │
│      • Sky       │                                   │  └─────────────┘  │
│      • Camera    │                                   │  ▾ Transform      │
│      • Sun       │                                   │    Translate XYZ  │
├──────────────────┤                                   │    Rotate    XYZ  │
│ (C) Asset        │                                   │    Scale     ...  │
│     Browser      │                                   │  ▾ Mesh           │
│  tree │ grid     │                                   │    Mesh Asset ... │
│       │ thumb    ├───────────────────────────────────┤                   │
│       │ detail   │ (F) Console                       │                   │
├───────┴──────────┴───────────────────────────────────┴───────────────────┤
│ Status Bar: siap │ pekerjaan latar: 0 │ folder project │ memori           │
└──────────────────────────────────────────────────────────────────────────┘
```

Preset workspace lain (menu **View → Workspace**): *Level* (di atas), *Material*,
*Particle*, *Terrain*, *Animation* — masing-masing memunculkan panel yang relevan.

---

## (A) Menu bar & toolbar

- **File** — New/Open/Save/Save As Level, Open Project, Recent, Import Asset, Exit
- **Edit** — Undo, Redo, Cut/Copy/Paste/Duplicate/Delete, Select All, Preferences
- **Entity** — Create Empty, Create Child, Create dari primitif, Save as Prefab
- **Tools** — Material Editor, Particle Editor, Terrain Editor, Vegetation Editor,
  Animation Editor, Lua Console, Script Editor, Profiler
- **View** — Reset Layout, Workspace, Grid, Gizmo, Show/Hide overlay
- **Window** — daftar panel (dibangkitkan otomatis oleh `PanelManager`)
- **Help** — Dokumentasi, About

Toolbar kiri: mode transform, ruang gizmo (world/local), snapping, pivot.
Toolbar kanan: **Play / Pause / Stop** untuk simulasi in-editor.

## (B) Entity Outliner

| Fitur | Catatan implementasi |
|---|---|
| Tree hierarki | `TreeView` dengan virtualisasi (`ImGuiListClipper`) untuk level besar |
| Search & filter | filter nama, tipe komponen, tag |
| Rename inline | dobel-klik → `InputText`, Enter menghasilkan `RenameCommand` |
| Drag reparent & reorder | drop di atas baris = jadi anak; drop di antara baris = ubah urutan |
| Multi-select | Ctrl (toggle), Shift (rentang) |
| Visibility & lock | dua kolom titik di kanan, seperti pada gambar acuan |
| Menu konteks | Create Child, Duplicate, Delete, Save as Prefab, Focus in Viewport |
| Indikator prefab | ikon berbeda; entri yang di-override diberi tanda |

## (C) Asset Browser

Panel kiri: pohon folder. Panel kanan: grid thumbnail (ukuran bisa diatur slider)
atau daftar. Panel bawah: detail aset terpilih — nama, ukuran file, dimensi, mips,
format, color space, GUID — persis seperti panel detail pada gambar acuan.

Fitur: breadcrumb, pencarian + filter tipe, favorit, drag ke viewport/Inspector,
menu konteks (Show in Explorer, Reimport, Copy GUID, Find References, Delete),
pembuatan aset baru (Material, Material Instance, Particle, Terrain, Lua Script).

## (D) Viewport

- **Kamera**: fly (tahan klik-kanan + WASD, Q/E turun-naik, roda mengubah
  kecepatan, Shift/Ctrl untuk cepat/pelan), orbit (Alt+drag kiri), pan (klik
  tengah), zoom (roda), **F** untuk fokus ke seleksi.

  Dua konvensi arah yang dipegang: menyeret mouse **ke bawah** membuat pandangan
  **menunduk**, dan menyeret **ke kanan** memutar pandangan **ke kanan**.
  Keduanya ditegakkan di rumus `OrbitCamera::Offset()`, bukan dengan membalik
  tanda di pemanggil — kalau dibalik di pemanggil, orbit dan fly akan mudah
  berbeda arah satu sama lain.

  Gerakan terbang menggeser titik fokus, bukan posisi kamera. Efeknya: begitu
  klik kanan dilepas, orbit langsung berputar mengelilingi titik di depan kamera
  alih-alih titik lama yang sudah tertinggal jauh.
- **(D1) Overlay alat** kiri-atas: translate / rotate / scale, ditambah tombol
  ruang gizmo (world/local) dan pivot (center/individual).
- **(D2) Overlay orientasi** kanan-atas: kubus arah yang bisa diklik untuk melihat
  dari sumbu tertentu, plus tombol **W**(wireframe) / **P**(perspective) / **L**(lit).
- **Grid** adaptif terhadap jarak kamera, dengan sumbu berwarna.
- **Picking**: klik untuk memilih, Ctrl+klik untuk menambah, drag pada ruang kosong
  untuk seleksi kotak.
- **Snapping**: grid (nilai bisa diatur), sudut, skala.
- **Statistik** pojok: fps, jumlah entity terlihat, draw call (setelah E8).
- **Terkunci di dockspace.** Viewport tidak bisa dilepas menjadi jendela OS
  tersendiri. Panel yang ditarik keluar dockspace mendapat viewport platform
  sendiri — berikut swapchain, acquire, dan present-nya — dan untuk gambar
  seukuran viewport penuh biayanya jauh di atas panel berisi teks. Ditegakkan
  lewat `Panel::IsDockLocked()`, yang memasang
  `ImGuiDockNodeFlags_NoUndocking` pada node dock panel dan
  `ImGuiWindowFlags_NoMove` pada jendelanya. Viewport tetap bisa ditutup,
  dibuka lagi dari menu Window, dan diubah ukurannya lewat pemisah dockspace.
- **Gizmo** menghasilkan tepat satu entri undo per gerakan (command di-merge).
- Bisa ada lebih dari satu viewport (panel multi-instance), masing-masing punya
  kamera dan mode tampilan sendiri.

## (E) Entity Inspector

Header: nama entity (bisa diedit), status, Entity ID (bisa disalin) — sesuai acuan.
Tombol **Add Component** dengan daftar yang bisa dicari, dikelompokkan per kategori.

Isi digambar otomatis dari `Reflect`:

| Tipe field | Widget |
|---|---|
| `float` / `int` | `DragFloat`/`DragInt`, jadi `SliderFloat` bila ada atribut `Range` |
| `Vec3` | tiga kolom dengan label **X/Y/Z** berwarna merah/hijau/biru seperti acuan |
| `Vec3` (skala) | sama, ditambah baris *Uniform Scale* yang menyetel ketiganya sekaligus |
| `Quat` | ditampilkan sebagai sudut Euler derajat, dikonversi saat baca/tulis |
| `Color` | swatch + color picker |
| `bool` | toggle |
| `enum` | combo dari `Attr::EnumNames` |
| `AssetRef<T>` | field dengan tombol pilih, target drag-drop, tombol X untuk kosongkan |
| `std::vector<T>` | daftar yang bisa dilipat, tambah/hapus/ubah urutan |
| kurva / gradient / graph | *custom drawer* yang didaftarkan tipe tersebut |

Multi-select: field dengan nilai berbeda menampilkan `—`; mengeditnya menetapkan
nilai ke semua entity terpilih dalam satu command.

### Perilaku field angka

Setiap field angka mendukung dua cara sekaligus:

- **Seret** untuk mengubah nilai secara kontinu — cepat, cocok untuk mencari
  nilai sambil melihat hasilnya di viewport.
- **Klik lalu ketik** untuk nilai pasti (0, 90, 1.5). Diaktifkan lewat
  `io.ConfigDragClickToInputText`; ImGui membedakan klik dari seret berdasarkan
  ada-tidaknya gerakan mouse, jadi keduanya tidak bertabrakan. Tanpa ini,
  mengetik nilai hanya bisa lewat Ctrl+Klik — cara yang tidak akan ditemukan
  pengguna yang belum tahu.

Enter mengaktifkan kembali field dan menyorot isinya
(`io.ConfigInputTextEnterKeepActive`), sehingga mengisi X → Tab → Y berjalan
tanpa mengangkat tangan dari keyboard.

Huruf sumbu **X/Y/Z** juga bisa diseret, bukan hanya kotaknya. Ini melebarkan
target seret tanpa memakan ruang — terasa saat panel Inspector disempitkan.
Karena teks biasa bukan item interaktif (`IsItemActive()` padanya selalu false),
hurufnya digambar di atas `InvisibleButton`, bukan lewat `Text()`.

Skala memakai `ImGuiSliderFlags_AlwaysClamp`: nol atau negatif membalik normal
dan membuat matriks tidak bisa dibalik, jadi nilai yang **diketik** pun dibatasi
— berbeda dari field lain yang sengaja menerima apa saja.

## (F) Console

Sink log langsung dari `Core::Log`. Filter level (Trace…Fatal), filter teks,
penggabungan pesan berulang (`×12`), auto-scroll yang bisa dimatikan, salin baris,
bersihkan. Baris yang merujuk aset atau entity bisa diklik untuk menyorotnya.
Sumber log ditandai kategori (`[Asset]`, `[Lua]`, `[RHI]`).

---

## Panel editor khusus (E7)

| Panel | Susunan |
|---|---|
| **Material Editor** | kiri: palet node + daftar parameter · tengah: node graph · kanan: properti node terpilih · pojok: preview mesh |
| **Particle Editor** | kiri: daftar emitter & modul · tengah: preview + timeline di bawahnya · kanan: properti modul (kurva & gradient inline) |
| **Terrain Editor** | kiri: daftar layer material · atas: pemilih alat (sculpt/paint/hole) · kanan: setting brush · viewport utama dipakai untuk melukis |
| **Vegetation Editor** | kiri: daftar layer vegetasi · kanan: aturan penempatan + setting instance · toolbar: paint/erase/scatter |
| **Animation Editor** | kiri: pohon skeleton · tengah-atas: preview · tengah-bawah: dope sheet / curve editor · kanan: properti keyframe/event · tab terpisah: state machine graph |

## Panel pendukung

- **AI Assistant** — percakapan dengan Claude, menampilkan rencana, tiap tool call
  beserta argumennya, dan hasilnya. Mode izin `read-only`/`ask`/`auto`, pratinjau
  perubahan sebelum disetujui, dan tombol *Undo semua yang dilakukan agen*.
  Detail: [PLAN-AI.md](PLAN-AI.md) A4.
- **AI Bridge** — status MCP server: port, token, klien yang tersambung, daftar
  request masuk beserta waktu eksekusinya, tombol start/stop. Detail: PLAN-AI.md A0.
- **History** — daftar command, klik untuk melompat ke titik tertentu. Command yang
  berasal dari agen diberi awalan `AI:` supaya mudah dibedakan.
- **Preferences** — tema, font & skala, pintasan, path project, setting editor.
- **Lua Console** — REPL.
- **Script Editor** — editor teks Lua dengan pewarnaan sintaks.
- **Asset References** — graf pemakaian aset.
- **Profiler** — waktu frame per bagian (dipakai serius mulai E8).
- **Statistics** — jumlah entity, memori, jumlah aset termuat.
