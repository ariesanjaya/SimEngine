# Plan Ray Query CPU (R0 → R6)

Sebuah modul `Sim::Raycast` yang menjawab satu pertanyaan — *"sinar ini kena
segitiga mana"* — untuk kebutuhan yang semuanya berada **di luar hot path
frame**: picking presisi, query authoring, dan path tracer referensi.

Penomoran **R** (ray) supaya tidak bertabrakan dengan E (editor), A (agentic AI),
dan C (kain) di [ROADMAP.md](ROADMAP.md).

> **Revisi — berkas ini dulu berjudul "Plan Integrasi Embree (R0 → R5)".**
>
> Namanya tidak diubah: [PLAN-PHYSICS.md](PLAN-PHYSICS.md) dan
> [PLAN-IMAGEIO.md](PLAN-IMAGEIO.md) menunjuk R2, R3, dan R4 dengan nama berkas
> ini, dan penomoran itu dipertahankan utuh. Yang berubah dua hal, keduanya
> karena pemeriksaan ulang terhadap kode yang sudah ada:
>
> 1. **Kebutuhan keempat — bake SDF per-mesh — sudah terjawab OpenVDB**, jadi ia
>    bukan lagi alasan mendatangkan Embree. Lihat bagian yang bersangkutan.
> 2. **Modul dipisahkan dari backend-nya.** R0 sekarang membangun `Sim::Raycast`
>    dengan BVH sendiri, dan [Intel Embree](https://github.com/RenderKit/embree)
>    4.4.1 turun menjadi **R6, bersyarat** — backend kedua yang masuk saat ada
>    angka yang menuntutnya, bukan di awal. Alasannya di "Keputusan pokok".
>
> Embree tetap backend tujuan. Yang dikoreksi hanya urutannya.

---

> **Status terukur, 23 Agustus 2026.** R0–R2 selesai, R3 sebagian, R4/R5 belum
> mulai. **R6 (Embree) ditutup sebagai tidak diperlukan** — throughput BVH
> sendiri diukur dan syarat masuknya tidak terpenuhi, dengan selisih dua orde
> magnitudo. Antarmukanya tetap berdiri, jadi keputusan ini bisa dibalik tanpa
> membongkar apa pun.
>
> **R4 langkah 1 dan 2 selesai.** Yang disebut jalur kritisnya — kembaran CPU
> dari `openpbr.slang` — ternyata tidak perlu ditulis sama sekali: `slangc
> -target cpp` memancarkannya dari sumber yang sama yang dijalankan GPU.
> Integratornya berdiri di `Code/Reference`, tak-bias, dan diuji terhadap
> jawaban analitik — termasuk `E/(1-ρ)` di rongga tertutup, yang menguji energi
> pantulan ke-n. Yang tinggal butuh GPU atau penulis EXR, jadi keduanya alat,
> bukan uji.
>
> `PLAN-EMBREE-GI.md` — usulan terpisah yang memakai Embree sebagai builder BVH
> untuk traversal compute — **dihapus**, sudah terjawab lain oleh arsitektur GI
> yang benar-benar dibangun. Alasannya diserap ke R6 di bawah.

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

**Backend adalah plugin, dan modulnya dibangun lebih dulu.** Aturan yang sama
dengan `TraceBackendKind` di GI, dan alasannya juga sama: yang memanggil
`Sim::Raycast` tidak boleh tahu apa yang menelusuri BVH-nya. Konsekuensinya
langsung — R0 tidak menambah satu pun dependensi.

Perhatikan siapa yang sebenarnya membutuhkan kecepatan Embree. Picking: satu ray
per klik. Query authoring: beberapa ribu ray per sapuan. Pada angka-angka itu
BVH ber-SIMD dan BVH sederhana tidak bisa dibedakan siapa pun. Yang benar-benar
menuntut throughput hanya path tracer referensi (R4) — dan **jalur kritis R4
bukan intersector-nya melainkan kembaran CPU dari `openpbr.slang`**. Sebelum itu
ada, intersector yang cepat hanya membuat BSDF yang salah dihitung lebih cepat.

Mendatangkan dependensi terberat yang pernah masuk untuk kemampuan yang
satu-satunya calon pemakainya belum ditulis berarti membayar waktu konfigurasi
dan waktu build di setiap build bersih, sekarang, demi manfaat yang datang
nanti. Karena itu R0 memakai BVH sendiri, dan R6 menukarnya **setelah R4 berjalan
dan intersector terbukti jadi penghambatnya.**

**Möller–Trumbore sudah ada di repo.** `Code/Whitebox/src/Picking.cpp` — 95 baris
untuk seluruh berkasnya — sudah menembakkan sinar ke segitiga di ruang lokal
untuk memilih sisi whitebox. Bagian yang paling sering dikira sulit itu sudah
ditulis dan sudah dipakai; yang belum ada hanya struktur percepatan di atasnya.

---

## Tiga kebutuhan, dan apa sebenarnya yang kurang

### 1. Picking presisi segitiga

`SceneView::Raycast` (`Code/EditorFramework/src/SceneView.cpp:375`) saat ini
menguji **AABB saja**, dengan pemindaian linear atas seluruh `pickables_`. Sinar
memang dibawa ke ruang lokal supaya objek yang diputar tetap diuji pada
kotaknya sendiri — itu sudah benar — tapi kotak tetaplah kotak. Klik di tengah
lubang sebuah donat memilih donat itu; klik di celah antara dua tangga memilih
salah satunya.

`Sim::Raycast` memberi dua hal sekaligus: uji per segitiga, dan BVH dua tingkat
yang mengubah pemindaian linear menjadi logaritmik. Keduanya didapat dari backend
mana pun — pada satu ray per klik, yang menentukan adalah struktur pohonnya,
bukan seberapa cepat satu simpul diuji.

### 2. Query authoring

Conform objek ke permukaan mesh, proyeksi decal, snapping ke geometri, dan
penempatan vegetasi **di atas mesh arbitrer**.

Batasnya harus jelas: penempatan vegetasi di atas terrain **tetap memakai
heightmap**. `VegetationBrush.cpp:134` memanggil `terrain.HeightAtWorld()`, dan
itu sudah eksak sekaligus lebih murah daripada ray cast mana pun. `Sim::Raycast`
hanya menambah kemampuan yang sekarang tidak ada sama sekali: menaruh sesuatu di
atas batu, atap, atau jembatan.

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

### ~~4. Bake SDF per-mesh~~ — sudah terjawab OpenVDB

**Kebutuhan keempat dicoret, dan ini koreksi terpenting di dokumen ini.**

Versi pertama rencana ini berargumen bahwa bake SDF per-mesh "persis pekerjaan
Embree", karena `rtcPointQuery` memberi jarak tak-bertanda dan paritas
perpotongan memberi tandanya. Argumen itu sah — tetapi pekerjaannya sudah
dilakukan pustaka yang **sudah menjadi dependensi**.

`Code/Volume/src/SdfBake.cpp` memanggil `openvdb::tools::meshToVolume`, dan yang
dikembalikannya persis kedua hal itu sekaligus: level set bertanda, plus
**indeks poligon terdekat per voxel** yang lalu dipetakan menjadi satu byte
atribut per segitiga. `BakeMeshSdf` menerima `positions` dan `indices` dalam
bentuk yang sudah dipegang `MeshData`, jadi tidak ada penyalinan.

Ia bahkan sudah menjawab kasus yang dulu jadi alasan menolak baker berbasis
rasterisasi: mesh yang tidak tertutup tetap dibake, tandanya di dalam menjadi
tidak berarti, dan yang dijamin tetap benar adalah besar jarak di dekat
permukaan — yaitu satu-satunya yang dipakai sphere tracing untuk berhenti.
Semuanya tertulis di `SdfBake.h`.

Yang **masih** tersumbat karena itu bukan penghitungan jaraknya melainkan
pengemasannya menjadi brick sparse yang dikonsumsi clipmap. Itu isi R5, dan ia
tidak menunggu Embree.

AO bake tetap ikut gratis begitu `Sim::Raycast` ada — backend mana pun.

---

## Ongkos dependensi (berlaku untuk R6)

Seluruh bagian ini menggambarkan harga yang dibayar **kalau** R6 dijalankan. R0
sampai R5 tidak menyentuhnya.

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

Waktu konfigurasi pertama akan naik terukur. R6 mewajibkan angkanya diukur dan
dicatat, bukan diperkirakan — dan angka itulah yang menentukan apakah ia layak
dibiarkan menyala secara bawaan.

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
      RayScene.h                      scene + pemetaan primitif → entity
      Query.h                         Raycast, Occluded, ClosestPoint
      Backend.h                       enum backend + pemilihannya
  src/
      RayScene.cpp                    build/refit, cache per GUID mesh
      Query.cpp
      BvhBackend.cpp                  BVH sendiri (R0) — bawaan, tanpa dependensi
      EmbreeBackend.cpp               RTCScene (R6) — hanya bila SIM_WITH_EMBREE

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

Aturan kedua, dan ia yang membuat R6 bisa ditunda tanpa utang: **tipe Embree
tidak boleh muncul di satu pun header publik.** `RayScene.h` menyebut handle
miliknya sendiri, bukan `RTCScene`. Begitu `RTCScene` bocor ke header, setiap
pemanggil ikut menyertakan `embree4/rtcore.h`, dan "backend adalah plugin"
berhenti menjadi benar pada hari itu juga.

### Berbagi buffer vertex tanpa repack

Berlaku untuk kedua backend, dan itu sebabnya ia ada di sini alih-alih di dalam
salah satu milestone.

`assets::MeshVertex` adalah struct interleaved 32 byte (`Vec3 position`,
`Vec3 normal`, `Vec2 uv`). BVH sendiri membacanya dengan stride; Embree
membacanya langsung lewat `rtcSetSharedGeometryBuffer` dengan `byteStride = 32`
dan `byteOffset = 0` — **tidak perlu menyalin atau memisahkan posisi ke array
tersendiri.**

Satu syarat halus yang harus dicatat karena melanggarnya menghasilkan pembacaan
di luar batas, bukan galat: Embree boleh membaca 16 byte pada setiap offset
vertex. Untuk vertex terakhir itu berarti `(N-1)*32 + 16 ≤ N*32` — terpenuhi
karena stride-nya 32 dan yang dibaca berlebih jatuh ke medan `normal` milik
vertex yang sama. Aturan ini pecah kalau suatu saat `MeshVertex` diperkecil di
bawah 16 byte; uji di R1 menguncinya.

---

## Milestone

### R0 — Modul `Sim::Raycast` & backend BVH sendiri · ✅

**Tidak ada dependensi baru di milestone ini.** Buat modul `Sim::Raycast` dengan
antarmuka lengkapnya — `RayScene`, `Raycast`, `Occluded`, `ClosestPoint` — dan
satu backend: BVH biner ber-SAH di atas Möller–Trumbore yang sudah ada di
`Code/Whitebox/src/Picking.cpp`. Pindahkan fungsi itu ke tempat yang bisa dipakai
berdua; whitebox tetap memanggilnya lewat jalur yang sama.

Yang menentukan bentuknya sejak awal, karena ketiganya mahal ditambahkan
belakangan: **dua tingkat** (BVH per aset mesh, di-cache per GUID; BVH atas
instance di atasnya), **buffer dibagi bukan disalin**, dan **pemetaan balik
`(geometri, primitif) → entity + segitiga`** disimpan di sisi kita.

Ini juga milestone yang membuat R6 opsional selamanya: kalau BVH sendiri ternyata
cukup untuk R4, Embree tidak pernah perlu masuk.

**Kriteria terima**
- `cmake --preset linux-clang-release` sukses tanpa dependensi tambahan, dan
  **waktu build tidak naik terukur** dibanding sebelum milestone ini.
- Uji doctest: sinar terhadap satu segitiga tunggal cocok dengan hasil
  `whitebox::` yang sudah ada, untuk kasus kena, meleset, dan sejajar bidang.
- Möller–Trumbore hanya punya **satu** salinan di repo sesudah milestone ini —
  dibuktikan dengan grep di test, bukan dengan ingatan.
- `Sim::Raycast` tidak me-link `Sim::RHI` maupun `Sim::Render`; dibuktikan
  dengan target CMake, dan `SimHeadless` memakainya tanpa display.

**Hasil**

- `Code/Raycast/`: BVH ber-SAH terbin (12 bin), dua tingkat, dengan `Raycast`,
  `Occluded`, dan `FindClosestPoint`. Empat TU, **2,1 detik** untuk seluruh
  modul; nol dependensi baru, waktu konfigurasi tidak berubah.
- Möller–Trumbore pindah ke `Sim/Core/Intersect.h` bersama uji slab dan titik
  terdekat pada segitiga. `Code/Whitebox/src/Picking.cpp` memanggilnya dari sana;
  salinan lamanya hilang, dan `SimRaycastTests` menyisir `Code/` untuk menjaganya
  tetap satu.
- Larangan dependensi GPU **ditegakkan saat konfigurasi**, bukan diminta:
  `sim_raycast_assert_no_gpu_deps` menelusuri closure link SimRaycast dan
  menggagalkan `cmake` bila `Sim::RHI` atau `Sim::Render` muncul. Diuji dengan
  melanggarnya sekali, lalu dipulihkan.
- 12 test case / 63 assertion, termasuk klik menembus lubang bingkai yang uji
  AABB salah jawab, 3 mesh × 100 instance, dan buffer yang digeser di tempat
  untuk membuktikan ia dibagi alih-alih disalin.

**Yang belum, dan sengaja:** belum ada satu pun pemanggil di luar test — itu R1
(mengisi dari `MeshData`) dan R2 (menggantikan jalur AABB `SceneView::Raycast`).
Kriteria "SimHeadless memakainya" karena itu baru bisa dicentang di sana; yang
sudah terbukti sekarang adalah modulnya tidak menuntut display, karena
binari testnya berjalan tanpa satu pun.

### R1 — `RayScene` dari MeshData · ✅

> **Temuan yang mengubah bentuk milestone ini: salinan CPU geometrinya tidak
> ada.**
>
> `IViewportRenderer::AcquireMesh` memuat sebuah berkas, mengunggahnya ke GPU,
> dan tidak menyimpan satu pun segitiga di RAM — pilihan yang benar untuk
> menggambar, dan tidak ada cache `MeshData` di mana pun di mesin ini.
> `MeshSdfBakery` memuat salinannya sendiri lalu membuangnya; `SceneView::Pickable`
> hanya memegang kotak batas dan matriks.
>
> Artinya "buffer dibagi, bukan disalin" di R0 tidak bisa dipenuhi begitu saja:
> tidak ada buffer untuk dibagi. Sesuatu harus memilikinya.
>
> **Yang dipilih: cache asinkron dengan jawaban pertama `Pending`,** mengikuti
> `MeshSdfBakery` dan `TextureBakery` yang sudah ada. Mengurai satu FBX memakan
> ratusan milidetik; memuatnya di dalam penanganan klik berarti editor membeku
> setiap kali seseorang mengklik benda yang belum pernah diklik. Sampai
> geometrinya siap, picking memakai jalur AABB — yang memang sudah direncanakan
> bertahan sebagai nilai mundur di R2.
>
> Ongkosnya nyata dan harus terlihat: satu salinan CPU tiap mesh di adegan.
> `MeshGeometryCache::BytesHeld()` ada untuk melaporkannya.

Isi `RayScene` dari mesh yang sudah diimpor: satu BVH segitiga per aset mesh
(di-cache per GUID), satu instance per entity yang membawa transform-nya.
Perubahan transform hanya menulis ulang matriks instance lalu commit — **bukan
membangun ulang BVH mesh-nya.**

Struktur dua tingkat ini bentuk yang sama di kedua backend; pada R6 ia dipetakan
ke `RTC_GEOMETRY_TYPE_INSTANCE` dan `rtcCommitScene` tanpa mengubah pemanggil.

Pemetaan balik `(geometri, primitif) → entity + segitiga` disimpan di sisi kita —
bukan di dalam backend, justru supaya kedua backend memberi jawaban yang sama.

**Kriteria terima**
- Uji doctest: scene dengan 3 mesh × 100 instance dibangun, satu sinar
  menembusnya, pasangan `(geometri, primitif)` dipetakan ke entity yang benar.
- Uji buffer bersama: `MeshVertex` dipakai apa adanya, dan uji `static_assert`
  mengunci `sizeof(MeshVertex) >= 16` beserta offset `position` di 0.
- Memindahkan entity lalu commit ulang < 1 ms untuk 1.000 instance.

**Sudah ada**

- `RayScene::ClearInstances()` — celah di API R0 yang baru terlihat saat dipakai:
  `SceneView` menyusun ulang isinya tiap frame, dan `Clear()` di sana berarti
  setiap BVH mesh dibangun ulang enam puluh kali per detik untuk geometri yang
  tidak berubah.
- `assets::MeshGeometryCache` — salinan CPU per berkas, `shared_ptr` ke data yang
  tidak pernah berubah supaya BVH yang menyimpan pointer telanjang ke dalamnya
  tidak pernah menggantung. `Adopt` untuk bentuk yang lahir di editor (whitebox,
  ubin terrain) yang tidak punya berkas untuk diurai.
- Enam test case baru: `ClearInstances` mempertahankan geometri dan handle-nya
  tetap sah, `Clear` membuangnya, cache mengembalikan pointer yang sama pada
  permintaan kedua, kegagalan diingat, dan adopsi kedua menimpa tanpa mengganggu
  yang masih memegang ref lama.

- `view::PickScene` — penyusunnya. **Dibangun saat ditanya, bukan tiap frame**,
  dan itu keputusan yang paling menentukan di sini: sebuah adegan menggambar
  enam puluh kali per detik dan diklik beberapa kali per menit. Yang tersisa di
  jalur frame hanyalah kunci geometri yang ikut di `SceneView::Pickable` —
  sebuah string, bukan segitiga.
- `MeshGeometryCache::Find` — menanyakan tanpa memuat dan tanpa menyisipkan,
  untuk bentuk yang hanya bisa datang lewat `Adopt`.
- `userData` menyimpan **`SelectionId`, bukan `Entity` mentah**: nol di sana
  sudah berarti "tidak ada", jadi entity pertama entt tidak tertukar dengan
  ketiadaan. Ada uji khusus untuk itu.

**Angka terukur.** Menggeser seribu instance lalu menyusun ulang:
**0,57 ms** di `linux-clang-release` — di bawah kriteria satu milidetik. Build
Debug menjalankannya di 23,6 ms, dan ambang ujinya karena itu berbeda per
konfigurasi; yang dijaga tetap nyata, karena geometri yang diam-diam dibangun
ulang melonjakkan angkanya dua orde, bukan beberapa persen.

**Yang belum, dan itu R2:** `SceneView::Raycast` masih memakai jalur AABB. Yang
menggantinya tinggal memanggil `PickScene`, dengan jalur lama bertahan untuk
entity tanpa mesh dan untuk mesh yang geometrinya belum selesai dimuat.

### R2 — Picking presisi menggantikan AABB · ✅

`SceneView::Raycast` memakai `Sim::Raycast`. Jalur AABB dipertahankan sebagai
nilai mundur untuk entity tanpa mesh (ikon, lampu, kamera) — yang memang tidak
punya segitiga untuk ditembak.

**Kriteria terima**
- Klik menembus lubang mesh berlubang **tidak** memilih mesh itu; uji otomatis
  dengan torus, bukan verifikasi manual.
- Dua objek bertumpuk terpilih sesuai bentuknya, bukan kotaknya.
- 10.000 entity di scene: satu picking < 1 ms, diukur dan dicatat.
- Perilaku `RectSelect` tidak berubah — ia memang bekerja di ruang layar dan
  tidak perlu ray cast sama sekali.

**Hasil, dengan angkanya.**

- Torus: sinar lewat lubangnya menjawab "tidak ada", lewat dagingnya menjawab
  torusnya. Dua benda bertumpuk — kubus kecil di dalam lubang torus — memilih
  kubusnya, yang uji AABB tidak mungkin jawab benar.
- **Picking di antara 10.000 entity: 0,193 ms** di `linux-clang-release`.
- Jalur kotak batas bertahan untuk yang belum terwakili segitiga, dan itu bukan
  hanya entity tanpa mesh: mesh yang geometrinya masih dimuat juga jatuh ke sana.
  Yang sudah terwakili **tidak** diuji kotaknya lagi — kotaknya selalu lebih
  besar daripada bentuknya, dan mengujinya kembali mengembalikan persis positif
  palsu yang baru saja dibuang.
- Jaraknya dibandingkan dalam satuan yang sama di kedua jalur, jadi benda yang
  geometrinya belum siap tetap bisa menang bila memang lebih dekat — dan
  jawabannya tidak melompat begitu pemuatannya selesai.

**Satu pelajaran yang mahal.** Penyusunan ulang dilewati bila tidak ada yang
bergeser, dan penjaganya sidik jari isi daftar. Versi pertamanya FNV-1a **byte
demi byte** atas 730 KB, dan itu sendiri memakan 1,25 ms — lebih mahal daripada
penelusuran yang seharusnya ia hemat, dan cukup untuk melanggar kriteria yang
menjadi alasannya ada. Per kata: 0,193 ms. Optimisasi yang tidak diukur adalah
tebakan, dan tebakan ini salah enam kali lipat.

### R3 — Query authoring · 🚧

`Conform`, `SnapToSurface`, dan proyeksi decal di atas mesh arbitrer. Penempatan
vegetasi mendapat mode kedua "di atas mesh" di samping mode heightmap yang tetap
jadi bawaan untuk terrain.

> **Vegetasi ditunda atas permintaan** — mode "di atas mesh" untuk brush-nya
> belum dikerjakan, dan kriteria terimanya di bawah ikut menunggu. Yang sudah:
> conform ke permukaan. Yang belum di luar vegetasi: proyeksi decal.

**Hasil sejauh ini**

- `SceneView::RaycastSurface` — titik, normal, dan pemiliknya. **Jalur kotak
  batas sengaja tidak dipakai di sini**, dan itu perbedaan yang menentukan
  terhadap `Raycast`: memilih benda dari kotaknya sedikit terlalu murah hati dan
  hasilnya tetap benda yang dimaksud, sementara menaruh sesuatu di *permukaan*
  sebuah kotak menaruhnya di udara — dengan orientasi mengikuti sisi yang tidak
  ada di bentuk aslinya. Yang geometrinya belum dimuat menjawab "tidak kena", dan
  pemanggil melewatkannya alih-alih memindahkannya ke tempat yang salah.
- `PickScene::RaycastExcluding` — benda yang dijatuhkan tidak boleh mendarat di
  dirinya sendiri. Dikerjakan dengan menembak ulang dari titik kena, dengan
  percobaan terbatas.
- `ConformToSurface` — fungsi bebas, murni geometri, jadi perilakunya bisa diuji
  tanpa dunia, seleksi, maupun history. **Putaran terpendek, bukan bingkai
  baru**: menyusun bingkai dari normal membuang seluruh orientasi yang sudah
  disetel orang, dan kursi yang menghadap pintu akan menghadap ke arah acak
  begitu dijatuhkan.
- Pintasan **End** (Shift+End meratakan ke normal), lewat `SetTransformsCommand`
  yang sudah ada — jadi satu perintah, satu entri undo.
- Sembilan pernyataan uji baru: permukaan miring, offset pivot sepanjang normal
  bukan sepanjang Y, arah hadap yang bertahan, normal terbalik dan normal nol
  yang tidak menghasilkan NaN, dan sinar yang melewatkan benda yang dijatuhkan.

**Catatan yang menentukan benar-salahnya:** seluruh seleksi dikecualikan dari
sinarnya, bukan hanya benda yang sedang dihitung. Menjatuhkan tumpukan kotak
sekaligus akan membuat yang di atas mendarat di yang di bawahnya — yang belum
sempat turun — dan hasilnya tumpukan yang tetap melayang dengan jarak yang sama.

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

#### Langkah 1 — kembaran CPU · ✅ **selesai, dan tidak ditulis tangan**

Ini yang selama ini disebut jalur kritis R4, dan ongkosnya ternyata **nol**.

Ketika rencana ini ditulis, kekhawatirannya jelas: `openpbr.slang` harus punya
kembaran CPU, dan berkas itu sudah 920 baris — termasuk sheen LTC Zeltner
dengan fit rasionalnya, EON beserta albedo terarah bentuk tertutup, Fresnel
F82, kompensasi energi GGX, dan penggelapan coat lewat albedo hemisferis
Fresnel. Kembaran yang meleset di salah satunya bukan acuan melainkan renderer
kedua yang kebetulan mirip, dan selisihnya akan terbaca sebagai galat GI
padahal asalnya BSDF.

**`slangc -target cpp` memancarkan C++ dari sumber yang sama yang dikompilasi
ke SPIR-V.** Tidak ada transkripsi, jadi tidak ada yang bisa meleset: drift
bukan sesuatu yang diuji, melainkan sesuatu yang **tidak bisa terjadi**.

- `Shaders/openpbr_cpu.slang` — pembungkus tanpa satu baris matematika pun.
  Entry point compute, karena itu bentuk yang bisa dipancarkan slangc ke CPU;
  ia tidak pernah dijalankan di GPU.
- `Tests/CMakeLists.txt` membangkitkannya saat build, dengan `openpbr.slang`
  sebagai dependensi eksplisit — tanpa itu uji ini akan lulus atas model
  shading yang sudah tidak ada, bug yang sama persis dengan yang dijaga
  identitas kompilator di `ShaderCache`.
- `Tests/OpenPbrCpuTests.cpp` — sebelas uji, mengunci temuan audit sebagai
  angka. Yang paling tajam: rasio antara logam bertepi warna dan logam bertepi
  putih di sudut `acos(1/7)` harus **tepat** `specularColor`, bukan sekadar
  "lebih gelap".

Dilewati bersih tanpa `slangc`, seperti target shader lainnya.

**Satu jebakan yang mahal, dan sudah dipasang penjaganya.** Koreksi F82
memuncak di `mu = 1/7` dan meluruh ke nol di kedua ujungnya — pada insidensi
tegak lurus bobotnya `3e-7`. Uji pertama yang saya tulis memeriksa
`specularColor` menghadap kamera, dan ia **lulus untuk material yang pin-nya
mati sama sekali**. Sudut ujinya sekarang disebut eksplisit di harness-nya,
beserta alasannya.

C++ yang sama inilah yang ditautkan path tracer acuan di langkah berikutnya,
jadi acuan dan runtime berbagi satu sumber model shading — bukan dua yang
dijaga tetap sama.

#### Langkah 2 — integrator · ✅

`Code/Reference` — modul CPU murni, tanpa `Sim::RHI` maupun `Sim::Render`,
dijaga guard CMake yang sama dengan `Sim::Raycast`.

**Model shading-nya dibangkitkan di sini, bukan di direktori uji.** C++ dari
`slangc` sekarang milik modulnya, dan seluruh nama bermangling dikurung di satu
berkas: `Shading.cpp`. Akhiran angka yang dipancarkan slangc bisa bergeser
ketika slangc diperbarui; dibiarkan bocor ke pemanggil, yang rusak setiap
pemakai model shading.

Bentuk integratornya persis yang ditetapkan bagian sebelumnya:

- **Estimator eksplisit** — `emisi + f·cos · L / pdf`, tiap faktor bisa dicetak
  sendiri.
- **Campuran dua strategi, satu sampel**, bobot tetap 50/50 antara sampling
  lampu dan kosinus. Yang memilih arah salah satunya; PDF-nya tetap gabungan
  keduanya, karena kalau tidak yang terjadi bukan campuran melainkan dua
  estimator yang dijumlahkan — dan itu bias.
- **Russian roulette dari throughput**, `maxDepth` hanya jaring pengaman.
- **Kamera pinhole + cakram defokus, stratified √spp × √spp.**

**Sampling BSDF sengaja kosinus, dan itu disebutkan.** `openpbr.slang` hanya
mengevaluasi — bentuk yang benar untuk model shading rasterizer. Menuliskan
penyampel yang cocok untuk kesembilan lobenya adalah proyek tersendiri, dan
yang dibutuhkan acuan lebih dulu adalah **benar**, bukan cepat konvergen.
Kosinus tak-bias untuk BSDF apa pun selama pembaginya PDF yang sama; yang
dibayar hanya derau pada lobe spekular sempit.

**Daftar lampu berdiri sendiri**, seperti yang sudah diperkirakan bagian
"Apa yang berbenturan": backend memberi `(geometri, primitif)`, bukan objek.
Satu konsekuensi yang perlu diketahui pengarang adegan — lampu harus ada di
**dua** tempat, di daftar lampu *dan* sebagai geometri, karena estimator
satu-sampel menemukan cahaya dengan mengenainya.

#### Kriteria terima, dan angkanya

Delapan uji, seluruhnya dibandingkan dengan jawaban yang **diketahui lebih
dulu** — bukan dengan keluaran renderer ini sendiri di masa lalu:

| Yang diuji | Terhadap apa |
| --- | --- |
| Tungku putih, albedo 1 | tepat 1,0 |
| Albedo 0,25 / 0,5 / 0,8 | tepat albedonya |
| PDF lampu kuad | `d²/(cos·A)` dihitung tangan |
| `Sample` lawan `Pdf` | keduanya harus sepakat |
| Lampu bidang di atas lantai | `ρ·L·A/(π·h²)`, hampiran sudut kecil |
| Derau turun saat spp naik | konvergensi, bukan rupa |

**Satu kelulusan palsu yang hampir lolos, dan cara ia ketahuan.** Kamera yang
melihat lurus ke bawah — sudut yang paling sering dipakai — membuat
`cross(forward, up)` nol dan seluruh basisnya NaN, sehingga setiap sinar
meleset dan gambarnya menjadi langit seluruhnya. **Uji tungku putih lulus di
sana**, karena langit seragam memang jawabannya. Yang menemukannya uji albedo,
tiga uji kemudian. Sekarang kameranya punya sumbu cadangan, dan uji tungku
putihnya membuktikan lebih dulu bahwa lantainya benar-benar kena.

#### Langkah 3 — adegan acuan · ✅ sebagian

`Scene` menyusun geometri, material per segitiga, dan daftar lampunya sekaligus.
**Lampu masuk ke dua tempat lewat satu panggilan** — `AddQuadLight` menaruhnya
di geometri *dan* di daftar — supaya keadaan setengah itu tidak bisa terjadi
karena lupa: yang hanya ada di daftar tidak akan pernah menerangi apa pun, yang
hanya ada di geometri tidak akan pernah disampel langsung.

`ImageCompare` menjawab **angka**: RMSE, selisih mutlak terbesar beserta
letaknya, dan rata-rata kedua gambar — yang terakhir yang membedakan bias dari
derau, karena derau tidak menggeser rata-rata. Daerah bernama ada karena RMSE
seluruh gambar menyembunyikan justru yang dicari: kebocoran lewat dinding
menyentuh beberapa persen piksel, dan ditenggelamkan rata-rata ia terbaca
sebagai selisih kecil yang bisa diabaikan.

**Dua adegan, dan yang pertama jawabannya eksak.**

`MakeEnclosedFurnace(ρ, E)` — rongga tertutup yang setiap dindingnya memancar
dan memantul. Radiansi kesetimbangannya deret geometri `E/(1-ρ)`. Itu menguji
energi pantulan ke-**n**, bukan pantulan pertama, dan ia yang memisahkan
integrator tak-bias dari yang memotong kedalaman: pada ρ = 0,8 pantulan kelima
ke atas masih menyumbang sepertiga jawabannya.

`MakeCornellBox()` — kotak tertutup, kameranya di dalam.

#### Kriteria terima yang sudah terpenuhi

| Kriteria | Keadaan |
| --- | --- |
| Uji tungku lulus (albedo 1, langit seragam 1) | ✅ tepat 1,0 |
| Menggandakan `max_depth` tidak menggeser rata-rata | ✅ 32 lawan 64, dalam 2% |
| Energi bounce kedua | ✅ `E/(1-ρ)` pada ρ = 0 / 0,5 / 0,8 |
| Kebocoran cahaya Cornell box | ✅ langit 50× lebih terang tidak menembus |
| Laporan perbandingan sebagai angka, bukan gambar | ✅ `ImageCompare`, RMSE per daerah |

#### Yang belum, dan kenapa

| Kriteria | Kenapa belum |
| --- | --- |
| `SimHeadless --reference-render <level> --spp N` menghasilkan EXR | Butuh penulis EXR dan jembatan dari level SimEngine ke `Scene`; keduanya pekerjaan tersendiri |
| Perbandingan GI runtime lawan acuan | **Butuh GPU**, jadi ia tidak bisa menjadi uji CI. Bentuknya alat, bukan uji |
| Nilai analitik faktor bentuk Cornell box | Yang dipakai `E/(1-ρ)`, yang lebih ketat *dan* lebih mudah dibaca ulang daripada faktor bentuk dinding |
| Satu bias diketahui, terdokumentasi dengan angkanya | Belum ada pembanding runtime, jadi belum ada bias yang bisa diukur |

**Satu bias yang sudah diketahui, dan disebut di sini karena harus:** sampling
BSDF-nya kosinus, bukan menurut lobe-nya. Itu **tidak** membuat hasilnya bias —
pembaginya PDF yang sama — tetapi ia membuat lobe spekular sempit konvergen
sangat lambat. Adegan acuan yang berisi logam licin akan berderau lama, dan
itu batas yang diketahui, bukan yang ditemukan nanti.

#### Dua kelulusan palsu, dan bagaimana keduanya ketahuan

Ditulis di sini karena polanya berulang, bukan karena kejadiannya menarik.

1. **Kamera yang melihat lurus ke bawah** menghasilkan basis NaN, setiap sinar
   meleset, gambarnya menjadi langit seluruhnya — dan uji tungku putih **lulus**
   di sana, karena langit seragam memang jawabannya. Ditemukan uji albedo, tiga
   uji kemudian. Uji tungku putihnya sekarang membuktikan lebih dulu bahwa
   lantainya benar-benar kena.
2. **Sisi mana yang merah ditebak, bukan dibuktikan.** Dengan pandangan ke +z
   dan `up` +y, sumbu kanan layar menunjuk ke dunia **-x**, jadi dinding di
   `x = 0` muncul di sisi kanan gambar. Uji kebocoran warnanya menebak
   terbalik. Sekarang warna dindingnya diperiksa lebih dulu, jadi tebakan yang
   salah gagal di baris yang mengatakan sebabnya.

Keduanya bentuk yang sama: **sebuah uji yang lulus untuk alasan yang salah lebih
buruk daripada tidak ada uji.**

Sengaja dibatasi: tidak ada volume, tidak ada SSS, tidak ada dispersi. Yang
divalidasi adalah GI difus dan spekular kasar — persis yang diaproksimasi
tumpukan SDF/probe/radiance-cache.

#### Bentuk integratornya, dan dari mana ia dicontek

*Ray Tracing in One Weekend* (`/home/arie/SDK/raytracing`, CC0) sudah menuliskan
integrator yang benar dalam bentuk yang paling mudah diaudit. Geometrinya tidak
bisa dipakai — ia tidak punya primitif segitiga sama sekali, hanya bola dan quad
— dan materialnya juga tidak, karena referensi wajib menilai OpenPBR. Yang layak
dicontek justru lapisan di antara keduanya:

- **Estimatornya ditulis eksplisit**, bukan disembunyikan di dalam akumulator:
  `emitted + attenuation × scattering_pdf × L / pdf`. Setiap faktor bisa dicetak
  dan diperiksa satu per satu, dan itulah yang membuat sebuah referensi bisa
  dipercaya. Referensi yang benar tapi tidak bisa dibaca ulang tidak menyelesaikan
  apa pun.
- **Mixture PDF antara PDF cahaya dan PDF BSDF**, satu sampel, bobot tetap.
  Tak-bias, dan jauh lebih sederhana daripada MIS power-heuristic penuh. Bobot
  50/50 milik buku boleh diganti, tapi jangan diganti sebelum ada adegan yang
  membuktikannya perlu.
- **Lobe delta melewati mesin PDF seluruhnya.** Cermin dan kaca menyetel penanda
  "lewati PDF" dan mengembalikan arah tunggalnya; tidak ada PDF yang dinilai dan
  tidak ada pembagian. Pola ini yang menjaga jalur spekular tidak menghasilkan
  pembagian nol — dan di OpenPBR jumlah lobe-nya lebih banyak, jadi polanya
  justru lebih dibutuhkan, bukan kurang.
- **Kamera pinhole + defocus disk, dengan subsampel stratified √spp × √spp.**
  Stratifikasi itu bukan hiasan: pada spp rendah ia yang membedakan derau yang
  turun rapi dari derau yang menggumpal, dan gambar acuan dinilai justru pada
  konvergensinya.

**Yang justru tidak dicontek: kedalaman yang dipotong keras.** Buku itu berhenti
di `max_depth` tanpa Russian roulette, dan itu **bias** — kecil pada 50 pantulan,
tetapi tumbuh justru di adegan paling terang, yaitu adegan yang dipakai menguji
GI. Sebuah renderer yang seluruh gunanya adalah tak-bias tidak boleh memungut
bias demi kesederhanaan. R4 memakai Russian roulette dengan probabilitas dari
throughput, dan `max_depth` disisakan hanya sebagai jaring pengaman.

#### Apa yang berbenturan dengan backend, dan apa yang tidak

Keempat pola di atas hidup di lapis yang berbeda dari intersector, jadi tidak ada
yang bertabrakan: backend menjawab *"kena apa"*, integrator menjawab *"arah mana
yang disampel dan bagaimana bobotnya"*. Keduanya bertemu hanya di hit record.

Satu hal **memang** berbenturan, dan harus dirancang ulang alih-alih disalin:
**sampling cahaya**. Di buku itu PDF cahaya adalah metode pada objek geometri —
sebuah quad menghitung PDF-nya dengan menembak dirinya sendiri. Backend BVH mana
pun, termasuk milik kita di R0, tidak memberikan objek: ia memberikan pasangan
`(geometri, primitif)`. Jadi daftar lampu harus berdiri sendiri di sisi kita,
dengan sampling area dan evaluasi PDF-nya sendiri.

Itu bukan kerugian. Lampu SimEngine bukan geometri emisif seperti di buku itu —
ia `LightComponent` punctual dan directional, ditambah permukaan emisif dan
langit. Lampu delta disampel dengan PDF satu dan satu shadow ray; permukaan
emisif butuh sampling area; langit butuh distribusi importance-nya sendiri.
Ketiganya lebih besar daripada yang buku itu punya, dan tidak satu pun bisa
diangkat apa adanya. Yang bertahan adalah **strukturnya**: campurkan PDF cahaya
dengan PDF BSDF, ambil satu sampel, bagi dengan PDF campurannya.

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
- **Uji tungku (furnace test) lulus**: albedo 1, langit seragam 1, tanpa
  matahari, hasilnya 1 di seluruh gambar dalam toleransi derau. Ini yang menangkap
  energi yang hilang atau berlebih di jalur pantulan, dan `--bench-furnace`
  sudah ada di `SimHeadless` untuk sisi runtime-nya.
- **Menggandakan `max_depth` tidak menggeser rata-rata gambar** di luar derau —
  bukti Russian roulette-nya tak-bias, bukan sekadar ada.

### R5 — Brick sparse & AO bake · ⬜

Membuka yang tersumbat di GI M1. **Jaraknya tidak dihitung di sini** — itu sudah
dilakukan `BakeMeshSdf` di atas OpenVDB, lengkap dengan tanda dan indeks poligon
terdekat. Yang kurang adalah pengemasannya: grid padat hasil bake menjadi brick
sparse yang dikonsumsi clipmap tanpa perubahan format.

AO bake menyusul, dan ia yang memakai `Sim::Raycast` — cosine-weighted hemisphere
per texel, backend apa pun.

**Kriteria terima**
- SDF hasil bake untuk bola satuan cocok dengan `length(p) - r` dalam toleransi
  setengah voxel — menguji rantai bake→brick, bukan OpenVDB-nya.
- Mesh non-manifold (dua segitiga saling menembus) tetap menghasilkan tanda yang
  konsisten, atau ditolak dengan pesan yang jelas — tidak menghasilkan brick
  rusak diam-diam.
- Brick kosong benar-benar tidak memakan tempat; rasio penghematannya dicatat
  untuk satu adegan sungguhan, bukan untuk bola.
- Bake berjalan di `TaskPool`, editor tidak membeku, progres terlihat.
- Brick hasilnya dikonsumsi jalur SDF clipmap yang sudah ada tanpa perubahan
  format.

### R6 — Backend Embree · ❌ **tidak diperlukan, terukur 23 Agustus 2026**

> **Syarat masuknya diperiksa, dan tidak terpenuhi.** Rencana ini menetapkan satu
> pemicu: penelusuran sinar mendominasi waktu render acuan, atau satu gambar
> acuan tidak konvergen dalam semalam. Keduanya diukur di bawah, dan keduanya
> jauh dari terpenuhi. Milestone ini karena itu ditutup sebagai "tidak
> diperlukan" — hasil yang sah, dan memang yang disebut rencana ini sejak awal.

#### Yang diukur — termasuk Embree-nya sendiri

Embree 4.4.1 dibangun dari sumber di `/home/arie/SDK/embree-4.4.1` dengan **opsi
persis seperti tabel "Ongkos dependensi"** di atas, lalu diadu langsung dengan
BVH sendiri pada geometri dan berkas ray yang sama persis.

**Ongkos dependensinya, terukur** (24 inti, Ninja, Release):

| | Terukur | Ambang risiko di rencana ini |
| --- | --- | --- |
| Konfigurasi | **1,0 detik** | — |
| Build | **143 detik** | > 5 menit |

**Perkiraan di rencana ini terlalu pesimistis.** Embree disebut "dependensi
terberat yang akan masuk", dan dengan `EMBREE_TUTORIALS=OFF`,
`EMBREE_MAX_ISA=AVX2`, serta lobe geometri yang tidak dipakai dimatikan, ia
selesai dalam dua setengah menit tanpa TBB. Argumen ongkos terhadap R6 **gugur**.

**Throughput, `linux-clang-release`, satu thread, permukaan bergelombang yang
segitiganya tidak koplanar:**

| Segitiga | | Bangun BVH | Throughput satu thread |
| --- | --- | --- | --- |
| 28.800 | BVH sendiri | < 1 ms | 1,60 juta ray/detik |
| 500.000 | BVH sendiri | 287 ms | 0,97 juta ray/detik |
| 500.000 | **Embree 4.4.1** | **37 ms** | **3,46 juta ray/detik** |

**Embree 3,6× lebih cepat menelusuri dan 7,8× lebih cepat membangun.** Keduanya
menjawab hal yang sama: dari 2.000.000 ray, BVH sendiri melaporkan 1.444.209
kena dan Embree 1.444.216 — tujuh ray berbeda, 0,0004%, seluruhnya di tepi
segitiga tempat presisi float memang menentukan. Itu sekaligus pemeriksaan
kebenaran BVH sendiri terhadap implementasi yang matang, dan ia lulus.

Mesin ini punya 24 inti. Pada penskalaan 80% yang konservatif, BVH sendiri
memberi **13–17 juta ray/detik**, Embree sekitar **50–66 juta**.

#### Apa artinya untuk R4

Satu gambar acuan Sponza 1280×720, 512 spp, tiga pantulan ditambah bayangan NEE
≈ 3,8 miliar ray. Pada 15 juta ray/detik: **sekitar empat menit** dengan BVH
sendiri, sekitar **satu menit** dengan Embree. Pemicunya berbunyi "tidak
konvergen dalam semalam" — yang terukur dua orde magnitudo di sisi yang lain,
pada **kedua** backend.

**Argumen yang bertahan bukan ongkos, melainkan kebutuhan.** Intersector bukan
penghambat R4, dan mempercepatnya 3,6× hanya membuat bagian yang benar-benar
menghambat menjadi porsi yang lebih besar lagi: kembaran CPU dari
`openpbr.slang` — 920 baris, dan naik dari 374 sejak audit spesifikasi
dikerjakan. Menukar empat menit menjadi satu menit tidak menyelesaikan satu pun
masalah yang dimiliki proyek ini hari ini.

Percepatan bangun 7,8× pun tidak menjawab apa-apa yang terlihat: BVH mesh
dibangun asinkron di `TaskPool` lewat `MeshGeometryCache`, dan bentuk whitebox
yang dibangun ulang di editor berukuran ratusan segitiga — mikrodetik di
kedua backend.

#### Apa artinya untuk GI

Tidak ada apa-apa, dan itu bukan hasil yang mengecewakan melainkan pertanyaan
yang salah. `TraceBackendKind` hanya mengenal `Null`, `SdfClipmap`, dan
`RayQuery` — **tidak ada seam CPU di GI sama sekali**, dan memang tidak
dirancang ada. Angkanya menegaskan kenapa: GI real-time menuntut ratusan juta
ray per detik, dan yang tersedia di CPU mesin ini 15 juta. `VK_KHR_ray_query`
terkonfirmasi ada di RTX 2060 di mesin ini lewat `vulkaninfo`, jadi jalur GPU
untuk GI memang terbuka.

#### Embree sebagai *builder* BVH untuk GPU — ditutup juga

Pernah ada rencana terpisah, `PLAN-EMBREE-GI.md`, yang mengusulkan jalur lain:
Embree dipakai **hanya sebagai builder** (`rtcBuildBVH`), hasilnya diratakan ke
tata letak GPU dan ditelusuri **compute shader** sebagai tier untuk GPU tanpa RT
core, dengan DDGI di atasnya.

Berkas itu dihapus, dan bukan karena Embree-nya. Ketiga pilarnya sudah dijawab
lain oleh kode yang benar-benar dibangun:

| Usulan di sana | Yang berlaku sekarang |
| --- | --- |
| Traversal BVH software di compute untuk GPU tanpa RT core | `TraceBackendKind::Sdf` — screen-space HiZ + clipmap SDF. [rencana-implementasi-gi.md](rencana-implementasi-gi.md) menolak BVH software eksplisit: *"5–10× lebih lambat dari sphere tracing di GPU tanpa RT core"* |
| DDGI sebagai algoritma GI | Screen probe + radiance cache. `DDGI` tidak muncul di satu baris kode pun |
| Embree sebagai builder untuk tata letak BVH GPU | Tata letak itu tidak pernah dibangun, dan tidak ada yang memintanya |

Yang dipertahankan dari sana hanya satu kalimatnya yang tetap benar, dan sudah
tertulis di bagian Keputusan pokok berkas ini: **Embree tidak punya interop apa
pun dengan Vulkan.** `RTCScene` tidak bisa menjadi `VkAccelerationStructureKHR`.

#### Kapan ini dibuka kembali

Tiga keadaan, dan hanya tiga:

1. **R4 berjalan dan profilnya menunjukkan intersector > 50% waktunya.** Angka di
   atas mengatakan ini tidak akan terjadi kecuali adegannya berubah orde.
2. **Bake yang menuntut alpha-test dedaunan lewat filter function.** Ini yang
   paling mungkin: BVH sendiri tidak punya callback per-perpotongan, dan
   menambahkannya bukan pekerjaan kecil.
3. **Motion blur natif atau instancing dua tingkat ber-SIMD** menjadi kebutuhan
   nyata, bukan kebutuhan yang diperkirakan.

Isinya kalau dibuka: `EmbreeBackend.cpp` di belakang antarmuka yang sudah berdiri
sejak R0, `SIM_WITH_EMBREE` default **OFF**, Embree `v4.4.1` lewat `FetchContent`.
Antarmukanya sudah siap menerimanya — itu memang gunanya dibangun lebih dulu.

<details>
<summary>Kriteria terima R6, disimpan untuk kalau salah satu dari tiga keadaan itu datang</summary>



**Milestone ini tidak dimulai tanpa angka dari R4.** Syarat masuknya satu:
profil R4 menunjukkan penelusuran sinar mendominasi waktu render acuan, dan
angkanya ditulis di sini sebelum satu baris kode ditambahkan. Kalau BVH R0
ternyata cukup, milestone ini ditutup sebagai "tidak diperlukan" — dan itu hasil
yang sah, bukan kegagalan.

Isinya: `EmbreeBackend.cpp` di belakang antarmuka yang sudah berdiri sejak R0,
`SIM_WITH_EMBREE` (default **OFF** sampai terbukti), Embree `v4.4.1` lewat
`FetchContent` dengan seluruh opsi di tabel "Ongkos dependensi", dan catatan di
[DEPENDENCIES.md](DEPENDENCIES.md).

Yang didapat begitu ia masuk, dan tidak didapat dari BVH sendiri tanpa kerja
besar: BVH ber-SAH yang matang, paket ray ber-SIMD, instancing dua tingkat,
motion blur natif, dan filter function untuk alpha-test dedaunan saat bake.

**Kriteria terima**
- **Angka pembanding dicatat**: waktu render adegan acuan yang sama, backend BVH
  sendiri versus Embree, pada spp yang sama.
- Gambar keduanya **cocok dalam toleransi derau**. Backend yang menghasilkan
  gambar berbeda bukan backend, melainkan renderer kedua.
- `-DSIM_WITH_EMBREE=OFF` tetap sukses dan seluruh test tetap lulus.
- `ldd` pada `SimEditor` tidak menunjukkan `libtbb` — bukti
  `EMBREE_TASKING_SYSTEM=INTERNAL` bekerja.
- **Waktu konfigurasi pertama dan waktu build Embree tercatat di
  DEPENDENCIES.md sebagai angka terukur**, bukan perkiraan.
- Tidak ada satu pun tipe Embree di header publik `Sim::Raycast`.
- Preset asan tetap bisa dibangun.

</details>

---

## Risiko

| Risiko | Tanda awal | Mundur ke |
| --- | --- | --- |
| Waktu build Embree merusak alur kerja | R6 mencatat > 5 menit | Turunkan ke `EMBREE_MAX_ISA=SSE4.2`; kalau masih berat, biarkan `SIM_WITH_EMBREE` OFF — R0 tetap jalan tanpanya |
| **Embree masuk sebelum ada yang mengukurnya** | R6 dimulai tanpa profil R4 di tangan | Tutup R6 sampai angkanya ada. Ini risiko yang sudah terjadi sekali: versi pertama rencana ini menaruh Embree di R0 |
| **BVH sendiri ternyata terlalu lambat untuk R4** | Satu gambar acuan Sponza tidak konvergen dalam semalam | Justru inilah pemicu R6 — bukan kegagalan, melainkan syarat masuknya yang terpenuhi |
| Tasking INTERNAL lebih lambat dari TBB saat commit | Commit scene besar terasa di editor | Ukur dulu; TBB hanya ditambahkan kalau selisihnya terbukti mengganggu, bukan karena asumsi |
| Path tracer referensi jadi proyek sendiri | R4 lewat dua minggu tanpa gambar konvergen | Potong ke Cornell box saja; satu adegan yang benar lebih berguna daripada lima yang setengah jadi |
| Referensi dan runtime tidak pernah cocok karena definisi material berbeda | Selisih besar dan konstan di semua adegan | Samakan BSDF-nya dulu di adegan satu material, sebelum menilai GI-nya |
| Duplikasi geometri CPU memakan memori | Scene besar membengkak | Buffer dibagi, bukan disalin (R1); yang tersisa hanya BVH-nya |

---

## Yang tidak boleh ditunda

- **`Sim::Raycast` tidak boleh menyentuh RHI.** Begitu ia butuh `VkDevice`,
  `SimHeadless` dan seluruh nilai CI-nya hilang.
- **Antarmuka backend-agnostik sejak R0.** Ia yang membuat R6 bisa ditunda tanpa
  utang — dan yang membuatnya bisa dibatalkan sama sekali.
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
- **Memakai PhysX untuk picking dan bake.** Arah sebaliknya, dan sama salahnya.
  PhysX sudah memasak `PxTriangleMesh` dan `PhysicsWorld::Raycast` sudah ada,
  jadi godaannya nyata — tetapi ia menjawab *keadaan simulasi*: bentuk tabrakan,
  yang untuk sebatang pohon adalah sebuah kotak, dan yang untuk hiasan tanpa
  collider tidak ada sama sekali. Yang dibutuhkan picking dan bake adalah
  *geometri yang digambar*. Batas ini sudah tertulis di `PhysicsQuery.h`; ia
  disebut ulang di sini karena inilah jalan pintas yang paling mudah diambil.
