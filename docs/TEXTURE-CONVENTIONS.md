# Konvensi tekstur

Aturan yang berlaku untuk setiap berkas gambar yang masuk ke SimEngine: ruang
warnanya, alfanya, dan kedalaman bitnya.

**Ditulis di sini, bukan hanya di kode.** Ketiganya adalah kesepakatan antara
yang membuat aset dan yang memuatnya, dan kesepakatan yang hanya hidup di dalam
sebuah fungsi tidak bisa dibaca oleh orang yang sedang menyiapkan tekstur di
Substance atau Photoshop.

Yang menegakkannya ada di `Code/ImageIO/src/TextureColor.cpp`; yang mengujinya
ada di `Tests/ImageIOTests.cpp`.

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
