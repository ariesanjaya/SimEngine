# Track AI — Engine sebagai MCP Server (A0 → A4)

**Tujuan track ini.** Membuat SimEngine bisa dikendalikan oleh agentic AI (Claude Code,
Claude Desktop, atau agen lain yang berbicara MCP) sehingga tugas-tugas pembuatan
konten dan bahkan pengembangan engine itu sendiri bisa dikerjakan oleh agen:
"buatkan level hutan dengan terrain berbukit dan 3 jenis pohon", "bikin material
air yang beriak", "perbaiki animasi berjalan yang kakinya menembus tanah".

Track ini berjalan **paralel** dengan E0..E7, bukan setelahnya. Alasannya ada di
bagian [Kenapa ini murah](#kenapa-ini-murah) — sebagian besar pekerjaannya sudah
dikerjakan oleh keputusan arsitektur di fase editor.

---

## Dua arah integrasi

Keduanya dibutuhkan, dan keduanya memakai modul yang sama (`Code/AIBridge`).

### Arah 1 — Engine sebagai **MCP server** (prioritas utama)

Editor menyalakan endpoint MCP di `127.0.0.1:<port>`. Claude Code menyambung ke
sana dan mendapat puluhan tool untuk membaca & memodifikasi project.

```
┌───────────────┐   MCP over HTTP    ┌──────────────────────────────┐
│  Claude Code  │◄──────────────────►│  SimEditor (atau SimHeadless)│
│  / agen lain  │   127.0.0.1:7777   │  ├─ MCP server (AIBridge)    │
└───────────────┘                    │  ├─ CommandHistory (undo)    │
                                     │  ├─ World / AssetDatabase    │
                                     │  └─ Viewport (screenshot)    │
                                     └──────────────────────────────┘
```

Pendaftaran dari sisi Claude Code:

```sh
claude mcp add simengine --transport http http://127.0.0.1:7777/mcp
```

### Arah 2 — Editor sebagai **MCP client** (panel AI Assistant)

Panel di dalam editor yang bicara ke Claude API, membawa tool yang sama, dan
menampilkan rencana + tool call + hasilnya. Ini untuk pengguna yang tidak ingin
keluar dari editor. Panel ini juga bisa menyambung ke MCP server lain (mis. server
dokumentasi, atau server aset).

---

## Kenapa ini murah

Empat keputusan yang sudah diambil di [ARCHITECTURE.md](ARCHITECTURE.md) membuat
track ini sebagian besar tinggal "menempel":

| Keputusan arsitektur | Yang jadi gratis untuk AI |
|---|---|
| **Semua mutasi lewat `ICommand`** | Setiap tindakan agen otomatis bisa di-undo. Agen yang salah tidak merusak project — cukup undo. Ini juga yang membuat checkpoint/rollback bisa diimplementasi dalam beberapa baris. |
| **Reflection sebagai sumber tunggal** | Skema JSON untuk tool `component.set` **dibangkitkan** dari `TypeRegistry`. Menambah komponen baru langsung bisa dipakai agen tanpa menulis tool baru. Ini konsumen keempat dari reflection, setelah Inspector, serialisasi, dan Lua. |
| **Semua aset berformat JSON + GUID** | Agen bisa membaca dan menalar isi level/material/partikel sebagai teks, bukan blob biner. Diff-nya juga terbaca manusia saat review. |
| **`IViewportRenderer` mengembalikan tekstur** | Tool `viewport.capture` tinggal membaca render target yang sudah ada dan mengembalikannya sebagai gambar PNG. Agen jadi bisa *melihat* hasil kerjanya, bukan hanya menebak. |

Yang benar-benar baru dan harus ditulis: transport HTTP + JSON-RPC, marshaling ke
main thread, lapisan izin, dan definisi tool.

---

## A0 — Transport & protokol · ~3 sesi · butuh E2

**Tujuan.** Claude Code bisa `claude mcp add`, melihat daftar tool, dan memanggil
tool sederhana yang benar-benar mengubah sesuatu di editor.

**Pekerjaan**

- `Code/AIBridge`: server HTTP (cpp-httplib) di localhost, **streamable HTTP
  transport** MCP + fallback SSE. Bind hanya ke `127.0.0.1`, port dari config,
  token bearer opsional yang dibangkitkan saat start dan ditampilkan di panel.
- Lapisan JSON-RPC 2.0: `initialize`, `tools/list`, `tools/call`, `resources/list`,
  `resources/read`, `prompts/list`, notifikasi `notifications/tools/list_changed`.
- **Marshaling ke main thread.** Ini bagian paling penting dan paling mudah salah.
  Request datang di thread jaringan; ia tidak boleh menyentuh `World`, ImGui, atau
  Vulkan. Alurnya: request → `MainThreadQueue` → dieksekusi di titik tetap dalam
  frame (setelah `DrainAsyncQueues`, sebelum panel menggambar) → hasil dikirim
  balik lewat future → thread jaringan membalas. Ada timeout supaya agen tidak
  menggantung kalau editor sedang modal.
- `ToolRegistry`: pendaftaran tool (nama, deskripsi, JSON Schema input, handler,
  tingkat izin), dan pembangkitan `tools/list`.
- Tool awal: `editor.status`, `editor.log_tail`, `editor.undo`, `editor.redo`,
  `editor.screenshot` (jendela penuh), `editor.execute_action` (aksi bernama dari
  `ActionRegistry` — langsung memberi agen akses ke semua menu).
- **Panel AI Bridge**: status server, port, daftar request masuk, tombol
  start/stop, dan mode izin.

**Kriteria terima**

1. `claude mcp add simengine --transport http http://127.0.0.1:7777/mcp` berhasil,
   dan `/mcp` di Claude Code menampilkan daftar tool SimEngine.
2. Agen memanggil `editor.screenshot` → menerima gambar jendela editor.
3. Agen memanggil `editor.execute_action("view.reset_layout")` → layout di editor
   benar-benar berubah, dan tidak ada race (diverifikasi dengan TSan selama 200
   panggilan beruntun).
4. Menutup editor saat ada request menggantung tidak menyebabkan crash.
5. Port yang sudah dipakai → editor memilih port berikutnya dan menuliskannya ke
   `~/.simengine/mcp.json` supaya agen bisa menemukannya.

---

## A1 — Tool scene & entity · ~4 sesi · butuh E3, E4

**Tujuan.** Agen bisa membaca dan menyusun level.

**Tool**

| Tool | Isi |
|---|---|
| `scene.describe` | ringkasan level: jumlah entity, hierarki (dibatasi kedalaman), bounding box dunia |
| `scene.query` | cari entity berdasarkan nama/tag/komponen/jarak; mengembalikan GUID + ringkasan |
| `entity.get` | seluruh komponen sebuah entity sebagai JSON (lewat reflection) |
| `entity.create` | buat entity, opsional dari prefab/primitif, dengan parent & transform |
| `entity.modify` | set field komponen (path seperti `Transform.position.x`), tambah/hapus komponen |
| `entity.delete` / `entity.reparent` / `entity.duplicate` | operasi hierarki |
| `selection.set` / `selection.get` | supaya agen dan manusia melihat objek yang sama |
| `viewport.capture` | render dari kamera editor **atau** dari sudut yang diminta agen (`from`, `look_at`, `mode`) — agen bisa memeriksa hasilnya dari beberapa sudut |
| `level.open` / `level.save` / `level.new` | |
| `history.checkpoint` / `history.rollback` | tandai titik aman, kembalikan seluruh perubahan sejak titik itu |

**Detail penting**

- **Skema dibangkitkan dari reflection.** `entity.modify` menerima
  `{"entity": "<guid>", "component": "Transform", "values": {...}}`, dan JSON Schema
  untuk `values` dihasilkan dari `TypeDesc`. Komponen baru langsung terjangkau.
- **Satu tool call = satu entri undo** (atau satu transaksi kalau tool melakukan
  banyak perubahan), diberi nama `AI: <nama tool>` supaya terlihat di panel History.
- **Batch.** `entity.create_many` menerima array — agen menempatkan 200 pohon
  dalam satu panggilan, bukan 200 panggilan.

**Kriteria terima**

1. Agen diminta "buat 3 kubus berjajar dengan jarak 2 meter dan sebuah lampu di
   atasnya" → hasilnya benar, terlihat di Outliner, dan **satu** kali `editor.undo`
   dari sisi manusia mengembalikan tiap langkah agen secara berurutan.
2. `viewport.capture` dari sudut yang diminta menghasilkan gambar yang sesuai.
3. `history.rollback` ke checkpoint mengembalikan level persis (dibandingkan
   byte-per-byte dengan simpanan sebelum checkpoint).
4. Tool call saat editor sedang membuka dialog modal ditolak dengan pesan jelas,
   bukan menggantung.

---

## A2 — Tool aset & project · ~3 sesi · butuh E5

| Tool | Isi |
|---|---|
| `asset.search` | cari aset berdasarkan nama/tipe/folder/tag; mengembalikan GUID + path |
| `asset.info` | metadata + siapa yang memakainya (dari graf ketergantungan E5) |
| `asset.import` | impor file dari disk ke folder aset, dengan setting import |
| `asset.create` | buat aset baru (material, material instance, particle, terrain, script) |
| `asset.thumbnail` | thumbnail sebagai gambar — agen bisa melihat tekstur/mesh sebelum memakainya |
| `project.info` | struktur project, level, setting |
| `file.read` / `file.write` | terbatas pada folder project, untuk script Lua dan file teks |

**Kriteria terima**

1. Agen menemukan tekstur yang sesuai deskripsi ("cari tekstur batu"), melihat
   thumbnail-nya, lalu memakainya di material — tanpa manusia menyebut nama file.
2. `file.write` di luar folder project ditolak (uji path traversal `../../etc/passwd`).
3. Aset yang dibuat agen muncul di Asset Browser tanpa restart.

---

## A3 — Tool authoring & Lua · ~4 sesi · butuh E6, E7

**Tujuan.** Agen bisa mengarang material, partikel, terrain, vegetasi, dan animasi —
bukan hanya menata entity.

| Tool | Isi |
|---|---|
| `lua.eval` | jalankan potongan Lua di konteks editor; jalan keluar umum untuk hal yang belum punya tool khusus |
| `lua.script_write` | tulis/ubah file script + reload |
| `material.graph_get` / `material.graph_set` | baca/tulis graph sebagai JSON; validasi dijalankan sebelum diterima |
| `material.preview` | render preview material sebagai gambar |
| `particle.get` / `particle.set` / `particle.preview` | definisi emitter + gambar preview pada waktu tertentu |
| `terrain.sculpt` | terapkan operasi brush terprogram (raise/flatten/noise) pada region |
| `terrain.heightmap_get` / `terrain.heightmap_set` | baca/tulis heightmap region sebagai array atau PNG |
| `vegetation.layer_set` / `vegetation.scatter` | atur aturan sebaran lalu jalankan |
| `animation.clip_info` / `animation.key_set` / `animation.preview` | |

**Detail penting**

- `lua.eval` adalah tool paling kuat sekaligus paling berisiko. Ia berada di tingkat
  izin tertinggi, dan di mode `ask` selalu meminta persetujuan dengan menampilkan
  kodenya.
- Semua tool authoring tetap lewat Command → tetap bisa di-undo.
- Tool preview mengembalikan gambar. Ini yang membuat agen bisa melakukan iterasi
  sendiri: ubah parameter → lihat → nilai → ubah lagi.

**Kriteria terima**

1. Perintah "buat material emas kasar" menghasilkan graph yang valid, dan preview
   yang dikembalikan agen benar-benar memperlihatkan permukaan logam.
2. Perintah "buat efek asap yang naik pelan lalu memudar" menghasilkan `.simfx`
   yang bisa dibuka manusia di Particle Editor dan modulnya masuk akal.
3. `lua.eval` yang error tidak mematikan editor; pesan error + traceback kembali
   ke agen.
4. Seluruh hasil kerja agen di sesi ini bisa di-undo sampai kosong.

---

## A4 — Panel AI Assistant & mode headless · ~4 sesi

**Panel AI Assistant** (MCP client, di dalam editor)

- Percakapan dengan Claude API (`claude-opus-5` / `claude-sonnet-5`), streaming.
- Menampilkan rencana agen, tiap tool call beserta argumennya, dan hasilnya.
- **Mode izin**: `read-only` (agen hanya boleh tool yang membaca), `ask` (setiap
  tool yang mengubah data minta persetujuan, dengan pratinjau perubahan), `auto`
  (jalan sendiri, tapi selalu membuat checkpoint sebelum mulai).
- Tombol **Undo semua yang dilakukan agen** — memanfaatkan checkpoint A1.
- Konteks otomatis yang dikirim ke model: level yang terbuka, seleksi saat ini,
  screenshot viewport, dan log error terakhir.
- Bisa menyambung ke MCP server lain lewat konfigurasi.

**Mode headless** (`SimHeadless`)

Editor tanpa GUI yang tetap menyalakan MCP server dan bisa merender offscreen.
Gunanya: agen menjalankan tugas batch (konversi aset, validasi seluruh level,
regresi visual) di CI tanpa perlu display.

```sh
SimHeadless --project /path/Project --mcp-port 7777 --headless
```

**Kriteria terima**

1. Dari panel, perintah "tambahkan pagar di sekeliling area spawn" berjalan sampai
   selesai dengan mode `ask`, dan tiap persetujuan menampilkan pratinjau yang benar.
2. Mode `read-only` benar-benar menolak semua tool yang mengubah data.
3. "Undo semua" mengembalikan project ke keadaan sebelum sesi agen.
4. `SimHeadless` jalan di mesin tanpa display (`XDG_SESSION_TYPE` kosong) dan
   `viewport.capture` tetap menghasilkan gambar.

---

## Model izin

Tiga tingkat, ditetapkan per tool saat registrasi, dan bisa dipersempit lewat config:

| Tingkat | Contoh tool | Perilaku bawaan |
|---|---|---|
| `read` | `scene.query`, `asset.info`, `viewport.capture` | selalu diizinkan |
| `write` | `entity.modify`, `asset.create`, `terrain.sculpt` | diizinkan, tercatat, bisa di-undo |
| `dangerous` | `lua.eval`, `file.write`, `asset.delete`, `project.*` | minta persetujuan, bahkan di mode `auto` |

Selain itu, keras dan tidak bisa dimatikan:

- Server hanya bind ke `127.0.0.1`. Tidak ada opsi bind ke `0.0.0.0`.
- `file.read`/`file.write` dikurung dalam folder project; path dinormalisasi dan
  symlink yang keluar folder ditolak.
- Tidak ada tool yang menjalankan proses eksternal.
- Setiap tool call tercatat ke `Logs/mcp-<tanggal>.jsonl` lengkap dengan argumen —
  supaya bisa diaudit setelah agen melakukan sesuatu yang tidak diharapkan.

---

## Resources & prompts MCP

Selain tool, server juga menyediakan:

**Resources** (dibaca agen sebagai konteks, tanpa tool call):
- `simengine://project/info` — struktur project
- `simengine://level/current` — level aktif sebagai JSON
- `simengine://docs/components` — daftar semua komponen + field-nya, dibangkitkan
  dari reflection. Ini yang membuat agen tahu apa yang bisa ia atur tanpa menebak.
- `simengine://logs/recent` — log terakhir

**Prompts** (template siap pakai di Claude Code):
- `sim:build-level` — alur menyusun level dari deskripsi
- `sim:make-material` — alur mengarang material
- `sim:diagnose` — kumpulkan log + screenshot + info scene untuk mendiagnosis masalah

---

## Urutan & ketergantungan

```
E2 ─► A0 ──► A1 ──► A2 ──► A3 ──► A4
      ▲      ▲      ▲      ▲
      │      │      │      │
     E2     E3,E4   E5    E6,E7
```

A0 bisa dimulai segera setelah E2 selesai — bahkan sebelum ada scene. Manfaatnya
langsung terasa: sejak A0, agen sudah bisa memeriksa editor lewat screenshot dan
menjalankan aksi menu, yang mempercepat pengembangan E3 ke atas.

## Risiko

| Risiko | Penanganan |
|---|---|
| Request MCP menyentuh state dari thread jaringan | Semua handler wajib lewat `MainThreadQueue`. Ditegakkan lewat assert: handler yang mengakses `World` memeriksa thread id di Debug. |
| Agen membuat ribuan tool call kecil dan editor jadi lambat | Tool batch (`entity.create_many`), rate limit per detik, dan respons yang membawa cukup konteks supaya agen tidak perlu bertanya ulang. |
| Respons terlalu besar (level 5.000 entity sebagai JSON) | Semua tool yang mengembalikan koleksi punya paginasi + `max_depth` + ringkasan default. `scene.describe` mengembalikan ringkasan, bukan seluruh isi. |
| Agen merusak project tanpa disadari | Checkpoint otomatis sebelum sesi, log audit, mode `ask` sebagai bawaan, "undo semua". |
| Skema tool dari reflection terlalu besar untuk `tools/list` | Skema komponen tidak dimasukkan ke `tools/list`; agen mengambilnya lewat resource `simengine://docs/components` saat butuh. |
