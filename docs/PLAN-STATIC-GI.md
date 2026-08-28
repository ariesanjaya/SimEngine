# Rencana Precomputed GI — dua kategori, bukan tiga tingkat

**Masalahnya satu kalimat:** tingkat bernama `Baked` hari ini memanggang
lingkungannya saja — sembilan angka untuk seluruh level, tanpa oklusi, tanpa
pantulan, dan tanpa variasi posisi — padahal seluruh premis tingkat itu adalah
matahari yang diam, yaitu persis keadaan di mana transport cahaya **bisa**
dipanggang.

Akibatnya sebuah ruangan tertutup disinari sama persis dengan lapangan terbuka.
[PLAN-IBL.md](PLAN-IBL.md) menuliskan itu sebagai risiko yang harus
didokumentasikan; rencana ini menghapus risikonya alih-alih mendokumentasikannya.

**Yang dibangun rencana ini:** dua kategori yang berdiri sendiri.

- **Precomputed GI** — IBL, GI, dan AO dipanggang semuanya. Untuk adegan yang
  mataharinya tidak bergerak.
- **Real-time GI** — probe layar dan penelusuran SDF/ray query, apa adanya
  sekarang. Untuk adegan yang mataharinya bergerak.

Bukan dua jalur render; satu jalur dengan dua sumber untuk suku yang sama —
aturan yang sama yang sudah dipegang seri B.

Prefiks milestone: **S** (static lighting).

---

## Keadaan sekarang, diukur

| yang dipanggang hari ini | ukuran | bergantung pada |
|---|---:|---|
| SH9 iradiansi difus | 108 B | lingkungan saja |
| Prefilter cubemap 64², 5 mip, RGBA32F | 0,50 MB | lingkungan saja |
| LUT DFG 64² | 16 KB | tidak ada, hanya BRDF |

**Ketiganya per-arah, tidak satu pun per-posisi.** Setiap permukaan di level —
di dalam peti tertutup sekalipun — membaca sembilan koefisien yang sama.

Pencarian ambient occlusion di seluruh shader dan renderer: nihil. Yang muncul
sebagai "occlusion" adalah *occlusion culling*, uji visibilitas untuk membuang
draw call.

`ProjectIrradiance` dan `BakeIblCpu` menerima `IEnvironmentSampler` dan tidak
menerima apa pun yang lain. **Keduanya secara harfiah tidak bisa menghitung
oklusi**, karena mereka tidak pernah tahu ada geometri.

### UV pertama, diukur

Pertanyaan "bisakah lightmap memakai UV yang sudah dibawa importir" dijawab
dengan mengukur isi yang benar-benar ada di pohon ini, lewat
`SimHeadless --dump-tri`:

| sumber geometri | UV pertamanya | layak jadi parameterisasi lightmap? |
|---|---|---|
| `shaderBall.fbx` (importir FBX) | 19,7% titik di luar `[0,1]`, rentang u[−1,00 … 1,66]; luas UV dibagi luas geometri = 0,144 | tidak — berulang dan jauh dari proporsional |
| `unitSphere.obj`, `unitCylinder.obj` | seluruhnya `(0,0)` | tidak — seluruh mesh jatuh ke satu texel |
| Whitebox (`WhiteboxMesh.cpp`) | `uv = (dot(pos, axisU), dot(pos, axisV))` — proyeksi planar world-space | tidak — dua dinding sejajar berjarak sepuluh meter mendapat UV **identik** |
| Terrain (`TerrainMesh.cpp`) | `uv = (pos.x, pos.z)` | **ya** — unik menurut konstruksi, karena heightfield fungsi dari (x, z) |

**Satu dari empat.** Dan yang paling menentukan bukan `shaderBall` — itu aset
pratinjau material, dan UV tekstur yang berulang di sana wajar. Yang menentukan
**whitebox**: ia geometri yang dibangkitkan mesin ini sendiri, ia isi yang paling
mungkin dipanggang seseorang, dan proyeksi planarnya membuat setiap permukaan
sejajar berbagi texel yang sama persis. Sebuah ruangan whitebox akan dipanggang
dengan lantai dan langit-langitnya berbagi cahaya.

Angka itu yang mengunci keputusan 7 di bawah.

---

## Yang sudah ada dan tidak perlu ditulis ulang

Sebagian besar mesin baker-nya sudah berdiri, dan sebagian besar dibangun untuk
alasan lain.

| Bagian | Di mana | Keadaan |
|---|---|---|
| Ray query CPU beserta backend Embree | `Sim::Raycast` | R6 selesai; Embree 2,4× lebih cepat menelusuri |
| Path tracer tak-bias, multi-pantulan, NEE | `reference::PathTracer` | R4 selesai; sudah menerima langit sebagai `SkySampler` sejak B5 |
| Lingkungan sebagai pencuplik | `render::IEnvironmentSampler` | `AtmosphereSky`, `EquirectEnvironment`, `CubemapEnvironment` |
| Penanda geometri statis | `scene::StaticFlagComponent` | ada, dan tooltipnya sudah berbunyi "boleh di-bake" |
| Pola artefak masak berkunci-hash | `IblCache`, `MeshSdfCache` | B3; tulis atomik, header divalidasi, versi di kuncinya |
| Bake di `TaskPool`, unggah di main thread | `VulkanRenderer` | B1–B2; `BakeIblCpu` / `UploadIbl` sudah terpisah |
| Tempat menyimpan maksud pengarang | `scene::WorldSettings` | B0; terdaftar refleksi, undo/redo, panel sendiri |
| SH9 di blok uniform + evaluasinya di shader | `shadow_common.slang` | B1; `skyIrradiance()` |
| Menyatakan kombinasi tidak sah | panel World Settings | B6; peringatan beserta dua jalan keluar |

**Yang belum ada, dan besar:**

- `assets::MeshVertex` cuma punya **satu** `Vec2 uv`. Lightmap menuntut set
  kedua — dan itu menyentuh importir, tata letak vertex GPU, `static_assert`
  yang mengunci offset-nya, serta setiap shader yang membaca vertex.
- Tidak ada unwrapper UV di pohon ini sama sekali.
- Penempatan probe, format penyimpanannya, dan pembacaannya di shader.
- Pemeriksa kelayakan UV, yang menentukan mesh mana yang boleh **melewati**
  unwrap (keputusan 7).
- Kontrol resolusi di editor, beserta tempatnya di `WorldSettings` dan
  penimpaan per-objek (keputusan 8).

---

## Keputusan yang dikunci di awal

### 1. Dua kategori, bukan tiga tingkat

`indirect` menjadi `None | Precomputed | RealTime`. Yang hari ini bernama
`Baked` menjadi `Precomputed` dan **tumbuh**: ia berhenti berarti "lingkungan
saja" dan mulai berarti "seluruhnya dipanggang".

Berkas lama yang menulis `"Baked"` dibaca sebagai `Precomputed` lewat langkah
migrasi. Nama enum adalah yang tertulis di berkas level; membiarkan dua nama
untuk satu tingkat berarti dua arti yang suatu saat berselisih.

### 2. Matahari langsung tetap dinamis

Yang dipanggang **pantulan dan oklusinya**, bukan cahaya langsungnya. Bayangan
matahari tetap lewat shadow map.

Alasannya dua. Bayangan panggang setajam texel-nya, dan tepi bayangan adalah
justru hal yang paling diperhatikan orang. Dan matahari masih boleh ditala —
menggeser sudutnya sedikit tidak menuntut bake ulang, karena yang dipanggang
tidak memuatnya.

Harganya jujur: pass bayangan tetap dibayar, dan pantulan dari matahari yang
digeser menjadi sedikit tidak sesuai sampai bake berikutnya.

### 3. Dua penyimpanan, satu besaran

Lightmap untuk permukaan statis, probe volume untuk objek dinamis. **Keduanya
menyimpan besaran yang sama** — iradiansi datang — sehingga sebuah permukaan
tidak berubah rupa saat ia menyeberang dari satu ke yang lain.

Melanggarnya menghasilkan cacat yang paling sulit dilacak di seluruh rencana
ini: peti yang warnanya berubah tepat saat ia diseret melewati batas.

### 4. Yang memanggang adalah path tracer acuan, bukan implementasi kedua

`reference::PathTracer` sudah tak-bias, sudah multi-pantulan, dan sudah diadu
dengan integrasi langsung (B5: cocok dalam 0,02%–1,5%). Menulis transport kedua
untuk baker berarti dua kumpulan bug dan dua jawaban yang harus dijelaskan
setiap kali berselisih.

Aturannya sama dengan yang sudah dipegang `openpbr.slang`: model yang punya dua
implementasi adalah model yang tidak punya satu pun.

### 5. Lingkungan menjadi **masukan** transport, bukan saudaranya

Begitu probe memuat transport, iradiansi langit sudah ada **di dalamnya**.
Menjumlahkan keduanya menghasilkan adegan terang dua kali lipat tanpa satu pun
galat di log.

> Suku difus punya satu pemilik. Aturan itu sudah ditulis sebagai keputusan 2
> di [PLAN-IBL.md](PLAN-IBL.md), dan di sini ia berlaku sekali lagi dengan
> pemilik yang berbeda.

Prefilter spekular **tetap** dari lingkungan: ia menjawab pertanyaan lain, dan
pantulan spekular per-posisi adalah reflection probe — pekerjaan tersendiri
yang belum punya rencana.

### 6. Time-of-Day tidak sah bersama Precomputed

Menggerakkan matahari di adegan yang transportnya sudah dipanggang berarti
pantulan yang datang dari matahari di tempat lain. Dinyatakan, bukan didiamkan —
pola yang sama dengan B6, beserta jalan keluarnya.

### 7. UV lightmap dibangkitkan, dan itu diukur

`assets::MeshVertex` tumbuh satu `Vec2`, dan unwrapper membangkitkannya saat
mesh di-cook.

**Menyalin UV pertama sempat menjadi keputusan ini, lalu diukur dan dibatalkan.**
Tabel di bagian "UV pertama, diukur" di atas adalah alasannya: satu dari empat
sumber geometri di pohon ini punya UV pertama yang layak, dan yang tidak layak
termasuk geometri yang dibangkitkan mesin ini sendiri.

Yang membuat pengukuran itu menentukan bukan angkanya melainkan siapa yang
gagal. Aset importir bisa diminta diperbaiki artisnya — tapi itu berarti artis
tetap mengarang UV kedua, hanya menyimpannya di slot pertama: **beban
pengarangan yang sama, dikurangi kemampuan mesin membangkitkannya sendiri**.
Whitebox tidak bisa diminta apa-apa; ia harus diubah kodenya. Dan primitif yang
UV-nya nol tidak punya siapa pun untuk dimintai.

Harganya sekali dan jelas: satu dependensi, satu `Vec2` di format vertex, satu
langkah saat cook. Sesudahnya setiap mesh layak **tanpa syarat apa pun pada
pengarangnya** — dan itu yang tidak bisa dibeli cara lain.

**Pemeriksa kelayakan tetap dibangun**, dan gunanya berbalik: ia bukan penjaga
gerbang melainkan pelewat. Mesh yang UV pertamanya sudah unik dan tidak tumpang
tindih — terrain, dan aset yang memang diarang untuk dipanggang — melewati
unwrap sama sekali, dan waktunya tidak dibayar. Yang tidak layak di-unwrap, dan
alasannya disebut.

**Unwrap masuk ke artefak cook mesh**, bukan dihitung tiap kali level dibuka —
pola yang sama dengan `.simibl` dan `MeshSdfCache` yang sudah berdiri.

### 8. Resolusi adalah setelan pengarang, dan ia masuk ke level

Kerapatan texel lightmap dan jarak antar-probe bisa disetel di editor, dengan
bawaan per-level dan penimpaan per-objek untuk yang menuntut lebih halus.

**Ini tidak melanggar keputusan 3 di [PLAN-IBL.md](PLAN-IBL.md), yang menaruh
"resolusi probe" di project — ia justru menunjukkan bahwa keduanya bukan hal
yang sama.** Resolusi probe *real-time* adalah anggaran mesin: ia dibayar tiap
frame, dan mesin lemah harus boleh menurunkannya. Resolusi *panggangan*
menentukan artefak yang dikirim: semua orang menerima lightmap yang sama, dan
mesin lemah tidak bisa memanggang ulang dengan angka lain. Yang menentukan
artefak adalah pengarangan, dan pengarangan tinggal di level.

Yang tetap di project cuma anggaran bake-nya sendiri — jumlah sampel dan jumlah
thread — karena itu memang berbeda antar-mesin dan tidak mengubah bentuk
hasilnya.

### 9. Probe lebih dulu, lightmap menyusul

Probe tidak menuntut UV apa pun, tidak menuntut dependensi baru, dan tidak
menyentuh format vertex. Lightmap menuntut ketiganya.

Mengerjakan yang mahal lebih dulu berarti membayar unwrapper, perubahan format
vertex, dan langkah cook baru **sebelum ada satu pun bukti bahwa transportnya
benar** — dan kalau transportnya ternyata salah, seluruh biaya itu dibayar untuk
memanggang jawaban yang keliru dengan lebih tajam.

Urutan ini juga yang membuat lightmap punya lawan bicara saat ia mendarat:
permukaan yang sama harus tampak sama lewat kedua jalur (keputusan 3), dan itu
hanya bisa diperiksa kalau salah satunya sudah berdiri.

---

## Bentuk yang dituju

| `indirect` | difus permukaan statis | difus objek dinamis | oklusi | spekular |
|---|---|---|---|---|
| `None` | nol | nol | — | nol |
| `Precomputed` | lightmap | probe volume | dipanggang | prefilter lingkungan |
| `RealTime` | probe layar | probe layar | ditelusuri | prefilter + screen trace |

Matahari langsung lewat shadow map di ketiganya.

---

## Milestone

### S0 — Dua kategori, dinyatakan

- `IndirectLighting` menjadi `None | Precomputed | RealTime`
- Migrasi berkas: `"Baked"` dibaca sebagai `Precomputed`
- Time-of-Day + `Precomputed` dinyatakan tidak sah, beserta jalan keluarnya
- Panel World Settings mengatakan apa yang sudah dipanggang dan apa yang belum

**Selesai kalau:** setiap level yang ada terbuka dan terbaca `Precomputed` tanpa
satu pun perubahan rupa; menyalakan Time-of-Day di level `Precomputed` memberi
notifikasi dengan jalan keluar sekali klik; dan `.simlevel` yang disimpan ulang
menulis nama baru.

### S1 — Probe volume: kisi, penempatan, penyimpanan

- Kisi iradiansi beraturan yang menutupi batas geometri statis
- **Jarak antar-probe disetel di editor**, bawaan per-level di World Settings
  (keputusan 8) — dan panelnya menyebutkan berapa probe dan berapa megabyte yang
  dihasilkan angka itu, sebelum ada yang menekan Bake
- SH per probe, artefak masak berkunci-hash — pola `.simibl`
- Shader membaca kisi lewat interpolasi trilinear

**Belum ada transport sama sekali di sini:** tiap probe diisi dari lingkungan
saja, persis seperti tingkat panggang hari ini.

**Selesai kalau:** pada adegan terbuka tanpa penghalang, gambarnya sama dengan
tingkat panggang seri B — dan satu-satunya selisih yang tersisa bisa dijelaskan
sebagai interpolasi kisi, dengan angkanya ditulis. Yang diuji di sini
plumbing-nya, dan memisahkannya dari transport berarti kegagalan berikutnya
punya satu penyebab, bukan dua.

Dan: menggeser jarak antar-probe mengubah jumlah probe serta ukuran artefaknya
sesuai angka yang ditampilkan panel, bukan sesuai angka yang harus ditebak.

### S2 — Transport: probe diisi path tracer acuan

- `reference::PathTracer` menelusuri dari tiap probe, multi-pantulan penuh
- Cahaya matahari langsung **dikecualikan** (keputusan 2)
- Bake berjalan di `TaskPool`, hasilnya artefak masak

**Selesai kalau:** ruangan tertutup berhenti disinari langit — terukur, bukan
terlihat; uji tungku lulus pada kisi seperti ia lulus pada jalur panggang (B2);
dan pada adegan terbuka iradiansi probe cocok dengan tingkat panggang seri B
dalam ambang yang ditulis di dokumen ini.

### S3 — Oklusi arah

- Tiap probe membawa visibilitasnya, bukan hanya iradiansinya
- Objek dinamis memakainya supaya tidak menerima cahaya dari arah yang
  terhalang

**Selesai kalau:** sebuah benda di bawah meja lebih gelap daripada benda yang
sama di sebelah meja, dan selisihnya diukur; benda yang bergerak keluar-masuk
bayangan berubah mulus, tanpa loncatan pada batas sel.

### S4 — UV lightmap: dibangkitkan, diperiksa, dan di-cook

- `assets::MeshVertex` tumbuh satu `Vec2`, beserta tata letak vertex GPU,
  `static_assert` yang mengunci offset-nya, dan importirnya
- Unwrapper sebagai dependensi **opsional** — yang membangun tanpanya kehilangan
  lightmap, bukan seluruh mesin. Aturan yang sama dengan MaterialX dan OpenUSD,
  dan `MaterialXImport.cpp` sudah menjadi contohnya: ia tetap ikut dibangun dan
  menolak dengan pesan yang menyebut sakelarnya
- Pemeriksa kelayakan yang mengukur UV pertama sebuah mesh — apakah ia keluar
  dari `[0,1]`, dan apakah segitiga-segitiganya tumpang tindih di ruang UV.
  **Yang sudah layak melewati unwrap**, dan waktunya tidak dibayar
- Keduanya masuk ke artefak cook mesh, bukan dihitung tiap buka

**Selesai kalau:** mesh statis punya UV lightmap yang chart-nya tidak tumpang
tindih — diperiksa uji, bukan mata; berkas mesh yang sudah ada di cache orang
tetap terbaca tanpa UV kedua, dan di-cook ulang alih-alih ditolak; whitebox dan
primitif ber-UV nol keduanya keluar dengan UV yang layak; dan terrain, yang UV
pertamanya sudah unik menurut konstruksi, terdeteksi layak dan melewati
unwrap-nya.

> Baris terakhir itu yang membuat pemeriksanya berguna alih-alih seremonial:
> kalau ia tidak bisa mengenali satu-satunya UV yang memang sudah layak di pohon
> ini, ia tidak bisa dipercaya mengenali yang lain.

### S5 — Lightmap: bake dan baca

- Iradiansi per-texel untuk permukaan statis, dari transport yang sama dengan S2,
  diparameterisasi UV pertama mesh-nya (keputusan 7)
- **Kerapatan texel disetel di editor** — texel per meter, bawaan per-level dan
  penimpaan per-objek untuk yang menuntut lebih halus (keputusan 8) — dan
  panelnya menyebutkan ukuran atlas yang dihasilkan angka itu sebelum Bake
  ditekan
- Shader membaca lightmap untuk yang statis dan probe untuk yang dinamis

**Selesai kalau:** kontak antar-permukaan dan bayangan halus terbaca pada
lightmap dan tidak terbaca pada probe — itu seluruh alasan lightmap ada; sebuah
permukaan yang sama tampak sama saat ia berpindah dari satu jalur ke jalur lain
(keputusan 3), dengan selisihnya diukur; dan menaikkan kerapatan texel
menajamkan kontaknya sementara menurunkannya mengaburkannya, keduanya sesuai
ukuran atlas yang diumumkan panel.

### S6 — Validasi terhadap path tracer acuan

- Adegan acuan **beroklusi** — bukan kuad cembung sendirian seperti B5
- Bandingkan seluruh kategori `Precomputed` terhadap jalur acuan

**Selesai kalau:** selisihnya di bawah ambang yang ditulis di dokumen ini, dan
angkanya dicatat di sini — bukan di pesan commit. Aturan yang sama dengan B5,
dan adegan yang punya oklusi adalah adegan yang B5 memang tidak bisa menilainya.

---

## Anggaran

### Memori

Probe volume, adegan 64 × 16 × 64 m dengan jarak antar-probe 1 m — 65.536 probe:

| bentuk | per probe | total |
|---|---:|---:|
| SH9 RGB float32 | 108 B | 7,1 MB |
| SH9 RGB float16 | 54 B | 3,5 MB |
| SH orde satu (4 koefisien) RGB float16 | 24 B | 1,6 MB |

Lightmap 1024² RGB float16: 6 MB per halaman atlas.

**Angka-angka itu memaksa sebuah keputusan yang belum diambil**, dan ia ditulis
di "yang masih terbuka" di bawah: kisi beraturan atas seluruh batas adegan
membayar probe untuk ruang kosong, dan sebagian besar adegan sebagian besarnya
ruang kosong.

### Waktu

**Belum diukur, dan sengaja tidak ditebak.** Yang diketahui bentuknya: 65.536
probe × sampel per probe × pantulan, dijalankan Embree pada `TaskPool`.

Yang ditetapkan bukan perkiraan melainkan syarat: **bila adegan menengah tidak
selesai dipanggang di bawah satu menit pada 24 inti, S2 meninjau ulang jumlah
sampelnya** — bukan menerima angkanya. Bake yang memakan sepuluh menit adalah
bake yang tidak pernah dijalankan siapa pun, dan pencahayaan yang tidak pernah
dipanggang lebih buruk daripada pencahayaan yang kasar.

---

## Risiko

**Bocor menembus dinding tipis.** Probe yang jatuh di dalam geometri, atau sel
yang satu sisinya di dalam ruangan dan sisi lainnya di luar, menyalurkan cahaya
menembus dinding. Ini cacat khas probe volume dan ia **akan** terjadi; yang
menentukan bukan apakah ia muncul melainkan apakah ada cara menyatakannya —
visibilitas per-probe (S3) adalah jawabannya, dan tanpa S3 rencana ini
menghasilkan kebocoran yang tidak bisa diperbaiki siapa pun.

**Dua penyimpanan yang tidak sepakat di batasnya.** Peti yang warnanya berubah
tepat saat diseret melewati tepi lightmap. Keputusan 3 mencegahnya di tingkat
rancangan; S5 harus mengukurnya.

**Bake yang tumbuh sampai tidak ada yang menjalankannya.** Setiap milestone
menambah sampel, dan tidak satu pun terasa mahal sendirian. Syarat satu menit di
atas ada supaya pertumbuhannya punya lawan.

**Lingkungan terhitung dua kali.** Begitu S2 mendarat, iradiansi langit ada di
dalam probe — dan jalur seri B masih menuliskannya ke `skyIrradianceSh`.
Keduanya menyala bersamaan menghasilkan adegan terang dua kali lipat tanpa satu
pun galat. Keputusan 5, dan ia harus punya ujinya sendiri di S2.

**UV kedua mengubah tata letak vertex.** `static_assert` yang mengunci offset
`uv` sudah ada di renderer, dan itu bagus — ia mengubah kelalaian menjadi galat
kompilasi. Yang **tidak** dijaganya berkas mesh masak yang sudah ada di cache
orang: ia ditulis dengan format lama, dan tidak ada satu pun assert yang berjalan
saat ia dibaca. Versi di kunci artefaknya yang harus menangkap itu — pola yang
sudah dipakai `IblCache` dan `MeshSdfCache`.

**Unwrap yang berkualitas buruk lebih sulit dilihat daripada unwrap yang gagal.**
Chart yang pecah-pecah menghasilkan jahitan; chart yang terlalu rapat
menghasilkan cahaya yang bocor antar-chart pada mip atau saat difilter. Keduanya
terlihat sebagai "lightmap-nya kotor", bukan sebagai unwrapper yang salah
disetel — dan keduanya baru muncul sesudah S5, jauh dari tempat sebabnya.
Padding antar-chart dan ukuran chart minimum karena itu setelan yang harus
terlihat, bukan konstanta di dalam baker.

---

## Yang sengaja tidak dikerjakan

**Bake di GPU.** Alasannya sama dengan yang ditulis `IblBaker.h` dan tetap
berlaku: matematikanya sudah ada dan sudah teruji di CPU, dan implementasi kedua
menunggu sebuah pengukuran — bukan sebuah dugaan.

**Reflection probe.** Spekular per-posisi adalah pekerjaan tersendiri. Yang di
rencana ini tetap memakai prefilter lingkungan (keputusan 5), yaitu spekular
per-arah — cukup untuk langit, tidak cukup untuk lorong.

**Directional lightmap / spherical gaussian.** Iradiansi skalar per-texel lebih
dulu. Menyimpan arahnya menggandakan memorinya dan menuntut jawaban atas
pertanyaan yang belum ada yang mengajukannya.

**Menyatukan `Precomputed` dan `RealTime` menjadi satu jalur campuran.** Yang
menuntutnya adalah adegan yang sebagian statis dan sebagian tidak, dan itu
pertanyaan yang lebih baik dijawab sesudah keduanya berdiri sendiri-sendiri.

---

## Yang masih terbuka

- **Penempatan probe.** Kisi beraturan membayar probe untuk ruang kosong;
  kaskade — seperti clipmap SDF yang sudah ada — atau penempatan adaptif
  membayar ketelitian di tempat yang membutuhkannya. Diputuskan sebelum S1
  mendarat, karena ia menentukan format penyimpanannya.
- **Ambang selisih S2 dan S6** terhadap tingkat panggang dan terhadap path
  tracer acuan: belum ditetapkan angkanya, dan seperti B5 ia sebaiknya diukur
  lebih dulu lalu ditetapkan dari hasilnya.
- **Unwrapper mana.** xatlas yang paling lazim — MIT, tanpa dependensi lain —
  tapi belum ada yang mengukur waktu bangunnya maupun waktu unwrap-nya di pohon
  ini, dan [DEPENDENCIES.md](DEPENDENCIES.md) menuntut keduanya dijawab sebelum
  sebuah dependensi masuk. Diputuskan sebelum S4 mendarat, dan diukur alih-alih
  diperkirakan — aturan yang sama yang dipakai MaterialX dan Embree.
- **Objek statis yang dipindahkan sesudah bake.** Ia membawa lightmap yang
  sudah tidak sesuai. Menyatakannya kotor itu mudah; memutuskan apa yang
  digambar sementara ia kotor tidak.
