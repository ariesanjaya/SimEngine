# Plan Integrasi Embree (R0 → R5)

Memakai [Intel Embree](https://github.com/RenderKit/embree) 4.4.1 sebagai mesin
ray tracing CPU untuk empat kebutuhan yang semuanya berada **di luar hot path
frame**: picking presisi, query authoring, path tracer referensi, dan bake SDF
per-mesh.

Penomoran **R** (ray) supaya tidak bertabrakan dengan E (editor), A (agentic AI),
dan C (kain) di [ROADMAP.md](ROADMAP.md).

---

## Keputusan pokok

**Embree bukan renderer, dan tidak dipakai untuk rendering runtime.** Ia hanya
menjawab "sinar ini kena apa, di mana". Tidak ada shading, material, atau cahaya
di dalamnya. Untuk ray tracing real-time di GPU, jalurnya adalah
`VK_KHR_ray_query` — yang sudah diprobe `Code/RHI/src/Device.cpp` dan didukung RT
core Turing di RTX 2060. Kedua jalur itu tidak bersaing.

**Dukungan GPU Embree tidak relevan untuk kita.** Embree 4 punya backend SYCL,
tapi hanya untuk Intel Xe HPG/HPC. Di mesin NVIDIA, Embree adalah library CPU
murni. Itu bukan kekurangan untuk keempat kebutuhan di sini — semuanya memang
lebih cocok di CPU.

**Yang menentukan real-time bukan Embree, tapi jumlah ray.** Picking satu ray per
klik, conform beberapa ribu ray per operasi brush, dan bake beberapa juta ray
sekali seumur aset. Tidak satu pun dari itu berkompetisi dengan anggaran frame.
Yang tidak dilakukan: path tracing penuh per frame — 1080p 1spp dengan 2 bounce
butuh ~745 juta ray/detik untuk 60 fps, dua orde magnitudo di atas CPU mana pun.

---

## Empat kebutuhan, dan apa sebenarnya yang kurang

### 1. Picking presisi segitiga

`SceneView::Raycast` (`Code/EditorFramework/src/SceneView.cpp:375`) saat ini
menguji **AABB saja**, dengan pemindaian linear atas seluruh `pickables_`. Sinar
memang dibawa ke ruang lokal supaya objek yang diputar tetap diuji pada
kotaknya sendiri — itu sudah benar — tapi kotak tetaplah kotak. Klik di tengah
lubang sebuah donat memilih donat itu; klik di celah antara dua tangga memilih
salah satunya.

Embree memberi dua hal sekaligus: uji per segitiga, dan BVH dua tingkat yang
mengubah pemindaian linear menjadi logaritmik.

### 2. Query authoring

Conform objek ke permukaan mesh, proyeksi decal, snapping ke geometri, dan
penempatan vegetasi **di atas mesh arbitrer**.

Batasnya harus jelas: penempatan vegetasi di atas terrain **tetap memakai
heightmap**. `VegetationBrush.cpp:134` memanggil `terrain.HeightAtWorld()`, dan
itu sudah eksak sekaligus lebih murah daripada ray cast mana pun. Embree hanya
menambah kemampuan yang sekarang tidak ada sama sekali: menaruh sesuatu di atas
batu, atap, atau jembatan.

### 3. Path tracer referensi — yang paling bernilai

Ini kebutuhan yang paling halus dan paling menentukan, jadi perlu diuraikan.

SimEngine sudah punya budaya "referensi CPU" yang kuat: `SdfVolume` menyimpan
byte yang sama dengan rumus yang sama untuk menguji backend GPU-nya, acuan CPU
screen probe lengkap dengan pemetaan oktahedral dan proyeksi SH, dan kriteria
selesai M2 secara eksplisit menuntut "lulus uji ray tunggal terhadap referensi
CPU".

Tapi perhatikan apa yang **tidak** bisa ditangkap referensi semacam itu.
Referensi CPU yang ada menjawab *"apakah GPU melakukan hal yang sama dengan
rumus CPU?"* — dan kalau rumusnya sendiri yang keliru, keduanya salah dengan
kompak dan setiap test tetap hijau. Yang bisa menjawab *"apakah hasilnya benar
secara transport cahaya?"* hanyalah gambar konvergen dari path tracer tak-bias.

Di engine dengan tumpukan aproksimasi sebanyak ini — SDF clipmap global,
lapis screen-space, screen probe, hash grid radiance cache — celah itu bukan
kemewahan. Setiap lapis punya bias khasnya sendiri (kebocoran cahaya, oklusi
hilang, energi berlebih di pantulan kedua), dan tanpa gambar acuan, satu-satunya
kriteria yang tersisa adalah "kelihatannya benar".

Path tracer referensi tidak menggantikan referensi CPU yang ada. Keduanya
menjawab pertanyaan berbeda dan keduanya tetap dibutuhkan.

### 4. Bake — SDF per-mesh, bukan lightmap

Godaan pertama adalah lightmap. Tolak: arah GI engine ini sepenuhnya dinamis,
dan lightmap akan jadi jalur kedua yang harus dirawat tanpa ada yang memintanya.

Yang benar-benar tersumbat sudah tertulis di
[PLAN-RENDER.md](PLAN-RENDER.md): *"Belum ada: bake SDF per-mesh menjadi brick
sparse"*, dengan catatan bahwa itu "menuntut ketelitian sebuah baker yang belum
ada". Itu persis pekerjaan Embree, dan primitifnya sudah tersedia —
`rtcPointQuery` mengembalikan titik terdekat pada geometri, yang memberi jarak
tak-bertanda; tandanya didapat dari paritas jumlah perpotongan sepanjang satu
sinar keluar. Ini jauh lebih kokoh daripada baker SDF berbasis rasterisasi yang
pecah pada geometri tipis atau non-manifold.

AO bake ikut gratis begitu scene Embree ada.

---

## Ongkos dependensi

Embree adalah dependensi terberat yang akan masuk — kernelnya dikompilasi ulang
per ISA, dan konfigurasi bawaannya membangun jauh lebih banyak dari yang kita
pakai. Opsi berikut wajib, bukan penyetelan:

| Opsi | Nilai | Alasan |
| --- | --- | --- |
| `EMBREE_TASKING_SYSTEM` | `INTERNAL` | **Menghapus TBB sepenuhnya.** Bawaannya `TBB`, yang berarti satu dependensi berat lagi. Tasking hanya dipakai `rtcCommitScene`; penelusuran ray tidak memakainya sama sekali |
| `EMBREE_TUTORIALS` | `OFF` | Bawaannya **ON** dan membangun puluhan executable contoh |
| `EMBREE_MAX_ISA` | `AVX2` | Memangkas kompilasi kernel AVX-512 yang tidak dipakai CPU target |
| `EMBREE_STATIC_LIB` | `ON` | Seragam dengan modul `Sim*` lain yang semuanya statis |
| `EMBREE_ISPC_SUPPORT` | `OFF` | Sudah OFF bawaannya; jangan dinyalakan, ia menuntut compiler ISPC terpasang |
| `EMBREE_GEOMETRY_SUBDIVISION` | `OFF` | Tidak ada subdiv di pipeline aset |
| `EMBREE_GEOMETRY_CURVE`, `_POINT`, `_GRID`, `_QUAD` | `OFF` | Semua mesh kita segitiga |
| `EMBREE_GEOMETRY_TRIANGLE`, `_INSTANCE`, `_USER` | `ON` | Segitiga untuk geometri, instance untuk transform per entity, user geometry disisakan untuk terrain prosedural |
| `EMBREE_RAY_MASK` | `ON` | Sudah ON bawaannya; dipakai memisahkan lapis pickable dari lapis collision |
| `EMBREE_FILTER_FUNCTION` | `ON` | Sudah ON; dipakai alpha-test dedaunan saat bake |

Catatan versi: `CMakeLists.txt` Embree menyatakan `cmake_minimum_required(3.10)`.
Aman di CMake 3.25, tapi ini titik yang perlu diperiksa kalau proyek naik ke
CMake 4.

Waktu konfigurasi pertama akan naik terukur. R0 mewajibkan angkanya diukur dan
dicatat, bukan diperkirakan.

**Interaksi dengan `sim::TaskPool`.** Jangan bersarang. `rtcCommitScene`
memakai tasking internal Embree dan dipanggil di luar loop frame; penelusuran
ray (`rtcIntersect1`) aman dari banyak thread dan tidak memakai tasking sama
sekali. Jadi paralelisme tile path tracer dikirim ke `TaskPool` seperti biasa,
dan hasilnya kembali lewat `MainThreadQueue` (seam #5 di
[ARCHITECTURE.md](ARCHITECTURE.md)).

---

## Arsitektur

```
Code/Raycast/                     ← modul dasar, dipakai editor dan baker
  include/Sim/Raycast/
      RayScene.h                      RTCScene + pemetaan primitif → entity
      Query.h                         Raycast, Occluded, ClosestPoint
  src/
      RayScene.cpp                    build/refit, cache per GUID mesh
      Query.cpp

Code/RefPath/                     ← path tracer referensi, tidak dipakai runtime
  include/Sim/RefPath/
      ReferenceRenderer.h             render(scene, kamera, spp) → buffer HDR
      ImageCompare.h                  RMSE, FLIP, laporan per-region
  src/
      ReferenceRenderer.cpp
      Bsdf.cpp                        Lambert + GGX, disamakan dengan OpenPBR
      ImageCompare.cpp

Code/Bake/                        ← baker SDF brick & AO
  include/Sim/Bake/
      SdfBaker.h
      AoBaker.h
```

Aturan yang ditegakkan CMake, bukan disiplin: **`Sim::Raycast` tidak boleh
bergantung pada `Sim::RHI` maupun `Sim::Render`.** Ia murni CPU dan geometri,
dan itulah yang membuatnya bisa dipakai `SimHeadless` tanpa display.

### Berbagi buffer vertex tanpa repack

`assets::MeshVertex` adalah struct interleaved 32 byte (`Vec3 position`,
`Vec3 normal`, `Vec2 uv`). Embree bisa membacanya langsung lewat
`rtcSetSharedGeometryBuffer` dengan `byteStride = 32` dan
`byteOffset = 0` — **tidak perlu menyalin atau memisahkan posisi ke array
tersendiri.**

Satu syarat halus yang harus dicatat karena melanggarnya menghasilkan pembacaan
di luar batas, bukan galat: Embree boleh membaca 16 byte pada setiap offset
vertex. Untuk vertex terakhir itu berarti `(N-1)*32 + 16 ≤ N*32` — terpenuhi
karena stride-nya 32 dan yang dibaca berlebih jatuh ke medan `normal` milik
vertex yang sama. Aturan ini pecah kalau suatu saat `MeshVertex` diperkecil di
bawah 16 byte; uji di R1 menguncinya.

---

## Milestone

### R0 — Dependensi & kerangka modul · ⬜

Tambahkan Embree ke `cmake/SimDeps.cmake` lewat `FetchContent` dengan tag
`v4.4.1` dan seluruh opsi di tabel di atas. Buat opsi `SIM_WITH_EMBREE`
(default **ON**) dan modul `Sim::Raycast` yang masih kosong. Catat di
[DEPENDENCIES.md](DEPENDENCIES.md) mengikuti format tabel yang sudah ada,
termasuk kolom risiko.

**Kriteria terima**
- `cmake --preset linux-clang-release` sukses; **waktu konfigurasi pertama dan
  waktu build Embree tercatat di DEPENDENCIES.md sebagai angka terukur.**
- `-DSIM_WITH_EMBREE=OFF` juga sukses, dan seluruh test tetap lulus (jalur
  AABB lama masih ada di titik ini).
- `ldd` pada `SimEditor` tidak menunjukkan `libtbb` — bukti
  `EMBREE_TASKING_SYSTEM=INTERNAL` bekerja.
- Preset asan tetap bisa dibangun.

### R1 — `RayScene` dari MeshData · ⬜

Bangun `RTCScene` dari mesh yang sudah diimpor: satu `RTCGeometry` segitiga per
aset mesh (di-cache per GUID), satu `RTC_GEOMETRY_TYPE_INSTANCE` per entity yang
membawa transform-nya. Perubahan transform hanya menulis ulang matriks instance
lalu `rtcCommitScene` — bukan membangun ulang BVH mesh-nya.

Pemetaan balik `(geomID, primID) → entity + segitiga` disimpan di sisi kita.

**Kriteria terima**
- Uji doctest: scene dengan 3 mesh × 100 instance dibangun, satu sinar
  menembusnya, `geomID`/`primID` dipetakan ke entity yang benar.
- Uji buffer bersama: `MeshVertex` dipakai apa adanya, dan uji `static_assert`
  mengunci `sizeof(MeshVertex) >= 16` beserta offset `position` di 0.
- Memindahkan entity lalu commit ulang < 1 ms untuk 1.000 instance.

### R2 — Picking presisi menggantikan AABB · ⬜

`SceneView::Raycast` memakai `Sim::Raycast`. Jalur AABB dipertahankan sebagai
nilai mundur untuk entity tanpa mesh (ikon, lampu, kamera) — yang memang tidak
punya segitiga untuk ditembak.

**Kriteria terima**
- Klik menembus lubang mesh berlubang **tidak** memilih mesh itu; uji otomatis
  dengan torus, bukan verifikasi manual.
- Dua objek bertumpuk terpilih sesuai bentuknya, bukan kotaknya.
- 10.000 entity di scene: satu picking < 1 ms, diukur dan dicatat.
- Perilaku `RectSelect` tidak berubah — ia memang bekerja di ruang layar dan
  tidak perlu Embree.

### R3 — Query authoring · ⬜

`Conform`, `SnapToSurface`, dan proyeksi decal di atas mesh arbitrer. Penempatan
vegetasi mendapat mode kedua "di atas mesh" di samping mode heightmap yang tetap
jadi bawaan untuk terrain.

**Kriteria terima**
- Menjatuhkan objek ke permukaan miring menempatkannya menyinggung permukaan,
  dengan orientasi mengikuti normal.
- Brush vegetasi di atas mesh batu menempatkan instance di permukaannya, dan
  di atas terrain tetap memakai heightmap (dibuktikan dengan menghitung
  panggilan, bukan dengan melihat hasilnya).
- Semua operasi lewat command/undo yang ada.

### R4 — Path tracer referensi & regresi visual · ⬜

Path tracer unidirectional sederhana: Lambert + GGX yang **parameternya
disamakan dengan OpenPBR** ([RENDER-OPENPBR.md](RENDER-OPENPBR.md)), next-event
estimation, tanpa denoise. Tidak perlu cepat; perlu benar dan tak-bias.

Sengaja dibatasi: tidak ada volume, tidak ada SSS, tidak ada dispersi. Yang
divalidasi adalah GI difus dan spekular kasar — persis yang diaproksimasi
tumpukan SDF/probe/radiance-cache.

Ditambah `ImageCompare` dan satu set adegan uji kecil: Cornell box (kebocoran
cahaya), koridor dengan pantulan tak-langsung (energi bounce kedua), dan objek
di atas bidang (oklusi kontak).

**Kriteria terima**
- Cornell box konvergen pada 4.096 spp cocok dengan nilai analitik faktor bentuk
  dinding dalam toleransi yang ditulis di test.
- Perintah `SimHeadless --reference-render <level> --spp N` menghasilkan EXR
  tanpa display.
- Laporan perbandingan GI runtime versus referensi tersimpan sebagai angka
  (RMSE per region), bukan sebagai gambar untuk dilihat manusia.
- **Satu bias yang sudah diketahui terdokumentasi dengan angkanya** — kalau
  hasilnya cocok sempurna di semua adegan, kemungkinan besar yang diuji bukan
  yang dipakai menggambar.

### R5 — Baker SDF brick & AO · ⬜

Membuka yang tersumbat di GI M1: bake SDF per-mesh menjadi brick sparse.
`rtcPointQuery` memberi jarak; paritas perpotongan memberi tanda. AO bake
menyusul memakai scene yang sama.

**Kriteria terima**
- SDF hasil bake untuk bola satuan cocok dengan `length(p) - r` dalam toleransi
  setengah voxel.
- Mesh non-manifold (dua segitiga saling menembus) tetap menghasilkan tanda yang
  konsisten, atau ditolak dengan pesan yang jelas — tidak menghasilkan brick
  rusak diam-diam.
- Bake berjalan di `TaskPool`, editor tidak membeku, progres terlihat.
- Brick hasilnya dikonsumsi jalur SDF clipmap yang sudah ada tanpa perubahan
  format.

---

## Risiko

| Risiko | Tanda awal | Mundur ke |
| --- | --- | --- |
| Waktu build Embree merusak alur kerja | R0 mencatat > 5 menit | Turunkan ke `EMBREE_MAX_ISA=SSE4.2`; kalau masih berat, jadikan `SIM_WITH_EMBREE` default OFF di preset debug |
| Tasking INTERNAL lebih lambat dari TBB saat commit | Commit scene besar terasa di editor | Ukur dulu; TBB hanya ditambahkan kalau selisihnya terbukti mengganggu, bukan karena asumsi |
| Path tracer referensi jadi proyek sendiri | R4 lewat dua minggu tanpa gambar konvergen | Potong ke Cornell box saja; satu adegan yang benar lebih berguna daripada lima yang setengah jadi |
| Referensi dan runtime tidak pernah cocok karena definisi material berbeda | Selisih besar dan konstan di semua adegan | Samakan BSDF-nya dulu di adegan satu material, sebelum menilai GI-nya |
| Duplikasi geometri CPU memakan memori | Scene besar membengkak | Buffer dibagi, bukan disalin (R1); yang tersisa hanya BVH-nya |

---

## Yang tidak boleh ditunda

- **`Sim::Raycast` tidak boleh menyentuh RHI.** Begitu ia butuh `VkDevice`,
  `SimHeadless` dan seluruh nilai CI-nya hilang.
- **Buffer dibagi sejak awal, bukan disalin.** Menambahkan berbagi buffer
  belakangan berarti membongkar jalur pembangunan scene setelah tiga milestone
  bergantung padanya.
- **Angka, bukan gambar, sebagai keluaran R4.** Regresi visual yang dinilai mata
  bukan regresi yang bisa dijalankan CI.

---

## Yang sengaja tidak dikerjakan

- **Rendering runtime dengan Embree.** Aritmetikanya tidak mendukung, dan
  `VK_KHR_ray_query` sudah tersedia di perangkat target.
- **Lightmap bake.** Arah GI engine ini dinamis; lightmap akan jadi jalur kedua
  tanpa peminta.
- **Backend SYCL Embree.** Hanya untuk Intel Xe.
- **Mengganti collision/physics query dengan Embree.** Embree dioptimalkan untuk
  ray terhadap geometri statis, bukan untuk sweep dan manifold kontak.
