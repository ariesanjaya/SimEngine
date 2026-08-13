# Plan Impor Gambar (I0 → I5)

Membuat impor gambar SimEngine menangani format yang dipakai produksi — EXR dan
TIFF 16/32-bit — lewat satu seam `Sim::ImageIO` dengan backend yang bisa
ditukar, di atas stb yang sudah ada.

**I0–I3 sudah mendarat.** Empat backend, berlapis: **stb** wajib; **tinyexr**,
**libtiff**, dan **OpenImageIO** opsional, dan yang lebih mampu menang untuk
format yang diperebutkan. Tujuh format tanpa satu pun dari ketiganya, sebelas
dengan semuanya.

Penomoran **I** (image) supaya tidak bertabrakan dengan E, A, C (kain), dan R
(Embree) di [ROADMAP.md](ROADMAP.md).

**Asumsi urutan.** Milestone di bawah diurutkan mengikuti celah yang terverifikasi
di kode, bukan mengikuti daftar fitur satu pustaka. Kalau ternyata ada format lain yang
benar-benar menghambat pekerjaan sehari-hari, urutannya digeser — I2 dan I3 bisa
ditukar tanpa konsekuensi.

---

## Keputusan pokok

> **Yang berubah dari rencana semula, dan kenapa.** Rencana ini menempatkan OIIO
> sebagai satu-satunya backend tambahan. Pelaksanaannya mengubah itu dua kali,
> dan hasil akhirnya lebih baik daripada keduanya:
>
> 1. OIIO dibangun penuh dan lulus seluruh uji, lalu **dicabut** ketika biayanya
>    terukur: 396 MB arsip di dalam pohon plus rantai boost/pugixml/fmt yang
>    tidak disebut paketnya. Digantikan tinyexr + libtiff.
> 2. OIIO **dikembalikan sebagai backend opsional yang didahulukan** — bukan
>    pengganti, melainkan lapisan di atas. Yang menyediakannya dapat DDS, PSD
>    utuh, dan metadata colorspace; yang tidak, tetap membangun seluruh mesin.
>
> **Yang menjadikannya syarat dihapus, bukan yang menjadikannya ada.** Membuat
> OIIO wajib berarti SimEngine kehilangan sifat "bisa dibangun hanya dengan
> FetchContent dan Vulkan SDK", dan itu harga yang tidak sepadan untuk satu
> format tambahan.
>
> **Bukan karena OpenUSD.** Layak diluruskan: USD tidak pernah mendekode gambar,
> dan kaitannya dengan OIIO hidup di Hydra/Hio — bagian imaging yang tidak
> dibawa paket USD di sini sama sekali. Alasan memakai OIIO adalah kemampuan
> gambarnya. Ukurannya ada di [DEPENDENCIES.md](DEPENDENCIES.md).
>
> **Yang tidak berubah sama sekali: seam-nya.** Tiga kali backend berganti —
> ditambah, dicabut, dikembalikan — tanpa satu pun titik dekode, `AssetTypes`,
> `Ibl.cpp`, atau uji nilai tersentuh. Itu alasan I0 didahulukan.

**Backend tambahan opsional, stb wajib.** Persis pola yang sudah dipakai OpenUSD
di `cmake/SimDeps.cmake` — dipakai kalau ada, dilewati kalau tidak. Yang tidak
punya satu pun tetap bisa membangun seluruh mesin; yang hilang hanya format
tambahan, dan daftarnya menyusut dari sebelas menjadi tujuh. Ketiganya bisa
dimatikan dengan sengaja lewat `-DSIM_WITH_OIIO=OFF`, `-DSIM_WITH_EXR=OFF`, dan
`-DSIM_WITH_TIFF=OFF`, supaya jalur tanpa mereka benar-benar diuji — dan
ketiganya memang dijalankan di setiap perubahan, bukan diasumsikan.

**Dekode gambar tidak pernah masuk runtime.** Ia pengondisian aset, bukan
pemuatan runtime. Batasnya: `Sim::ImageIO` boleh dipakai importir, editor,
baker, dan `SimHeadless`; tidak boleh oleh jalur yang dikirim ke pemain.

**Tekstur terkompresi GPU bukan urusan plan ini.** Tidak satu pun backend di
sini mengompresi ke BCn, dan tidak seharusnya. Kontainer KTX2 dan kompresornya
(astcenc/bc7enc/basisu) adalah sumbu yang berbeda dan rencananya terpisah.

---

## Kondisi sekarang

### Lima titik dekode, tiga kedalaman bit

| Lokasi | Panggilan | Bentuk data |
| --- | --- | --- |
| `Code/Assets/src/Thumbnail.cpp:111` | `stbi_load_from_memory` | 8-bit RGBA, dari memori |
| `Code/Vegetation/src/VegetationIo.cpp:44` | `stbi_load` (1 kanal) | 8-bit, peta kepadatan |
| `Code/Terrain/src/TerrainIo.cpp:42` | `stbi_load` (1 kanal) | 8-bit, heightmap |
| `Code/Terrain/src/TerrainIo.cpp:350` | `stbi_load_16` (1 kanal) | 16-bit, heightmap |
| `Code/Render/src/Ibl.cpp:153` | `stbi_loadf` (3 kanal) | float linear, Radiance RGBE |

Ditambah satu penulis: `Code/Terrain/src/HeightmapPng.cpp` menulis PNG 16-bit
**dengan tangan** di atas `stbi_zlib_compress`, karena `stbi_write_png` hanya
bisa 8-bit. Komentarnya menyebut alasannya dengan jujur — dan kode itu bisa
dihapus begitu ada backend yang menulis PNG 16-bit sendiri.

### Tiga celah yang terverifikasi

**1. `.psd` diiklankan tapi dukungannya bersyarat.** `AssetTypes.cpp:45`
mendaftarkan `.psd` sebagai `AssetType::Texture`, sementara `stb_image.h` yang
diambil FetchContent menyatakan sendiri di baris 28: *"PSD (composited view
only, no extra channels, 8/16 bit-per-channel)"*. PSD tanpa composite gagal,
CMYK gagal, layer hilang tanpa peringatan.

Ini cacat struktural, bukan cacat format: **daftar format dipatok konstan
sementara kemampuannya tidak.** Perbaikannya bukan menghapus `.psd`, melainkan
membangkitkan daftarnya dari backend yang aktif.

**2. Tidak ada EXR.** `Ibl.cpp` hanya membaca Radiance RGBE (`.hdr`). HDRI
produksi datang sebagai `.exr` — 16-bit half, multi-channel, dengan metadata.

**3. Tidak ada TIFF.** Heightmap dari World Machine, Gaea, dan sumber GIS
umumnya TIFF 16/32-bit, sementara jalur terrain terkunci ke PNG 16-bit.

### Jebakan yang sudah ditutup

`Third-Party/OpenUSD/include/OpenImageIO/` dulu berisi header OpenImageIO yang
ikut terbawa distribusi OpenUSD prebuilt — **tanpa pustakanya**. Akibatnya
`#include <OpenImageIO/imageio.h>` kompilasi dengan sukses lalu gagal saat link,
dan header itu ada di include path setiap target yang menautkan `Usd::Usd`,
termasuk target yang tidak boleh melihatnya.

**Ditutup dua kali, keduanya struktural.** Salinan di dalam paket USD dihapus.
OIIO yang dipakai sekarang tinggal di root sendiri, `Third-Party/OpenImageIO/`,
yang hanya masuk include path `Sim::ImageIO` — jadi tidak ada lagi jalan bagi
header itu untuk bocor ke target lain. Dan SimDeps memeriksa **header dan
pustaka sekaligus**: kalau hanya header yang ada, backendnya dilewati dengan
pesan yang menyebut sebabnya, bukan dinyalakan untuk gagal saat link.

## Format yang didukung, dan yang ditolak

Pustaka yang dipakai bisa membaca lebih banyak daripada yang diekspos — stb
sendiri membaca GIF, PIC, dan PNM. **Yang diekspos SimEngine hanya sembilan.**
Daftar ini dikurasi, bukan diwarisi: setiap format yang terdaftar di
`AssetTypes` adalah janji dukungan, dan janji yang tidak bisa ditepati lebih
buruk daripada format yang tidak ada.

Tiga saringan yang menghasilkan daftarnya: apakah ada tool di jalur kerja game
yang menghasilkannya; apakah sudah ada padanan lebih baik yang didukung; dan
apakah ada jalur runtime yang bisa memakainya.

### Didukung

| Format | Baca | Tulis | Backend | Kenapa ada di daftar |
| --- | --- | --- | --- | --- |
| **PNG** | ✅ | ✅ grey 8/16 | stb / oiio | Albedo, mask, heightmap 16-bit |
| **JPEG** | ✅ | — | stb / oiio | Tekstur sumber dan foto referensi |
| **TGA** | ✅ | — | stb / oiio | Masih keluar dari DCC lama — `shaderBall.fbx` sendiri merujuk `checkerA.tga` |
| **HDR/RGBE** | ✅ | — | stb / oiio | Peta lingkungan untuk IBL |
| **BMP** | ✅ | — | stb / oiio | Tidak menambah dependensi apa pun, sesekali muncul dari tool lama |
| **PSD** | ✅ | — | stb / oiio | Artis mengirimnya. Lewat stb **composited view 8/16-bit saja**, di luar itu gagal dengan pesan; lewat OIIO utuh |
| **OpenEXR** | ✅ | — | tinyexr / oiio | HDRI untuk IBL, dan nanti keluaran path tracer referensi R4 |
| **TIFF** | ✅ | ✅ | libtiff / oiio | Heightmap 16/32-bit dari World Machine, Gaea, dan sumber GIS |
| **DDS** | ✅ | — | **oiio saja** | Aset lama. Dibaca sebagai berkas **sumber** — bloknya didekompres menjadi piksel mentah, bukan diteruskan sebagai bentuk runtime |

**Sisi tulis jauh lebih sempit daripada sisi baca, dan itu disengaja.** Menulis
sebuah format berarti menjanjikan berkas yang bisa dibuka alat lain, dan janji
itu hanya dibuat untuk format yang memang ditulis jalur kerja di sini: PNG
greyscale untuk heightmap, weightmap, peta hole, dan peta kepadatan; TIFF untuk
pertukaran dengan alat terrain.

**Kolom backend menyebut siapa yang bisa, bukan siapa yang dipakai.** Yang
dipakai adalah yang pertama di daftar prioritas dan benar-benar terbangun: OIIO
bila ada, lalu tinyexr/libtiff, lalu stb. `imageio::BackendFor(".png")`
menjawabnya saat itu juga, dan pesan galat menyebutnya.

**DDS dibaca, tapi tidak pernah diteruskan sebagai bentuk runtime.** Bloknya
didekompres menjadi piksel mentah, jadi ia berperan sebagai berkas sumber
seperti PNG. Bentuk runtime tetap KTX2 sebagaimana
[PLAN-TEXTURE.md](PLAN-TEXTURE.md) menetapkannya — mencampuradukkan keduanya
berarti tekstur yang sudah terkompresi didekompresi lalu dikompresi ulang.

### Ditolak, dan alasannya

Tidak satu pun dari daftar ini punya backend di pohon ini, jadi menolaknya
tidak butuh saklar apa pun — ia tertolak karena memang tidak ada yang
membacanya:

| Ditolak | Alasan |
| --- | --- |
| WebP, GIF, JPEG-2000, JPEG XL, HEIF/AVIF | Belum ada peminta, dan masing-masing menyeret satu pustaka |
| RAW (libraw), R3D | Camera raw. Artis mengekspor dulu; ini bukan format aset |
| Movie via FFmpeg (AVI/MOV/MP4) | Video texture adalah subsistem tersendiri, bukan impor gambar |
| OpenVDB, Ptex | Volume dan per-face texture. Tidak ada jalur runtime yang memakainya |
| DICOM, FITS | Medis dan astronomi |
| Cineon, DPX | Format film DI dan scan |
| IFF, RLA, SGI, Softimage PIC, PNM, Zfile, ICO | Warisan; tidak ada yang menghasilkannya di jalur kerja ini |
| GIF, PIC, PNM | **stb membacanya, dan tetap tidak diekspos.** Inilah bedanya daftar yang dikurasi dari daftar yang diwarisi |

Bahaya konkret dari daftar yang terlalu panjang: Asset Browser menawarkan impor,
artis mengimpor GIF animasi, dan yang didapat adalah frame pertama tanpa
peringatan.

**Dampaknya ke dependensi**, dan inilah alasan sebenarnya daftar ini penting:
rantai wajibnya **kosong** — stb sudah di pohon. Lapisan opsionalnya menambah
tinyexr + zlib, lalu libtiff, lalu (bila disediakan) OIIO beserta
boost/pugixml/fmt-nya. Tidak satu pun lapisan itu menyeret libraw, libheif,
ffmpeg, OpenVDB, Ptex, giflib, libwebp, OpenJPEG, maupun libjxl.

### Yang sebenarnya bertambah dibanding stb

Jujur tentang untungnya: stb sudah membaca JPEG, PNG (termasuk 16-bit), TGA,
BMP, HDR, dan PSD terbatas. Yang **benar-benar baru** hanya **TIFF** dan
**OpenEXR**.

Sisa nilainya bukan pada jumlah format melainkan pada kebenaran: kedalaman bit
yang tidak ditebak, konversi tipe tanpa gamma tersembunyi, kuantisasi float yang
dicatat alih-alih dilakukan diam-diam, dan nama kanal yang terbawa — satu-satunya
hal yang membedakan HDRI dari render multi-channel yang bentuknya sama persis.

**Colorspace berkas tidak ikut bertambah**, dan itu tidak menghalangi I4: I4
sendiri menetapkan bahwa yang memutuskan sRGB atau linear adalah **slot material
yang memakainya**, bukan metadata berkasnya. Yang perlu dibaca dari berkas cuma
kasus yang menyatakan dirinya sendiri, dan EXR sudah melakukannya secara
definisi.

### Waktu yang tepat, dan kenapa

[PLAN-RENDER.md](PLAN-RENDER.md) mencatat jalur tekstur material sebagai
**sedang berjalan** — "pipeline material menggantikan `box.frag` di pass
forward" belum mendarat, dan bersamanya jalur yang memuat tekstur material dari
berkas. Kode itu belum ditulis.

Menaruh seam sekarang berarti jalur tekstur material lahir sudah memakainya.
Menundanya berarti menulis titik dekode keenam langsung ke stb, lalu
memindahkannya lagi nanti.

---

## Arsitektur

```
Code/ImageIO/
  include/Sim/ImageIO/
      Image.h              buffer piksel + deskriptor (ukuran, kanal, tipe,
                           colorspace, nama kanal, alfa premultiplied)
      ImageIO.h            Read/Write/Encode/Probe + kueri kapabilitas
  src/
      Backend.h            antarmuka IBackend
      ImageIO.cpp          registry: pemilihan backend per ekstensi
      PixelOps.cpp         konversi tipe & jumlah kanal, dipakai bersama
      PngWrite.cpp         enkoder PNG greyscale 8/16-bit
      BackendStb.cpp       selalu ada
      BackendOiio.cpp      isinya hanya bila SIM_WITH_OIIO — **didahulukan**
      BackendExr.cpp       isinya hanya bila SIM_WITH_TINYEXR
      BackendTiff.cpp      isinya hanya bila SIM_WITH_LIBTIFF
```

Antarmukanya menampung apa yang dipakai keenam titik dekode: `uint8`, `uint16`,
dan `float`; paksa jumlah kanal (1, 3, atau 4); baca dari berkas maupun dari
memori.

**Konversi tinggal di satu tempat.** `PixelOps` dipakai keempat backend, karena
aturannya harus sama persis: dua backend yang membulatkan 16-bit ke 8-bit
sedikit berbeda menghasilkan berkas yang terbaca berbeda menurut backend mana
yang kebetulan memegang formatnya. Uji lintas-backend menegakkan itu — dan ia
punya gigi justru karena backend-backend itu **sengaja dibiarkan bertindih**.

**Kapabilitas adalah data, bukan konstanta.** `ReadableExtensions()` dan
`WritableExtensions()` mengembalikan gabungan kemampuan backend yang aktif, dan
`AssetTypes` membacanya — bukan sebaliknya. Inilah yang menutup cacat `.psd`
secara permanen.

**`Sim::ImageIO` tidak boleh bergantung pada `Sim::RHI`.** Ia hanya menghasilkan
piksel di memori; yang mengunggahnya ke GPU adalah pemanggilnya. Aturan yang
sama menjaga `SimHeadless` tetap bisa jalan tanpa display.

---

## Milestone

### I0 — Seam `Sim::ImageIO` dengan backend stb · ✅

Modul baru, backend stb sebagai satu-satunya implementasi. Kelima titik dekode
dialihkan lewatnya. `AssetTypes` membaca daftar format dari kapabilitas backend.

**Milestone ini bernilai berdiri sendiri.** Bahkan kalau tidak satu pun backend
tambahan pernah masuk, cacat `.psd` tertutup dan jalur tekstur material punya
tempat yang benar untuk memanggil. Terbukti langsung: backend EXR/TIFF berganti
pustaka seluruhnya setelah I0, tanpa satu pun titik panggil berubah.

**Kriteria terima**
- Tidak ada `stbi_` di luar `Code/ImageIO/src/BackendStb.cpp` — diuji dengan
  grep di test, bukan dengan disiplin.
- Thumbnail, heightmap 8-bit dan 16-bit, peta kepadatan vegetasi, dan IBL `.hdr`
  semuanya masih bekerja persis seperti sebelumnya.
- Uji doctest: membaca PNG 8-bit, PNG 16-bit, dan HDR dari `Resources/` dan
  memeriksa dimensi, jumlah kanal, serta tipe datanya.
- Berkas rusak menghasilkan galat dengan pesan, bukan crash — diuji dengan
  berkas yang sengaja dipotong.

### I1 — Backend gambar opsional · ✅

`SIM_WITH_EXR`, `SIM_WITH_TIFF`, dan `SIM_WITH_OIIO` (semuanya ON) memilih
backend tambahan. tinyexr diambil lewat FetchContent seperti stb dan cgltf;
libtiff dicari di sistem; OIIO dicari di `Third-Party/OpenImageIO` dan
**diperiksa dua sisi — header dan pustaka**, karena header tanpa pustaka
kompilasi dengan sukses lalu gagal saat link. Yang tidak ada dilewati dengan
pesan yang menyebut apa yang harus disediakan.

**Prioritas ditetapkan di satu tempat**, di registry: OIIO, lalu tinyexr, lalu
libtiff, lalu stb. Yang lebih mampu menang untuk format yang diperebutkan —
bukan karena lebih cepat, melainkan karena ia membawa metadata yang lain tidak
punya.

**Kriteria terima** — semuanya terpenuhi:
- Tiga konfigurasi dibangun dan diuji di setiap perubahan, bukan diasumsikan:
  lengkap (11 format), tanpa OIIO (10), dan stb saja (7). Ketiganya lulus
  seluruh suite.
- Versi tiap backend tercatat di log startup dan di
  [DEPENDENCIES.md](DEPENDENCIES.md):
  `ImageIO backend: oiio 2.5.18 + tinyexr 3.2.0 + libtiff 4.5.1 + stb (11 format)`.
- **Backend yang berbagi format membaca berkas yang sama secara identik, bit per
  bit.** Ini kriteria yang paling berharga dari seluruh milestone ini, dan ia
  baru punya gigi setelah OIIO ada: penyusunan ulang kanal EXR dan pembacaan
  strip TIFF yang ditulis tangan di sini diadu dengan implementasi yang dipakai
  seluruh industri, pada berkas yang sama. Keduanya sepakat.
- Tidak ada pustaka gambar yang dipanggil dari luar backend-nya —
  `SimImageIOTests` menyisir seluruh `Code/` untuk `stbi_`, `tinyexr.h`, dan
  `tiffio.h`.
- Header OIIO hanya masuk include path `Sim::ImageIO`, tidak ke target lain.

### I2 — EXR untuk IBL · ✅

Konsumen nyata pertama. `Ibl.cpp` menerima `.exr` di samping `.hdr`. EXR half
dikonversi ke float saat baca.

**Kriteria terima**
- HDRI `.exr` menghasilkan IBL yang **cocok dengan versi `.hdr` dari sumber yang
  sama** dalam toleransi yang ditulis di test — bukan sekadar "menghasilkan
  gambar".
- Berkas `.exr` multi-channel yang bukan RGB ditolak dengan pesan yang
  menyebutkan nama kanal yang ditemukan.
- Tanpa backend EXR, `.exr` tidak muncul di daftar format sama sekali dan Asset
  Browser tidak menawarkannya — ditolak, bukan dimuat separuh.

### I3 — Heightmap TIFF 16/32-bit · ✅

Terrain membaca TIFF 16-bit dan 32-bit float di samping PNG. Penulis PNG 16-bit
tangan **pindah ke belakang seam** sebagai `Code/ImageIO/src/PngWrite.cpp`.

Rencananya semula menghapus enkoder itu begitu ada pustaka yang menulis PNG
16-bit sendiri. Tidak ada: tinyexr menulis EXR, libtiff menulis TIFF, dan
`stbi_write_png` hanya 8 bit. Jadi ia tetap ada, permanen — yang berubah cuma
tempatnya, dan `Sim::Terrain` tidak lagi mengenal stb sama sekali.

**Kriteria terima**
- Heightmap TIFF 16-bit dari World Machine (atau berkas uji yang setara) dimuat
  dengan rentang tinggi yang benar — diperiksa nilainya, bukan bentuknya.
- Round-trip tulis-baca 16-bit identik bit-per-bit lewat kedua jalur penulis.
- TIFF 32-bit float tidak dipotong ke 16-bit diam-diam; kalau dikuantisasi,
  kuantisasinya dicatat di log.

### I4 — Colorspace dan alpha · ✅

Bagian yang paling halus dan paling sering salah, karena kesalahannya tidak
pernah muncul sebagai galat.

Aturannya ditulis lengkap di [TEXTURE-CONVENTIONS.md](TEXTURE-CONVENTIONS.md) —
di luar kode, karena ia kesepakatan antara yang membuat aset dan yang memuatnya.
Yang menegakkannya ada di `Code/ImageIO/src/TextureColor.cpp`.

- Metadata colorspace dibaca dan dibawa di `ImageDesc`, tidak dibuang. Backend
  yang bisa membacanya (OIIO) mengisinya; yang tidak, meninggalkannya `Unknown`
  — dan `Unknown` diperlakukan sebagai keadaan yang jujur, bukan sebagai sRGB
  yang tersirat.
- **Slot yang menentukan, bukan berkasnya.** `UsageForSlot` adalah tabelnya, dan
  ia eksplisit dengan sengaja. Slot tak dikenal jatuh ke `Data`, arah yang
  dipilih menurut kesalahan mana yang lebih mudah ditemukan.
- **Dekode 8-bit terjadi di GPU, bukan di CPU.** `NeedsSrgbGpuFormat` yang
  memutuskannya. Mendekode ke delapan bit di CPU menjatuhkan seluruh nilai sRGB
  0..15 ke linear 0 atau 1 — jalur CPU karena itu menaikkannya ke float.
- Alfa premultiplied dikenali dan dinormalkan ke **straight**, dan urutannya
  ditegakkan oleh satu pintu masuk (`PrepareTexture`): alfa lebih dulu, baru
  warna.

**Kriteria terima** — semuanya terpenuhi:
- Albedo yang sama dimuat dari `albedo-srgb.png` dan `albedo-linear.exr`
  menghasilkan nilai linear yang sama. Padanannya **tepat**, bukan kira-kira:
  nilai EXR-nya dihitung dari byte PNG-nya, jadi yang diuji adalah kebenaran
  dekodenya — termasuk kedua cabang kurva sRGB.
- **Uji yang mengunci**: berkas yang sama, yang ditandai sRGB oleh backend yang
  membaca metadatanya, dimuat lewat slot `normal`, `roughness`, `metalness`,
  `height`, `occlusion`, dan `mask` — dan bytenya tidak berubah satu pun.
  Padanan GPU-nya ikut dikunci: `NeedsSrgbGpuFormat` selalu false untuk `Data`.
- Konvensi alfa tercatat di [TEXTURE-CONVENTIONS.md](TEXTURE-CONVENTIONS.md),
  beserta akibat praktisnya bagi yang menyiapkan aset — termasuk kenapa piksel
  beralfa nol menjadi hitam, dan kapan itu berarti asetnya sebaiknya PNG.

**Satu cacat lintas-backend ditemukan dan ditutup di sini.** tinyexr tidak
melaporkan alfa premultiplied sama sekali, sementara OIIO melaporkannya — jadi
berkas EXR beralfa yang sama terbaca dengan dua konvensi berbeda menurut backend
mana yang kebetulan aktif. Alfa EXR premultiplied menurut spesifikasinya; kini
keduanya mengatakan itu.

### I5 — Jalur referensi dan regresi CI · ⬜

Menyambung ke [PLAN-EMBREE.md](PLAN-EMBREE.md) R4, yang membutuhkan penulisan
EXR dan pembandingan gambar.

- Path tracer referensi menulis EXR lewat `Sim::ImageIO`.
- **`ImageCompare` di R4 ditulis sendiri, bukan diserahkan ke OIIO.** OIIO
  memang menyediakan `ImageBufAlgo::compare()` — tapi ia **opsional**, dan
  regresi visual yang hanya berjalan di mesin yang menyediakannya bukan regresi
  visual. Jadi RMSE, error rata-rata, error maksimum, dan PSNR ditulis di sini:
  sekitar empat puluh baris di atas dua buffer float, dan sebagai gantinya
  ambangnya bisa dinyatakan per-kanal — yang justru dibutuhkan regresi visual,
  dan yang tidak diberikan `compare()`.
- `SimHeadless` bisa membandingkan dua EXR dan mengembalikan kode keluar
  berdasarkan ambang — itulah bentuk "regresi visual" yang disebut A4 di
  [PLAN-AI.md](PLAN-AI.md).

**Kriteria terima**
- `SimHeadless --compare a.exr b.exr --threshold 0.01` mengembalikan 0 atau 1
  sesuai hasilnya, dan mencetak angkanya.
- Satu uji regresi visual berjalan di CI dan **pernah gagal sekali secara
  sengaja** untuk membuktikan ia bisa gagal.

---

## Risiko

| Risiko | Tanda awal | Mundur ke |
| --- | --- | --- |
| Aset diimpor dengan backend lengkap lalu dibuka di mesin tanpanya | Tekstur hilang diam-diam | Sudah ditutup: format yang tidak didukung ditolak dengan pesan yang menyebut backend yang dibutuhkan, dan `.exr` tidak muncul di daftar format sama sekali |
| Dua backend membaca berkas yang sama secara berbeda | Uji lintas-backend gagal | Itu memang gunanya uji itu; selidiki sebelum lanjut, jangan pilih yang "kelihatan benar" |
| Dekode dari `TaskPool` menemukan jalur yang tidak thread-safe | Crash acak saat impor massal | Satu handle per panggilan, tidak pernah dipakai bersama. Keadaan global stb tidak pernah disentuh — konversi tipe dikerjakan sendiri justru karena itu |
| TIFF di alam liar memakai varian yang tidak tertangani | Berkas ditolak saat impor | libtiff menangani strip/tile, LZW/deflate/PackBits, dan predictor secara transparan. Yang belum: planar (separated), yang ditolak dengan pesan yang menyebutnya |
| EXR berubin atau multipart | Gambar kosong, atau bagian yang salah dimuat | Berubin disusun ulang; multipart ditolak tinyexr dengan menyebut sebabnya, dan ditangani OIIO bila ada |
| Aset diimpor lewat OIIO, dibuka di mesin tanpa OIIO | `.dds` tidak terbaca; PSD berlapis gagal | Ditolak dengan pesan yang menyebut backend yang dibutuhkan. **Ini alasan sisi tulis tidak pernah menghasilkan DDS**: apa pun yang ditulis mesin ini harus terbaca oleh build paling minimal |
| Backend opsional membuat perilaku berbeda antar-mesin | Berkas sama, hasil beda | Uji lintas-backend menuntut bit-per-bit identik untuk format yang diperebutkan; ketiga konfigurasi dijalankan di setiap perubahan |

## Yang tidak boleh ditunda

- **Seam sebelum jalur tekstur material ditulis.** Ini satu-satunya alasan I0
  mendesak. Setelah kode itu lahir memakai stb langsung, memindahkannya berarti
  menyentuh jalur material yang sedang aktif dikerjakan.
- **Daftar format dibangkitkan dari backend.** Daftar konstan adalah akar cacat
  `.psd`, dan akan melahirkan cacat yang sama untuk setiap format berikutnya.
- **Colorspace dibawa sejak baca.** Metadata yang dibuang di lapisan I/O tidak
  bisa ditemukan kembali di lapisan material.

---

## Yang sengaja tidak dikerjakan

- **Kompresi BCn dan kontainer KTX2.** Sumbu berbeda, rencana terpisah.
- **Dekode gambar di runtime.** Ia pengondisian aset, bukan pemuatan runtime.
- **Cache tekstur di luar RAM.** Berguna nanti untuk path tracer referensi
  dengan tekstur lebih besar dari RAM, tapi bukan sekarang — R4 belum sampai ke
  sana.
- **DDS di build tanpa OIIO.** Hanya OIIO yang membacanya, dan itu diterima apa
  adanya: belum ada yang benar-benar membawa aset DDS lama ke folder aset. Kalau
  suatu saat ada dan OIIO tidak bisa diandalkan hadir, barulah ia dapat backend
  sendiri.
- **Dua puluh empat format sisanya.** Daftar lengkap beserta alasan penolakannya
  ada di bagian [Format yang didukung, dan yang ditolak](#format-yang-didukung-dan-yang-ditolak).
  Aturannya: format masuk ketika ada yang benar-benar membawanya ke folder aset,
  bukan ketika pustakanya kebetulan bisa membacanya.
