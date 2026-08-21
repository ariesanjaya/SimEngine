# Konvensi tekstur

Aturan yang berlaku untuk setiap berkas gambar yang masuk ke SimEngine: ruang
warnanya, alfanya, kedalaman bitnya, dan dari sudut mana ia dibaca.

**Ditulis di sini, bukan hanya di kode.** Ketiganya adalah kesepakatan antara
yang membuat aset dan yang memuatnya, dan kesepakatan yang hanya hidup di dalam
sebuah fungsi tidak bisa dibaca oleh orang yang sedang menyiapkan tekstur di
Substance atau Photoshop.

Yang menegakkannya ada di `Code/ImageIO/src/TextureColor.cpp`; yang mengujinya
ada di `Tests/ImageIOTests.cpp`. Konvensi UV di bawah ditegakkan di tempat lain —
di setiap importir mesh — dan diuji di `Tests/AssetTests.cpp`.

---

## Ruang warna: slot yang menentukan, bukan berkasnya

Sebuah PNG 8-bit tidak menyatakan apakah isinya warna atau angka. Berkas yang
sama bisa berisi albedo (warna, tersandi sRGB) atau roughness (angka, apa
adanya), dan **tidak ada yang bisa membedakannya dari isi berkasnya**.

Yang membedakan adalah **slot material yang memakainya**.

| Slot | Kegunaan | Ruang warna |
| --- | --- | --- |
| `baseColor`, `albedo`, `diffuse` | warna | **sRGB** |
| `emissive`, `emission` | warna | **sRGB** |
| `specularColor` | warna | **sRGB** |
| `normal` | angka | linear |
| `roughness`, `specularRoughness` | angka | linear |
| `metalness`, `metallic` | angka | linear |
| `height`, `displacement` | angka | linear |
| `occlusion`, `ao` | angka | linear |
| `opacity`, `mask` | angka | linear |
| *slot lain apa pun* | angka | linear |

Nama slot dibandingkan tanpa memedulikan besar-kecil hurufnya: `baseColor`,
`basecolor`, dan `BaseColor` adalah slot yang sama.

**Slot yang tidak dikenal dianggap angka.** Arahnya dipilih menurut kesalahan
mana yang lebih mudah ditemukan:

- Slot **warna** baru yang lupa didaftarkan → teksturnya tampil terlalu terang.
  Terlihat seketika, oleh siapa pun.
- Slot **angka** baru yang lupa didaftarkan, kalau bawaannya sRGB → nilainya
  didekode diam-diam. Normal map yang melewati dekode sRGB menghasilkan
  pencahayaan yang salah **sedikit** di seluruh permukaan: tidak ada peringatan,
  tidak ada piksel yang jelas keliru, hanya bayangan yang bentuknya agak lain.

Kesalahan kedua bisa bertahan berbulan-bulan. Karena itu bawaannya angka.

### Berkas yang menyatakan ruang warnanya sendiri

Sebagian format menyatakannya, dan pernyataan itu dibaca bila backend yang
aktif bisa membacanya (OpenImageIO bisa; stb tidak):

- **EXR dan HDR selalu linear** menurut spesifikasinya. Berkas float yang dipakai
  di slot warna **tidak didekode** — ia sudah linear.
- **PNG dan JPEG** boleh menyatakan sRGB. Bila menyatakannya dan slotnya warna,
  hasilnya sama saja: keduanya didekode.
- Bila berkas menyatakan **sRGB** sementara slotnya **angka**, itu konflik.
  Nilainya **dibiarkan apa adanya** — slot yang menentukan — dan konfliknya
  dicatat di log. Biasanya ia berarti normal map diekspor lewat jalur yang
  menandainya sebagai warna.

### Di mana dekodenya terjadi

| Bentuk | Yang mendekode |
| --- | --- |
| Warna 8-bit → GPU | **perangkat keras**, lewat format `_SRGB` |
| Warna 8-bit → konsumen CPU | `PrepareTexture`, dan hasilnya **dinaikkan ke float** |
| 16-bit dan float | tidak ada; sudah cukup teliti untuk linear apa adanya |

Tekstur warna 8-bit yang diunggah ke GPU **tidak** didekode di CPU. Perangkat
kerasnya melakukannya saat menyampel — gratis dan dengan ketelitian penuh —
sementara mendekode ke delapan bit di CPU menghancurkan bagian gelapnya: seluruh
nilai sRGB 0..15 akan jatuh ke linear 0 atau 1.

Konsumen CPU (baker, path tracer acuan, pembanding gambar) tidak punya perangkat
keras itu, jadi bagi mereka dekodenya dilakukan ke **float**, bukan di tempat.

---

## Alfa: straight, bukan premultiplied

**Konvensi tunggal di mesin ini adalah alfa *straight* (unassociated).** Setiap
gambar yang keluar dari `Sim::ImageIO` lewat `PrepareTexture` sudah dalam bentuk
itu, apa pun bentuknya di dalam berkas.

Alasannya: straight adalah yang dihasilkan alat gambar dan yang diharapkan
artis, sementara premultiply adalah keputusan **sisi render** — ia bergantung
pada apakah dilakukan sebelum atau sesudah dekode sRGB, dan karena itu tidak
boleh sudah terlanjur dilakukan di dalam berkas.

Yang perlu diketahui saat menyiapkan aset:

- **EXR beralfa premultiplied menurut spesifikasinya.** Ia dikenali dan
  diluruskan saat dimuat; tidak ada yang perlu dilakukan di sisi aset.
- **PNG dan TGA selalu straight** menurut spesifikasinya. Tidak ada yang berubah.
- Piksel yang **alfanya nol menjadi hitam** setelah diluruskan. Warnanya memang
  tidak bisa dipulihkan dari nol dikali apa pun. Kalau warna di daerah tembus
  pandang penting — misalnya untuk menghindari halo gelap saat di-mipmap —
  simpanlah asetnya sebagai PNG straight, bukan EXR premultiplied.
- **Alfa sendiri tidak pernah didekode sRGB.** Ia cakupan, bukan warna.

**Urutannya**: alfa diluruskan **lebih dulu**, baru warnanya didekode. Berkas
yang premultiplied melakukannya pada nilai yang masih tersandikan, jadi
pembaginya harus bekerja di ruang yang sama. Membalik urutannya menghasilkan
tepi yang salah terang pada setiap tekstur beralfa — dan itu, sekali lagi,
kesalahan yang tidak muncul sebagai galat.

### Tiga mode, dan material yang menentukannya

Alfa sebuah tekstur belum menyatakan apa yang harus dilakukan dengannya. Yang
menyatakannya adalah `alphaMode` di node `output.surface` material:

| Mode | Yang terjadi | Jalur gambarnya |
| --- | --- | --- |
| *(tidak disetel)* | alfa diabaikan; permukaan pejal | daftar buram |
| `mask` | fragmen di bawah `alphaCutoff` **dibuang** | daftar buram, keluar dari prepass dan bayangan |
| `blend` | warnanya **dicampur** menurut alfanya | daftar tersortir, belakang ke depan |

**Ketiganya berangkat dari alfa yang sama dan berakhir sangat berbeda, jadi
memilih yang salah bukan soal selera.** Yang dibuang kehilangan seluruh
gradasinya menjadi tepi biner; yang dipadu mempertahankannya. Untuk kotoran,
noda, dan sapuan tipis bedanya menentukan: yang dipadu mencampur *sedikit*
warnanya ke permukaan di belakangnya, sedangkan yang ditopeng **menggantikan**
warna permukaan itu dengan warnanya sendiri. Sponza pernah dipasang dengan
decal `BLEND`-nya diciutkan menjadi `mask`, dan hasilnya bercak bertepi keras
yang hampir hitam: 80,7% piksel yang lebih gelap dari 70 di potongan lengkung
kamera acuan adalah kuad decal itu. Sesudah dipadu, angkanya 12,9% → 6,1%.

Pagar kawat, dedaunan, dan huruf berlubang justru sebaliknya — `mask` yang
benar untuk mereka. Yang dibuang tidak menuntut urutan gambar apa pun, tetap
menulis kedalamannya, dan karena itu jauh lebih murah.

---

## Asal UV: kiri atas

**`v = 0` adalah baris pertama gambarnya.** Satu konvensi untuk seluruh mesin,
dan `Sim::Assets::MeshVertex::uv` adalah tempat ia tertulis.

Formatnya sendiri tidak sepakat, jadi sebagian importir harus membalik `v` dan
sebagian tidak:

| Format | Asal UV di berkasnya | Yang dilakukan importir |
| --- | --- | --- |
| glTF | kiri atas | dipakai apa adanya |
| FBX | kiri bawah | `v → 1 − v` |
| OBJ | kiri bawah | `v → 1 − v` (dibaca FBX SDK yang sama) |
| USD | kiri bawah | `v → 1 − v` |

**Kekeliruannya tidak pernah muncul sebagai galat.** UV yang tercermin tetap UV
yang sah: nilainya tetap di dalam jangkauan, teksturnya tetap terpasang, dan
yang berbeda hanya baris mana yang terbaca. Yang terlihat adalah hiasan yang
mendarat di tempat yang salah dan tulisan yang terbalik — hal-hal yang mudah
disangka salah aset.

Terukur atas Sponza, yang berkas FBX dan glTF-nya model yang sama: 99,41% titik
hasil impor FBX hanya cocok dengan hasil impor glTF **setelah** `1 − v`, dan
0,01% cocok apa adanya. Sesudah importirnya membalik, angkanya bertukar tempat —
99,43% cocok apa adanya, 0,00% perlu dibalik.

Yang menguncinya `uvQuad.obj`, `uvQuad.gltf`, `uvQuad.usda`, dan `uvQuad.fbx`
di `Resources/Meshes/`: satu segi empat yang sama, ditulis empat kali menurut
konvensi masing-masing format, dan satu uji yang menuntut keempatnya diimpor
menjadi UV yang sama persis.

### Set UV: yang diminta materialnya, bukan yang pertama

Sebuah mesh boleh membawa lebih dari satu set UV, dan **urutannya tidak
menyatakan apa-apa**. Yang menentukan set mana yang dipakai sebuah tekstur
adalah materialnya: `UVSet` pada tekstur di FBX, `TEXCOORD_n` pada glTF.

Set kedua lazimnya UV lightmap — pemetaan yang sengaja tidak tumpang tindih —
dan memakainya untuk tekstur biasa menghasilkan permukaan yang teksturnya
teregang ke petak-petak kecil, tanpa satu pun galat. Sponza membawa dua set di
tiap mesh FBX-nya (456 `LayerElementUV` di 230 mesh); materialnya kebetulan
meminta yang pertama, jadi mengambil "yang pertama" di sana benar karena
kebetulan, bukan karena dipilih.

**Pilihan itu lewat material muka, jadi muka yang materialnya tidak terpetakan
kehilangan pilihannya.** Berkas yang membawa elemen material bermode `eNone` di
sebelah elemen yang benar — bentuk yang ditinggalkan sebagian pengekspor —
dulu membuat seluruh mukanya bermaterial −1. Node bermaterial tunggal sekarang
memakainya kapan pun elemennya tidak menjawab, bukan hanya ketika elemennya
tidak ada.

`uvQuadTwoSets.fbx` menguncinya: `lightmapUV` lebih dulu, `map1` sesudahnya,
dan teksturnya menyebut `map1`. Angka kedua set sengaja berjauhan — yang benar
0,2/0,7 dan 0,6/0,1, yang salah 0/1 di keempat sudutnya.

### Bingkai tangent ikut terbalik

**Membalik `v` membalik arah tangan, dan tandanya harus ikut dibalik.**
Bitangent adalah `dP/dv`; mengganti `v` dengan `1 − v` membalik arahnya, jadi
arah tangan yang tertulis di sebuah berkas berlaku untuk UV yang **belum**
dibalik. Yang lupa membalikkannya menghasilkan peta normal yang tampak cekung
di tempat yang seharusnya cembung — sekali lagi, tanpa satu pun galat.

**Tangent milik berkasnya dipakai kalau ada, dan hanya dihitung ulang kalau
tidak ada.** Peta normal dipanggang terhadap bingkai tangent tertentu, dan
bingkai yang diturunkan ulang dari UV belum tentu bingkai yang sama; jahitannya
lalu terlihat sebagai garis yang pencahayaannya patah. Berlaku untuk glTF
(atribut `TANGENT`) maupun FBX (`LayerElementTangent`, arah tangannya dari
`LayerElementBinormal` bila ada). Satu sudut tanpa tangent membuat **seluruh
mesh** dihitung ulang: mencampur kedua sumber menghasilkan jahitan tepat di
tempat keduanya bertemu.

`uvQuad.fbx` menguncinya, dan ia berkas FBX **ASCII** supaya angkanya bisa
dibaca mata. Tangentnya sengaja `+Y` padahal UV-nya naik searah `+X`: yang
dihitung ulang dari UV pasti `+X`, jadi importir yang mengabaikan berkasnya
gagal pada sumbu yang berbeda — bukan pada tanda halus yang bisa lolos karena
kebetulan.

---

## Kedalaman bit

| Isi | Bentuk yang diharapkan |
| --- | --- |
| Albedo, emissive | PNG/JPEG 8-bit |
| Normal, roughness, metalness, mask | PNG 8-bit; 16-bit bila ada banding |
| Heightmap terrain | **PNG 16-bit atau TIFF 16/32-bit** — 8 bit adalah 256 tingkat tinggi, yaitu langkah empat meter pada terrain setinggi kilometer |
| Peta lingkungan (IBL) | HDR atau EXR |

Heightmap TIFF 32-bit float diterima dan **dikuantisasi ke 16-bit**, karena
itulah bentuk penyimpanan terrain. Kuantisasinya dicatat di log, beserta rentang
tinggi yang dipetakan — heightmap yang diam-diam kehilangan ketelitian terlihat
persis seperti heightmap yang memang begitu.

---

## Format yang diterima

Daftarnya **dibangkitkan dari backend yang terbangun**, bukan dipatok, jadi ia
menyusut atau bertambah mengikuti kemampuan yang sungguh ada. Asset Browser
membacanya dari sumber yang sama, sehingga ia tidak pernah menawarkan impor
untuk format yang tidak bisa dibaca.

Yang berlaku di build ini bisa dilihat di baris startup:

```
ImageIO backend: oiio 2.5.18 + tinyexr 3.2.0 + libtiff 4.5.1 + stb (11 format)
```

Rincian per format, dan alasan format lain ditolak, ada di
[PLAN-IMAGEIO.md](PLAN-IMAGEIO.md). Cara menyediakan backend tambahan ada di
[DEPENDENCIES.md](DEPENDENCIES.md).

**Sisi tulis jauh lebih sempit daripada sisi baca**, dan itu disengaja: menulis
sebuah format berarti menjanjikan berkas yang bisa dibuka alat lain. Yang
ditulis mesin ini hanya PNG greyscale 8/16-bit (heightmap, weightmap, peta hole,
peta kepadatan) dan TIFF (pertukaran dengan alat terrain).
