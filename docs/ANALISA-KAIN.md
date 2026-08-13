# Analisa: PBD PhysX melawan XRTailor GPU

Keputusan yang ditunda di [PLAN-PHYSICS.md](PLAN-PHYSICS.md) sampai seluruh
milestone PhysX CPU selesai. P0–P6 kini selesai, jadi ini saatnya.

**Ringkasnya: perbandingan yang direncanakan sudah tidak berlaku.** PBD kain
usang di versi yang kita pakai, dan sesudah kain ditetapkan sebagai fitur
**baseline**, ketiga kandidat CUDA gugur sekaligus. Penggantinya ada di bagian
[Alternatif](#alternatif-yang-benar-benar-bisa-jalan-di-baseline).

---

## Temuan yang membalik premisnya

### PBD kain sudah usang di versi yang kita pakai

`Third-Party/PhysX/include/PxParticleBuffer.h`, PhysX 5.6.1 — versi yang sudah
di-vendor dan dipakai P0–P6:

```
\deprecated Particle-cloth, -rigids, -attachments and -volumes have been deprecated.
```

Enam belas penanda `PX_DEPRECATED` di berkas itu saja. `PxParticleClothDesc`,
`PxParticleClothPreProcessor`, dan `PxParticleClothCooker` semuanya termasuk.

Ini bukan peringatan gaya penulisan. Membangun fitur di atas API yang penulisnya
sendiri sudah menandainya untuk dihapus berarti menjadwalkan penulisan ulang pada
waktu yang ditentukan orang lain.

**Jadi "PBD PhysX untuk kain" gugur sebelum dibandingkan.** Bukan karena kalah
cepat atau kalah bagus — karena ia tidak lagi ditawarkan.

### Penggantinya `PxDeformableSurface`, dan ia menuntut CUDA yang sama

```cpp
virtual PxDeformableSurface* createDeformableSurface(PxCudaContextManager&) = 0;
```

Referensi, bukan pointer — sama seperti PBD. Tidak ada jalur CPU.

Jadi pilihan PhysX untuk kain berubah dari "PBD" menjadi "deformable surface",
dan syarat perangkat kerasnya persis sama.

### Dan syarat itu memotong baseline sendiri

`docs/rencana-implementasi-gi.md` baris 3–4:

```
Baseline:  GTX 1660 Super / RX 5600 XT (tanpa RT core, 6 GB)
Tier atas: RTX 2060 (30 RT core, 6 GB)
```

**RX 5600 XT adalah RDNA1 — AMD.** Tidak ada versi driver yang membuatnya
menjalankan CUDA.

Ini berlaku untuk **ketiga** kandidat: PBD, deformable surface, dan XRTailor —
seluruhnya CUDA. Di separuh baseline yang sudah tertulis, ketiganya sama-sama
tidak ada.

---

## Yang sebenarnya harus diputuskan lebih dulu

Bukan "solver mana", melainkan:

> **Kain interaktif itu fitur baseline atau fitur tier atas?**

Jawabannya menentukan segalanya sesudahnya, dan hanya ada dua:

**Kalau baseline** — kain harus jalan di RX 5600 XT — maka ketiga kandidat gugur
sekaligus. Yang dibutuhkan solver di atas compute shader portabel (Vulkan
compute) atau CPU, dan itu proyek yang berbeda sama sekali dari keduanya. Tidak
ada gunanya membandingkan dua solver CUDA untuk menjawab pertanyaan ini.

**Kalau tier atas** — kain adalah kemewahan RTX 2060+ yang dimatikan di bawahnya,
seperti ray query di rencana GI — maka perbandingannya berlanjut, tetapi antara
**`PxDeformableSurface` dan XRTailor**, bukan PBD dan XRTailor.

---

## Perbandingan yang tersisa, bila kain adalah fitur tier atas

| | `PxDeformableSurface` | XRTailor Swift |
| --- | --- | --- |
| Status API | Aktif, baru di PhysX 5.4+ | Stabil, v1.9.0 |
| Sudah di pohon ini | **Ya** — header ter-vendor, P0–P6 memakainya | Tidak |
| Ongkos integrasi | Bangun ulang PhysX dengan CUDA (P8) | Ekstraksi 14 rb baris + 3,5 rb baris lem |
| Angka terukur di mesin ini | **Belum ada** | 19,67 ms untuk 55.824 partikel di RTX 2060 |
| Perawatan | Satu solver, bersama rigid body | Solver kedua, seumur hidup proyek |
| Cakupan | Kain, soft body FEM, cairan, inflatable | Kain saja |
| Mode kualitas untuk bake | Tidak ada | Ada (4–19 fps, dipakai offline) |

### Batas ekstraksi XRTailor — diverifikasi ulang, bukan disalin

Diukur langsung dari `/home/arie/SDK/xrtailor-1.9.0`, dan **setiap angka di
PLAN-CLOTH.md cocok persis**:

| Bagian | Berkas | Baris | `Global::` |
| --- | --- | --- | --- |
| `physics/` | 95 | 13.712 | **3** |
| `memory/` | 11 | 561 | **0** |
| `pipeline/` | 26 | 3.466 | 267 |
| `runtime/` | 79 | 10.190 | 111 |

`exit()` muncul 36 kali di seluruh pohon; tiga di antaranya di
`memory/MemoryPool.cu` baris 24, 35, 45 — persis `NODE_POOL_RUN_OUT`,
`EDGE_POOL_RUN_OUT`, `FACE_POOL_RUN_OUT` yang disebut rencananya. Interop
CUDA–OpenGL memang terkurung di dua berkas (`PhysicsMesh.cu`/`.cuh`).

Rencana C bukan tebakan: ia sudah memeriksa sumbernya. Itu memberi bobot pada
sisi XRTailor yang tidak dimiliki `PxDeformableSurface` di sini — kita tahu
persis apa yang akan kita masukkan.

### Yang tidak kita ketahui, dan itu justru yang menentukan

**Tidak ada satu pun angka `PxDeformableSurface` di mesin ini.** Build PhysX yang
di-vendor CPU-only, jadi ia belum pernah dijalankan.

XRTailor punya 19,67 ms untuk 55.824 partikel di RTX 2060. Tanpa pembanding,
angka itu tidak bisa dipakai memilih — ia hanya membuktikan XRTailor cukup cepat,
bukan bahwa ia lebih cepat.

---

## Keputusan: kain interaktif adalah fitur **baseline**

Ditetapkan. Konsekuensinya langsung dan keras: **ketiga kandidat gugur
sekaligus.** PBD usang dan CUDA; `PxDeformableSurface` CUDA; XRTailor CUDA. RX
5600 XT tidak menjalankan satu pun di antaranya, dan ia ada di baris pertama
rencana GI.

PLAN-CLOTH.md karena itu **tidak bisa dijalankan seperti tertulis** — seluruh
premisnya bersandar pada solver CUDA.

---

## Berapa besar kain yang sebenarnya muat

Sebelum mencari penggantinya, ukurannya harus jelas — karena ukuran itulah yang
menentukan solver mana yang masuk akal, dan angka yang selama ini dipakai
(55.824 partikel) berasal dari adegan yang **tidak** harus berbagi frame dengan
apa pun.

Dari pengukuran XRTailor sendiri: 19,67 ms untuk 55.824 partikel di RTX 2060 =
**0,352 µs per partikel per frame**.

| Anggaran kain | Partikel yang muat | Kira-kira |
| --- | --- | --- |
| 1,5 ms | 4.257 | grid 65×65 |
| 2,0 ms | 5.676 | grid 75×75 |
| 3,0 ms | 8.514 | grid 92×92 |

Frame 60 fps hanya 16,67 ms, dan GI sudah mengambil 3,0 ms di baseline. Jadi
anggaran kain yang jujur adalah **1,5–3 ms**, bukan 19,67.

**Ini membalik alasan memilih GPU.** Rencana C memilih solver GPU karena adegan
55 ribu partikel menuntutnya. Adegan yang sebenarnya muat sepuluh kali lebih
kecil — dan pada ukuran itu, CPU kembali masuk hitungan.

### Diukur, bukan diduga

Tolok ukur XPBD kasar di mesin ini (`clang -O2`, kendala jarak struktural +
geser, gaya Jacobi):

| Partikel | Kendala | Iterasi | Waktu (1 thread) |
| --- | --- | --- | --- |
| 4.225 | 16.512 | 10 | 3,61 ms |
| 5.625 | 22.052 | 10 | 5,18 ms |
| 5.625 | 22.052 | 6 | 3,00 ms |
| 5.625 | 22.052 | 4 | **2,25 ms** |
| 8.464 | 33.306 | 10 | 6,37 ms |

Sekitar **23 ns per proyeksi kendala**, satu thread, kode skalar biasa tanpa
SIMD. Iterasi berskala linear, seperti yang diharapkan.

Angka paralel dari tolok ukur yang sama **tidak dipakai di sini**: ia membuat
thread baru tiap iterasi — dua ribu kali per frame — sehingga overhead-nya
menutupi speedup apa pun (4 thread hanya 1,27× lebih cepat). Implementasi
sungguhan memakai `TaskPool` yang sudah ada, dan angka itu harus diukur ulang di
sana alih-alih ditebak dari tolok ukur yang cacat di titik itu.

Yang bisa disimpulkan dengan jujur: **satu thread pun sudah mendekati anggaran**
pada 4–6 iterasi, dan itu sebelum SIMD maupun pembagian kerja.

---

## Alternatif yang benar-benar bisa jalan di baseline

Diurutkan dari yang paling murah.

### A. XPBD di CPU, di dalam `Sim::Cloth` — **usulan utama**

Portabel **karena konstruksinya**, bukan karena diuji di banyak GPU: tidak ada
GPU yang terlibat. Ia jalan di RX 5600 XT, di iGPU Intel, dan di server dedicated
yang tidak punya perangkat grafis sama sekali.

Yang sudah ada dan langsung dipakai: `TaskPool` untuk pembagian kerja, langkah
tetap dan akumulator dari `Sim::Physics`, serta `PhysicsWorld::OverlapSphere` dan
`SweepSphere` dari P2 untuk tabrakan terhadap collider yang sudah ada.

Yang perlu ditulis: solver XPBD itu sendiri — kendala jarak, bending, tabrakan
diri, dan penambatan ke mesh ber-skin. Ini pekerjaan nyata, tetapi ia **tidak
menyeret infrastruktur baru**.

### B. Compute shader Vulkan — bila CPU terbukti tidak cukup

Portabel lintas vendor (AMD, Intel, NVIDIA sama-sama menjalankan Vulkan compute),
jadi ia memenuhi baseline dengan cara yang CUDA tidak bisa.

**Tetapi mesin ini belum punya compute pipeline sama sekali.** Empat puluh tujuh
shader di `Shaders/`, seluruhnya vertex atau fragment — GI pun dikerjakan di
fragment shader. Menempuh jalur ini berarti membangun dukungan compute di
`Sim::RHI` lebih dulu.

Itu investasi yang **terbayar di luar kain**: GI, partikel, dan GPU skinning
sama-sama menginginkannya. Tapi ia harus dihitung sebagai bagian dari ongkos
kain, bukan diasumsikan sudah ada.

### C. Ping-pong tekstur di fragment shader — jalan tengah

Posisi partikel disimpan di tekstur RGBA32F, kendala dijalankan sebagai tap
tetangga tetap. Untuk kain bergrid teratur ini bekerja rapi, dan **cocok persis
dengan gaya mesin yang sudah ada** — GI sudah multi-pass fragment.

Tanpa infrastruktur baru sama sekali. Lebih canggung daripada compute untuk
tabrakan diri, tetapi untuk kain grid ia jujur bersaing.

### D. Perkiraan dengan articulation — sudah ada hari ini

P5 sudah memberi rantai articulation yang kaku dan murah. Sebuah jubah sebagai
strip bertaut adalah perkiraan kasar, tetapi ia jalan **sekarang**, di CPU, di
mana pun — dan untuk jubah kecil di kejauhan, tidak ada yang bisa membedakannya.

Nol baris baru.

### E. Bake dan putar ulang — untuk yang tidak interaktif

XRTailor tetap dipakai apa adanya sebagai **alat luar**, menghasilkan cache
Alembic. Bendera, gorden, dan busana latar tidak bereaksi terhadap apa pun, jadi
mereka tidak menuntut solver runtime sama sekali.

Ini tidak pernah bertabrakan dengan pilihan mana pun di atas, karena ia tidak
berjalan di dalam mesin. Dan ia satu-satunya jalur yang memberi kualitas Quality
mode.

---

## Rekomendasi

**Kerjakan A, sediakan tempat untuk B atau C, pakai E sekarang juga.**

1. **`Sim::Cloth` dengan solver XPBD CPU** sebagai isi pertamanya. Ia memenuhi
   baseline karena konstruksinya, dan pengukuran di atas menunjukkan ukuran yang
   memang muat di anggaran sudah mendekati jangkauan satu thread sebelum SIMD
   maupun `TaskPool` ikut dihitung.

2. **Seam-nya dirancang berbackend sejak awal**, persis pola `Sim::ImageIO` yang
   sudah berganti pustaka dua kali tanpa satu pun titik panggil berubah. Kalau
   CPU ternyata tidak cukup, backend compute (B) atau ping-pong (C) masuk di
   belakang antarmuka yang sama — dan yang sudah menulis level tidak perlu tahu.

3. **E dipakai mulai sekarang** untuk kain yang tidak bereaksi. Ia tidak menunggu
   apa pun dan tidak bertabrakan dengan apa pun.

4. **D adalah jaring pengaman**, bukan tujuan: kalau kain runtime tertunda, jubah
   articulation sudah bisa dipakai hari ini.

**Yang harus diukur lebih dulu, sebelum menulis solvernya:** ulangi tolok ukur di
atas memakai `TaskPool` alih-alih thread sekali pakai. Itu satu sore kerja, dan ia
menentukan apakah anggaran 2 ms memuat 5.600 partikel atau hanya 2.000 —
perbedaan antara satu jubah penuh dan satu selendang.

---

## Keputusan akhir: opsi B, algoritma dari NvCloth

Ditetapkan. **Vulkan compute**, dengan solver berbasis fase NvCloth sebagai
rujukan algoritma. Rencananya di [PLAN-CLOTH.md](PLAN-CLOTH.md).

### NvCloth: masih relevan, tapi tidak lagi hidup

Diperiksa langsung, bukan diingat:

| | Temuan |
| --- | --- |
| Backend | `sse2/`, `avx/`, `neon/`, `scalar/`, `cuda/`, `dx/`, `NvSimd/` — **tidak ada Vulkan** |
| Commit terakhir | **10 Januari 2024**, isinya *"Remove more unused packman settings"* |
| Pekerjaan substantif terakhir | 2020–2021. Total 29 commit seumur proyek |
| Versi | 1.1.6, dinyatakan cocok dengan PxShared milik **PhysX 4.0** |
| Dipakai siapa | **O3DE** (HEAD 2026-07-21) lewat `NvClothCreateFactoryCPU()` |

Dua hal membuatnya tidak ditautkan:

**Tidak ada backend Vulkan.** CUDA gugur di baseline; DX11 hanya Windows
sementara mesin ini Vulkan di Linux. Jadi "pakai pustakanya" tidak mungkin
diartikan secara harfiah — yang bisa diambil hanya rancangannya.

**Dan tipe dasarnya bertabrakan dengan PhysX 5.** NvCloth 1.1.6 menyasar PxShared
milik PhysX 4.0; di PhysX 5 tidak ada PxShared terpisah sama sekali — ia sudah
menyatu ke `foundation/`. Keduanya mendefinisikan `physx::PxVec3` dan
`physx::PxAllocatorCallback`. Menautkan keduanya di satu biner adalah risiko ODR
yang nyata, bukan teoretis.

### Yang tetap diambil darinya

**Pembagian kendala menjadi fase** — `eVERTICAL`, `eHORIZONTAL`, `eSHEARING`,
`eBENDING` — supaya tidak ada dua kendala di dalam satu fase yang berbagi
partikel. Itu menjawab pertanyaan paralelisasi yang tertinggal di analisa
sebelumnya: bukan Jacobi, melainkan Gauss-Seidel per fase, satu dispatch per
fase. Dan **fase dihitung saat memasak aset**, bukan saat memuat level.

Solver CPU-nya (`SwSolver`, `SwSelfCollision`) tetap rujukan terbaik untuk bagian
tersulitnya, yaitu self-collision.

### Koreksi: lisensinya bukan BSD

Di analisa lisan sebelumnya saya menyebutnya BSD-3 **dari ingatan, dan itu
salah**. Ia "Nvidia Source Code License (1-Way Commercial)": permisif dan
mengizinkan karya turunan, tetapi menuntut salinan lisensi ikut didistribusikan
dan atribusi dipertahankan bila karyanya disertakan, dengan klausul yang
menghentikan izin bila pemakainya menuntut paten NVIDIA.

Praktisnya: **menulis ulang algoritmanya bersih; menyalin berkasnya membawa
kewajiban.** Rencana C memilih yang pertama, dan menuliskan batas itu supaya
pertanyaannya tidak pernah perlu diajukan.

---

## Yang tidak berubah dari keputusan ini

`Sim::Cloth` sebagai **nama modul dan bentuk API** tetap persis seperti yang
direncanakan. Yang berubah adalah isinya, bukan seamnya — dan itu justru bukti
seam-nya benar: keputusan sebesar "ganti seluruh solver dan seluruh vendor GPU"
tidak menyentuh satu pun titik panggil.

**Jalur bake Alembic tetap milik XRTailor apa adanya**, dijalankan sebagai alat
luar. Ia tidak pernah bertabrakan dengan solver runtime mana pun, karena ia tidak
berjalan di dalam mesin — dan ia satu-satunya yang memberi kualitas Quality mode.

**Dan XRTailor tidak sia-sia dibaca.** Batas ekstraksinya sudah terpetakan, dan
`physics/`-nya adalah rujukan XPBD yang bagus untuk ditiru algoritmanya — tanpa
harus menyeret CUDA, `exit()` di 36 tempat, dan state global bersamanya.
