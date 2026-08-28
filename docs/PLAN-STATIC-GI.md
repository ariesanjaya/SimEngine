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

### 9. Kisi jarang, dan formatnya disiapkan sejak S1

Probe disimpan sebagai **brick**: kisi yang sama, tapi hanya blok yang dekat
geometri yang dialokasikan.

**Kaskade seperti clipmap SDF sempat menjadi kandidat, dan kodenya sendiri yang
membatalkannya.** `SdfClipmap.h` berbunyi "beberapa kaskade kubik yang
**mengikuti kamera**", dan seluruh rancangannya — pengalamatan toroidal,
pengancingan titik asal, penulisan ulang hanya lempeng tepi — ada karena
kameranya bergerak. Panggangan tidak punya kamera. Yang tersisa dari analogi itu
cuma kisi bersarang yang berbagi nama dengan sesuatu yang cara kerjanya lain.

**Yang memaksa brick bukan S1 melainkan pertumbuhan kubiknya.** Memperhalus dua
kali lipat membayar delapan kali lipat, dan itu terlihat di angkanya. Angka di
bawah memakai SH9 RGB float16 — 54 B per probe — yang **tidak pernah jadi
dipakai**: artefaknya menulis `Sh9` apa adanya (108 B) dan buffer GPU-nya
menempati 144 B. Bentuk pertumbuhannya tetap benar; kalikan 2 untuk artefak dan
2,7 untuk GPU.

| adegan | 2 m | 1 m | 0,5 m |
|---|---:|---:|---:|
| 32×8×32 | 0,1 MB | 0,5 MB | 3,7 MB |
| 64×16×64 | 0,5 MB | 3,7 MB | 28,3 MB |
| 128×32×128 | 3,7 MB | 28,3 MB | **221 MB** |

Kisi beraturan membayar probe untuk ruang kosong **dan untuk ruang di dalam
benda pejal**, dan pada adegan besar itu yang menghabiskan anggarannya. Dengan
brick, biayanya mengikuti luas permukaan alih-alih volume.

**Formatnya disiapkan sejak S1, tapi sparsity-nya datang di S2**, dan pembagian
itu disengaja:

- **S1 menulis format brick dan mengisinya penuh** — setiap brick ada. Itu
  menaruh indireksinya di tempatnya tanpa menuntut S1 memutuskan brick mana yang
  boleh hilang.
- **S2 yang membuang brick kosong**, karena ia satu-satunya milestone yang
  memang sudah menelusuri geometri dan karena itu sudah tahu di mana
  permukaannya.

Indireksinya tidak bisa ditambahkan belakangan dengan murah: pencarian di
shader, format artefak, dan cara editor melaporkan ukurannya semuanya
bergantung padanya, dan ketiganya sudah berdiri saat S5 tiba.

**Volume yang ditempatkan pengarang ditunda, bukan dibuang.** Ia lapisan
pengarangan di atas brick — halus di ruang tamu, kasar di lapangan — dan
menambahkannya nanti tidak mengubah format penyimpanannya.

### 10. Probe lebih dulu, lightmap menyusul

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

### S0 — Dua kategori, dinyatakan · ✅

- `IndirectLighting` menjadi `None | Precomputed | RealTime`
- Migrasi berkas: `"Baked"` dibaca sebagai `Precomputed`
- Time-of-Day + `Precomputed` dinyatakan tidak sah, beserta jalan keluarnya
- Panel World Settings mengatakan apa yang sudah dipanggang dan apa yang belum

**Selesai kalau:** setiap level yang ada terbuka dan terbaca `Precomputed` tanpa
satu pun perubahan rupa; menyalakan Time-of-Day di level `Precomputed` memberi
notifikasi dengan jalan keluar sekali klik; dan `.simlevel` yang disimpan ulang
menulis nama baru.

#### Keadaannya sesudah S0

`IndirectLighting::Baked` menjadi `Precomputed`, dan skema level naik ke 5.
Dua level yang bedanya hanya kata itu — satu versi 4 menulis `"Baked"`, satu
versi 5 menulis `"Precomputed"` — menghasilkan tangkapan yang **identik byte
demi byte**.

**Migrasinya ada justru karena tanpanya berkas lama tetap terbaca benar.**
`"Baked"` tidak ada di daftar nama yang baru, jadi pembacanya membiarkan nilai
bawaan — dan bawaannya kebetulan `Precomputed`, nilai yang sama. Kebetulan itu
berhenti berlaku pada hari seseorang mengubah bawaannya, di tempat yang tidak
ada hubungannya sama sekali. Ujinya karena itu memeriksa dua hal sekaligus:
`"Baked"` naik menjadi `Precomputed`, dan `"None"` maupun `"RealTime"` **tidak**
tersentuh — migrasi yang mengubah lebih daripada janjinya menghapus pilihan
orang tanpa menyebutkannya.

**Time-of-Day dinyatakan bertabrakan**, dengan dua jalan keluar sekali klik:
menghentikan Time-of-Day, atau pindah ke `RealTime`. Yang pertama tidak lewat
perintah — `timeOfDayEnabled` setelan editor, bukan isi level, dan yang tidak
pernah tertulis ke berkas tidak punya tempat di riwayat undo level.

> **Nadanya netral, dan tinjauan S0 yang memaksanya begitu.** Versi pertama
> memberi peringatan berwarna dan menyatakan "pantulan dan oklusinya akan datang
> dari matahari di tempat lain" — kalimat yang **tidak benar hari ini**, karena
> belum ada pantulan maupun oklusi yang dipanggang. Yang dipanggang
> `Precomputed` baru lingkungannya, dan iradiansinya dipanggang ulang tiap
> matahari bergeser: kombinasi itu justru yang dibangun dan diuji B1, dan
> kriteria terimanya berbunyi "ambient yang ikut berubah saat Time-of-Day
> menggerakkan matahari".
>
> Jadi rencana ini sempat menyuruh editor mengatakan bahwa susunan yang bekerja
> itu rusak, dan menawarkan pindah ke `RealTime` sebagai perbaikan atas masalah
> yang belum ada — memindahkan orang dari yang murah ke yang dibayar tiap frame,
> tanpa alasan.
>
> **Tabrakannya nyata, tapi ia datang bersama S2.** Sampai transport dipanggang,
> yang ditampilkan pemberitahuan bernada biasa yang menyebutkan keduanya masih
> boleh dan kapan itu berubah. S2 yang menaikkannya menjadi peringatan.

**Panelnya mengatakan apa yang belum dipanggang**, dan itu bukan hiasan:
`Precomputed` hari ini memanggang lingkungannya dan itu saja. Tingkat yang
menjanjikan lebih daripada yang diberikannya membuat orang mencari cacat pada
adegannya alih-alih pada rencananya. Daftarnya menyusut tiap milestone —
transport di S2, oklusi di S3, lightmap di S5.

Tiga uji baru di `SimSceneTests`, 25 dari 25 suite lulus.

### S1 — Probe volume: kisi, penempatan, penyimpanan · ✅

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

#### Keadaannya sesudah S1 · ✅

Kisinya hidup dari World Settings sampai ke shader. `bench.simlevel`, 1280×720,
kamera tetap, eksposur manual **EV4**, Debug:

| Jarak | Kisi | Probe | Brick | GPU | Artefak | `forward-opaque` | `cpu-total` |
|---|---|---:|---:|---:|---:|---:|---:|
| — (SH panggang) | — | — | — | — | — | 0,692 ms | 11,08 ms |
| 4 m | 22×2×22 | 2.304 | 36 | 3,2 MB | 2,4 MB | 0,938 ms | 11,74 ms |
| 2 m (bawaan) | 42×3×42 | 7.744 | 121 | 10,6 MB | 8,0 MB | 0,939 ms | 11,77 ms |
| 1 m | 82×4×82 | 28.224 | 441 | 38,8 MB | 29,1 MB | 0,954 ms | 11,68 ms |
| 0,5 m | 162×7×162 | 215.168 | 3.362 | 295 MB | 222 MB | 0,973 ms | 11,88 ms |
| 0,05 m | ditolak | 154.368.960 | — | — | — | — | — |

Selisih terhadap tingkat panggang seri B: **1–2 kanal dari 2.764.800, selisih
maksimum 1 dari 255** — pembulatan float pada bobot trilinear, bukan cahaya yang
berbeda. Gambar acuannya rata-rata 44,2 dengan 98,2% piksel bukan nol.

**Kontrol positif, dan ia yang membuat angka nol di atas berarti sesuatu.**
Tanpa transport tiap probe memuat SH yang sama, sehingga gambar lewat kisi
identik dengan gambar lewat SH panggang *baik ketika kisinya benar maupun ketika
ia tidak pernah dibaca satu piksel pun*. Satu angka nol menjawab dua keadaan yang
berlawanan. `--bench-probe-debug` mengisi probe dengan papan catur atas koordinat
X dan Z: gambarnya lalu berubah pada **60,8% kanal**, dan tiga jarak berbeda
menghasilkan tiga pola berbeda (66,9% kanal antara 4 m dan 2 m). Kisinya memang
dibaca, dan yang dibacanya memang bergantung pada posisi dan bentuk kisinya.

Papan caturnya berselang-seling di X dan Z saja. Yang juga berselang-seling di Y
tidak terlihat sama sekali: sebuah permukaan lazimnya berada di antara dua baris
probe secara tegak, dan interpolasinya meratakan dua nilai yang berlawanan
menjadi satu — polanya hilang justru di tempat ia harus terlihat.

**Yang masih belum terbukti, dan disebutkan supaya tidak dikira terbukti:**
pemetaan indeks di shader belum diadu satu-satu dengan `SampleProbeVolume` di
CPU. Yang sudah: kisinya dibaca, jawabannya bergantung pada posisi dan jarak, dan
sisi CPU-nya diuji langsung — termasuk brick yang dilubangi. Yang belum: bahwa
probe (x,y,z) tertentu di shader adalah probe yang sama dengan di CPU. Itu baru
bisa diadu ketika probe benar-benar membawa nilai yang berbeda-beda, yaitu di S2
— dan **kriteria terima S2 harus memuatnya**, bukan mengandaikannya sudah lewat.

##### Empat cacat yang ditemukan peninjauan, semuanya nyata

1. **Jalur material tidak pernah membaca kisinya.** Pembacaan probe dipasang di
   `box_shading.slang` saja — jalur kotak tanpa material — sedangkan setiap mesh
   bermaterial digambar lewat shader yang dirakit `MaterialShaderModule.cpp`, dan
   di sana `skyIrradiance` masih berdiri sendirian. Kisinya mati untuk hampir
   seluruh adegan, dan yang terlihat adalah gambar yang identik dengan sebelumnya
   — yaitu persis bentuk "lulus" yang dicari kriteria S1.

2. **Putaran lingkungan hilang di jalur probe.** `skyIrradiance` membaca lewat
   arah yang diputar (keputusan 4 di PLAN-IBL); `probeIrradiance` tidak. Diukur
   pada HDRI berputar 2,0 rad: **11,93% kanal berbeda, maksimum 14** antara kedua
   jalur yang seharusnya menjawab hal yang sama. Sekarang ruang isi probe
   dikirim sebagai angka (`probeCounts.w`), karena S2 mengisi probe di ruang
   dunia dan menerapkan putarannya lagi di sana akan memutarnya dua kali.

3. **Panel melaporkan ukuran yang tidak pernah dibayar siapa pun.** Angkanya RGB
   float16 — 54 byte per probe — sementara artefaknya menulis 108 dan buffer GPU
   menempati 144. Tiga angka untuk satu hal. Panel sekarang menyebut keduanya
   yang nyata, GPU lebih dulu, dan sebuah uji menjaga angka artefaknya tetap
   sama dengan ukuran berkas yang benar-benar ditulis.

4. **Panel dan renderer menghitung batas adegan dari daftar yang berbeda.**
   Panel memakai `pickables_`, yang sengaja tidak memuat entity terkunci —
   sedangkan mengunci latar statis justru cara orang menjaganya tidak terpilih
   tak sengaja, dan latar statis itulah isi adegan berpanggang. Keduanya sekarang
   memakai `meshes_`, daftar yang sama yang dipakai renderer.

##### Dan satu cacat di cara mengukurnya, yang membatalkan angkanya sendiri

Pengukuran S1 yang pertama dijalankan pada **EV12**, dan pada eksposur itu
seluruh tangkapannya hitam: rata-rata 0,0 dengan maksimum 1 dari 255. "0 dari
2.764.800 kanal berbeda" yang tercatat sebelumnya membandingkan dua gambar hitam.
Seri B memakai EV0; angka di atas EV4.

Pelajarannya bukan "pilih eksposur yang benar" melainkan **sebuah perbandingan
gambar wajib menyebutkan kecerahan gambar acuannya**, karena tanpa itu "tidak ada
selisih" dan "tidak ada gambar" adalah kalimat yang sama.

##### Yang belum ada di S1

Artefak `.simprobe` sudah bisa ditulis dan dibaca, tetapi renderer belum
memakainya — ia menyusun kisinya di memori tiap kali adegan dibuka. Itu benar
selama isinya sesalin SH langit; ia berhenti benar begitu S2 mengisinya dengan
penelusuran yang mahal, dan di sanalah cache berkunci-hash itu mulai dipakai.

Bendera ukur yang ditambahkan: `--bench-probe-spacing <m>` (nol mematikan
kisinya tanpa mengubah tingkat pencahayaan) dan `--bench-probe-debug` (papan
catur sebagai kontrol positif).

### S2 — Transport: probe diisi path tracer acuan · ✅

- `reference::PathTracer` menelusuri dari tiap probe, multi-pantulan penuh
- Cahaya matahari langsung **dikecualikan** (keputusan 2)
- Bake berjalan di `TaskPool`, hasilnya artefak masak

**Selesai kalau:** ruangan tertutup berhenti disinari langit — terukur, bukan
terlihat; uji tungku lulus pada kisi seperti ia lulus pada jalur panggang (B2);
dan pada adegan terbuka iradiansi probe cocok dengan tingkat panggang seri B
dalam ambang yang ditulis di dokumen ini.

Dan: **pemetaan indeks shader diadu satu-satu dengan `SampleProbeVolume` di
CPU** — S1 tidak bisa membuktikannya karena seluruh probenya bernilai sama, dan
S2 adalah milestone pertama yang probenya benar-benar berbeda-beda. Tanpa itu,
sebuah selisih di S2 punya dua tersangka: transportnya, dan pembacaannya.

Dan: **pemberitahuan Time-of-Day naik menjadi peringatan di sini.** Sampai
milestone ini, menggerakkan matahari di level `Precomputed` masih sah — yang
dipanggang cuma lingkungannya, dan ia mengikuti mataharinya. Sesudahnya tidak
lagi, dan panelnya harus mengatakan itu dengan nada yang berbeda.

#### Keadaannya sesudah S2 · ✅

Transport ditelusuri `reference::TraceProbeIrradiance` — estimator yang **sama
persis** dengan gambar acuan. Loop jalurnya dikeluarkan menjadi `TracePath` dan
dipakai keduanya; dua salinan akan berselisih pada pantulan ke berapa pun yang
pertama kali disunting salah satunya.

Ketiga kriteria terima, diukur:

| yang diuji | hasil | yang diharapkan |
|---|---:|---|
| ruang terbuka lawan `ProjectIrradiance` | dalam 2% pada empat normal | sama |
| ruang tertutup | **0 lawan 1,3122** | langitnya berhenti masuk |
| tungku ρ=0,5, E=1 | 6,266 lawan **6,28319** (0,27%) | `πE/(1−ρ)` |

Baris kedua yang menjadi seluruh alasan milestone ini ada: tingkat panggang
seri B menyinari ruang tertutup **persis seterang ruang terbuka**, karena
sembilan angka untuk seluruh level tidak pernah memeriksa apakah ada dinding.

**Matahari lewat next-event estimation saja**, dan itu yang menegakkan
keputusan 2 tanpa cabang khusus: NEE hanya berjalan di permukaan, jadi sinar
yang berangkat dari titik kosong — sebuah probe — tidak pernah menemuinya. Yang
terpanggang pantulannya; yang langsung tetap diantarkan lampu terarah yang
berbayang saat menggambar. Diperiksa dua arah: tanpa geometri, probe menjawab
nol walaupun matahari menyala sepuluh kali; dengan lantai di bawahnya, ia
menjawab 7,89.

**Pemetaan indeks shader diadu satu-satu, yang S1 tidak bisa lakukan.**
Aritmatikanya pindah ke `Shaders/probe_grid.slang`, di-`#include`
`shadow_common.slang` **dan** dikompilasi ke C++ lewat `slangc -target cpp` —
pola dan alasan yang sama dengan `openpbr_cpu.slang`. 8.888 pemeriksaan atas
tiga kisi yang jumlahnya bukan kelipatan ukuran brick, pada titik di dalam
kisi, di tepinya, dan di luarnya. Menukar x dan z di indeks lokal sisi C++
menggagalkan 1.466 — ujinya punya gigi.

Panggangan berjalan di `TaskPool` lewat `view::ProbeBakery`, dan hasilnya
artefak `.simprobe` berkunci hash. Diukur ujung ke ujung di `bench.simlevel`,
Debug:

Angkanya ada di tabel sesudah daftar cacat di bawah — yang pertama kali
tercatat di sini salah, dan sebabnya nomor 6 di sana.

Selisihnya kecil karena `bench.simlevel` adalah adegan terbuka: langit memang
mendominasi di sana, dan yang berubah cuma oklusi tanah beserta pantulannya.
Angka yang tajam ada di baris "ruang tertutup" di tabel pertama.

**Peringatan Time-of-Day naik nadanya**, dan syaratnya bukan tingkat yang
dipilih melainkan apa yang benar-benar sudah dipanggang: selama yang ada baru
kisi lingkungan, ia tetap ikut mataharinya dan nada netralnya benar. Begitu ada
kisi yang transportnya ditelusuri, ia berwarna.

##### Enam cacat yang ditemukan peninjauan

1. **Artefak `.simprobe` ditulis tetapi tidak pernah dibaca.** Sebuah cache yang
   tidak pernah menghemat apa pun bukan cache melainkan berkas yang menumpuk.
   Sekarang dibaca lebih dulu: 11,31 s menjadi **0,00 s**, dengan gambar yang
   identik byte demi byte.

2. **Dan kuncinya cuma memuat bentuk kisi.** `environmentKey` tidak pernah diisi
   satu pemanggil pun, jadi menyalakan pembacaan tanpa memperbaikinya akan
   membaca panggangan matahari sore sebagai panggangan matahari pagi — tanpa
   satu pun galat, karena berkasnya memang sah. Kunci sekarang memuat langit
   (sidik jari dari pemanggil, karena langitnya sebuah `std::function` yang tidak
   bisa di-hash), matahari, albedo, jumlah cuplikan, dan matriks tiap benda.
   Kontrol negatifnya: menggeser matahari pada kisi yang bentuknya sama persis
   memaksa panggangan ulang — 11,28 s, bukan 0,00 s — dan gambarnya berbeda pada
   88,4% kanal.

3. **Tugas latar menangkap `PickScene` milik bakery lewat referensi.** Sebuah
   level yang ditutup di tengah panggangan membebaskan BVH yang sedang
   ditembaki, dan yang keluar bukan galat melainkan pembacaan memori bebas.
   Geometrinya sekarang dipegang tugasnya sendiri lewat `shared_ptr` — aturan
   "tidak pernah menangkap `this`" berlaku sama untuk referensi ke anggotanya.

4. **Kotak tiap benda ditaksir dari matriksnya**, sebagai titik asal ± skala
   terbesarnya. Sebuah lantai 80×0,5×80 karena itu menjadi kubus bersisi 160 —
   aman, tetapi bentuknya salah, dan yang dibayar waktu panggang untuk brick
   yang tidak perlu. Kotak dunia yang sebenarnya kini ikut dari `SceneView`,
   yang memang sudah menghitungnya.

5. **Kisi panggang tidak pernah dilepas saat level berganti.** Level berikutnya
   disinari cahaya tak-langsung adegan sebelumnya: sebuah ruangan yang tidak
   pernah dipanggang menerima pantulan dari ruangan lain. Viewport sekarang
   **membandingkan** kisi yang terpasang alih-alih memasangnya sekali di ujung
   panggangan — kisinya juga bisa hilang, dan pemasangan yang hanya terjadi saat
   selesai tidak pernah memberitahu renderer tentang itu.

6. **Dan lagi-lagi cacatnya ada di cara mengukurnya.** Jalur `--bench-probe-bake`
   memanggang dengan matahari tegak lurus ke atas dan tanpa iradiansi, sementara
   langit yang tergambar memakai matahari adegan. Selisih gambar yang tercatat
   sebelumnya karena itu sebagian **dua langit yang berbeda**, bukan oklusi.
   Diukur: panggangan bermatahari salah berbeda 17,5% kanal dari yang benar.

Angka yang benar, `bench.simlevel`, EV4, sesudah keenamnya diperbaiki:

| jarak | probe | spp | panggang | dari cache | GPU | selisih lawan kisi langit |
|---|---:|---:|---:|---:|---:|---|
| 4 m | 2.304 | 64 | 11,3 s | 0,00 s | 0,32 MB | — |
| 2 m | 6.400 | 128 | 67,7 s | 0,00 s | 0,88 MB | 32,5% kanal, maks 22 |

##### Riak di tanah: dilacak sampai sebabnya

Peta selisih S2 memperlihatkan riak samar di bidang tanah. Dugaan pertama —
"resolusi kisi yang terlihat menembus, hilang dengan jarak lebih rapat" —
**diukur dan salah**, dan dua dugaan berikutnya juga gugur:

| dugaan | uji | hasil |
|---|---|---|
| resolusi kisi | jarak 2 m → 1 m | amplitudo **naik** 2,66×, bukan turun |
| derau Monte Carlo | 128 → 512 cuplikan | tidak bergerak sama sekali (0,98×) |
| kuantisasi 8-bit | dibaca ulang di HDR linear | riaknya **lebih kuat**, bukan hilang |

Uji ketiga menuntut `IViewportRenderer::CaptureHdr` — radiance linier sebelum
eksposur dan pemetaan nada. Itu ditambahkan untuk pertanyaan ini, dan ia
menutup satu kelas kesalahan pengukuran seluruhnya: dua gambar yang berselisih
setengah tingkat kuantisasi terbaca sebagai sama, dan peta selisih yang
dikuatkan mengubah tangga kuantisasi menjadi pola yang tampak seperti temuan.

Sebabnya ditemukan dengan membuka artefak `.simprobe` dan membaca probenya satu
per satu: **sebagian probe jatuh di dalam benda pejal, dan yang di sana
dipanggang tepat nol.** Nol itu lalu ikut ke dalam interpolasi permukaan di
dekatnya.

| jarak | probe nol | amplitudo riak |
|---|---:|---:|
| 4 m | 0 dari 2.304 — 0,00% | 1,00× |
| 2 m | 35 dari 6.400 — 0,55% | 1,39× |
| 1 m | 246 dari 21.312 — 1,15% | 3,69× |

Kisi yang lebih rapat menaruh **lebih banyak** probe di dalam geometri, jadi
memperhalus jaraknya memperburuk artefaknya alih-alih memperbaikinya. Itu
membalik intuisi yang wajar, dan itulah yang membuatnya layak ditulis di sini.

Yang memperbaikinya sudah terjadwal: **S3**, karena membedakan "di dalam
dinding" dari "di depan dinding" menuntut probe membawa visibilitasnya, bukan
hanya iradiansinya. Sampai itu ada, jarak probe yang lebih rapat bukan
peningkatan kualitas yang gratis.

##### Tiga hal yang belum ada, dan disebutkan supaya tidak dikira ada

1. **Albedo satu angka untuk seluruh adegan** — 0,5, angka dan alasan yang sama
   dengan `kBounceAlbedo` di jalur clipmap SDF. Albedo per-material menuntut
   menjalankan graph material di CPU untuk tiap segitiga; sampai ada, yang
   meleset kecerahan pantulannya, bukan keberadaannya.

2. **Kubus bawaan, whitebox, dan terrain tidak menghalangi panggangan.**
   Ketiganya tidak punya kunci geometri di jalur ray cast mana pun, jadi
   `PickScene` tidak memuat segitiganya — sebuah ruangan yang dibangun dari
   kubus bawaan tetap disinari langit seolah di luar ruangan. **Ini disebutkan
   angkanya, bukan didiamkan:** bakery melaporkan berapa objek yang tidak punya
   geometri CPU sebelum memanggang. Yang memperbaikinya mengadopsi bentuk
   bawaan ke `MeshGeometryCache`, dan itu prasyarat S5.

3. **Berkas HDR belum bisa memanggang transport** — jalur panel memakai langit
   atmosferik atau hitam. Membacanya di sisi CPU adalah pekerjaan tersendiri.

### S3 — Oklusi arah · ✅

- Tiap probe membawa visibilitasnya, bukan hanya iradiansinya
- Objek dinamis memakainya supaya tidak menerima cahaya dari arah yang
  terhalang

**Selesai kalau:** sebuah benda di bawah meja lebih gelap daripada benda yang
sama di sebelah meja, dan selisihnya diukur; benda yang bergerak keluar-masuk
bayangan berubah mulus, tanpa loncatan pada batas sel.

#### Keadaannya sesudah S3 · ✅

Tiap probe membawa peta kedalaman oktahedral 8×8 — rata-rata jarak dan
kuadratnya per texel — dan uji Chebyshev membobot tiap sudut interpolasi
menurut apakah titik yang dinaunginya berada di depan atau di balik geometri
terdekat pada arah itu.

**Kriteria terimanya terukur.** Benda di bawah meja **39,9% seterang** benda
yang sama di sebelahnya; melintasi tepi meja, langkah terbesar **3,1% dari
rentangnya** — peralihan, bukan loncatan di batas sel. Dan pada sel yang
benar-benar melintasi dinding, diadu dengan acuan yang ditelusuri langsung:
kesalahan **0,120 menjadi 0,018**, enam setengah kali lebih dekat.

Riak yang tertinggal dari S2 ikut turun, dan angkanya menunjukkan bahwa ia
memang lahir dari probe yang terkubur:

| kisi | S2 | S3 terpasang |
|---|---:|---:|
| 2 m | 0,008977 | 0,008199 — **−9%** |
| 1 m | 0,023916 | 0,015717 — **−34%** |

##### Tiga hal yang ditemukan pengukuran, bukan perancangan

1. **Uji "benda di bawah meja" lulus tanpa menyentuh visibilitas sama sekali.**
   Kedelapan sudut selnya berada di sisi yang sama, jadi tidak ada apa pun untuk
   ditolak — angkanya identik dengan dan tanpa. Itu ditulis di ujinya, dan uji
   dinding tipis ditambahkan untuk menutupi apa yang ia lewatkan.

2. **Dugaan arahnya salah.** Saya menulis uji yang menuntut visibilitas
   *menggelapkan* titik di dekat dinding — cahaya bocor menembusnya. Yang
   terukur kebalikannya: kebocorannya **menggelapkan**, karena sudut sel di
   dalam dinding dipanggang nol. Ujinya sekarang menuntut lebih dekat ke acuan,
   bukan bergerak ke arah yang diduga.

3. **Tanpa bias permukaan, visibilitas memperburuk kisi rapat.** Sebuah
   permukaan berada tepat di batas geometri yang dilihat probe di dekatnya, jadi
   uji Chebyshev berayun di sekitar ambangnya. Diukur pada 1 m: **+12%** tanpa
   bias, **−34%** dengan bias 0,15 m. Biasnya tetap dalam meter dan bukan
   pecahan jarak antar-probe, dan itu juga diukur — 0,35 × jarak memberi −4% dan
   −30%, sementara 0,15 m tetap memberi −9% dan −34%.

   Tegangannya nyata dan ditulis apa adanya: **tanpa bias adalah yang terbaik di
   2 m** (−23%), dan bencana di 1 m. 0,15 m adalah kompromi yang dipilih dengan
   angka, bukan yang terbaik di kedua kisi.

##### Dan satu cacat yang hampir lolos

**Artefak `.simprobe` tidak menulis peta kedalamannya.** Berkasnya tetap sah dan
tetap terbaca; yang hilang cuma visibilitasnya, dan bersamanya seluruh S3 — pada
jalan kedua, diam-diam. Yang membongkarnya cuma angka megabyte di log: 4,00 MB
saat dipanggang, 0,88 MB saat dibaca kembali. Ia sempat memakan satu pengukuran
1 m sebelum ketahuan.

Versi artefak naik ke 2, ukuran peta masuk ke header, dan pembacanya menolak
ukuran yang bukan miliknya — peta 16×16 yang dibaca sebagai 8×8 tidak gagal, ia
menghasilkan visibilitas yang benar isinya dan salah tempatnya.

##### Yang ditemukan peninjauan: batas yang tidak bisa dibobot

**Koridor yang lebih sempit daripada jarak antar-probe adalah keadaan tempat
visibilitas kehabisan pilihan.** Diuji pada celah 0,6 m dengan kisi 2 m, diadu
dengan acuan yang ditelusuri langsung (0,129):

| | jawaban | kesalahan |
|---|---:|---:|
| tanpa visibilitas | 0,086 | 0,042 — terlalu gelap |
| dengan visibilitas | 0,289 | 0,160 — terlalu terang |
| **kisi 0,5 m, keduanya** | **0,113** | **0,016** |

Sudut selnya hampir seluruhnya berada di dalam dinding, dan yang di dalam
dipanggang nol. Menolaknya benar — nilainya memang tidak berarti — tetapi yang
tersisa adalah probe di luar koridor, dan menormalkan ke sana berarti menyatakan
seluruh bobotnya milik mereka. Nol-nol itu sebelumnya kebetulan menyeimbangkan
ke arah yang lain.

**Tidak ada pembobotan yang bisa mengarang informasi yang tidak dipanggang.**
Kisi 2 m tidak punya satu pun probe yang melihat apa yang dilihat lantai
koridor. Pada kisi 0,5 m kedua jalur menjawab **identik** — tidak ada yang
terhalang, jadi tidak ada yang perlu ditolak — dan sepuluh kali lebih dekat ke
acuan. Yang menyelesaikannya kerapatan, dan itu setelan pengarang (keputusan 8),
bukan bobot.

**Dua penambal dicoba dan ditinggalkan dengan angka.** Melantai bobot tiap probe
pada 0,40 menyelamatkan koridor (kesalahan 0,028) tetapi memotong perbaikan riak
kisi 1 m dari −34% menjadi **−10%**: ia melemahkan penolakan di mana-mana demi
keadaan yang jarang. Mencampur balik menurut bobot yang bertahan, dengan ambang
0,35, memberi hasil yang sama buruknya (riak 1 m −13%) sambil hampir tidak
menolong koridornya. Yang tersisa terpasang adalah campuran itu dengan ambang
**0,05** — ia berperilaku identik dengan tanpa campuran pada seluruh kasus yang
diukur, dan hanya menyala ketika benar-benar tidak ada sudut yang bertahan.

Sebuah lorong hitam pekat yang bentuknya mengikuti kisi lebih buruk daripada
cahaya yang salah, dan itulah yang dijaga ambang itu.

##### Satu penyederhanaan yang disebutkan

Peta kedalaman dibaca **texel terdekat**, tanpa interpolasi antar-texel dan
tanpa texel tepi. 64 arah per probe berarti bobot visibilitasnya bertangga dalam
arah. Uji kemulusan tidak menangkapnya — langkah terbesarnya 3,1% dari rentang —
tetapi ia belum diuji pada geometri yang tepinya tajam dan dekat.

##### Harganya

Peta kedalaman lima kali SH-nya. `bench.simlevel` pada 2 m: 0,88 MB menjadi
**4,00 MB**; pada 1 m, 2,93 MB menjadi **13,33 MB**. Waktu panggangnya naik 58%
pada 2 m (60,4 s → 95,3 s) dan 77% pada 1 m (207,9 s → 367,5 s) dengan 16 sinar
per texel.

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
