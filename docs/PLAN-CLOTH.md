# Plan Kain Interaktif (C0 → C6)

Solver kain di **Vulkan compute**, dengan algoritma berbasis fase dari NvCloth
sebagai rujukan.

Penomoran **C** supaya tidak bertabrakan dengan E (editor/render), P (fisika),
A (agentic AI), R (Embree), I (gambar), dan M (GI) di [ROADMAP.md](ROADMAP.md).

> Rencana ini **menggantikan** versi sebelumnya yang bersandar pada ekstraksi
> XRTailor/CUDA — disimpan sebagai
> [PLAN-CLOTH-XRTAILOR-LAMA.md](PLAN-CLOTH-XRTAILOR-LAMA.md) karena analisa
> ekstraksinya masih berguna. Alasan pergantiannya, beserta seluruh pengukuran
> yang mendasarinya, ada di [ANALISA-KAIN.md](ANALISA-KAIN.md).

---

## Keputusan pokok

**Kain interaktif adalah fitur baseline.** Baseline mencakup RX 5600 XT (RDNA1,
AMD), yang tidak menjalankan CUDA. Itu mencoret PhysX PBD, `PxDeformableSurface`,
XRTailor, **dan** backend GPU NvCloth sekaligus — keempatnya CUDA.

**Karena itu Vulkan compute.** Ia satu-satunya jalur GPU yang berjalan di AMD,
Intel, dan NVIDIA dengan kode yang sama. Bukan pilihan gaya: ia satu-satunya yang
memenuhi syarat yang sudah ditetapkan.

**Algoritmanya dari NvCloth, pustakanya tidak.** NvCloth tidak punya backend
Vulkan — hanya `cuda/` dan `dx/` — jadi tidak ada yang bisa ditautkan. Yang
diambil adalah **rancangan solvernya**, yang memang layak ditiru: ia solver kain
PhysX 3 yang matang, sudah memecahkan self-collision, dan masih dikapalkan O3DE
sampai hari ini lewat `NvClothCreateFactoryCPU()`.

### Batas pemakaian NvCloth, dan kenapa ia ditulis di sini

Lisensinya **bukan BSD**. Ia "Nvidia Source Code License (1-Way Commercial)":
permisif, mengizinkan karya turunan dengan syarat berbeda, tetapi menuntut
**salinan lisensi ikut didistribusikan** dan **pemberitahuan atribusi
dipertahankan** bila karyanya disertakan. Ada pula klausul yang menghentikan izin
bila pemakainya menuntut paten NVIDIA.

Konsekuensinya untuk rencana ini:

- **Yang diambil adalah algoritmanya** — pembagian kendala menjadi fase dan
  urutan penyelesaiannya — dibaca, dipahami, lalu ditulis ulang di sini. Gagasan
  tidak dilindungi hak cipta; ungkapannya iya.
- **Tidak ada berkas NvCloth yang disalin ke pohon ini.** Kalau nanti ada yang
  benar-benar diadaptasi baris demi baris, berkas lisensinya wajib ikut dan
  atribusinya wajib dipertahankan — dan itu keputusan sadar yang dicatat di
  commit, bukan sesuatu yang terjadi karena seseorang menempel kode.

Ini bukan nasihat hukum. Ini batas kerja yang dipilih supaya pertanyaannya tidak
pernah perlu diajukan.

---

## Kenapa berbasis fase, bukan Jacobi

Kendala kain berbagi partikel: dua kendala yang menyentuh partikel yang sama
tidak boleh diproyeksikan bersamaan tanpa balapan. Ada dua jawaban, dan NvCloth
memilih yang kedua:

**Jacobi** — semua kendala dihitung dari posisi lama, hasilnya dirata-ratakan.
Bebas balapan, satu dispatch, tetapi konvergensinya lebih lambat: kain terasa
lebih kenyal pada jumlah iterasi yang sama.

**Fase (pewarnaan)** — kendala dikelompokkan sehingga **tidak ada dua kendala di
dalam satu fase yang berbagi partikel**. Tiap fase diproyeksikan Gauss-Seidel
penuh, satu dispatch per fase, barrier di antaranya. Konvergensi jauh lebih baik
per iterasi.

NvCloth memakai fase bertipe `eVERTICAL`, `eHORIZONTAL`, `eSHEARING`, dan
`eBENDING` — pengelompokan yang sekaligus punya arti fisik, bukan sekadar
pewarnaan graf sembarang. Itu yang ditiru.

**Dan pengelompokannya dikerjakan saat memasak aset, bukan saat memuat level.**
Pola yang sama dengan bake SDF di `Sim::Volume` dan cooking convex di
[PLAN-PHYSICS.md](PLAN-PHYSICS.md): hasilnya selalu sama, jadi menghitungnya tiap
kali level dimuat adalah detik yang dibayar berulang untuk jawaban yang identik.

---

## Anggaran, dan ukuran yang muat di dalamnya

Dari [ANALISA-KAIN.md](ANALISA-KAIN.md):

| Anggaran kain | Partikel | Kira-kira |
| --- | --- | --- |
| 1,5 ms | 4.257 | grid 65×65 |
| 2,0 ms | 5.676 | grid 75×75 |
| 3,0 ms | 8.514 | grid 92×92 |

Frame 60 fps hanya 16,67 ms dan GI sudah mengambil 3,0 ms di baseline. **Target
C4: satu busana ~6.000 partikel di bawah 2 ms pada baseline.**

Angka acuan CPU yang sudah diukur di mesin ini — 2,25 ms untuk 5.625 partikel,
satu thread, 4 iterasi, tanpa SIMD — bukan pesaing melainkan **pagar**: solver
GPU yang lebih lambat daripada itu tidak layak dipakai.

---

## Arsitektur

```
Code/Cloth/
  include/Sim/Cloth/
      ClothTypes.h      partikel, kendala, fase — tanpa tipe Vulkan
      Fabric.h          data kendala termasak, hasil pengondisian aset
      ClothSolver.h     antarmuka solver; backend dipilih saat dibuat
  src/
      FabricCook.cpp    mesh → fase, dijalankan importir
      SolverCpu.cpp     acuan kebenaran, selalu ada
      SolverCompute.cpp backend Vulkan
Shaders/
      cloth_integrate.comp.slang
      cloth_solve_phase.comp.slang
      cloth_normals.comp.slang
```

**Dua backend di balik satu antarmuka**, pola yang sudah terbukti dua kali di
pohon ini — `Sim::ImageIO` berganti pustaka dua kali tanpa satu titik panggil
berubah, dan `Sim::Volume` punya acuan CPU untuk raymarch GPU-nya.

**Acuan CPU bukan cadangan melainkan alat uji.** Ia yang membuat pertanyaan
"apakah solver GPU-nya benar" bisa dijawab angka, bukan tangkapan layar. Aturan
yang sama dipakai `VolumeRaymarch` terhadap `volume_raymarch.frag.slang`.

**`Sim::Cloth` tidak bergantung pada `Sim::Render`.** Ia menghasilkan posisi
vertex; yang menggambarnya pemanggilnya. Sama seperti `Sim::Physics`.

### Yang membuat Vulkan lebih sederhana daripada CUDA di sini

XRTailor harus memanggil `cudaGraphicsGLRegisterBuffer` dan memetakan sumber daya
bolak-balik — lima titik panggil yang seluruhnya harus ditulis ulang untuk
Vulkan. Di sini masalah itu **tidak ada**: buffer yang ditulis compute shader
*adalah* buffer yang dibaca vertex shader. Satu `VkBuffer`, dua bit usage, satu
barrier.

---

## Milestone

### C0 — `DeviceBuffer` di `Sim::RHI` · ⬜

Buffer device-local dengan staging upload dan readback. `DynamicBuffer` yang ada
sekarang host-visible dan dipetakan permanen — memori yang salah untuk data yang
dibaca-tulis puluhan ribu kali per frame.

**Ini utang yang sudah tercatat**, bukan ongkos kain: komentar `Buffer.h` sendiri
menyebut "buffer device-local dengan staging di E8", dan mesh statis E8
membutuhkannya lebih dulu.

**Kriteria terima**
- Unggah, baca balik, bandingkan — byte yang keluar sama dengan yang masuk.
- Dipakai dari dua frame berturut-turut tanpa hazard, diverifikasi lapisan
  validasi.

### C1 — Dispatch compute pertama · ⬜

Pipeline compute, descriptor, dan barrier. **Tidak butuh perubahan build maupun
device** — sudah diverifikasi: `slangc` mengompilasi `.comp.slang` dengan flag
yang sama persis seperti shader lain, dan family antrian grafis yang dipilih
`Device.cpp` juga membawa `VK_QUEUE_COMPUTE_BIT`.

Polanya mengikuti pass yang sudah ada (`VolumePass`, `SkyAtmosphere`): satu
berkas memiliki pipeline-nya sendiri, bukan lapisan RHI baru.

**Kriteria terima**
- Satu shader yang mengalikan buffer dengan skalar menghasilkan angka yang
  dihitung CPU, dibandingkan elemen per elemen.

### C2 — Memasak fabric · ⬜

Mesh → daftar kendala → fase. Dijalankan importir, disimpan di samping asetnya.

**Kriteria terima**
- **Tidak ada dua kendala di dalam satu fase yang berbagi partikel** — disisir
  seluruhnya, bukan disampel. Inilah invarian yang membuat solver GPU-nya bebas
  balapan, dan satu pelanggaran saja menghasilkan kain yang bergetar acak di
  mesin tertentu saja.
- Memasak dua kali menghasilkan byte yang sama.

### C3 — Solver CPU sebagai acuan · ⬜

XPBD berbasis fase, satu thread, ditulis untuk dibaca. Bukan untuk performa.

**Kriteria terima**
- Kain digantung dua sudut mengendap simetris; simpangan kiri-kanan di bawah
  toleransi yang ditulis.
- Kain jatuh bebas mempertahankan panjang rusuk dalam 1% setelah 600 langkah.

### C4 — Solver compute · ⬜

Integrasi, penyelesaian per fase, penghitungan normal — tiga shader.

**Kriteria terima**
- **Hasilnya cocok dengan acuan CPU** dalam toleransi yang ditulis, pada adegan
  dan jumlah iterasi yang sama.
- 6.000 partikel di bawah **2 ms** pada baseline. Diukur, dicatat, dan
  dibandingkan dengan pagar CPU 2,25 ms.

### C5 — Tabrakan · ⬜

Terhadap kapsul dan sphere dari collider yang sudah ada, ditambah self-collision.

Bentuk tabrakannya diambil dari `Sim::Physics` — P2 sudah menyediakan
`OverlapSphere` dan `SweepSphere`, dan collider karakter sudah ada di sana. Kain
tidak membangun dunia keduanya.

**Kriteria terima**
- Kain yang dijatuhkan ke atas sphere tidak menembusnya pada langkah mana pun.
- Self-collision menahan lipatan: kain yang dilipat tidak melewati dirinya
  sendiri.

### C6 — Komponen, aset, dan anggaran · ⬜

`ClothComponent` lewat refleksi, aset `.simcloth`, dan degradasi saat anggaran
terlampaui.

**Kriteria terima**
- Anggaran ditegakkan: melewatinya menurunkan iterasi lalu mematikan kain, dan
  **mengatakannya di log** alih-alih diam-diam melambat.
- Prefab kain bisa dijatuhkan ke level seperti Physics Box.

---

## Risiko

**Compute belum pernah dipakai di mesin ini.** Empat puluh tujuh shader,
semuanya vertex atau fragment — GI pun fragment. C1 sengaja dibuat sekecil
mungkin supaya kegagalan di sana menunjuk infrastruktur, bukan solver.

**Self-collision adalah bagian tersulit**, dan itu sebabnya ia di C5, bukan C4.
NvCloth memakai grid spasial; menirunya di compute menuntut sorting atau atomic —
keduanya jauh lebih rumit daripada kendala jarak. Kalau C5 meleset, kain tanpa
self-collision masih berguna untuk bendera dan jubah longgar.

**Baseline tidak ada di meja ini.** Pengukuran dilakukan di mesin pengembangan;
angka RX 5600 XT harus datang dari perangkat sungguhan sebelum C4 disebut
selesai. Menandainya lulus dengan angka RTX 2060 berarti menunda kejutan, bukan
menghindarinya.

---

## Yang tidak berubah

**Jalur bake Alembic tetap milik XRTailor apa adanya**, dijalankan sebagai alat
luar untuk kain yang tidak bereaksi — bendera, gorden, busana latar. Ia tidak
berjalan di dalam mesin, jadi ia tidak bertabrakan dengan apa pun di atas, dan ia
satu-satunya yang memberi kualitas Quality mode.

**Perkiraan articulation dari P5 tetap tersedia** sebagai jaring pengaman. Jubah
sebagai strip bertaut jalan hari ini, di CPU, di mana pun — dan untuk jubah kecil
di kejauhan tidak ada yang bisa membedakannya.
