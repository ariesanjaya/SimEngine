# Rencana Implementasi Global Illumination

**Baseline:** GTX 1660 Super / RX 5600 XT (tanpa RT core, 6 GB)
**Tier atas:** RTX 2060 (30 RT core, 6 GB)
**Target:** 1080p 60 fps, anggaran GI ≤ 3,0 ms (baseline) / ≤ 3,5 ms (tier atas)

---

## Prinsip arsitektur

Satu keputusan yang menentukan seluruh rencana ini:

> **Backend tracing adalah plugin. Sisa sistem GI tidak boleh tahu ray-nya ditembak ke apa.**

```
interface ITraceBackend
{
    TraceResult trace(float3 origin, float3 dir, float tMax);
}
```

Implementasi:
- `SdfTraceBackend`   → screen-space HiZ + global SDF clipmap  (semua GPU)
- `RayQueryBackend`   → DXR 1.1 / VK_KHR_ray_query             (RTX 2060+)

Semua sistem di atasnya — screen probe, hash grid, denoiser, integrasi shading — identik di kedua tier. Kalau ada satu saja pass yang bercabang berdasarkan tier di luar backend ini, desainnya bocor dan biaya perawatannya akan berlipat.

---

## Anggaran

### Waktu per frame (1080p, 60 fps)

| Pass | Baseline | RTX 2060 |
|---|---|---|
| Update SDF clipmap / BVH refit | 0,4 ms | 0,3 ms |
| Screen probe trace | 1,4 ms | 1,2 ms |
| Hash grid update + resolve | 0,4 ms | 0,4 ms |
| Denoise (temporal + a-trous) | 0,6 ms | 0,6 ms |
| Upsample + integrate | 0,2 ms | 0,2 ms |
| **Total** | **3,0 ms** | **2,7 ms** + ray tambahan |

Sisa anggaran di RTX 2060 dipakai untuk menaikkan jumlah ray, bukan menambah pass baru.

### VRAM

| Alokasi | Baseline | RTX 2060 |
|---|---|---|
| Global SDF clipmap (3× 128³ R8) | 6 MB | 6 MB (tetap dipakai untuk fallback jauh) |
| Screen probe atlas + history | 20 MB | 20 MB |
| Hash grid radiance (2²⁰ × 16 B) | 16 MB | 16 MB |
| Radiance history ½ res RGBA16F | 8 MB | 8 MB |
| **BVH** | — | **150–400 MB** |
| **Total** | **~50 MB** | **~250–450 MB** |

⚠️ **BVH adalah risiko terbesar tier atas.** RTX 2060 juga hanya 6 GB. Rencanakan proxy-BVH (LOD kasar untuk objek jauh) sejak awal, bukan sebagai optimasi belakangan.

---

## Milestone

Tiap milestone punya kriteria selesai yang bisa diuji. Jangan lanjut sebelum kriterianya lulus — GI adalah sistem yang error-nya menumpuk diam-diam.

### M0 — Fondasi (±1 minggu)
- Interface `ITraceBackend` + stub yang selalu mengembalikan miss
- Debug view: albedo, normal, irradiance mentah, ray count, jumlah langkah march
- Counter profiling terpisah per pass (bukan satu angka "GI")

**Selesai kalau:** bisa menampilkan heatmap jumlah langkah SDF per pixel. Ini alat diagnostik yang paling sering kamu pakai selama 3 bulan ke depan.

### M1 — Global SDF clipmap (±3 minggu) ✅

- ✅ Bake SDF per-mesh offline lewat OpenVDB → `sim::SdfGrid` padat per mesh, dikomposit lewat `BakedSceneField`. Opsional: tanpa OpenVDB, clipmap mundur ke `BoxSceneField`. Lihat [DEPENDENCIES.md](DEPENDENCIES.md)
- ✅ `MeshSdfBakery` di `Sim::Assets`: bake di `TaskPool`, cache `.simsdf` berkunci isi berkas, hasilnya diserahkan ke renderer lewat `IViewportRenderer::SetMeshDistanceField`
- ✅ 3 kaskade 128³ R8_UNORM, voxel 10 cm / 40 cm / 1,6 m → jangkauan ±102 m
- ✅ Toroidal scroll: hanya irisan tepi yang ditulis ulang saat kamera bergerak
- ✅ Komposit objek statis ke kaskade, di CPU maupun di compute (G4)

**Selesai:** biaya pembaruan 0,033 ms saat kamera mengorbit — `cpu-sdf` 0,009 ms
ditambah `sdf-fill` 0,024 ms — jauh di bawah anggaran 0,5 ms. Penelusuran sphere
dari kamera, dengan lapis screen-space dimatikan (`--bench-no-screen-trace`),
memperlihatkan siluet Sponza: pilar di kedua sisi, serambi yang menjauh,
lengkungan langit-langit. Korelasi peringkatnya terhadap depth buffer raster
0,79 — dibatasi voxel 12,7 cm terhadap geometri raster, dan oleh tangkapan
8-bit yang menjepit seluruh rentang 2–30 m ke sekitar tujuh puluh tingkat.

**Yang paling menjelaskan bedanya**, isi kaskade terhalus pada adegan yang sama:

| | kotak batas | dari segitiga |
|---|---:|---:|
| Voxel "jauh di dalam benda" | 84,4% | 4,4% |
| Voxel di dalam pita | 12,5% | 43,7% |
| Voxel ruang bebas | 3,1% | 51,9% |

Angka pertama itu yang dulu membuat setiap sinar probe mengenai sesuatu di jarak
nol: kotak batas Sponza memuat seluruh gedung beserta udaranya.

**Dua hal yang ditemukan sambil mengukurnya**, keduanya tercatat di komentar
kodenya:

- **Sisi voxel diperbesar sampai muat, bukan mesh-nya ditolak.** Sponza pada
  voxel 10 cm adalah 17,3 juta voxel, di atas anggaran baker. Menolak berarti
  gedung itu kembali menjadi kotak pejal — persis keadaan yang M1 mengakhiri.
- **Pita sepadan dengan bendanya, bukan jumlah voxel tetap.** Di luar pita nilai
  medan jenuh, jadi pita adalah jangkauan langkah sphere tracing. Pita 4 voxel
  menjawab 0,45 m di posisi kamera acuan; pita 16 voxel menjawab 1,58 m, yaitu
  jarak sebenarnya, dengan biaya bake yang sama. Tetapi 16 voxel tetap pada mesh
  sebesar kursi berarti grid lima kali lebih lebar daripada kursinya di tiap
  sumbu, jadi yang dipakai pecahan sisi terpanjangnya.

### M2 — Lapis screen-space (±1 minggu)
- HiZ march di depth buffer, maks 16 langkah
- Fallback berjenjang: HiZ miss → global SDF → sky cubemap

**Selesai kalau:** `SdfTraceBackend` lengkap dan lulus uji ray tunggal terhadap referensi CPU.

### M3 — Screen probe (±3 minggu)
- Tile 16×16 px, oktahedral 4×4 = 16 ray/probe/frame
- Placement: snap ke permukaan via G-buffer depth, dengan probe tambahan untuk pixel disocclusion
- Jitter arah per frame (sekuens low-discrepancy), akumulasi 8–16 frame
- Interpolasi ke pixel dengan bobot depth + normal

**Selesai kalau:** Cornell box menunjukkan color bleeding yang benar dan stabil (tidak berdenyut) saat kamera diam.

### M4 — Hash grid radiance cache (±2 minggu)
- Kunci: posisi terkuantisasi + arah mayor, 2²⁰ entri fixed
- Diisi dari hit ray screen probe, di-resolve tiap frame
- Query saat ray mengenai permukaan → memberi **multi-bounce** hampir gratis

**Selesai kalau:** furnace test — adegan albedo 1,0 di bawah pencahayaan seragam mendekati putih rata, tidak menggelap.

### M5 — Denoise & temporal (±3 minggu)
- Reprojection dengan motion vector, tolak sampel via depth + normal
- A-trous 2 pass, bobot bilateral
- Penanganan disocclusion: naikkan ray count sementara di region baru

**Selesai kalau:** lampu dinyalakan-matikan, GI merespons < 200 ms tanpa ghosting yang terlihat.

### M6 — Integrasi ke shading OpenPBR (±1 minggu)
- Irradiance GI masuk sebagai `irradiance` di `evalOpenPBR_IBL`
- Hormati albedo scaling: kontribusi difus dikalikan `(1 - E_spec)`
- Metal mengambil dari lapis spekular, bukan irradiance difus

**Selesai kalau:** white furnace test lulus untuk seluruh rentang roughness dan metalness.

### M7 — Backend ray query (±2 minggu)
- `RayQueryBackend` di belakang interface yang sama
- Proxy-BVH untuk objek jauh; BVH refit (bukan rebuild) untuk objek dinamis
- Deteksi kapabilitas runtime → pilih backend otomatis, dengan override manual

**Selesai kalau:** kedua backend menghasilkan gambar yang secara perseptual setara di adegan uji yang sama; selisihnya hanya detail kontak dan geometri tipis.

### M8 — Tier & preset kualitas (±1 minggu)

| Parameter | Low | Medium | High (2060) |
|---|---|---|---|
| Backend | SDF | SDF | Ray query |
| Ray/probe/frame | 8 | 16 | 32 |
| Resolusi GI | ¼ | ½ | ½ |
| Kaskade SDF | 2 | 3 | 3 (fallback) |
| Langkah march maks | 24 | 32 | — |

---

## Yang sengaja TIDAK dibangun

Menahan diri di sini sama pentingnya dengan mengerjakan daftar di atas:

- **Mesh SDF per-objek sebagai lapis trace terpisah** — memori dan kompleksitasnya tidak sepadan di 6 GB
- **BVH software di compute shader** — 5–10× lebih lambat dari sphere tracing di GPU tanpa RT core
- **Denoiser berbasis ML** — TU116 tidak punya tensor core, RDNA1 apalagi
- **Specular GI penuh di baseline** — mulai dari SSR + probe fallback saja
- **GI volumetrik / participating media** — tunda sampai difus benar-benar stabil

---

## Risiko utama

| Risiko | Gejala | Mitigasi |
|---|---|---|
| Update SDF jadi pos biaya terbesar | Frame time melonjak saat banyak objek bergerak | Counter terpisah sejak M0; batasi objek dinamis yang masuk komposit |
| Geometri tipis hilang di SDF | Pagar/daun tidak menghasilkan bayangan indirect | Terima di baseline; ray query menutupinya di tier atas |
| BVH tidak muat di 6 GB | OOM di adegan besar pada RTX 2060 | Proxy-BVH + streaming, direncanakan di M7 bukan sesudahnya |
| Leaking lewat dinding tipis | Cahaya menembus ruangan | Naikkan resolusi kaskade terdekat; uji dengan adegan dinding 10 cm |
| Latensi temporal terlihat | Perubahan cahaya lambat menyusul | Naikkan ray count adaptif di region yang berubah |

---

## Estimasi total

**±16 minggu** untuk satu orang penuh waktu sampai M8. Angka ini optimis kalau ini implementasi GI pertamamu — M3 dan M5 (probe dan denoise) hampir selalu makan waktu 2× perkiraan awal, karena keduanya bukan soal algoritma melainkan soal menjinakkan artefak.

Urutan ini disusun supaya kamu punya GI yang **terlihat** sejak akhir M3, bukan di akhir semuanya. Itu penting untuk menjaga momentum dan untuk menemukan masalah kualitas selagi masih murah untuk diubah.

---

## Adegan uji yang perlu disiapkan sejak M0

1. **Cornell box** — validasi color bleeding & energi
2. **Furnace test** — albedo 1,0, pencahayaan seragam; harus hilang ke latar
3. **Dinding tipis 10 cm** — deteksi leaking
4. **Koridor dengan pintu** — occlusion jarak menengah
5. **Adegan padat + objek bergerak** — biaya update SDF/BVH
6. **Outdoor luas** — jangkauan kaskade & fallback langit

### `Resources/Levels/gi-sponza.simlevel` — dan gambar yang harus disamainya

Intel Sponza (`NewSponza_Main_glTF_003`), 3,75 juta segitiga. Ia menutup sekaligus
nomor 3, 4, dan 6 di daftar di atas: dinding tipis, serambi yang hanya menerima
cahaya lewat lengkungan, dan halaman terbuka ke langit.

**Yang membuatnya adegan uji dan bukan sekadar model besar: berkasnya membawa
kamera dan matahari yang dipakai render acuannya.** Node `PhysCamera001` dan
`SUN` ada di dalam glTF-nya, jadi sudut pandang dan arah cahaya tidak perlu
ditebak — dan gambar yang dibandingkan adalah gambar yang sama, bukan gambar yang
mirip.

- Matahari: dari `SUN` ke `SUN.Target`, arah `(0,551, −0,828, 0,105)` — 55,9°
  di atas cakrawala. Sudah tertulis di levelnya sebagai kuaternion.
- Kamera: `--bench-camera -8.807,1.592,-0.858,-3.004,2.762,0.237`, yaitu posisi
  dan target `PhysCamera001` apa adanya.
- Acuannya `Render_Main_A..F.png` di sebelah berkas modelnya.

**Asetnya tidak ada di repo ini.** 140 MB geometri dan 2,6 GB tekstur; `Resources`
disalin utuh ke direktori binary tiap build, jadi menaruhnya di sana berarti
membayar salinan itu setiap kali. Ia dipasang sebagai symlink di dalam project
(`Assets/Sponza/`) beserta `.meta` ber-GUID tetap, dan levelnya merujuk GUID itu.
Tanpa asetnya, level ini memuat langit dan matahari tanpa geometri — bukan gagal,
tapi juga bukan uji.

**Materialnya dibangkitkan, bukan dibuat tangan.** `Tools/sponza-assets.py`
memasang symlink asetnya, menulis 72 `.meta` tekstur, dan membangkitkan 28
`.simmat` — masing-masing sebuah graf OpenPBR: base color, normal, dan kekasaran
(kanal G) beserta kelogaman (kanal B) dari tekstur gabungan glTF. GUID-nya
`uuid5` dari nama berkasnya, jadi menjalankannya ulang menghasilkan angka yang
sama persis; GUID acak akan memberi identitas baru pada aset yang sama dan
memutus level yang sudah menyebut yang lama.

Larik `materials` di level diindeks **nomor ruas**, dan urutan ruas adalah urutan
material saat pertama ditemui menelusuri node lalu primitive — sama persis dengan
yang dilakukan importir glTF, dan `GroupByMaterial` mengurutkan ruasnya menurut
indeks itu. Skrip yang sama menuliskannya ke level.

### Dua cacat alat ukur yang ditemukan adegan ini

Keduanya diam, dan keduanya baru bisa ditemukan oleh adegan yang **punya**
tekstur — sampai Sponza masuk, tidak ada satu pun adegan uji headless yang
punya:

- **SimHeadless tidak pernah menyerahkan texture bakery ke `SceneView`.** Hanya
  `ViewportPanel` yang melakukannya. Tanpa itu `UploadedTexture` menjawab
  "tekstur tidak ada", materialnya tetap dikompilasi — dengan tekstur kosong —
  dan hasilnya permukaan putih rata yang tidak bisa dibedakan dari material yang
  memang tidak bertekstur. Tidak ada satu pun peringatan di sepanjang jalur itu.
- **Batas tunggu kompilasi material dihitung dalam frame, bukan waktu.** 3.000
  frame berarti enam detik; adegan yang teksturnya 72 lembar 4K butuh sekitar
  satu menit untuk memanggangnya pada jalan pertama. Yang terukur lalu adegan
  yang setengah materialnya masih jalur mundur. Sekarang batasnya waktu.

### Empat sebab serambi Sponza gelap, dan yang tersisa sesudahnya

Adegan ini menagih janji M0–M6 dengan satu pertanyaan: kenapa menyalakan GI
**mengambil** cahaya? Pada eksposur yang sama, serambinya beradiansi 0,001
dengan GI hidup dan 0,025 dengan GI mati. Empat sebab, masing-masing
menyembunyikan yang berikutnya — jadi tiga perbaikan pertama terlihat hampir
tidak berpengaruh sampai yang keempat ikut diperbaiki.

1. **Langit GI bukan langit yang tergambar.** `giSky` sebuah gradien tetap
   dengan catatan "langit sungguhan datang di E8.8". E8.8 sudah lama datang.
   Sekarang penelusur mencuplik LUT sky-view yang sama dengan `sky_draw.frag`.
2. **"Tidak tahu" dihitung sebagai "tidak ada".** Sinar yang mengenai clipmap di
   luar jangkauan cache radiansi dibuang; di dalam serambi 98% sinar justru
   jenis itu, jadi probe tanpa satu pun sinar yang diketahui menjawab nol.
   Sekarang ditaksir satu pantulan penuh dengan albedo tebakan.
3. **Satu NaN meracuni seluruh probe.** `sdfSurfaceNormal` menormalkan gradien
   yang bisa tepat nol. Koefisien SH dijumlahkan, jadi satu dari enam belas
   sinar cukup untuk menghitamkan probe. Selama sinar itu dibuang (sebab 2),
   NaN-nya ikut terbuang — cacat ini baru muncul sesudah sebab 2 diperbaiki.
4. **Medan jaraknya kotak batas, dan Sponza satu mesh.** `BoxSceneField`
   menyusun tiap mesh sebagai kotak batasnya; kotak Sponza 36×20×24 m memuat
   seluruh gedung beserta udaranya. 84% voxel kaskade terhalus terbaca "jauh di
   dalam benda", dan setiap sinar probe mengenai sesuatu di jarak nol. Sinar
   yang berangkat dari dalam medan sekarang melewatkan lapis itu.

Ditambah satu di jalur resolve: piksel yang keempat probe tetangganya ditolak
uji bilateral menjawab nol — 21,8% piksel di adegan ini.

Terukur pada kamera acuan, GI hidup, EV100 = -4:

| | awal | sesudah 1–3 | sesudah 1–4 | GI mati |
|---|---:|---:|---:|---:|
| Iradiansi serambi (E/π) | 0,006–0,015 | 0,031–0,096 | 0,153–0,502 | 0,080 |
| Radiansi serambi | 0,001 | 0,013–0,020 | 0,045–0,115 | 0,025 |
| Radiansi lantai | 0,000 | 0,000 | 0,020–0,058 | 0,025 |
| Piksel hitam | 27,8% | 28,4% | 5,9% | 5,9% |
| Median 8-bit | 11 | 42 | 47 | 36 |
| Piksel putih pecah | 7,1% | 0,1% | 0,0% | 0,0% |
| `gi-probe-trace` | 0,414 ms | 0,453 ms | 0,390 ms | — |

**Yang tersisa, dan kenapa ia menuntut M1.** Cahaya isian sekarang seluruhnya
langit: 94–97% sinar probe berakhir di langit, 3–6% dijawab lapis layar, dan
lapis SDF tidak menjawab apa pun di adegan ini. Hasilnya serambi biru langit,
sementara acuan Intel abu-abu hangat — warnanya datang dari matahari yang
memantul di batu, bukan dari langit. Menaikkan anggaran langkah lapis layar
sudah diuji sebagai jalan pintas: 16 → 64 langkah menaikkan jawaban lapis layar
dari 6% ke 15–32% dan biayanya dari 0,39 ms ke 0,93 ms, sementara rata-rata
gambarnya bergerak 57,4 → 58,4. Sinar tambahan itu menemukan permukaan yang
warnanya sudah ikut biru — satu sumber biru ditukar dengan sumber biru yang
lain. Yang mengubahnya adalah medan jarak yang mengikuti segitiga alih-alih
kotak batas, yaitu M1, beserta albedo yang tersimpan bersamanya.

### Sesudah M1

Medan jaraknya sekarang mengikuti segitiga, dan yang berubah pada gambarnya
adalah bentuk cahayanya, bukan hanya jumlahnya: lengkungan menggelap ke arah
sudutnya, pilar punya bayangan ambien, lantai menerima langit lewat lubang
lengkung alih-alih lewat tebakan.

| | GI mati | GI, medan kotak | GI, medan segitiga | acuan A |
|---|---:|---:|---:|---:|
| Rata-rata | 45,3 | 46,6 | **56,8** | 78,7 |
| Median | 36 | 37 | **46** | 69 |
| Piksel hitam | 5,9% | 5,9% | 4,9% | 1,2% |
| `gi-probe-trace` | — | 0,390 ms | 0,613 ms | — |

Pass GI naik 57% karena sinarnya akhirnya benar-benar berjalan; sebelumnya
setiap sinar berhenti di langkah pertama, pada kotak pejal yang memuat seluruh
gedung.

**Yang masih memisahkannya dari acuan: warna.** Serambinya biru, acuannya
abu-abu hangat. Cahaya isian di sini masih didominasi langit, sementara warna
acuan datang dari matahari yang memantul di batu — dan pantulan itu menuntut
**albedo** yang tersimpan bersama medan jaraknya. Sekarang ia sebuah tebakan
tetap (`kBounceAlbedo` = 0,5), jadi setiap permukaan memantulkan abu-abu netral
alih-alih warnanya sendiri. Itu langkah berikutnya, dan ia milik M4: cache
radiansi yang tahu warna permukaan yang tidak pernah terlihat layar.
