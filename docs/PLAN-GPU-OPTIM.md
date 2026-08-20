# Plan Optimasi GPU (G0 → G8)

Dokumen ini bukan lanjutan E8. E8 menjawab "apakah gambarnya benar"; dokumen ini
menjawab "apakah harganya wajar". Keduanya sengaja dipisah, karena keduanya
punya kriteria selesai yang berbeda: yang pertama diperiksa dengan mata, yang
kedua hanya bisa diperiksa dengan angka.

**Perangkat yang menentukan seluruh isinya sama dengan yang menentukan plan GI:**
GTX 1660 Super / RX 5600 XT sebagai garis dasar — tanpa RT core, 6 GB, bus
192-bit — dan RTX 2060 sebagai tier atas. Target 1080p 60 fps, artinya 16,6 ms
untuk seluruhnya. Angka itu yang membuat sebagian keputusan di sini terlihat
ekstrem; pada GPU 24 GB dengan bus 384-bit, separuh dokumen ini memang tidak
perlu ada.

---

## Titik berangkat

**Yang sudah ada dan sudah benar.** Frame graph dengan barrier yang disimpulkan
dari deklarasi pass dan alias memori berbasis selang umur. Depth prepass tanpa
fragment shader, opaque dengan uji `EQUAL`. Clustered light culling. CSM tiga
kaskade plus shadow atlas point/spot. Reversed-Z. Instancing per mesh lewat
`vkCmdDrawIndexed` ber-`instanceCount`. Dynamic rendering dan `synchronization2`
sejak E1. GI lengkap sampai M5 — SDF clipmap, screen-space trace, screen probe,
hash-grid radiance cache, denoise temporal. Ini bukan renderer yang perlu
diperbaiki arsitekturnya.

**Yang belum ada, dan tiap satunya adalah milestone di bawah.** Diverifikasi
terhadap kode, bukan diingat:

| Yang tidak ada | Bukti |
|---|---|
| Compute pipeline — satu pun tidak ada di seluruh engine | Tidak ada `vkCreateComputePipelines`; tidak ada berkas shader bertahap compute. Bagian ketiga baris ini — "`cmake/SimShaders.cmake` hanya mengenali akhiran `.vert`/`.frag`" — **salah**, dan itu baru ketahuan saat dikerjakan: lihat G3 |
| Bindless / `descriptorIndexing` | `Device.cpp:351-360` hanya menyalakan `shaderDrawParameters`, `shaderDemoteToHelperInvocation`, `dynamicRendering`, `synchronization2` |
| Indirect draw | `multiDrawIndirect` dan `drawIndirectFirstInstance` tidak diminta (`Device.cpp:293-306`) |
| Antrean kedua (async compute / transfer) | `deviceInfo.queueCreateInfoCount = 1` |
| Timeline semaphore, buffer device address | Tidak diminta di mana pun |
| Pipeline cache yang bertahan antar-jalan | `Device.cpp:118-120` membuatnya kosong; `vkGetPipelineCacheData` tidak pernah dipanggil — dan ternyata lebih buruk daripada itu, lihat G2 |
| Culling per-kaskade bayangan | `RecordShadowPass` (`VulkanRenderer.cpp:3819`) menggambar seluruh `casterRuns_` ke setiap kaskade |
| Occlusion culling | `hiz_` ada dan dibangun tiap frame, tapi pembacanya hanya penelusur screen-space — bukan culling draw |
| Perekaman command multi-thread | Satu-satunya `std::thread` di modul Render ada di `CloudNoise.cpp`, dan itu pekerjaan CPU sekali di awal |

**Satu angka yang perlu diingat sepanjang dokumen ini.** Pengukuran pertama E8
pada adegan hampir kosong (RTX 2060, 1277×614) total 0,315 ms, dan
`shadow-cascades` sendirian mengambil 0,151 ms — hampir separuhnya, pada adegan
yang belum berisi apa-apa. Itu bukan kebetulan dan bukan derau; itu gejala dari
baris di tabel atas soal culling per-kaskade.

Garis dasar G0 di bawah mengukur adegan yang berisi, dan gejalanya ternyata
jauh lebih besar daripada yang disimpulkan dari adegan kosong — serta terletak
di tempat yang berbeda. **Urutan milestone dokumen ini sudah direvisi menurut
angka itu, bukan menurut dugaan yang mendahuluinya.**

---

## Prinsip yang menentukan urutan

**Tidak ada milestone yang berangkat dari tebakan.** Tiap satunya dibuka dengan
angka `GpuProfiler` sebelum, dan ditutup dengan angka sesudah, pada adegan uji
yang sama. Optimisasi tanpa dua angka itu bukan optimisasi melainkan perubahan
kode yang kebetulan terasa lebih cepat.

**Yang murah dan berdiri sendiri lebih dulu.** G1 dan G2 tidak menuntut
infrastruktur baru sama sekali dan bisa mendarat minggu ini. Menaruh compute di
depan hanya karena ia terdengar lebih arsitektural berarti menunda kemenangan
yang sudah terukur demi kemenangan yang belum.

**Compute dibangun sekali, bukan tiga kali.** E8.6 menuntut "GPU culling (compute
+ indirect draw)", E8.7 menuntut "simulasi GPU (compute shader)", dan G5 di bawah
menuntut keduanya. Tiga milestone yang masing-masing menemukan sendiri cara
membuat compute pipeline akan menghasilkan tiga cara yang berbeda, dan yang
keempat harus memilih salah satunya. G3 ada supaya pilihan itu dibuat sekali, di
tempat yang benar, sebelum ada yang bergantung padanya.

**Setiap milestone harus bisa dimatikan.** Jalur lama tidak dihapus di milestone
yang menggantikannya; ia jadi jalur mundur di belakang sakelar yang terlihat,
persis seperti pemilih backend GI. Perangkat tanpa `descriptorIndexing` tetap
harus menjalankan editor, dan jalur yang tidak pernah dijalankan siapa pun adalah
jalur yang rusaknya baru ketahuan di mesin orang lain.

---

## G0 — Garis dasar yang bisa dipercaya · ✅ selesai

Alatnya sudah ada (`rhi::GpuProfiler`, lingkup per pass di
`FrameGraphExecutor.cpp:133`). Yang belum ada adalah **adegan yang layak diukur**.
Angka 0,315 ms di atas diambil dari adegan hampir kosong, dan adegan kosong
menyembunyikan persis hal-hal yang dokumen ini urus: draw call, ikatan descriptor,
caster bayangan, cluster yang penuh.

- Satu adegan uji yang disimpan sebagai aset dan tidak berubah antar-pengukuran:
  ratusan instance dari beberapa mesh berbeda, beberapa material bertekstur, satu
  karakter berkulit, sepuluh sampai dua puluh lampu berbayang, GI menyala.
- Mode pengukuran yang mengunci kamera ke lintasan tetap. Kamera yang digerakkan
  tangan menghasilkan angka yang tidak bisa dibandingkan dengan angka kemarin.
- Ringkasan CPU di samping ringkasan GPU. Sebagian isi dokumen ini —
  `AssignLights`, `Gather`, `stable_sort` — adalah biaya CPU, dan profiler yang
  hanya melihat GPU akan melaporkan bahwa memindahkannya ke compute tidak
  mengubah apa pun.

**Kriteria selesai:** menjalankan adegan uji dua kali berturut-turut menghasilkan
total yang berselisih di bawah 5%, dan tabel per pass tersalin ke dokumen ini
sebagai baris "sebelum".

### Yang mendarat

- **`SimHeadless --bench`.** Mode ukur di dalam program headless yang sudah ada,
  bukan aplikasi kelima yang menirunya — alasan yang sama dengan yang tertulis
  di kepala `Apps/SimHeadless/src/main.cpp`. Ia memuat level, menjalankan
  lintasan kamera terkunci, lalu keluar; server MCP tidak pernah dinyalakan,
  karena permintaan yang datang di tengah lintasan mengubah adegan yang sedang
  diukur. Bendera: `--bench-frames`, `--bench-warmup`, `--bench-gi`,
  `--bench-out`, ditambah `--level-file` yang memuat level dari jalur berkas
  alih-alih dari nama di dalam project.
- **Lintasan terkunci.** Satu putaran penuh mengelilingi kotak batas adegan,
  sudutnya diturunkan dari nomor frame dan bukan dari jam dinding, dengan delta
  tetap 1/60 s supaya animasi dan eksposur maju sama jauhnya di mesin mana pun.
  Kotak batasnya dihitung sekali dari frame pertama: menghitungnya ulang tiap
  frame membuat lintasan bergantung pada apa yang kebetulan terlihat.
- **`Resources/Levels/bench.simlevel`** — 260 entity: lantai, 240 prop dari
  empat mesh (kubus, bola, silinder, shader ball) dengan tujuh material buram
  ditambah kaca untuk jalur tembus pandang, 16 lampu point/spot berbayang, satu
  matahari, satu langit atmosferik. Sebarannya kisi ber-geseran berbenih tetap,
  bukan acak penuh: kepadatan yang berubah-ubah membuat biaya per sudut pandang
  ikut berubah tanpa ada yang mengubahnya.
- **`IViewportRenderer::CpuTimings()` dan `TimingSerial()`.** Yang pertama
  membawa tahap CPU dalam bentuk yang sama dengan yang GPU, dan Statistics Panel
  menampilkannya sebagai tabel kedua — bukan baris tambahan di tabel pertama,
  karena angka GPU datang beberapa frame terlambat sementara angka CPU adalah
  frame yang baru lewat. Yang kedua naik hanya saat hasil timestamp benar-benar
  berganti: `GpuProfiler::Collect` melewati hasil yang belum siap alih-alih
  menunggunya, jadi tanpa penanda itu sebuah frame yang sama ikut berhitung dua
  kali dalam rata-rata.

**Satu bug ditemukan oleh harness sebelum ia sempat mengukur apa pun**
(`Code/Assets/src/AssetDatabase.cpp`). Akar indeks aset dinormalkan sebelum
dipakai memotong awalan, sementara jalur hasil iterasi tidak — jadi akar
`./Resources` menghasilkan jalur "relatif" yang justru jalur utuhnya, dan
`AbsolutePath` menempelkan akarnya untuk kedua kalinya:
`./Resources/./Resources/Meshes/...`. Akibatnya **setiap mesh bawaan gagal
dimuat dan diam-diam digambar sebagai kubus satuan** — persis kondisi yang
membuat pengukuran pertama terlihat murah. Hanya muncul saat programnya
dijalankan lewat jalur relatif, yaitu cara setiap executable di folder build
dijalankan. Dikunci test di `Tests/AssetTests.cpp`, yang mengunci kontraknya —
jalur yang dikembalikan indeks harus bisa dibuka — bukan bentuk stringnya.

### Garis dasar (baris "sebelum")

RTX 2060, 1920×1080, GI menyala, 120 frame pemanasan + 240 diukur,
`Resources/Levels/bench.simlevel`. Angka median, milidetik. Dua jalan berturut
menghasilkan total GPU yang berselisih **0,4%** dan total CPU **1,3%**.

| Pass GPU | ms | | Tahap CPU | ms |
|---|---:|---|---|---:|
| `shadow-atlas` | **7,977** | | `cpu-total` | **11,402** |
| `shadow-cascades` | 1,417 | | `cpu-sdf` | **5,397** |
| `forward-opaque` | 0,562 | | `cpu-record` | 2,403 |
| `depth-prepass` | 0,502 | | `cpu-clusters` | 0,512 |
| `gi-probe-trace` | 0,322 | | `cpu-gather` | 0,043 |
| `bloom` | 0,134 | | *(belum bernama)* | ≈3,05 |
| `sky` | 0,102 | | | |
| `aerial` | 0,094 | | | |
| `post-meter` | 0,082 | | | |
| `hiz-build` | 0,070 | | | |
| `tonemap` | 0,055 | | | |
| `grid` | 0,037 | | | |
| `forward-transparent` | 0,001 | | | |
| `lines` | 0,001 | | | |
| **total GPU** | **11,356** | | | |

**Tiga hal yang dikatakan tabel ini, dan ketiganya mengubah dokumen ini.**

1. **Bayangan adalah 83% waktu GPU** (9,394 dari 11,356 ms) — dan yang besar
   bukan kaskade melainkan **atlas point/spot**, 7,977 ms sendirian. Dugaan
   sebelum pengukuran menunjuk kaskade, karena itu yang terlihat besar pada
   adegan kosong. G1 di bawah sudah ditulis ulang mengikuti angka ini.
2. **Atlas itu sedang menjatuhkan pekerjaan dan tetap semahal itu.** Log
   melaporkan `4 shadow-casting lights did not fit the atlas` setiap frame: 16
   lampu berbayang tidak muat, empat di antaranya dibuang diam-diam. Angka 7,977
   ms adalah harga untuk dua belas lampu, bukan enam belas.
3. **Pembaruan clipmap SDF di CPU 5,397 ms, sementara anggarannya 0,4 ms.**
   Tiga belas kali lipat, dan ia satu-satunya tahap terbesar di sisi CPU. Ia
   sudah punya pengukurnya sendiri (`SdfUpdateMilliseconds`, dipakai panel GI)
   tetapi tidak pernah muncul bersama tahap lain, jadi tidak ada yang melihat
   perbandingannya. Lihat catatan di G4.

Sisa ≈3,05 ms CPU belum punya lingkupnya sendiri. Itu pekerjaan pertama yang
mendahului G4: yang tidak bernama tidak bisa dipindahkan.

### Dua hal yang diminta G0 dan belum ada di adegannya

Ditulis di sini, bukan didiamkan, karena garis dasar yang lubangnya tidak
tercatat akan dikutip seolah-olah ia mengukur semuanya.

- **Material bertekstur.** Kedelapan material di `Resources/Materials` adalah
  graph berparameter tetap; tidak satu pun menyampel tekstur. Jalur tekstur
  karena itu hanya terwakili oleh set jalur mundur yang seragam, sementara
  justru keragaman set material yang menjadi alasan G5 ada. Angka G5 akan
  terlihat lebih kecil daripada seharusnya sampai adegannya punya material
  bertekstur sungguhan.
- **Karakter berkulit.** Tidak ada satu pun rig di dalam repo — yang dipakai
  menguji skinning selama ini adalah berkas Mixamo di luar repo. Jadi varian
  pipeline berkulit, unggahan buffer skin, dan biaya `ComputeSkinning` tidak ikut
  terukur sama sekali. Menyalin rig ke dalam repo adalah keputusan tersendiri —
  beberapa megabyte berkas biner beserta lisensinya — dan bukan keputusan yang
  boleh diambil diam-diam oleh sebuah adegan uji.

---

## G1 — Bayangan berhenti menggambar yang tidak terlihat · ✅ selesai

Milestone paling murah di seluruh dokumen, dan menurut garis dasar yang paling
menguntungkan sejauh ini: **9,394 dari 11,356 ms waktu GPU ada di sini.**

**Hasilnya: GPU 11,393 → 3,241 ms (−71,6%), dengan gambar yang identik piksel
per piksel.** Rincian dan cara pembuktiannya di akhir bagian ini.

### Atlas point/spot lebih dulu — 7,977 ms

Yang paling mahal, dan yang paling jelas sebabnya. Enam belas lampu berbayang
berarti sampai 8 × 6 muka point ditambah 8 muka spot; `RecordShadowPass` untuk
atlas menggambar daftar caster yang sama ke setiap muka, dan daftar itu di-cull
terhadap frustum **kamera**. Sebuah lampu berjangkauan 16 m di pojok adegan
menggambar setiap prop sejauh 80 m, enam kali.

- Cull per muka terhadap volume lampunya: frustum kerucut untuk spot, dan enam
  frustum 90° untuk point. Sebuah caster yang berada di luar jangkauan lampu
  tidak pernah muncul di peta bayangannya, jadi menggambarnya adalah pekerjaan
  yang seluruh hasilnya dibuang.
- Muka yang tidak berisi caster mana pun tidak perlu di-render sama sekali — ia
  cukup di-clear, dan `loadOp` yang sudah `CLEAR` membuat itu gratis.
- **Empat lampu sedang dibuang diam-diam** (`4 shadow-casting lights did not fit
  the atlas`, tiap frame). Itu keputusan yang benar untuk atlas yang penuh, tapi
  ia harus terlihat: yang menyalakan lampu keenam belas dan tidak mendapat
  bayangan tidak punya cara mengetahui kenapa. Sekurang-kurangnya satu angka di
  Statistics Panel, dan pemilihan mana yang dibuang berdasarkan kepentingan —
  jarak dan intensitas — bukan urutan kemunculan.

### Kaskade — 1,417 ms

**Daftar caster di-cull terhadap frustum kamera, lalu dipakai apa adanya oleh
setiap kaskade.** `Gather` menyaring dengan `Frustum(viewProj)`
(`VulkanRenderer.cpp:1712`) dan `RecordShadowPass` menggambar awalan daftar yang
sama ke ketiga kaskade. Kaskade nol — yang mungkin hanya mencakup sepuluh meter
pertama — ikut menggambar setiap caster sampai batas pandang jauh. Biayanya
dibayar tiga kali, dan dua di antaranya menghasilkan segitiga yang jatuh di luar
ortografiknya.

- Cull per kaskade terhadap ortografik kaskade itu, bukan terhadap kamera.
  Volumenya berbeda dan tidak saling memuat: benda di belakang kamera tetap bisa
  menjatuhkan bayangan ke dalam pandangan, jadi ini **bukan** sekadar mempersempit
  daftar yang ada — daftarnya harus dibangun ulang per kaskade dari seluruh
  adegan, bukan dari hasil culling kamera.
- Ini menutup cacat yang sekarang ada dan belum terlihat: caster di luar frustum
  kamera sudah dibuang sebelum pass bayangan, jadi bayangan benda yang berada di
  luar layar hilang. Yang membuatnya belum terlihat adalah adegan uji yang masih
  kosong.
- Kaskade jauh di-cache. Isi kaskade terjauh hampir seluruhnya statis; menggambar
  ulang geometri statis enam puluh kali per detik untuk menghasilkan tekstur yang
  identik adalah biaya yang dibayar tanpa imbalan. Pisahkan statis dan dinamis:
  yang statis digambar ulang hanya saat kaskadenya bergeser lebih dari ambang
  atau saat adegan berubah, yang dinamis digambar tiap frame di atasnya. Teknik
  yang sama dengan cached shadow map CryEngine.

**Kriteria selesai:** pada adegan uji G0, `shadow-atlas` turun ke bawah 2,0 ms
dan `shadow-cascades` turun secara terukur, dengan gambar yang tidak berubah
selain bayangan yang sebelumnya hilang; memutar kamera menjauh dari sebuah benda
tidak menghilangkan bayangannya; jumlah lampu yang dibuang atlas terlihat di
panel.

### Hasil

Min dari lima jalan (lihat catatan protokol di bawah), adegan uji G0, RTX 2060,
1920×1080, GI menyala. Milidetik.

| | pra-G1 | G1 | |
|---|---:|---:|---:|
| `shadow-atlas` | 8,005 | **0,485** | −93,9% |
| `shadow-cascades` | 1,420 | **0,803** | −43,5% |
| `depth-prepass` | 0,504 | 0,500 | −0,8% |
| `forward-opaque` | 0,565 | 0,561 | −0,7% |
| **total GPU** | **11,393** | **3,241** | **−71,6%** |
| `cpu-record` | 2,458 | **0,653** | −73,4% |
| `cpu-total` | 11,427 | **4,861** | −57,5% |

**Gambarnya identik, dan itu diperiksa bukan disimpulkan.** Dengan GI dimatikan,
frame terakhir lintasan pra-G1 dan G1 **sama persis di seluruh 2.073.600
piksel** — nol perbedaan, bukan "perbedaan kecil". Itu memang yang seharusnya:
yang dibuang penyaringan ini adalah segitiga yang hasilnya sudah dibuang oleh
clip.

**`cpu-record` turun karena jumlah draw call turun**, dan itu memang tujuannya.
Tiga tahap CPU lain — `cpu-sdf`, `cpu-clusters`, `cpu-gather` — juga turun
sekitar 28% **tanpa satu baris pun di antaranya disentuh**, dan itu bukan
keberhasilan yang boleh diklaim milestone ini. Dugaan yang paling cocok dengan
polanya: ketiganya menulis ke memori yang terlihat GPU, dan tulisan seperti itu
melambat ketika GPU sedang menjenuhkan bus dengan 8 ms tulisan depth ke atlas
4096². Kalau dugaan itu benar, biaya "sesungguhnya" `cpu-sdf` adalah 3,8 ms dan
bukan 5,4 ms — dan angka itulah yang harus dipakai G4, bukan angka garis dasar.
Belum diuji, dan tidak perlu diuji sekarang: yang berubah hanyalah angka
targetnya, bukan urutan pekerjaannya.

### Yang mendarat

- **`Sim/Render/DrawRun.h`** — `DrawRun` dan `SplitRuns` pindah ke header publik
  yang bebas Vulkan, mengikuti aturan yang sama dengan `Frustum`,
  `LightCluster`, dan `ShadowAtlas`: aritmetika yang memutuskan apa yang
  digambar harus bisa dijalankan tanpa kartu grafis, kalau tidak ia tidak pernah
  diuji. Lima test di `Tests/RenderTests.cpp`, termasuk yang mengunci jebakan
  yang paling mudah dibuat — memakai offset di dalam ruas sebagai indeks
  instance, yang menghasilkan instance milik ruas lain tergambar dengan material
  yang salah tanpa satu pun galat.
- **Penyaringan per muka atlas dan per kaskade**, keduanya lewat `SplitRuns`
  terhadap `Frustum` volume masing-masing.
- **Caster di luar pandangan kamera tidak lagi dibuang.** Sejak E8.1, `Gather`
  membuang setiap mesh yang tidak terlihat kamera — termasuk yang bayangannya
  jatuh ke dalam pandangan. Cacat itu tidak pernah terlihat selama adegan ujinya
  kosong. Sekarang yang dibuang hanya mesh yang tidak terlihat **dan** tidak
  menjatuhkan bayangan.
- **Kunci urut kedua: yang terlihat kamera lebih dulu.** Ditambahkan setelah
  pengukuran, bukan sebelumnya: tanpa itu, caster di luar layar tersebar di
  antara yang terlihat dan memecah ruas pandangan utama menjadi potongan pendek
  — `depth-prepass` naik 0,070 ms dan `forward-opaque` 0,044 ms, dua pass yang
  tidak punya urusan dengan bayangan sama sekali. Dengan kunci itu keduanya
  kembali ke angka pra-G1.
- **`RenderStats`** di antarmuka perender, ditampilkan Statistics Panel:
  instance buram yang digambar berbanding yang ada di buffer, jumlah caster dan
  muka bayangan, dan jumlah lampu yang dibuang atlas. Yang terakhir tadinya
  hanya sebuah `SIM_WARN` yang keluar enam puluh kali per detik — yaitu derau,
  bukan peringatan. Sekarang log hanya bicara saat angkanya berpindah.

### Dua hal yang harness-nya sendiri harus perbaiki dulu

**Gambar frame terakhir tidak deterministik, dan itu ketahuan saat dipakai.**
Perbandingan piksel pertama saya melaporkan 96 piksel berbeda dan saya nyaris
menuliskannya sebagai "bayangan yang bertambah". Ia bukan: menjalankan binary
yang **sama** dua kali menghasilkan gambar yang berbeda juga. Sebabnya perender
mengukur langkah waktunya sendiri dari jam dinding — keputusan yang benar untuk
editor, dan yang menghancurkan setiap perbandingan gambar. Sekarang ada
`ViewportDesc::fixedDeltaSeconds`, negatif secara bawaan sehingga tidak ada
pemanggil lain yang perlu tahu, dan mode ukur memaksanya ke 1/60 s. Sesudah itu:
**GI mati → tiga jalan bit-identik**; GI menyala → beda paling banyak 2 dari 255
per kanal, yang memang harus begitu karena cache radiansi merebut slotnya dengan
atomik dan urutan atomik antar-thread GPU tidak dijamin. Karena itu pembuktian
gambar dilakukan dengan GI mati, dan angka GI dibuktikan terpisah lewat waktunya.

**Satu jalan bisa tercemar, dan kriteria "dua jalan berturut" bisa tertipu.**
Sesekali sebuah jalan melaporkan setiap pass naik sekitar dua kali lipat —
termasuk `sky` dan `bloom`, yang tidak mungkin terpengaruh perubahan mana pun di
milestone ini. Penyebabnya di luar program (kemungkinan besar clock GPU), dan
yang penting bukan penyebabnya melainkan bahwa ia ada. **Protokol pengukuran
untuk seluruh milestone sesudah ini: jalankan lima kali, ambil yang terkecil per
pass.** Median di dalam satu jalan menahan hentakan satu frame; minimum antar
jalan menahan hentakan satu jalan penuh. Angka G0 di atas diambil sebelum aturan
ini ada — kebetulan keduanya sepakat, tetapi itu kebetulan.

### Yang sengaja tidak jadi dikerjakan: cache kaskade statis

Ada di rencana G1 semula, dan dicoret oleh pengukurannya sendiri. Sesudah
penyaringan per kaskade, seluruh bayangan tinggal 1,288 ms dari 3,241 ms — dan
cache statis menuntut bendera statis mengalir lewat seam perender, dua daftar
caster per kaskade, aturan pembatalan cache, serta lampiran depth yang bertahan
antar-frame. Itu pekerjaan besar dengan risiko regresi nyata untuk sebagian dari
0,803 ms, sementara `cpu-sdf` di sebelahnya berharga 3,8 ms.

**Ditinjau ulang saat E8.5 (terrain) dan E8.6 (vegetation) mendarat.** Keduanya
menambah geometri statis dalam jumlah yang mengubah timbangan ini sepenuhnya:
sebuah lanskap ber-vegetasi adalah kasus di mana hampir seluruh isi kaskade jauh
tidak pernah berubah.

---

## G2 — Yang murah dan tidak boleh menunggu · ✅ selesai

Tiga hal kecil yang tidak saling berhubungan kecuali dalam satu hal: semuanya
lebih mahal dikerjakan setelah ada yang bergantung padanya.

**Pipeline cache disimpan ke disk.** `Device.cpp:118-120` membuat cache kosong
setiap kali proses jalan dan membuangnya saat keluar, jadi setiap peluncuran
editor mengompilasi ulang seluruh pipeline dari SPIR-V. Yang terlihat adalah
tersendat beberapa ratus milidetik saat material pertama muncul di viewport, dan
itu terjadi lagi besok. Muat dari berkas di project saat `Create`, tulis di
`Destroy`. Header cache **harus divalidasi** — vendor ID, device ID, dan UUID
driver — karena cache dari driver lain bukan cache yang lambat melainkan perilaku
tak terdefinisi.

**Deklarasi fitur opsional yang punya satu tempat.** Sekarang fitur dinyalakan
dengan menempelkan baris di tengah `CreateDevice`, dan tiap milestone di bawah
menambah satu. Tanpa tempat yang jelas, empat milestone akan menaruhnya di empat
tempat dan tidak ada yang bisa menjawab "apa yang sebenarnya aktif di mesin ini".
Yang dibutuhkan: satu daftar fitur-yang-diinginkan, satu laporan fitur-yang-didapat
di log dan di panel statistik, dan satu cara bagi kode di atasnya untuk bertanya
tanpa menyentuh `VkPhysicalDeviceFeatures`. Polanya sama dengan
`supportsBlockCompression_` yang sudah ada — termasuk memperingatkan saat sesuatu
tidak ada, bukan mendiamkannya.

**Antrean kedua diminta sekarang, dipakai nanti.** Alasannya sama dengan
`dynamicRendering` yang dinyalakan di E1 padahal baru dipakai di E8: pembuatan
device adalah tempat yang tidak enak disentuh ulang, dan G7 akan menuntutnya.
Minta antrean compute khusus bila keluarga antreannya ada; catat kalau tidak.

**Kriteria selesai:** peluncuran kedua editor pada project yang sama tidak
menampilkan tersendat kompilasi pipeline; log startup menyebut daftar fitur yang
diminta beserta yang didapat; `vkCreateDevice` berhasil di mesin yang hanya punya
satu keluarga antrean.

### Yang ditemukan sambil mengerjakannya

**Cache pipeline tidak pernah menyimpan satu pun pipeline sungguhan.** Bukan
"tidak disimpan ke disk" seperti yang tercatat di tabel titik berangkat — lebih
buruk. `Device::PipelineCache()` ada dan benar, tetapi dari sebelas tempat yang
membuat graphics pipeline, **hanya `StubRenderer` yang menyerahkannya**. Sepuluh
sisanya — seluruh pipeline material, prepass, bayangan, langit, GI, post-process,
presenter, pratinjau material — memanggil `vkCreateGraphicsPipelines` dengan
`VK_NULL_HANDLE`. Cache yang tidak diberi apa pun untuk disimpan tentu saja tidak
menghemat apa pun, dan berkas 36 byte yang tertulis pada percobaan pertama —
hanya kepalanya — adalah yang membuatnya ketahuan. Sesudah kesepuluhnya
diarahkan ke cache: 554 KB.

Ini persis bentuk kesalahan yang sulit ditemukan tanpa mengukur. Semua yang
terlihat benar: kelasnya ada, pembuatannya berhasil, dan tidak ada satu pun
galat. Yang tidak ada hanyalah pemakaiannya.

### Hasil

Waktu dari peluncuran sampai frame terakhir sebuah jalan pendek, minimum dari
tiga jalan:

| | tanpa cache | dengan cache | |
|---|---:|---:|---:|
| cache shader driver **dingin** | 2.550 ms | **1.608 ms** | −37% |
| cache shader driver hangat | 1.550 ms | 1.538 ms | dalam derau |

**Dua baris, dan baris kedua sama pentingnya dengan yang pertama.** Driver NVIDIA
menyimpan hasil kompilasinya sendiri di disk, jadi begitu sebuah mesin pernah
menjalankan program ini, sebagian besar penghematannya sudah diberikan driver.
Yang tersisa untuk cache kita adalah keadaan yang justru paling terasa bagi orang
lain: mesin yang baru, driver yang baru diperbarui, dan driver yang memang tidak
punya cache sendiri. Mengukur hanya pada mesin yang cache drivernya sudah hangat
akan menyimpulkan bahwa pekerjaan ini sia-sia.

Waktu frame tidak berubah (GPU 3,241 → 3,237 ms), dan memang tidak boleh.

### Yang mendarat

- **`DeviceDesc::pipelineCachePath`** beserta pemuatan dan penyimpanannya.
  Kepalanya divalidasi terhadap vendorID, deviceID, dan `pipelineCacheUUID`
  perangkat ini sebelum diserahkan ke driver: cache dari kartu atau driver lain
  bukan cache yang lambat melainkan perilaku tak terdefinisi, dan berkas di
  folder konfigurasi bisa saja datang dari mesin lain lewat folder yang
  disinkronkan. Ditulis ke berkas sementara lalu di-`rename` — proses yang mati
  di tengah penulisan kalau tidak akan meninggalkan cache yang kepalanya sah dan
  isinya tidak.
- **Kesepuluh pemanggilan `vkCreateGraphicsPipelines`** kini memakai cache itu.
- **`DeviceCapabilities`** — satu struct, satu accessor, dua baris log. Yang
  dilaporkan mencakup fitur yang belum dipakai siapa pun (bindless, buffer device
  address, timeline semaphore, indirect, fp16, async compute), justru karena
  pertanyaan "apakah milestone berikutnya bisa dikerjakan di mesin ini" harus
  punya jawaban sebelum milestone-nya dimulai. **Di RTX 2060 ini semuanya
  tersedia**, termasuk keluarga antrean compute yang terpisah.
- **Antrean compute khusus dibuat sejak sekarang**, dipakai G7. Yang dicari
  adalah keluarga yang punya bit compute dan **tidak** punya bit grafis: keluarga
  yang punya keduanya hampir selalu keluarga grafis itu sendiri, dan dua antrean
  di atasnya tidak membeli tumpang tindih apa pun.

---

## G3 — Fondasi compute · ✅ selesai

Bukan optimisasi apa pun dengan sendirinya. Ini yang membuat G4, G5, E8.6, dan
E8.7 tidak masing-masing menemukan jalannya sendiri.

- **Build.** `cmake/SimShaders.cmake` memetakan akhiran berkas ke tahap shader
  dan hanya mengenali `.vert` dan `.frag`. Tambah `.comp`, dengan aturan yang
  sama untuk Slang maupun GLSL.
- **RHI.** Pembuatan compute pipeline di samping yang grafis, memakai
  `VkPipelineCache` yang sama, dengan bentuk yang sama supaya kode pemanggilnya
  tidak perlu tahu ia sedang membuat yang mana.
- **Frame graph.** Pass compute adalah pass biasa dengan dua perbedaan: bind
  point-nya lain, dan ia menulis ke storage image/buffer, bukan ke attachment.
  Penyimpulan barrier yang sudah ada harus mengerti keduanya. Ini bagian yang
  paling mudah salah dan paling layak diuji tanpa GPU — persis seperti test
  barrier yang sudah ada.
- **`GpuProfiler`** membungkusnya tanpa perubahan; lingkupnya sudah datar dan
  tidak peduli isi pass.

**Kriteria selesai:** satu pass compute sepele — mengisi sebuah storage image
dengan gradien lalu menampilkannya sebagai debug view — jalan lewat frame graph,
muncul di tabel profiler, dan barrier-nya lulus validation layer tanpa satu pun
peringatan. Test barrier bertambah untuk transisi baca-tulis compute.

### Yang ditemukan sambil mengerjakannya

**Build tidak perlu diubah sama sekali, dan kalimat pertama milestone ini
salah.** Tabel titik berangkat menulis bahwa `cmake/SimShaders.cmake` "memetakan
akhiran berkas ke tahap shader dan hanya mengenali `.vert` dan `.frag`". Ia tidak
memetakan apa pun: yang menyimpulkan tahap adalah glslc, dari akhiran berkasnya,
dan Slang, dari atribut `[shader("...")]` di entry point-nya. `foo.comp.slang`
menempuh jalur yang sama persis dan menghasilkan `foo.comp.spv` tanpa satu baris
CMake pun berubah. Yang ditambahkan hanya komentar — supaya yang berikutnya tidak
menambahkan cabang khusus untuk masalah yang tidak ada.

Ini bentuk kesalahan yang berlawanan dengan yang ditemukan G2. Di sana sesuatu
tampak ada dan ternyata tidak dipakai; di sini sesuatu tampak harus dikerjakan
dan ternyata sudah beres. Keduanya berasal dari sumber yang sama: dokumen ini
ditulis dari membaca kode, dan membaca kode tidak sama dengan menjalankannya.

**`RWTexture2D<float4>` tanpa format menuntut fitur perangkat yang tidak diminta
siapa pun.** Slang memancarkan `StorageImageWriteWithoutFormat` untuk storage
image yang formatnya tidak disebut, dan kapabilitas itu menuntut
`shaderStorageImageWriteWithoutFormat` — yang tidak ada di daftar fitur
`CreateLogicalDevice`. Kegagalannya akan muncul sebagai shader yang ditolak
driver, jauh dari sebabnya, dan tidak pada setiap driver. `[format("rgba8")]` di
atas deklarasinya menghapus kedua kapabilitas itu dari SPIR-V yang dihasilkan.
Diperiksa dengan `spirv-dis`, bukan diandaikan.

**Validasi sinkronisasi tidak menyala lewat variabel lingkungan, dan diamnya
sempurna.** `VkValidationFeaturesEXT` di `pNext` hanya dibaca bila ekstensi
`VK_EXT_validation_features` ikut diminta — dan ekstensi itu disediakan **oleh
lapisan validasi**, jadi `vkEnumerateInstanceExtensionProperties(nullptr, …)`
tidak pernah menyebutkannya. Percobaan pertama karena itu menanyakannya ke daftar
yang salah, menjawab "tidak ada", dan mematikan diri tanpa memberi tahu siapa
pun. Yang membuatnya ketahuan bukan sebuah galat melainkan satu kata di baris log
startup: `(validation on)` tanpa `, sync`.

**Dan sesudah menyala, ia tetap tidak melaporkan apa pun — termasuk pada
percobaan yang sengaja dirusak.** Sisi-tunggu setiap barrier dibuang
(`srcStageMask` menjadi `TOP_OF_PIPE`, `srcAccessMask` menjadi `NONE`), sehingga
layout tetap berpindah dengan benar tetapi tidak ada satu pun yang menunggu.
Lapisannya tidak mengeluarkan satu pun `SYNC-HAZARD`, lewat ketiga jalur yang
dicoba: `VkValidationFeaturesEXT`, `VK_LAYER_VALIDATE_SYNC=1`, dan berkas
`vk_layer_settings.txt`. **Jadi jalurnya ada dan diterima lapisan, tetapi belum
pernah terlihat menangkap apa pun di mesin ini** — dan sampai ada yang melihatnya
menangkap sesuatu, ia belum boleh dihitung sebagai jaring pengaman. Yang
benar-benar menjaga G3 karena itu adalah validasi biasa, dan itu diperiksa
dengan cara yang sama: mencabut deklarasi `Write` pass compute membuatnya
langsung berteriak (`VUID-vkCmdDraw-imageLayout-00344`, tiap frame), jadi barrier
yang disimpulkan graph memang barrier yang sedang bekerja.

### Hasil

RTX 2060, 1920×1080, GI menyala, 120 frame pemanasan + 240 diukur, dua jalan tiap
baris. Median, milidetik.

| | GPU total | CPU total |
|---|---:|---:|
| debug view **mati** | 3,243 / 3,241 | 5,003 / 5,016 |
| debug view **menyala** | 3,327 / 3,324 | 4,972 / 4,896 |

**Baris pertama sama dengan garis dasar G2 (3,241 ms), dan itu yang harus
dilaporkan lebih dulu.** Aturan barrier yang baru berlaku untuk `ShaderWrite`,
dan sebelum pass ini tidak ada satu pun pass yang mendeklarasikannya — jadi
fondasi compute tidak boleh memindahkan satu pun angka, dan tidak memindahkannya.

Selisih baris kedua **+0,083 ms**, dan dua pass barunya berjumlah persis
0,083 ms (`compute-gradient` 0,034 + `compute-gradient-blit` 0,049). Keduanya
cocok sampai digit terakhir: yang bertambah di total adalah yang terbaca di dua
baris itu, bukan biaya tersembunyi di tempat lain.

Gambarnya menutupi 1920×1080 penuh sampai ke tepi kanan dan bawah — yaitu bukti
`GroupCount` dan uji batas kernel keduanya benar, karena salah satunya salah akan
menyisakan pita hitam selebar kurang dari satu grup di dua tepi itu saja.
Diperiksa lewat dua jalan yang berbeda: `--bench-capture` di headless, dan
sakelar "Compute path (gradient)" di panel Statistics editor. Yang kedua bukan
pengulangan — jalur headless memakai `ViewportDesc` yang disusun harness ukur,
sedangkan yang di editor menempuh `EditorContext` dan panel Viewport, dan sebuah
sakelar yang tidak tersambung tampak persis sama dengan sakelar yang tersambung
ke pass yang gagal dibuat.

### Yang mendarat

- **`rhi::ComputePipeline`** (`Code/RHI/include/Sim/RHI/Pipeline.h`) — layout,
  pipeline, dan `Bind` yang tidak menawarkan bind point sebagai pilihan.
  Mengikat pipeline compute di bind point grafis bukan galat kompilasi melainkan
  dispatch yang menjalankan pipeline lain, dan itu satu-satunya perbedaan yang
  benar-benar terlihat di sisi perekam. Cache-nya `VkPipelineCache` milik
  `Device` — yang sama dengan kesepuluh pipeline grafis sejak G2.
- **`rhi::LoadShaderModule`**, di satu tempat. Modul Render punya lima salinan
  fungsi yang sama; pemakai compute berikutnya akan melahirkan yang keenam.
  Kelimanya sengaja **belum** dialihkan ke sini — itu perubahan mekanis di lima
  berkas yang tidak ada hubungannya dengan compute, dan menumpangkannya di sini
  membuat diff milestone ini tidak bisa dibaca.
- **`rhi::GroupCount`**, beserta test-nya. Ditulis `(count - 1) / groupSize + 1`
  dan bukan `(count + groupSize - 1) / groupSize`: yang kedua meluap pada count
  besar dan menghasilkan **nol** grup, yaitu dispatch yang diam-diam tidak
  mengerjakan apa pun. Dikunci test pada `0xFFFFFFFF`.
- **Barrier tulis-setelah-tulis untuk storage.** Graph memancarkan barrier
  `ShaderWrite → ShaderWrite` walaupun keadaannya tidak berpindah. Vulkan tidak
  menjanjikan urutan apa pun antara dua perintah, dan keluaran compute tidak
  lewat tahap fungsi-tetap yang punya urutannya sendiri seperti lampiran warna —
  jadi dua dispatch berurutan atas storage yang sama bisa berjalan berbarengan.
  Barrier yang tampak kosong itu satu-satunya yang menutupnya. Lampiran warna dan
  depth beruntun sengaja **tidak** ikut: menambahkan barrier di antara setiap pass
  adegan adalah perubahan biaya yang harus diukur sendiri, bukan efek samping
  milestone compute.
- **`VkBufferMemoryBarrier2` di eksekutor.** Buffer tidak punya layout, jadi ia
  tidak bisa lewat jalur yang sama dengan image. Belum ada pass yang memakainya:
  pemakai pertamanya penetapan lampu ke cluster di G4. Ada sejak sekarang dengan
  alasan yang sama seperti antrean compute di G2 — jalur yang ditambahkan
  bersama pemakai pertamanya adalah jalur yang dirancang untuk satu pemakai.
- **`Shaders/debug_gradient.comp.slang` dan `ComputeGradient`**, beserta sakelar
  "Compute path (gradient)" di panel Statistics dan `--bench-compute` di
  `SimHeadless`. Dua pass: dispatch menulis storage image dalam `GENERAL`, lalu
  segitiga penutup layar membacanya sebagai tekstur — persis transisi yang akan
  dituntut Hi-Z, clipmap SDF, dan penetapan cluster di G4. Ia tetap ada sesudah
  pemakai sungguhan datang, dengan alasan yang sama seperti pemilih backend GI
  yang terlihat sejak M0: jalur yang tidak punya cara memeriksa dirinya sendiri
  adalah jalur yang diam-diam berhenti bekerja di mesin orang lain.
- **`DeviceDesc::enableSyncValidation` dan `--validate-sync`.** Statusnya
  tertulis apa adanya di atas: diminta, diterima lapisan, dilaporkan di log
  startup — dan belum pernah terlihat menangkap apa pun.
- **Empat test barrier compute** di `Tests/RenderTests.cpp`: transisi
  tulis-lalu-baca, tulis-setelah-tulis yang tidak boleh dilewatkan, dua pembacaan
  yang tidak boleh dipisahkan (tanpa yang ini, "selalu pancarkan barrier" ikut
  lulus), dan pass compute yang keluarannya tidak dibaca siapa pun tetap dibuang.

**Satu galat lama yang tercatat dan bukan milik G3.** `vkDestroyDevice`
melaporkan tujuh objek yang tidak dibebaskan — query pool, satu shader module,
satu pipeline beserta layout-nya, dan satu descriptor pool/set/layout. Angkanya
persis sama dengan `ComputeGradient` dimatikan seluruhnya, jadi ia sudah ada
sebelum milestone ini. Dicatat di sini supaya jalan validasi berikutnya tidak
mengira tujuh itu baru.

---

## G4 — Pemakai pertama compute · ✅ selesai

Tiga pekerjaan yang sudah ada dan bentuknya memang bentuk compute. Ketiganya
terukur sendiri-sendiri dan tidak mengubah apa pun yang terlihat di layar —
sehingga kalau gambarnya berubah, itu bug, bukan selera. Yang pertama di bawah
adalah yang terbesar di seluruh sisi CPU; dua sisanya kecil dan dipilih justru
karena kecil, sebagai pembuktian jalur sebelum yang besar melewatinya.

**Hi-Z.** `DepthPyramid::Record` membuka `vkCmdBeginRendering`, menggambar satu
segitiga layar penuh, dan memasang barrier — **per tingkat mip**
(`DepthPyramid.cpp:356-406`). Pada 1080p itu sekitar sebelas render pass dan
sebelas barrier setiap frame untuk menghasilkan satu piramida. Satu dispatch
bergaya single-pass downsampler menghasilkan piramida yang sama dengan satu
barrier di ujungnya. Nilai yang direduksi tetap sama dan reversed-Z tetap berarti
`min`/`max` yang sama; yang berubah hanya cara menempuhnya.

**Komposit clipmap SDF — 5,397 ms, tiga belas kali anggarannya.** Yang terbesar
di sisi CPU dan, sesudah bayangan, yang terbesar di seluruh frame.
`SdfClipmap::Update` menyusun ulang lempeng voxel dari mesh adegan setiap kali
kamera bergeser, dan lintasan G0 memang menggerakkan kamera tiap frame — yaitu
keadaan normal sebuah game, bukan kasus terburuk yang dibuat-buat. Rencana GI
sendiri sudah menyebutnya sebagai pekerjaan yang "masih berjalan di CPU sampai
ia pindah ke compute"; garis dasar memberi angka pada kata "sampai" itu.
Voxelisasi adalah pekerjaan yang bentuknya persis bentuk compute: satu voxel per
thread, tanpa ketergantungan antar-voxel.

**Penetapan lampu ke cluster.** `AssignLights` (`VulkanRenderer.cpp:3695`)
menjalankan 16×9×24 = 3.456 uji bola-terhadap-kotak di CPU setiap frame, lalu
mengunggah hasilnya. Ini pekerjaan yang embarrassingly parallel dan sedang
dikerjakan satu core. `ClusterGrid` sengaja bebas Vulkan dan **tetap begitu**:
yang pindah ke GPU adalah penetapannya, sementara pembangunan kisi dan seluruh
test tanpa-GPU-nya tinggal di tempatnya. Jalur CPU dipertahankan sebagai jalur
mundur dan sebagai pembanding — dua implementasi yang harus sepakat adalah cara
termurah menemukan yang mana yang salah.

**Kriteria selesai:** gambar sebelum dan sesudah identik piksel per piksel pada
adegan uji (kecuali beda pembulatan float yang bisa dijelaskan); `cpu-sdf` turun
ke bawah anggaran 0,4 ms rencana GI; `hiz-build` dan `cpu-clusters` di tabel
profiler turun; sakelar jalur CPU masih menghasilkan gambar yang sama.

### Garis dasar G4

Diambil ulang sesudah G3, pada build yang sama, dua jalan. RTX 2060, 1920×1080,
GI menyala, 120 + 240 frame. Median, milidetik.

| | jalan 1 | jalan 2 |
|---|---:|---:|
| **total GPU** | 3,238 | 3,239 |
| `hiz-build` | 0,070 | 0,070 |
| **total CPU** | 4,999 | 4,948 |
| `cpu-sdf` | 3,926 | 3,855 |
| `cpu-clusters` | 0,345 | 0,340 |

**`cpu-sdf` di sini 3,9 ms, bukan 5,397 ms seperti di tabel G0.** Selisihnya
tidak dijelaskan oleh satu pun perubahan sejak itu — G1 menyentuh bayangan, G2
menyentuh pipeline cache, G3 tidak menyentuh apa pun di jalur ini. Yang paling
mungkin: garis dasar G0 diambil pada mesin yang sedang mengerjakan hal lain, atau
pada keadaan termal yang berbeda. Dicatat apa adanya, dan angka **di atas** yang
dipakai sebagai baris "sebelum" G4 — karena hanya angka yang diambil pada build
yang sama, berturut-turut, yang boleh dibandingkan.

### Hi-Z: dipindahkan ke compute, diukur, lalu dikembalikan

**Premisnya tidak selamat dari pengukuran.** Bagian ini menuduh sebelas render
pass sebagai biayanya. Yang sebenarnya mahal adalah penderetan antar-tingkat,
dan compute membayarnya sedikit lebih mahal daripada render pass.

Port-nya utuh dan berjalan: dua shader compute (`hiz_copy`, `hiz_reduce`), rumus
reduksi disalin baris demi baris termasuk aturan texel terakhir, seluruh mip
tinggal di `GENERAL` sepanjang pembangunan sehingga pemisah antar-tingkat cukup
`VkMemoryBarrier2`. Sebelas render pass dan dua puluh dua barrier menjadi sebelas
dispatch dan sepuluh barrier. Gambarnya tidak berubah di luar derau frame ke
frame renderer sendiri.

| | median, ms |
|---|---:|
| grafis (jalur lama) | **0,070** |
| compute, 11 dispatch + 10 barrier | 0,108 |
| compute, sumber dicuplik alih-alih dibaca sebagai storage | 0,109 |
| compute, hanya tingkat nol | 0,036 |
| compute, 11 dispatch **tanpa** barrier (batas bawah, hasilnya salah) | 0,090 |

Dibaca dari bawah ke atas: tingkat nol memindahkan 2,07 juta texel dan berharga
0,036 ms — terikat bandwidth, dan compute kehilangan kompresi lampiran warna yang
dinikmati jalur grafis. Sepuluh tingkat sisanya seluruhnya hanya 0,69 juta texel
tetapi berharga 0,054 ms bahkan **tanpa satu pun barrier**: sekitar 5,4 µs per
dispatch, yang bukan pekerjaan melainkan biaya tetap peluncuran. Barrier-nya
sendiri hanya 1,8 µs masing-masing. Jalur grafis membayar sekitar 4,5 µs per
tingkat untuk hal yang sama.

**Sebuah single-pass downsampler pun tidak menyelamatkannya, dan alasannya ada
di dalam rumus reduksi ini sendiri.** Aturan "texel terakhir merangkum sisa
barisnya" membuat sebuah texel di tingkat N+1 kadang bergantung pada tiga texel
sumber alih-alih dua — dan pada ukuran ganjil, ketiganya bisa jatuh di dua ubin
workgroup yang berbeda. Reduksi berbagi-memori tidak bisa menjangkaunya. Yang
bisa: menghitung tingkat N+2 langsung dari tingkat N (maks bersifat asosiatif dan
idempoten, jadi hasilnya identik) — tetapi pembacaan berlebihnya berharga lebih
mahal daripada dispatch yang dihemat di puncak piramida, dan di ekornya
penghematannya berhenti di sekitar 0,065 ms. Yaitu di dalam derau.

Jadi jalur grafisnya dikembalikan, dan yang tersisa dari pekerjaan ini adalah
tabel di atas. Aturan dokumen ini sendiri yang memutuskannya: *"optimisasi tanpa
dua angka itu bukan optimisasi melainkan perubahan kode yang kebetulan terasa
lebih cepat."* Kalau kelak `hiz-build` benar-benar mengganggu, yang harus
diserang adalah jumlah tingkatnya — bukan cara menempuh tiap tingkat.

### Penetapan lampu ke cluster: pindah ke compute · ✅

**Bagian ini justru lebih besar daripada yang tertulis.** `cpu-clusters` 0,344 ms,
dan pemecahannya menjadi tahap bernama — pekerjaan yang memang harus mendahului
pemindahan apa pun — menunjukkan penetapannya sendiri 0,328 ms dari jumlah itu:
95%. Alokasi atlas bayangan 0,009 ms, unggahan buffer 0,005 ms.

| | jalur CPU | jalur GPU |
|---|---:|---:|
| `cpu-clusters` | 0,343 / 0,342 | **0,012 / 0,012** |
| `cluster-assign` (GPU) | — | 0,009 / 0,009 |
| **total CPU** | 4,970 / 4,984 | **4,655 / 4,634** |
| **total GPU** | 3,239 / 3,249 | 3,247 / 3,246 |

0,33 ms pekerjaan CPU ditukar dengan 0,009 ms pekerjaan GPU, dan total GPU tidak
bergerak — yaitu dispatch-nya tenggelam di dalam derau.

**Gambarnya identik byte per byte, dan itu bukan "di dalam derau".** Dengan GI
dimatikan, perender ini deterministik: dua jalan menghasilkan berkas PNG dengan
md5 yang sama. Dan pada keadaan itu, jalur CPU dan jalur GPU menghasilkan gambar
yang **nol pikselnya berbeda**. Penetapan cluster tetap menentukan seluruh lampu
punctual dengan GI mati, jadi uji itu justru menguji tepat yang berubah — tanpa
akumulasi temporal GI yang membuat dua jalan mana pun berselisih beberapa ribu
piksel sebesar satu LSB.

**Satu bug ditemukan validation layer, dan bentuknya patut dicatat.**
`ClusterAssign` menyimpan buffer keluaran per slot frame dan saya menulis
`kSlots = 2`, mengikuti `Swapchain::kFramesInFlight`. `VulkanRenderer` punya
**tiga** slot. Selisih itu tidak menghasilkan galat apa pun di sisi C++: ia
menghasilkan pembacaan di luar batas larik slot, yaitu `VkBuffer` sampah yang
diserahkan ke descriptor — dan yang terlihat adalah `VK_ERROR_DEVICE_LOST`
beberapa detik kemudian, tanpa satu pun petunjuk. Sekarang dikunci
`static_assert` di `VulkanRenderer`, tempat kedua angka itu bisa dilihat
bersamaan.

### Yang mendarat

- **`ClusterAssign`** (`Code/Render/src/ClusterAssign.{h,cpp}`) beserta
  `Shaders/cluster_assign.comp.slang` — satu thread per cluster, 3.456 cluster
  dalam satu dispatch, buffer keluaran device-local per slot. Keluarannya
  berjarak tetap alih-alih padat: tiap cluster memiliki blok selebar
  `maxLightsPerCluster` miliknya sendiri, sehingga tidak ada satu pun atomic di
  jalur utama. Yang dibaca fragment shader tetap `uint2(offset, count)` yang
  sama, jadi `cluster_common.slang` tidak berubah satu baris pun.
- **Pemakai pertama barrier buffer di eksekutor**, persis seperti yang
  diperkirakan G3. Dua resource graph (`cluster-ranges`, `cluster-indices`)
  diimpor sebagai buffer; graph menyimpulkan `ShaderRead → ShaderWrite` sebelum
  dispatch dan `ShaderWrite → ShaderRead` sebelum pass forward, dan
  memancarkannya sebagai `VkBufferMemoryBarrier2`.
- **`ClusterGrid::TanHalfX/TanHalfY/NearZ/FarZ`**, beserta test-nya. Shader harus
  memakai angka yang **sudah dijepit** `Build`; jalur GPU yang membaca angka
  sebelum penjepitan menghitung kotak cluster yang berbeda pada kamera ekstrem,
  dan selisihnya muncul sebagai lampu yang hilang hanya pada fov tertentu.
- **Tiga tahap CPU baru yang bernama** — `cpu-shadow-atlas`,
  `cpu-cluster-assign`, `cpu-cluster-upload`. Ditambahkan sebelum memindahkan
  apa pun, karena yang tidak bernama tidak bisa dipindahkan; dan yang pertama
  dijawabnya adalah "berapa bagian dari 0,344 ms ini yang benar-benar
  penetapan".
- **Sakelar "GPU cluster assignment"** di panel Statistics dan
  `--bench-cpu-clusters` di `SimHeadless`. Berpindah jalur menunggu device
  menganggur lalu menulis ulang dua binding descriptor — bukan
  `WriteShadowDescriptors`, yang mengalokasi set baru dari pool yang hanya cukup
  untuk satu putaran.
- **Peringatan pemotongan cluster tetap ada di kedua jalur.** Di GPU angkanya
  ditulis shader lewat `InterlockedAdd` ke satu `uint` host-visible dan dibaca
  CPU satu frame belakangan — pada slot yang fence-nya memang sudah ditunggu.
  Dilaporkan hanya saat angkanya berubah, alasan yang sama dengan lampu yang
  tidak muat atlas.

### Komposit clipmap SDF: pindah ke compute · ✅

**Yang terbesar dari ketiganya.** Sesudah cluster pindah, `cpu-sdf` bukan lagi
salah satu tahap termahal melainkan **84% dari seluruh sisi CPU**.

| | jalur CPU | jalur GPU |
|---|---:|---:|
| `cpu-sdf` | 3,923 / 3,889 | **0,079 / 0,079** |
| `sdf-fill` (GPU) | 0,066 / 0,066 | 0,315 / 0,315 |
| **total CPU** | 4,710 / 4,629 | **3,220 / 3,234** |
| **total GPU** | 3,326 / 3,325 | 3,579 / 3,577 |

**0,25 ms pekerjaan GPU ditukar dengan 3,81 ms pekerjaan CPU.** Dan yang lebih
penting daripada rasio itu: frame ini tadinya terikat CPU (4,63 lawan 3,33 ms) dan
sekarang terikat GPU (3,23 lawan 3,58). Batas atasnya turun dari 4,63 ke 3,58 ms.

`cpu-sdf` yang tersisa — 0,079 ms — bukan sisa evaluasi voxel melainkan
`BoxSceneField::Build` (membalik 240 matriks), penyalinan entri ke bentuk yang
dibaca shader, dan perhitungan wilayah geser. **Anggaran 0,4 ms rencana GI
terpenuhi dengan sisa lima kali lipat**, dan itu yang membuka jalan ke 128³ yang
diminta rencana itu — keputusan yang sekarang punya angka untuk bersandar.

**Baris `sdf-fill` yang muncul di kedua kolom itu sendiri sebuah koreksi.**
Sebelum G4, salinan staging clipmap direkam di luar setiap lingkup ukur — 0,066
ms yang tidak ada di tabel mana pun. Kompositnya pindah ke tempat yang sama, jadi
tanpa lingkup baru ini, 0,315 ms pekerjaan GPU akan tampak sebagai penghematan
CPU yang gratis. Pekerjaan yang tidak muncul di tabel mana pun adalah pekerjaan
yang dianggap gratis.

### Bagaimana ia diperiksa, dan kenapa bukan dengan gambar

**Perbandingan gambar tidak bisa menjawab pertanyaannya.** Satu-satunya pembaca
kaskade SDF adalah penelusuran GI, dan GI yang mematikan determinisme perender —
dua jalan dari binary yang sama berselisih ratusan piksel sebesar satu LSB, kadang
lebih. Kaskade juga hanya diperbarui ketika GI menyala, jadi jalan keluar yang
dipakai uji cluster — matikan GI, bandingkan gambar — tertutup di sini.

Yang dibandingkan karena itu **isi voxelnya**. `Texture3D::Readback` dan
`--bench-dump-sdf` menyalin seluruh isi ketiga kaskade ke berkas; dua jalan
dengan lintasan kamera yang sama, satu di tiap jalur, lalu dibandingkan.

| lintasan | jalur CPU | jalur GPU |
|---|---|---|
| 40 + 40 frame | `20615bab…` | `20615bab…` |
| 240 + 120 frame | `17916619…` | `17916619…` |

786.432 voxel, **identik byte demi byte pada kedua lintasan**. Dan kedua lintasan
menghasilkan berkas yang berbeda satu sama lain — kontrol yang diperlukan, karena
dua berkas identik yang isinya tidak bergantung pada apa pun juga akan lulus uji
di atas.

Sebagai pemeriksaan kedua, gambar dengan GI menyala: jalur CPU melawan jalur GPU
berselisih 52 piksel sebesar satu LSB, sementara jalur CPU melawan **dirinya
sendiri** pada jalan kedua berselisih 118 piksel sampai 23 LSB. Selisih antar-jalur
lebih kecil daripada derau perender itu sendiri — persis yang diperkirakan dari
voxel yang identik.

### Yang mendarat

- **`Shaders/sdf_fill.comp.slang`** — satu voxel per thread, salinan
  `BoxSceneField::Row` dan penyandian `SdfVolume::WriteBoxRows` baris demi baris.
  Dispatch-nya satu dimensi dan indeksnya dibongkar sendiri: kotak yang harus
  diisi hampir selalu lempeng tipis seperti 1×56×9, dan grup kerja 3D atas bentuk
  seperti itu membuang sebagian besar thread-nya di sumbu yang tebalnya satu.
- **Pembuangan per voxel, bukan per baris — dan hasilnya tetap identik.** Sebuah
  entri yang lolos uji baris tetapi gagal uji voxel pasti berjarak lebih jauh
  daripada pita, dan nilai di luar pita menjenuh ke 255 pada kedua jalur. Yang
  berbeda hanya kapan pekerjaannya dibuang.
- **Tidak ada barrier di antara dispatch.** `SplitWrapped` menjamin tidak ada dua
  kotak yang menyentuh texel yang sama dan tidak satu pun membaca tulisan yang
  lain, jadi belasan dispatch kecil per frame berjalan bersamaan. Yang tetap ada
  hanya dua perpindahan layout per kaskade, di kedua ujungnya.
- **`Texture3D` bisa dibuat sebagai storage image, dan bisa dibaca balik.**
  Keduanya diminta, bukan dinyalakan diam-diam: usage storage bisa mematikan
  kompresi tekstur, dan `Readback` menunggu queue idle. `SupportsStorage`
  menanyakan `VK_FORMAT_R8_UNORM` ke perangkat — ia **bukan** format storage yang
  diwajibkan spesifikasi — dan jalur CPU yang dipakai bila jawabannya tidak.
- **`Texture3D::RecordTransition` menurunkan tahap dari layout**, bukan dari arah
  perpindahannya. Bentuk sebelumnya menjawab satu pertanyaan biner — "apakah
  tujuannya TRANSFER_DST" — yang benar selama hanya ada dua layout; kaskade yang
  diisi compute menambahkan yang ketiga.
- **`BoxSceneField::GpuEntry`**, terpisah dari `Entry` yang dipakai jalur CPU.
  Yang di dalam diatur demi CPU — `Vec3` rapat tanpa padding — sementara std430
  menuntut setiap vektor sejajar enam belas byte. Menyerahkan yang pertama
  sebagai yang kedua adalah bug tanpa galat: jarak yang salah mulai dari mesh
  kedua.
- **Sakelar "GPU SDF composite"** di panel Statistics, `--bench-cpu-sdf` dan
  `--bench-dump-sdf` di `SimHeadless`.
- **Satu kebocoran lama ikut tertutup.** `sdfClipmap_.Destroy()` tidak pernah ada
  di `Shutdown`, jadi ketiga tekstur kaskade termasuk di antara tujuh objek yang
  dilaporkan `vkDestroyDevice`. Komposit compute menambahkan pipeline, descriptor
  pool, dan tiga buffer entri ke tumpukan yang sama — dan tumpukan yang sudah
  bocor adalah tempat kebocoran baru bersembunyi tanpa terlihat.

### Hasil G4 secara keseluruhan

Dari garis dasar G4 ke keadaan sesudahnya, adegan dan lintasan yang sama:

| | sebelum | sesudah |
|---|---:|---:|
| **total CPU** | 4,999 | **3,234** |
| `cpu-sdf` | 3,926 | 0,079 |
| `cpu-clusters` | 0,345 | 0,012 |
| **total GPU** | 3,238 | 3,579 |
| `sdf-fill` | 0,066 | 0,315 |
| `cluster-assign` | — | 0,009 |
| `hiz-build` | 0,070 | 0,070 |

**Yang menentukan waktu frame berpindah sisi.** Sebelumnya CPU 5,0 ms melawan
GPU 3,2; sekarang CPU 3,2 melawan GPU 3,6. Yang harus diserang berikutnya karena
itu bukan lagi sisi CPU — dan G5 dan G6, yang keduanya menyerang sisi CPU
perekaman, harus dibaca ulang dengan angka ini di tangan.

---

## G5 — Bindless

Prasyarat G6, dan berdiri sendiri sebagai kemenangan CPU.

**Sekarang setiap ruas mesh mengikat descriptor set-nya sendiri.** `DrawRuns`
(`VulkanRenderer.cpp:2033-2145`) memanggil `vkCmdBindDescriptorSets` untuk set 2
dan `vkCmdPushConstants` di dalam gelung ruas, dan `vkCmdBindVertexBuffers` di
dalam gelung run. Untuk adegan berisi itu berarti ribuan ikatan per frame — dan
ketiga kaskade bayangan mengulangi sebagiannya.

- `VK_EXT_descriptor_indexing` (inti sejak Vulkan 1.2): satu larik tekstur besar
  yang tidak berbatas, diindeks dari data material. Set material per ruas hilang;
  yang tersisa satu ikatan di awal pass.
- Data material pindah ke storage buffer yang diindeks, bukan ke push constant
  per ruas. Push constant lalu hanya membawa apa yang benar-benar berubah per
  draw, atau hilang sama sekali begitu G6 mendarat.
- **Jalur mundur wajib dan wajib diuji.** Perangkat tanpa `descriptorIndexing`
  memakai jalur set-per-ruas yang sekarang. Sakelar paksa yang terlihat, dengan
  alasan yang sama dengan pemilih backend GI: tanpa itu, jalur mundur berhenti
  dijalankan siapa pun pada hari bindless menyala.
- Umur descriptor tetap masalah lama yang sama — pembebasan ditunda sebanyak
  frame in-flight (`kFramesInFlight = 2`). Larik bindless membuatnya lebih mudah
  salah, bukan lebih mudah benar: slot yang dipakai ulang terlalu cepat
  menampilkan tekstur milik benda lain, dan tidak ada galat yang menyertainya.

**Kriteria selesai:** jumlah `vkCmdBindDescriptorSets` per frame pada adegan uji
turun dari ribuan ke puluhan (dihitung, bukan ditaksir); waktu CPU perekaman
turun; kedua jalur menghasilkan gambar yang sama.

---

## G6 — GPU-driven: indirect draw dan occlusion culling dua fase

Ini bagian yang benar-benar "terinspirasi CryEngine", dan satu-satunya yang tidak
boleh dikerjakan sebelum lima milestone di atasnya selesai.

**Sekarang CPU memutuskan setiap frame apa yang digambar.** `Gather` menyalin,
memfrustum-cull, dan `stable_sort` seluruh instance setiap frame
(`VulkanRenderer.cpp:1694-1826`). Biayanya tumbuh linear terhadap isi adegan, dan
ia dibayar di thread yang juga harus merekam command buffer.

- Frustum culling pindah ke compute, menghasilkan buffer perintah untuk
  `vkCmdDrawIndexedIndirectCount`. Butuh `multiDrawIndirect` dan
  `drawIndirectCount` — dua fitur di baris tabel G2.
- Occlusion culling dua fase memakai `hiz_` yang **sudah dibangun tiap frame dan
  belum ada yang memakainya untuk ini**: fase satu menggambar yang terlihat frame
  lalu, piramida dibangun dari hasilnya, fase dua menguji sisanya terhadap
  piramida itu dan menggambar yang lolos. Dua fase, bukan satu, karena piramida
  dari frame lalu salah persis di tempat yang paling merugikan — saat kamera
  berputar cepat, benda yang baru masuk pandangan dianggap tertutup dan berkedip
  masuk satu frame terlambat.
- Sortir per material hilang dengan sendirinya begitu material diindeks
  (G5). Yang tersisa adalah sortir tembus pandang, dan itu memang harus tetap
  berurutan jarak — alasannya sudah tertulis di `Gather` dan tidak berubah.
- **Yang tidak ikut:** transparan, dan objek yang jumlahnya sedikit tapi
  materialnya unik. GPU-driven menang pada ribuan draw yang seragam, dan kalah
  pada belasan draw yang tiap satunya berbeda karena biaya penyiapannya tetap.

**Kriteria selesai:** adegan uji dengan ribuan instance menggambar jumlah segitiga
yang lebih sedikit dengan gambar yang sama; waktu CPU `Gather` mendekati nol;
memutar kamera cepat tidak menghasilkan benda yang berkedip masuk.

---

## G7 — Async compute

Terakhir di antara yang struktural, karena tanpa G3–G6 tidak ada yang bisa
ditumpangkan.

Yang menganggur pada renderer satu antrean adalah unit yang tidak dipakai pass
yang sedang jalan: pass bayangan menghabiskan rasterizer dan hampir tidak
menyentuh ALU; pass GI dan post-process kebalikannya. Menjalankan keduanya
bersamaan pada antrean berbeda adalah cara mendapatkan waktu yang tidak bisa
didapat dengan mempercepat salah satunya.

- Kandidat pertama: pembangunan Hi-Z dan penetapan cluster (keduanya sudah
  compute sejak G4) ditumpangkan di atas pass bayangan.
- Kandidat kedua: penelusuran GI dan pembaruan clipmap SDF ditumpangkan di atas
  geometry pass frame berikutnya.
- Sinkronisasinya lewat timeline semaphore, bukan biner. Semaphore biner memaksa
  satu penunggu per sinyal, dan ketergantungan lintas-antrean yang sebenarnya
  berbentuk graf harus dipaksa jadi rantai.
- **Ini bagian yang paling mudah menghasilkan bug yang tidak bisa direproduksi.**
  Balapan lintas-antrean muncul sebagai kedipan di satu GPU dan tidak pernah di
  GPU lain, dan validation layer tidak melihatnya. Konsekuensinya: sakelar mati
  yang mengembalikan semuanya ke satu antrean wajib ada dan wajib dipakai
  membandingkan, dan penyimpulan barrier frame graph harus mengerti kepemilikan
  antrean — bukan diurus tangan di tempat pemanggilan.

**Kriteria selesai:** total waktu GPU per frame turun sementara jumlah waktu tiap
pass tidak berubah — yaitu bukti bahwa yang didapat adalah tumpang tindih, bukan
pekerjaan yang hilang; mematikan sakelarnya mengembalikan angka lama dan gambar
yang identik.

---

## G8 — Resolusi dinamis

Yang membuat anggaran di kepala dokumen ini realistis pada garis dasar 1660
Super, dan yang tidak bisa dikerjakan sebelum TAA ada di E8.8 — resolusi yang
berubah tanpa akumulasi temporal menghasilkan ketajaman yang berdenyut, dan itu
lebih mengganggu daripada frame rate yang turun.

- Skala resolusi render mengikuti waktu GPU frame-frame terakhir, dengan histeresis
  supaya ia tidak berosilasi. Target diambil dari anggaran, bukan dari fps yang
  terukur: mengejar 60 fps membuat skalanya turun tepat setelah terlambat.
- `RenderTarget` sudah mengalokasi lebih besar daripada yang digambar dan hanya
  membangun ulang saat melewati ambang — sifat yang ada supaya menyeret pemisah
  dock tidak mengalokasi ulang. Sifat itu persis yang dibutuhkan di sini, jadi
  yang berubah bukan alokasinya melainkan petak yang digambar. `DepthPyramid`
  sudah mengikuti aturan yang sama.
- Yang tidak ikut menyusut: UI, dan pass yang resolusinya sudah punya arti sendiri
  (LUT langit, shadow map). Menyusutkan shadow map bersama resolusi layar
  menukar aliasing tepi dengan aliasing bayangan, dan yang kedua lebih terlihat.

**Kriteria selesai:** memaksa beban naik — resolusi jendela, jumlah lampu — membuat
skala turun dan waktu frame tetap di anggaran; melepas beban mengembalikannya
tanpa berdenyut.

---

## Yang sengaja tidak ada di plan ini

**Deferred shading dengan G-buffer.** Ditolak, dan bukan karena selera.

Alasan pertama, materialnya. Renderer ini memakai OpenPBR (`Shaders/openpbr.slang`,
`docs/ANALISA-OPENPBR.md`) — coat, sheen, transmission, subsurface. Lapisan-lapisan
itu tidak muat di G-buffer tipis, dan G-buffer yang cukup tebal untuk memuatnya
harus dibaca dan ditulis setiap frame pada bus 192-bit. Bandwidth adalah sumber
daya yang paling langka di garis dasar kita, dan deferred membelanjakannya di
muka untuk seluruh layar tanpa peduli berapa banyak yang benar-benar butuh.

Alasan kedua, yang dibeli deferred sudah dimiliki. Yang membuat ratusan lampu
dinamis murah bukan deferred melainkan penyaringan lampu per wilayah layar — dan
itulah `LightCluster`, yang sudah jalan. Depth prepass yang sudah ada sudah
menghapus overdraw shading, yaitu keuntungan kedua yang biasanya dikaitkan dengan
deferred.

Alasan ketiga, CryEngine yang dimaksud biasanya CryEngine 3 (2009). CryEngine 5.x
sendiri hybrid: G-buffer tipis dengan penyaringan lampu bergaya tile/cluster di
compute shader. Yang layak ditiru darinya adalah bagian compute-nya — G3 sampai
G7 di atas — bukan G-buffer-nya.

**Menyimpan posisi dunia sebagai tekstur G-buffer.** Kalaupun suatu hari ada
alasan untuk G-buffer, posisi direkonstruksi dari depth dan inverse view-proj.
Menyimpannya berarti membuang belasan megabyte bandwidth per frame untuk data
yang sudah ada.

**SSDO dan voxel cone tracing.** Keduanya sudah dilewati oleh yang sudah
dikerjakan: SDF clipmap global plus screen probe plus hash-grid radiance cache
(M1–M5) menjawab pertanyaan yang sama dengan hasil yang lebih baik dan anggaran
yang sudah terukur.

**Perekaman command multi-thread.** Bukan ditolak, tapi ditunda sampai ada
buktinya. G5 dan G6 keduanya memangkas biaya CPU perekaman; memecahnya ke banyak
thread sebelum itu berarti memparalelkan pekerjaan yang seharusnya dihapus.
Kalau setelah G6 profil CPU masih menunjukkan perekaman sebagai puncaknya, ia
kembali ke daftar.

---

## Urutan, ketergantungan, dan status

| Milestone | Isi | Butuh | Status |
|-----------|-----|-------|--------|
| G0 | Adegan uji tetap + kamera terkunci + garis dasar tercatat | — | ✅ |
| G1 | Cull bayangan per muka atlas dan per kaskade | G0 | ✅ |
| G2 | Pipeline cache ke disk, deklarasi fitur, antrean kedua | — | ✅ |
| G3 | Fondasi compute: build, RHI, pass di frame graph | G2 | ✅ |
| G4 | Clipmap SDF, Hi-Z, dan penetapan cluster pindah ke compute | G3 | ✅ (Hi-Z diukur lalu dikembalikan — lihat catatannya) |
| G5 | Bindless (`descriptorIndexing`) + material terindeks | G2 | ⏳ |
| G6 | Indirect draw + occlusion culling dua fase | G3, G5 | ⏳ |
| G7 | Async compute lewat timeline semaphore | G4, G6 | ⏳ |
| G8 | Resolusi dinamis | E8.8 (TAA), G0 | ⏳ |

G1 dan G2 tidak saling menunggu dan tidak menunggu apa pun kecuali G0. G5 hanya
menunggu G2. Selebihnya berurutan.

**Sesudah G1, yang terbesar berpindah ke sisi CPU.** Frame sekarang 3,2 ms GPU
melawan 4,9 ms CPU, dan 3,8 ms dari yang CPU adalah `cpu-sdf` sendirian. Urutan
di tabel ini tidak berubah karenanya — G2 dan G3 tetap mendahului G4 karena
keduanya prasyaratnya — tetapi target G4 sudah jelas siapa.

**Titik sambung ke E8.** G3 harus mendarat sebelum E8.6 (vegetation, yang
plannya sudah menyebut "GPU culling: compute + indirect draw") dan sebelum E8.7
(partikel, "simulasi GPU"). Kalau tidak, dua milestone itu akan membangun sendiri
fondasi yang sama, dan G3 berubah dari pekerjaan membangun menjadi pekerjaan
menyatukan tiga hal yang sudah terlanjur berbeda.

Satu milestone = satu branch = satu PR, dengan aturan yang sama seperti seluruh
roadmap: kriteria selesai lulus dulu, baru lanjut. Bedanya di sini kriterianya
berupa angka, dan angkanya tercatat di dokumen ini — baris "sebelum" dan
"sesudah" untuk tiap milestone, pada adegan uji G0 yang sama.
