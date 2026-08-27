# Rencana IBL & World Settings — dua skenario pencahayaan

**Masalahnya satu kalimat:** selagi GI mati, cahaya tak-langsung di mesin ini
adalah **konstanta 0,25** yang tidak berasal dari langit mana pun, dan GI mati
secara bawaan.

**Yang dibangun rencana ini:** dua tingkat pencahayaan yang berdiri sendiri —
panggang (pra-GI) dan real-time — plus tempat yang sah untuk memilih di antara
keduanya. Bukan dua jalur render; satu jalur dengan dua sumber untuk suku yang
sama.

Prefiks milestone: **B** (bake).

---

## Keadaan sekarang, diukur

Level uji: tiga prefab bawaan (`Ground` + `Sky Dome` + `Directional Light`),
kamera tetap, GI mati, 640×360.

```
SimHeadless --project P --render 640x360 --level-file <level> \
            --bench --bench-frames 4 --bench-warmup 2 \
            --bench-camera 0,2.5,9,0,0,0 --bench-capture out.png
```

| Langit | Eksposur | piksel langit | piksel tanah |
|---|---|---:|---:|
| Atmosphere (Sky Gain 20) | otomatis | 47 | **91** |
| Atmosphere (Sky Gain 20) | manual EV0 | 179 | **223** |
| HDR (HDR Gain 1) | otomatis | 42 | **109** |
| HDR (HDR Gain 1) | manual EV0 | 134 | **223** |

**Dua angka 223 yang identik itu keadaan yang rencana ini akhiri.** Menukar
seluruh langit tidak menggerakkan pencahayaan permukaan satu tingkat pun: yang
menyinari bukan langit melainkan konstanta.

Selisih 91 lawan 109 pada baris otomatis bukan cahaya, melainkan eksposur:
eksposur otomatis menimbang seluruh frame termasuk langit (`exposure.frag.slang`),
dan langit ber-gain 20 menariknya turun.

**Di mana konstantanya:**

- `Shaders/box_shading.slang:58` — `float3 indirect = float3(kFallbackAmbient);`
- `Code/Material/src/MaterialShaderModule.cpp:552` — `float3 irradiance = float3(0.25);`
- `Code/Material/src/MaterialShaderModule.cpp:560` — prefilter dan DFG dikirim
  **nol** ke `evaluateOpenPBR_IBL`, jadi logam gelap di luar sorotan langsungnya.

---

## Yang sudah ada dan tidak perlu ditulis ulang

Separuh mesinnya sudah berdiri, hanya belum pernah disambungkan ke viewport.

| Bagian | Di mana | Keadaan |
|---|---|---|
| Panggangan IBL: SH9 iradiansi, prefilter spekular, LUT DFG | `Code/Render/src/IblBaker.h` | jalan, dipakai pratinjau material |
| Sumber lingkungan sebagai antarmuka | `IEnvironmentSampler` di `Render/Ibl.h` | `GradientSky` dan `EquirectEnvironment` mengimplementasikannya |
| Pembaca HDR/EXR | `LoadHdrEquirect` + `Sim::ImageIO` | `.hdr` selalu; `.exr` saat backend EXR terbangun |
| Matematika atmosfer sisi CPU | `Render/Atmosphere.h` | transmitansi, hamburan, kedalaman optik, fase |
| Tempat memasukkannya ke shader | `evaluateOpenPBR_IBL(...)` | dipanggil, argumennya nol |
| Probe GI | `gi_trace.slang`, `gi_resolve.slang` | jalan, mati secara bawaan |

Pratinjau material sudah tersinari lingkungan sejak lama
(`VulkanMaterialPreview.cpp:102`, dengan `GradientSky`). Viewport-nya yang tidak.
Jadi ini pekerjaan plumbing dan daur hidup, **bukan matematika baru**.

---

## Keputusan yang dikunci di awal

Enam, dan semuanya lebih murah diputuskan sekarang daripada ditemukan di
milestone keempat.

### 1. HDRI adalah teknik pra-GI, bukan masukan GI

Berkas HDR adalah satu foto yang sudah berisi matahari, langit, dan pantulan —
pengganti transport cahaya, bukan masukannya. Menyuntikkannya kembali ke sistem
yang sudah punya transport cahaya menghasilkan tiga cacat sekaligus: matahari
terhitung dua kali (foto itu berisi matahari sementara lampu directional juga
menyala), langitnya diam saat Time-of-Day menggerakkan bayangan, dan radiansinya
tidak sesatuan dengan atmosfer sehingga dua pengali harus ditala terpisah
selamanya.

Ketiganya hilang sendiri bila sumbernya langit prosedural. Karena itu:

> **Tingkat real-time disinari skydome. Tingkat panggang boleh disinari skydome
> atau berkas. Berkas tidak pernah menjadi masukan probe GI.**

### 2. Suku difus punya satu pemilik

Iradiansi datang dari SH panggang **atau** dari probe, tidak pernah dari
keduanya. Melanggarnya menghasilkan adegan terang dua kali lipat tanpa satu pun
galat di log — jenis cacat yang hanya ketahuan lewat mata, berbulan-bulan
kemudian.

### 3. Maksud masuk ke level, anggaran masuk ke project, alat ukur tidak disimpan

- *Maksud* — "adegan ini dirancang dengan GI real-time", "adegan ini disinari
  HDRI". Kalau ia berubah saat berpindah mesin, pengarangannya rusak. Masuk ke
  **World Settings** di dalam berkas level.
- *Anggaran* — resolusi probe, jumlah kaskade, backend ray query. Level yang
  disimpan di mesin kuat tidak boleh memaksakan setelannya ke mesin lemah. Masuk
  ke **project**.
- *Alat ukur* — `screenTrace`, `debugView`, `furnaceTest`, saklar CPU/GPU.
  `TraceBackend.h:167` sudah menyatakannya sendiri: "bukan tombol kualitas
  melainkan alat ukur". **Tidak disimpan sama sekali.** Menyimpan alat ukur ke
  level mengubah percobaan sementara menjadi data level.

### 4. Rotasi lingkungan tidak boleh memicu bake ulang

`hdriRotation` memutar arah cuplikan, bukan lingkungannya. Salah memilih di sini
mengubah slider yang mulus menjadi slider yang menghentikan editor tiap kali
digeser.

### 5. Prefab tidak membawa World Settings

Format `.simprefab` sama persis dengan level (`Project.h:47`), jadi `SavePrefab`
harus menolak menulis bloknya. Prefab adalah potongan; potongan yang diam-diam
mengubah pencahayaan level tempat ia dijatuhkan adalah bug yang tidak akan
dicari siapa pun di sana.

### 6. Bake di luar main thread, hasilnya dimasak

Beberapa ratus milidetik di main thread adalah editor yang membeku tepat saat
orang mengetik jalur berkas — dan yang membeku akan disalahkan ke hal lain.
Hasilnya artefak masak seperti tekstur BC (`Assets/Cook.cpp`,
`TextureBake.cpp`), bukan pekerjaan yang diulang tiap kali level dibuka.

---

## Bentuk yang dituju

### Tiga tingkat

| `indirect` | `environment` | Suku difus | Spekular | Untuk |
|---|---|---|---|---|
| `None` | — | nol | nol | pengukuran, tampilan albedo |
| `Baked` | `Sky` | SH dari atmosfer | prefilter dari atmosfer | bawaan; project apa pun |
| `Baked` | `File` | SH dari `.hdr`/`.exr` | prefilter dari berkas | **skenario pra-GI**: HDRI + lampu |
| `RealTime` | — | probe GI | probe + screen trace | adegan yang menuntut oklusi benar |

`RealTime` + `File` **bukan kombinasi yang sah** dan harus dinyatakan begitu di
editor, bukan didiamkan — lihat B6.

### World Settings

```jsonc
{
  "schemaVersion": 4,
  "world": {
    "indirect": "Baked",      // None | Baked | RealTime
    "environment": "Sky",     // Sky | File
    "exposureMode": "Automatic",
    "exposureCompensation": 0.0
  },
  "entities": [ /* ... */ ]
}
```

`struct WorldSettings` di `Sim::Scene`, dipegang `World`, **didaftarkan ke
`reflect::TypeRegistry` seperti komponen**. Itu yang membuatnya hampir gratis:
PropertyGrid merender tipe terdaftar apa pun, `SceneCommands` memberi undo/redo,
dan alat MCP melihatnya lewat refleksi yang sama. Tidak ada satu widget pun yang
perlu ditulis tangan.

**`SkyComponent` tidak dipindahkan.** Ia tetap memegang langit yang *tergambar*
— sumber, berkas, rotasi, gain — dan keberadaannya tetap yang menyalakan pass
langit; alasan itu sudah tertulis di komponennya. World Settings memegang
bagaimana adegan *disinari*. Satu sumber untuk "langit yang mana", satu setelan
untuk "menyinari bagaimana".

---

## Milestone

Tiap milestone punya kriteria selesai yang bisa diuji. Urutannya bukan selera:
B0 adalah tempat menyimpan jawaban, dan tanpanya B1–B6 hanya menambah centang
viewport yang tidak tercatat di berkas mana pun.

### B0 — World Settings · ✅

- `scene::WorldSettings`, dipegang `World`, terdaftar di `TypeRegistry`
- Blok `"world"` sebagai saudara `"entities"`; `kLevelSchemaVersion` naik ke 4
- Blok yang tidak ada berarti bawaan — pola `root.value(...)` yang sudah dipakai
  `LoadProject`
- `SavePrefab` tidak menulisnya
- `context.gi.enabled` menjadi **cerminan** `world.Settings().indirect`, bukan
  kebenaran kedua. Panel langit sudah memilih ini dengan benar sekali:
  "dua tempat menyunting satu hal adalah dua tempat yang suatu saat tidak
  sepakat" (`SceneStubPanels.cpp:151`)
- `SimHeadless` dan `SimRuntime` membacanya

**Selesai kalau:** level yang disimpan dengan `indirect: RealTime` dibuka di
mesin lain — dan di `SimHeadless --bench` — memakai tingkat itu tanpa satu klik
pun; level lama tanpa blok `world` terbuka apa adanya; `.simprefab` yang
disimpan dari sub-pohon tidak memuat blok itu.

> Cacat yang sama pernah terjadi sekali dan tercatat di `SceneView.h`: setiap
> pengukuran headless dulu menggambar langit bawaan berapa pun angka yang
> tertulis di level. Kalau `SimHeadless` tidak membaca `world.indirect`, setiap
> bench mengukur tingkat pencahayaan yang bukan milik level itu.

#### Keadaannya sesudah B0

`scene::WorldSettings` ada di `Code/Scene/include/Sim/Scene/WorldSettings.h`,
dipegang `World`, dan terdaftar di `TypeRegistry` — **tidak** di
`ComponentRegistry`, dan keduanya disengaja: yang pertama membuat PropertyGrid
merendernya tanpa satu widget pun ditulis tangan, yang kedua adalah daftar yang
menentukan apa yang boleh menempel di entity dan apa yang ikut ke `.simprefab`.

Yang menjembatani level ke renderer adalah satu fungsi,
`view::ApplyWorldSettings`, dipanggil viewport editor, jalur bench headless, dan
player. Satu aturan di satu tempat: kalau masing-masing menurunkannya sendiri,
"level yang sama disinari sama di mana pun" berhenti berlaku tanpa satu pun
galat. `--bench-gi` tetap ada sebagai paksaan eksplisit untuk satu jalan ukur,
dan bawaannya sekarang benar-benar mengikuti level — kalimat yang sudah tertulis
di teks bantuannya sejak G0, dan baru sekarang bisa ditepati.

Panelnya sendiri, `World Settings`, ter-dock sebagai tab di samping Entity
Inspector di ketiga workspace. Ia bukan bagian Inspector karena tidak dimiliki
entity mana pun: menempelkannya di keadaan "tidak ada yang terpilih" berarti
setelan level yang hanya bisa dicapai dengan lebih dulu membatalkan seleksi, dan
hilang lagi begitu ada yang diklik. Penempatannya dikunci uji di
`SimEditorTests`, bukan diingat.

Sakelar "GI enabled" di panel Statistics menjadi baris keadaan; mode eksposur dan
kompensasinya tetap di sana tapi sekarang menyunting World Settings lewat
`SetWorldSettingsCommand`, jadi keduanya ikut undo/redo. Dua widget yang merender
satu nilai tidak apa-apa; dua tempat yang menyimpannya yang tidak.

**Yang diuji:** enam kasus di `SimSceneTests` (bolak-balik lewat berkas, blok
yang tidak ada, tidak mewarisi level sebelumnya, prefab tanpa blok, kombinasi
tidak sah, terdaftar sebagai tipe bukan komponen), empat di `SimLevelEditorTests`
(jembatan ke `ViewportDesc`, dan undo perintahnya), satu di `SimEditorTests`
(dock di samping Inspector). 25 dari 25 suite lulus.

**Kriteria bench-nya diverifikasi ujung-ke-ujung**, sesudah sebuah crash yang
memblokirnya dilacak dan diperbaiki (lihat di bawah). Dua level yang bedanya
tepat satu kata di blok `"world"`, dijalankan tanpa satu bendera pun:

```
--level-file baked.simlevel     →  - GI: mati
--level-file realtime.simlevel  →  - GI: menyala
--level-file realtime.simlevel --bench-gi off  →  - GI: mati
```

Baris ketiga yang menyatakan sisanya: level adalah kebenaran, bendera adalah
paksaan eksplisit di atasnya.

> **Crash `--bench` yang menghalangi, dan sebabnya.** `--bench` crash pada mesin
> ini sejak sebelum B0 — diperiksa dengan binary yang dibangun tanpa satu pun
> perubahan B0. Penyebabnya bukan pencahayaan sama sekali: FBX SDK tidak
> thread-safe, dan satu `FbxManager` per thread **tidak cukup** karena
> `FbxObject::Construct` menyunting `FbxPropertyPage` yang milik proses. Main
> thread memuat `shaderBall.fbx` lewat `VulkanRenderer::AcquireMesh` sementara
> sebuah worker `TaskPool` memuat `unitCylinder.obj` untuk `MeshSdfBakery`;
> keduanya di dalam `FbxPropertyPage` pada saat yang sama. Seluruh pemakaian SDK
> kini diserialkan lewat `sim::FbxSdkMutex()`, dan bench lulus 5 dari 5 jalan.
> Rinciannya di [DEPENDENCIES.md](DEPENDENCIES.md).

**Tiga hal yang belum tuntas, dan sebaiknya tidak ditemukan sebagai kejutan:**

1. **`None` dan `Baked` belum bisa dibedakan.** Keduanya mematikan probe, dan
   cahaya tak-langsung di jalur itu masih konstanta 0,25 yang tidak berasal dari
   langit mana pun. Yang membedakannya adalah iradiansi panggang yang
   menggantikan konstanta itu — B1. Begitu pula `environment`: ia tersimpan di
   berkas tapi belum berpengaruh, karena yang memakainya panggangannya.
2. **`manualEv100` tidak ikut di blok `"world"`.** Skema di dokumen ini menyebut
   `exposureMode` dan `exposureCompensation` saja, dan B0 menulis persis itu.
   Akibatnya sebuah level yang disimpan pada mode `Manual` terbuka kembali pada
   `Manual` tetapi dengan EV100 bawaan editor, bukan angka yang disetel
   pengarangnya. Memasukkannya berarti menambah satu field ke skema — murah,
   tapi mengubah bentuk berkas yang baru saja ditetapkan, jadi ia keputusan
   tersendiri.
3. **Alat MCP belum melihatnya.** Rencana ini menyebut refleksi membuat World
   Settings terjangkau alat MCP "lewat refleksi yang sama"; ternyata tidak.
   `AiSceneTools` menelusuri `ComponentRegistry`, bukan `TypeRegistry`, jadi
   sebuah agen belum bisa membaca atau mengubah tingkat pencahayaan sebuah
   level. Yang dibutuhkan sepasang tool tersendiri — `world.settings.get`/`set`
   — dan **bukan** memasukkan World Settings ke `ComponentRegistry`, karena itu
   akan membuatnya ikut ke setiap `.simprefab`: persis cacat yang keputusan 5
   cegah.

### B1 — Iradiansi panggang dari langit prosedural (`Baked` + `Sky`) · ✅

- `AtmosphereSky : IEnvironmentSampler` di atas matematika CPU `Atmosphere.h`
- `BakeIbl` dipanggil dengannya di `TaskPool`; SH9 menggantikan konstanta 0,25
  di kedua shader
- Panggang ulang saat matahari bergeser — **SH saja**, prefilter menyusul lebih
  malas

**Selesai kalau:** `kFallbackAmbient` tidak ada lagi di `box_shading.slang`
maupun di codegen material; adegan template dengan GI mati punya ambient yang
ikut berubah saat Time-of-Day menggerakkan matahari; dan iradiansi SH dari
`AtmosphereSky` cocok dengan integrasi langsung atas pencuplik yang sama di
`SimRenderTests`.

#### Keadaannya sesudah B1

**Premis dokumen ini patah, dan itu terukur.** Pengukuran pembuka di atas
diulang dengan yang berubah hanya Sky Gain — bukan seluruh langit — supaya yang
tersisa memang cuma pencahayaannya:

| Sky Gain | piksel langit | piksel tanah (EV+4) | piksel tanah (EV0) |
|---:|---:|---:|---:|
| 20 | 21,3 | **65,5** | 229,3 |
| 2 | 0,4 | **57,1** | 224,3 |

Sebelum B1 kedua baris "tanah" itu **56,1 dan 56,1** — sama persis, sampai ke
digit terakhir. Sekarang keduanya berbeda 15%. Selisihnya sederhana karena tanah
di adegan ini didominasi matahari langsung; yang teduh bergerak jauh lebih
banyak.

`AtmosphereSky` menghitung radiansi langit yang sama yang tergambar, di CPU,
tanpa cakram mataharinya — lampu directional adegan yang mengantarkan cahaya
langsungnya, dan menyertakan cakramnya membuat matahari terhitung dua kali.
Hasilnya sembilan koefisien SH di blok uniform per-frame, dibaca `box_shading`
maupun codegen material lewat `skyIrradiance()` yang sama.

**Konstanta 0,25 yang hilang ternyata dua konstanta.** Ia diperlakukan sebagai
E/π di `box_shading.slang` dan sebagai E di codegen material — satu angka dengan
dua arti di dua berkas, dan selisih pi itu ikut lenyap bersamanya.

**Tabel transmitansi di CPU, karena tanpanya panggangannya tidak layak.**
Sebagai integral bersarang, satu proyeksi SH 1024 sampel memakan 289 ms (Debug);
dengan tabel 64 ms. Rencana ini mengandaikan panggang ulang tiap matahari
bergeser itu murah — angka pertama membuatnya tidak. Tabelnya hanya bergantung
pada udaranya, bukan pada arah cahayanya, jadi matahari yang bergeser tidak
menyentuhnya sama sekali.

**Panggangannya asinkron di editor dan player, sinkron di `SimHeadless`.** Yang
kedua bukan kelalaian: sebuah panggangan asinkron membuat gambar yang tertangkap
bergantung pada apakah worker sempat selesai, yaitu pada waktu thread — dan dua
jalan dari binary yang identik lalu menghasilkan gambar yang berbeda. Ditemukan
persis begitu saat mengukur tabel di atas: empat tangkapan pertama seluruhnya
identik karena bench selesai sebelum panggangannya mendarat.

**Yang diuji:** lima kasus di `SimRenderTests` — SH melawan integrasi langsung
atas pencuplik yang sama (kriteria ketiga), iradiansi yang bergerak bersama
mataharinya dan memerah saat senja (kriteria kedua, di tingkat yang bisa diuji
tanpa GPU), cakram matahari yang tidak ikut, tabel transmitansi melawan
integral, dan tabel yang tidak mengubah jawaban. 25 dari 25 suite lulus.

**Dua hal yang belum tuntas:**

1. **Langit `HDR Map` tidak menyinari apa pun.** `AtmosphereSky` hanya menjawab
   langit prosedural; untuk berkas, iradiansinya nol sampai B3 memanggang dari
   `EquirectEnvironment`. Nol dipilih alih-alih sebuah konstanta pengganti
   karena konstanta itulah yang baru saja dibuang — sebuah angka yang berpura-
   pura menjadi langit adalah persis cacat yang B1 akhiri.
2. **Frame pertama sesudah level dibuka lebih gelap** di editor dan player:
   panggangannya berjalan di kolam tugas dan butuh beberapa ratus milidetik,
   dan sebelum ia mendarat tidak ada iradiansi yang bisa dipakai. Menahannya
   dengan konstanta akan menukar satu kedipan dengan satu kebohongan.

### B2 — Spekular panggang + DFG di viewport

- Prefilter cubemap dan LUT DFG diikat ke set forward; tiga nol di
  `MaterialShaderModule.cpp:560` diganti
- Rantai yang sama persis dengan yang sudah dipakai pratinjau material

**Selesai kalau:** bola logam kekasaran nol memantulkan langit yang sama dengan
yang tergambar di belakangnya; dan **uji tungku** lulus di jalur panggang —
langit seragam 1, albedo 1, tanpa matahari, setiap permukaan berradiansi 1.
Kriteria itu sudah ada dan sudah bernama (`TraceBackend.h:173`, M4 GI); yang
ditambahkan di sini hanya menjalankannya pada jalur panggang.

### B3 — Berkas HDR/EXR sebagai lingkungan (`Baked` + `File`)

- `EquirectEnvironment` → `BakeIbl`; tidak ada satu baris matematika yang
  berubah, persis seperti yang dijanjikan komentar kelasnya di `Ibl.h`
- Rotasi memutar arah cuplikan (keputusan 4)
- Artefak masak `.simibl` di sebelah `.meta` berkasnya

**Selesai kalau:** membuka level pra-GI tidak memanggang apa pun — cubemap kecil
dimuat langsung; menggeser rotasi tidak memicu bake; dan berkas `.exr` pada build
tanpa backend EXR ditolak dengan pesan yang menyebut backend yang kurang, bukan
"format tidak dikenal" (jalurnya sudah ada di `Ibl.cpp:180`).

### B4 — Ekstraksi matahari dari HDRI

Berkas HDR sudah berisi mataharinya. Kalau level juga punya `Sun`, ada dua.

- Deteksi kawasan paling terang → arah + radiansi
- Keluarkan dari lingkungan **sebelum** dipanggang
- Tawarkan mengisi lampu directional dengan hasilnya

**Selesai kalau:** iradiansi total dari (lingkungan tanpa matahari + lampu hasil
ekstraksi) sama dengan iradiansi dari peta utuh, dalam toleransi yang ditulis di
ujinya. Tidak ada energi yang hilang, tidak ada yang dihitung dua kali.

### B5 — Validasi terhadap path tracer acuan

`reference::PathTracer` baru punya `Vec3 skyRadiance` — langit satu warna
(`PathTracer.h:71`). Tanpa lingkungan, tidak ada yang bisa mengatakan
panggangannya benar.

- `PathTracer` menerima `IEnvironmentSampler` menggantikan konstanta itu
- Adegan acuan berlangit: bandingkan tingkat `Baked` terhadap jalur acuan

**Selesai kalau:** selisihnya di bawah ambang yang ditulis di dokumen ini, dan
angkanya dicatat di sini — bukan di pesan commit.

### B6 — Kombinasi yang tidak sah dinyatakan

Hari ini `skyParams.x` dipaksa nol untuk `HdrMap` (`VulkanRenderer.cpp:5468`),
jadi GI jatuh ke gradien analitik `giSkyGradient` (`gi_trace.slang:458`) — adegan
disinari langit yang bukan langit yang tergambar. Komentarnya sendiri menyebut
itu cacat.

Di bawah keputusan 1, jawabannya bukan menyambungkan HDRI ke probe, melainkan
menyatakan bahwa kombinasinya memang tidak didukung:

- `RealTime` + langit `HDR Map` → notifikasi editor yang menyebutkan bahwa
  fotonya latar, bukan cahaya, dan menawarkan dua jalan keluar (pindah ke
  `Baked`, atau pakai langit atmosfer)
- Gradien analitik dihapus: yang tersisa hanya langit yang benar-benar aktif

**Selesai kalau:** tidak ada lagi jalur di mana adegan disinari langit yang tidak
tergambar, dan tidak ada kombinasi yang gagal diam-diam.

---

## Anggaran

### Memori

| Alokasi | Ukuran | Catatan |
|---|---:|---|
| Prefilter cube 64², 5 mip, RGBA16F | 0,25 MB | bawaan `IblBakeSettings` |
| LUT DFG 64² RG16F | 16 KB | satu untuk seluruh material |
| SH9 iradiansi | 108 B | |
| **Tambahan tingkat panggang** | **< 0,3 MB** | |
| Peta HDR 4096×2048 RGBA16F + 8 mip | ~85 MB | **sudah dibayar** untuk latarnya |
| Dekode HDR di CPU | ~100 MB | sementara, saat memuat |

Tingkat panggang praktis gratis di VRAM. Yang mahal adalah petanya, dan itu
harga latar belakang yang sudah dibayar sebelum rencana ini.

### Waktu

| Pekerjaan | Perkiraan | Kapan |
|---|---:|---|
| Bake penuh (prefilter 6×5456 texel × 64 sampel + iradiansi 8192 sampel) | ratusan ms | sekali per lingkungan, di `TaskPool` |
| Panggang ulang SH saja | jauh lebih murah | tiap matahari bergeser (`Baked` + `Sky`) |
| Muat dari artefak masak | ~0 | tiap buka level (`Baked` + `File`) |

---

## Risiko

**Ambient panggang tidak punya oklusi.** Lingkungan tetap berlaku pada setiap
permukaan: langit bocor menembus dinding, ke ruangan tertutup sekalipun.
Konstanta 0,25 hari ini punya cacat yang sama, hanya cukup redup untuk tidak
diperhatikan; menaikkannya ke iradiansi langit sungguhan membuat kebocoran itu
terlihat. Itu memang batas tingkat pra-GI, dan harus ditulis di dokumentasinya —
bukan ditemukan pengguna sebagai "kenapa interior saya terang".

**Eksposur bawaan perlu ditala ulang.** Permukaan menjadi lebih terang,
eksposur otomatis membaca frame yang lain, dan Sky Gain 20 / HDR Gain 1 /
intensitas matahari 4 tidak lagi menghasilkan gambar yang sama. Setiap gambar
acuan yang ada langitnya — termasuk tangkapan bench di
[PLAN-GPU-OPTIM.md](PLAN-GPU-OPTIM.md) — harus diambil ulang setelah B2.

**Bake yang menghalangi.** Kalau keputusan 6 dilanggar sekali saja, gejalanya
bukan galat melainkan editor yang tersendat setiap kali orang menyentuh
lingkungan.

---

## Yang sengaja tidak dikerjakan

**Bake IBL di GPU.** `IblBaker.h` sudah menyebut syaratnya: ia menjadi menarik
begitu lingkungan bisa berganti saat berjalan. B1 memenuhi syarat itu (matahari
bergerak), tapi hanya untuk SH — dan SH cukup murah untuk tetap di CPU. Prefilter
GPU menunggu sebuah pengukuran, bukan sebuah dugaan.

**HDRI menyinari probe GI.** Keputusan 1. Kalau suatu saat dibatalkan, yang
harus dijawab lebih dulu: matahari ganda, dan firefly dari piksel yang ribuan
kali lebih terang daripada tetangganya.

**Transmission, subsurface, thin film** — di luar rencana ini, dibahas di
[RENDER-OPENPBR.md](RENDER-OPENPBR.md).

---

## Yang sudah diputuskan sejak rencana ini ditulis

**Bawaan prefab `Sky Dome` adalah `Atmosphere`.** Sempat `source: "HDR Map"`
dengan `hdriPath: "Environment/golden_gate_hills_4k.hdr"`; sekarang kembali ke
langit prosedural dengan `hdriPath` kosong, yaitu bawaan yang konsisten dengan
`indirect: Baked` + `environment: Sky`.

Alasannya dua, dan yang kedua tidak terlihat dari dalam rencana ini. Yang
pertama memang konsistensi. Yang kedua: berkas HDRI-nya 25 MB, lebih besar dari
seluruh riwayat repo ini digabung, dan riwayat git tidak bisa dikecilkan lagi
sesudah berkas sebesar itu masuk. Bawaan yang menuntut sebuah unduhan sebelum
level baru menggambar apa pun juga bukan bawaan yang baik — yang tidak
mengunduhnya tidak mendapat galat melainkan langit hitam, karena renderer hanya
mencatat peringatan lalu melewati pass-nya.

Berkas HDRI karena itu di-gitignore (`Resources/Environment/*.hdr`, `*.exr`),
dan project yang memang pra-GI menaruh berkasnya sendiri di sana lalu
memilihnya lewat satu dropdown di Inspector. `ResolveHdriPath` tetap melengkapi
jalur relatif terhadap `Resources` bawaan; ujinya membuat berkasnya sendiri
alih-alih menuntut satu yang tidak ikut di repo.

---

## Yang masih terbuka

- **Ambang selisih B5** terhadap path tracer acuan: belum ditetapkan angkanya.
- **Satuan.** Sky Gain dan HDR Gain tidak sesatuan, dan alasannya sudah tertulis
  di `SkyComponent`. Kalibrasi keduanya ke satuan fotometrik adalah pekerjaan
  tersendiri, bersama satuan emisi yang juga belum dikalibrasi
  ([PLAN-MATERIALX.md](PLAN-MATERIALX.md)).
