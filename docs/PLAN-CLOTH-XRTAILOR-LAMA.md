# Plan Kain Interaktif (C0 → C6)

Mengangkat solver kain GPU XRTailor menjadi library dan memakainya sebagai modul
`Sim::Cloth` untuk kain yang bereaksi terhadap gerakan karakter saat runtime —
bukan cache yang di-bake lebih dulu.

Dokumen ini memakai penomoran **C** supaya tidak bertabrakan dengan milestone
editor (E) dan agentic AI (A) di [ROADMAP.md](ROADMAP.md).

---

> ## ⚠️ Rencana ini disusul keputusan — jangan dijalankan seperti tertulis
>
> **Kain interaktif ditetapkan sebagai fitur baseline**, dan baseline mencakup
> RX 5600 XT (RDNA1, AMD) yang tidak menjalankan CUDA. Seluruh premis rencana ini
> bersandar pada solver CUDA, jadi ia tidak bisa memenuhi syarat itu.
>
> Analisa lengkap beserta penggantinya: **[ANALISA-KAIN.md](ANALISA-KAIN.md)**.
> Ringkasnya: anggaran frame yang sebenarnya hanya memuat ~5.600 partikel, bukan
> 55.824 — dan pada ukuran itu XPBD di CPU kembali masuk hitungan.
>
> **Yang masih berlaku dari dokumen ini:** batas ekstraksi XRTailor (diverifikasi
> ulang dan cocok persis), arsitektur `Sim::Cloth` sebagai modul dan bentuk
> API-nya, serta jalur bake Alembic — yang tetap memakai XRTailor apa adanya
> sebagai alat luar.

## Keputusan pokok

**Yang diambil dari XRTailor hanya solvernya.** Aplikasi XRTailor membawa
engine-nya sendiri: jendela GLFW, renderer OpenGL, scene graph, panel ImGui,
loader glTF, dan exporter Alembic. Semuanya dibuang. Yang menarik cuma
`physics/` dan `memory/`.

**Kain tidak menggantikan jalur bake.** XRTailor tetap dipakai apa adanya untuk
membangkitkan cache Alembik dari simulasi Quality mode. Modul `Sim::Cloth`
melayani kasus lain: kain yang harus bereaksi terhadap input, jadi hanya Swift
mode yang diport.

**Bergantung pada E8.4.** Obstacle kain adalah mesh karakter yang sudah di-skin.
Selama GPU skinning E8.4 belum mendarat, C4 ke atas tidak bisa dikerjakan. C0–C3
tidak bergantung padanya dan bisa dimulai sekarang.

---

## Kenapa jalur ini layak

Diukur di RTX 2060 (mesin pengembangan yang sama dengan target), XRTailor v1.9.0
Swift mode, garment 4.716 vertex di atas karakter 9.011 vertex:

| Yang diukur | Hasil |
| --- | --- |
| Partikel | 55.824 |
| Waktu solver | **19,67 ms/frame → 50,84 FPS** |
| GPU time total (termasuk render OpenGL bawaan XRTailor) | 19,75 ms |
| Headless, 389 frame | 50–57 fps |
| Quality mode (universal, 200 iterasi + impact zone) | **4–19 fps** |

Kesimpulannya: **fisikanya sudah real-time, pembungkusnya yang tidak.** Swift
mode masuk anggaran frame; Quality mode tidak dan memang tidak diport.

Angka 19,67 ms itu untuk satu kain besar tanpa apa pun berjalan di sebelahnya.
Di SimEngine ia harus berbagi frame dengan renderer, jadi C6 menetapkan anggaran
waktu dan mekanisme mundurnya.

---

## Batas ekstraksi

Diukur dari pohon sumber XRTailor v1.9.0:

| Bagian | Ukuran | Pemakaian `Global::` | Perlakuan |
| --- | --- | --- | --- |
| `physics/` | 13.712 baris, 95 file | **3** | **Ambil hampir apa adanya** |
| `memory/` | 561 baris, 11 file | **0** | **Ambil apa adanya** |
| `core/Scalar.hpp`, sebagian `utils/` | ~1.100 baris | — | Ambil |
| `pipeline/` | 3.466 baris, 26 file | **267** | **Tulis ulang jadi facade tipis** |
| `runtime/` | 10.190 baris, 79 file | banyak | **Buang** |
| `config/` | 880 baris, 8 file | — | Buang, diganti aset `.simcloth` |

Rasio inilah yang membuat rencana ini masuk akal: **14 ribu baris fisika bisa
diambil hampir utuh, dan yang harus ditulis ulang hanya 3,5 ribu baris lem.**
Solver-nya ternyata sudah hampir bersih — `physics/` menyentuh state global cuma
tiga kali, dan hanya dua file yang mengimpor sesuatu dari `runtime/`
(`DebugDrawingHelper.hpp` untuk gambar debug, dan `sdf/Collider.hpp` yang
menarik `Actor.hpp`). Keduanya dipotong di C1.

### Empat hal yang harus diselesaikan

**1. `exit()` di 33 tempat.** Termasuk yang paling berbahaya:
`MemoryPool.cu:24` mematikan proses saat node pool habis. Sebuah library tidak
boleh membunuh host-nya. Semua diganti kode galat yang merambat ke pemanggil.

**2. State global.** `Global::engine`, `Global::sim_params`, `Global::sim_config`
adalah variabel `inline` di `Global.hpp` — satu simulasi per proses. Diganti
struct context yang dioper eksplisit. Isinya sudah terpetakan: 30-an medan yang
dipakai `pipeline/`, mayoritas parameter solver (`num_iterations`,
`num_substeps`, `delta_time`, `max_speed`, `bvh_tolerance`,
`num_collision_passes`, `enable_self_collision`, `long_range_stretchiness`, …)
plus penghitung ukuran buffer.

**3. Interop grafis.** `PhysicsMesh.cu` memanggil `cudaGraphicsGLRegisterBuffer`
supaya solver menulis langsung ke VBO OpenGL. SimEngine memakai Vulkan, jadi ini
harus diganti. Kabar baiknya permukaannya sempit: `RegisterBuffer(GLuint)`,
`RegisterNewBuffer(GLuint)`, satu `cudaGraphicsResource*`, dan sepasang
map/unmap — lima titik panggilan. Lihat C3 untuk pilihan penggantinya.

**4. Sumber pose obstacle.** `ClothSolver.cpp:201` memanggil
`gltf_loader_->UpdateAnimation(frame_index, …)` — pose diambil dari keyframe
berkas GLB memakai penghitung frame internal. Diganti masukan dari luar: array
posisi vertex obstacle yang sudah di-skin, dikirim SimEngine tiap frame.

---

## Arsitektur target

```
Code/Cloth/
  include/Sim/Cloth/            ← C++20 murni, tanpa CUDA, tanpa thrust
      ClothSolver.h                 facade pImpl
      ClothContext.h                parameter solver (pengganti Global::sim_params)
      ClothAsset.h                  data .simcloth hasil parse
      ClothComponent.h              komponen EnTT
  src/                          ← C++20, boleh lihat Vulkan lewat Sim::RHI
      ClothSolver.cpp               jembatan ke solver_impl
      ClothSystem.cpp               iterasi EnTT, anggaran waktu, LOD
      ClothIo.cpp                   baca/tulis .simcloth
      VulkanCudaBridge.cpp          berbagi buffer dengan RHI
  solver/                       ← dikompilasi nvcc, TIDAK dilihat clang
      xrtailor/physics/**           vendored, patch minimal
      xrtailor/memory/**            vendored
      SolverImpl.cu                 pengganti pipeline/ XRTailor
```

Aturan yang menegakkan pemisahan: **tidak satu pun header di
`include/Sim/Cloth/` boleh menyebut `cuda`, `thrust`, atau `__device__`.**
Ditegakkan uji di C1, bukan disiplin — pola yang sama dengan aturan
`Code/Editor` tidak boleh `#include <vulkan/vulkan.h>`.

Alur data per frame:

```
Animation (E8.4)  →  skinning buffer (VkBuffer, posisi obstacle)
                            │
                            ▼
ClothSystem.Update(dt)  →  Solver::SetObstaclePositions()
                        →  Solver::Step(dt)          [CUDA]
                        →  posisi kain di VkBuffer
                            │
                            ▼
Render               →  draw seperti mesh biasa, tanpa readback
```

---

## Toolchain: CUDA di dalam build clang

SimEngine dibangun clang 18 dengan `LANGUAGES C CXX`; belum ada CUDA sama
sekali. Ada dua cara dan yang kedua yang dipilih.

**Ditolak — `enable_language(CUDA)` di build utama.** Menyeret pertanyaan
kecocokan nvcc dengan clang sebagai host compiler ke seluruh proyek, dan membuat
`SIM_WITH_CLOTH=OFF` tidak lagi benar-benar melepas CUDA dari konfigurasi.

**Dipilih — target CUDA terpisah, nvcc dengan host g++ 13.3, ditautkan statis.**
`Code/Cloth/solver/` dibangun sebagai static library sendiri lewat
`ExternalProject` atau subdirektori dengan `enable_language(CUDA)` terpagar,
memakai host compiler g++ yang sudah ada. Hasilnya `.a` yang ditautkan ke
`SimCloth`. Karena keduanya memakai libstdc++ yang sama, ABI-nya cocok. Clang
tidak pernah melihat satu baris pun CUDA, sehingga:

- `-Wconversion` dan kawan-kawan di `SIM_STRICT_WARNINGS` tidak meledak di kode
  vendored,
- preset **asan/tsan tetap bisa dibangun** dengan `SIM_WITH_CLOTH=OFF`, dan itu
  memang cara memakainya — sanitizer clang tidak mengerti kode device,
- opsi `SIM_WITH_CLOTH` default **OFF**, sehingga kontributor tanpa CUDA Toolkit
  tetap bisa membangun editor.

Arsitektur GPU dipatok `-DSIM_CLOTH_CUDA_ARCH=75` (RTX 2060), bisa ditimpa.

### Perbaikan XRTailor yang wajib ikut

Sumber v1.9.0 **tidak bisa dikompilasi apa adanya** dengan CUDA 12.8/GCC 13.
Tiga perbaikan ini sudah diverifikasi di `~/SDK/xrtailor-1.9.0` dan harus ikut
saat vendoring:

1. **`BVH.cu:644`** — libcu++ 12.x menolak extended `__device__` lambda sebagai
   operator `thrust::reduce` tanpa trailing return type. Tambahkan `-> Bounds`.
2. **Namespace ABI Thrust** — Thrust menyisipkan `__CUDA_ARCH_LIST__` ke inline
   namespace; makro itu hanya ada di translation unit nvcc, jadi berkas host
   yang menyebut `thrust::host_vector` di tanda tangan fungsi mengait ke
   namespace berbeda dan link gagal. Definisikan `THRUST_DISABLE_ABI_NAMESPACE`,
   `THRUST_IGNORE_ABI_NAMESPACE_ERROR`, `CUB_DISABLE_NAMESPACE_MAGIC`,
   `CUB_IGNORE_NAMESPACE_MAGIC_ERROR` untuk semua bahasa di target solver.
   *Di SimEngine masalah ini hilang dengan sendirinya kalau aturan "tidak ada
   thrust di header publik" ditegakkan — tapi tetap berlaku di dalam
   `solver/`.*
3. **jsoncpp** tidak diperlukan sama sekali di sini; parsing config diganti
   `.simcloth`. Ini menghapus satu dependensi beserta jebakan C++17-nya.

Catatan bug yang sudah ditemukan dan harus tidak ikut terbawa: XRTailor crash
(`cudaErrorIllegalAddress` di `PhysicsMesh.cu:679`) saat scene dibangun ulang
lewat tombol Reset. Jalur rebuild itu tidak diport, tapi kalau nanti
`Solver::Reset()` dibuat, akar masalahnya harus diselidiki dulu — bukan disalin.

---

## Milestone

### C0 — Vendoring & build · ⬜

Salin `physics/`, `memory/`, `core/Scalar.hpp`, dan `utils/` yang dibutuhkan ke
`Code/Cloth/solver/xrtailor/`. Terapkan tiga perbaikan build di atas. Buat
target CUDA statis + opsi `SIM_WITH_CLOTH` (default OFF).

Vendoring **dicatat asalnya**: berkas `VENDOR.md` berisi versi XRTailor, commit,
dan daftar patch lokal, mengikuti pola `docs/DEPENDENCIES.md`. Tidak lewat
FetchContent — kode ini dipatch, dan patch yang hidup di build tree hilang saat
cache dibersihkan.

**Kriteria terima**
- `cmake --preset linux-clang-release` tanpa opsi → sukses, tidak memanggil nvcc.
- Ditambah `-DSIM_WITH_CLOTH=ON` → sukses, `libSimClothSolver.a` terbentuk.
- Preset `linux-clang-asan` tetap sukses (dengan cloth OFF).

### C1 — Facade tanpa CUDA & tanpa global · ⬜

`Sim::Cloth::Solver` dengan pImpl. `ClothContext` menggantikan
`Global::sim_params`. Semua `exit()` di kode yang diport diganti
`std::expected`-style error atau kode galat. Potong `DebugDrawingHelper.hpp` dan
ketergantungan `Actor.hpp` di `sdf/Collider.hpp`.

**Kriteria terima**
- Uji doctest membuat **dua** `Solver` dalam satu proses, masing-masing
  di-`Step()` 100 kali, keduanya memberi hasil berbeda sesuai parameternya.
- Uji yang mem-`grep` header publik: nol kemunculan `cuda`, `thrust`,
  `__device__`, `Global::`.
- Pool memory yang habis mengembalikan galat, bukan mematikan proses — diuji
  dengan pool sengaja dikecilkan.

### C2 — Simulasi headless yang benar · ⬜

Adegan uji: satu kain persegi jatuh ke bola statis, tanpa render sama sekali.
Bandingkan dengan hasil XRTailor asli pada parameter yang sama.

**Kriteria terima**
- 300 frame tanpa NaN, tanpa energi meledak, kain diam di akhir.
- Dua run dengan seed sama menghasilkan posisi identik bit-per-bit.
- Waktu per frame tercatat ke log; untuk kain 5k vertex harus < 10 ms.

### C3 — Kain tampil di viewport · ⬜

Menyambungkan keluaran solver ke Vulkan. **Mulai dari yang sederhana:**

*Tahap A — staging copy.* `cudaMemcpy` device→host, unggah ke `DynamicBuffer`.
Terdengar boros, tapi hitung dulu: 4.716 vertex × 12 byte = **55 KB posisi per
frame**, plus normal jadi 110 KB. Di 60 fps itu 6,6 MB/s — tidak terukur di PCIe
mana pun. Biaya sebenarnya adalah titik sinkronisasi, dan XRTailor sudah
memanggil `cudaDeviceSynchronize()` tiap frame.

*Tahap B — zero-copy, hanya kalau profiling menuntutnya.* `VK_KHR_external_memory_fd`
di sisi Vulkan, `cudaImportExternalMemory` + `cudaExternalMemoryGetMappedBuffer`
di sisi CUDA, disinkronkan `VK_KHR_external_semaphore_fd` /
`cudaImportExternalSemaphore`. Perlu menambah ekstensi device di
`Code/RHI/src/Device.cpp` — saat ini hanya swapchain, debug utils, GPPD2, dan
ray query opsional yang diaktifkan.

Mendahulukan tahap A menjaga C3 tidak berubah jadi proyek interop berminggu-minggu
sebelum ada satu piksel kain pun di layar.

**Kriteria terima**
- Kain bergerak terlihat di viewport editor.
- **Nol** galat validation layer.
- Waktu pass `cloth-upload` tercatat di GpuProfiler.

### C4 — Obstacle dari modul Animation · ⬜ (butuh E8.4)

Ganti `UpdateAnimation(frame_index)` dengan `SetObstaclePositions(span)` yang
diisi dari skinning buffer E8.4. BVH obstacle di-refit tiap frame.

**Kriteria terima**
- Kain mengikuti karakter yang animasinya dikendalikan Animation Graph.
- Mengubah kecepatan playback tidak merusak simulasi.
- Karakter di-teleport tidak membuat kain meledak — kain di-reset lembut, bukan
  ditarik menembus badan.

### C5 — Aset `.simcloth`, komponen, panel · ⬜

Format aset baru mengikuti pola `.simmat`/`.simfx`/`.simveg`: GUID stabil,
referensi ke mesh garment, `MASS`, `ATTACHED_INDICES`, dan blok `BINDING`
(`BOUNDARY`/`NEIGHBOR`/`UV_ISLAND`/`NONMANIFOLD_EDGES`, masing-masing dengan
`INDICES`/`STIFFNESS`/`DISTANCE` yang sejajar per indeks) — struktur ini disalin
dari config garment XRTailor karena sudah terbukti memadai. Plus parameter
solver Swift mode.

`ClothComponent` di EnTT menunjuk aset + entity karakter sebagai obstacle.
Panel editor mengikuti konvensi E7, lengkap dengan command/undo.

**Kriteria terima**
- Pasang kain ke karakter lewat editor, simpan `.simlevel`, muat ulang, kain
  tetap terpasang.
- Rename berkas aset tidak memutus referensi (GUID, bukan path).
- Uji doctest untuk serialisasi `.simcloth` — round-trip identik.

### C6 — Anggaran waktu & degradasi · ⬜

Kain adalah satu-satunya sistem yang bisa menghabiskan seluruh anggaran frame
sendirian. Mekanisme yang wajib ada:

- Anggaran milidetik per frame; kain yang melewatinya diturunkan iterasinya.
- Kain di luar frustum atau jauh dari kamera ditidurkan, bangun bertahap dengan
  warmup singkat — XRTailor memakai 50–120 frame pre-simulation untuk settle,
  dan itu tidak bisa dibayar saat spawn di tengah gameplay.
- Batas keras jumlah kain aktif; sisanya jatuh ke animasi bake.

**Kriteria terima**
- 10 karakter berkain di satu adegan tetap di atas 60 fps, dengan penurunan
  kualitas yang terlihat wajar.
- Statistik kain (jumlah aktif, ms terpakai) tampil di panel profiler.

---

## Risiko dan titik mundur

| Risiko | Tanda awal | Mundur ke |
| --- | --- | --- |
| Interop Vulkan-CUDA berlarut | C3 tahap B lewat dua minggu | Tetap di tahap A; 110 KB/frame tidak akan jadi hambatan |
| nvcc + libstdc++ bentrok dengan clang | Link error simbol C++ di C0 | Bungkus batas library dengan `extern "C"` |
| Solver tidak stabil dengan `dt` variabel | Kain meledak saat frame drop | Fixed timestep dengan akumulator, substep dibatasi |
| Kain 5k vertex terlalu berat untuk anggaran | C2 mencatat > 10 ms | Turunkan resolusi garment; solver ini skalanya linear terhadap partikel |
| E8.4 mundur jauh | — | C0–C3 tetap jalan dengan obstacle statis/analitik |

---

## Yang tidak boleh ditunda

Mengikuti prinsip yang sama di [ROADMAP.md](ROADMAP.md), tiga hal ini mahal
diubah belakangan:

- **Header publik bebas CUDA.** Kalau `thrust` bocor ke `include/Sim/Cloth/`
  sekali saja, seluruh engine ikut butuh nvcc dan `SIM_WITH_CLOTH=OFF` berhenti
  berarti. Ditegakkan uji sejak C1.
- **Context, bukan global.** Menambahkan context ke kode yang sudah menulis ke
  `Global::` berarti menyentuh 267 tempat dua kali.
- **Catatan vendoring.** Tanpa `VENDOR.md` yang mencatat patch, upgrade XRTailor
  berikutnya adalah pekerjaan arkeologi.

---

## Lampiran: kenapa bukan jalur bake saja

Jalur bake (`XRTailor → .abc → putar di engine`) tetap yang paling murah dan
tetap dipertahankan untuk cutscene serta NPC beranimasi tetap. Plan ini
dikerjakan hanya kalau kain harus **bereaksi** — terhadap tabrakan pemain,
angin yang dikendalikan gameplay, atau pose yang tidak diketahui saat bake.
Kalau kebutuhannya bukan itu, C0–C6 tidak perlu dikerjakan sama sekali.
