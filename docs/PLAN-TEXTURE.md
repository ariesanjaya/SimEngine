# Plan Tekstur Terkompresi GPU (T0 → T5)

Mengubah jalur tekstur runtime dari RGBA8 mentah menjadi blok terkompresi BCn di
dalam kontainer KTX2, dikompresi sekali saat impor dan di-`memcpy` apa adanya ke
`VkImage` saat muat.

Penomoran **T** (texture). Bersambung erat dengan [PLAN-IMAGEIO.md](PLAN-IMAGEIO.md)
— I0–I4 membaca berkas sumber, plan ini menghasilkan bentuk runtime-nya. Keduanya
dua ujung dari satu langkah impor.

---

## Keputusan pokok

**BCn langsung, bukan Basis Universal.** KTX2 bisa membawa keduanya. Basis
menyelesaikan portabilitas — satu berkas yang di-transcode ke BCn, ASTC, atau
ETC2 sesuai perangkat — dan itu masalah yang belum kita punya. Target sekarang
desktop, jadi BC4/BC5/BC6H/BC7 langsung memberi kualitas terbaik per pemakaian,
kendali penuh atas format per slot, dan **nol biaya transcode saat muat**. Kalau
target mobile muncul, UASTC ditambahkan sebagai jalur kedua tanpa membongkar
kontainernya.

**Kompresi hanya saat impor, tidak pernah saat runtime.** Encoder BC7 dan BC6H
berkualitas tinggi butuh detik per tekstur 4K.

**Pemakaian (usage) melekat pada aset tekstur, bukan pada slot material.**
Alasannya di bawah — ini keputusan desain yang paling menentukan di seluruh plan.

---

## Kondisi sekarang

Tiga fakta yang diverifikasi di kode:

- ~~**`Device.cpp:279` belum meminta `textureCompressionBC`**~~ — **sudah,
  di T0.** Ketiadaannya dicatat di log, bukan disimpulkan dari VRAM yang penuh.
- **`Texture.cpp` mengunggah `VK_FORMAT_R8G8B8A8_UNORM`** — masih satu-satunya
  jalur unggah yang ada.
- ~~**Jalur tekstur material belum ditulis**~~ — **sudah mendarat**, sebagai
  Jalur A di [ANALISA-TEKSTUR-PERMUKAAN.md](ANALISA-TEKSTUR-PERMUKAAN.md):
  `IViewportRenderer::AcquireTexture` memuat lewat `Sim::ImageIO` menjadi RGBA8,
  dan material impor sudah menyebut teksturnya.

**Yang ketiga sudah terjadi, dan itu mengubah urgensinya menjadi hutang yang
sudah berjalan.** Peringatan di plan ini — "begitu jalur itu lahir memuat PNG
langsung ke RGBA8, KTX2 menjadi jalur kedua yang harus dirawat selamanya" —
terjadi persis seperti tertulis. Konsekuensinya bukan membatalkan Jalur A
melainkan **T3 harus menggantikan isi `AcquireTexture`, bukan berdiri di
sebelahnya**: satu fungsi, dua sumber format, dan yang RGBA8 menjadi jalur mundur
untuk berkas yang belum di-bake.

Satu hal yang sudah benar dan harus dijaga: `MaterialParameterBlock.h:101`
mencatat bahwa renderer mengikat seluruh tekstur sekali sebagai **satu larik
bindless** dan material hanya menyimpan nomornya. Larik bindless boleh berisi
`VkFormat` yang berbeda-beda, jadi mencampur BC7, BC5, dan BC6H di satu larik
tidak menimbulkan masalah.

---

## Keputusan desain: di mana pemakaian dicatat

Sebuah tekstur harus dikompresi berbeda tergantung untuk apa ia dipakai. Albedo
menjadi BC7 sRGB, normal menjadi BC5 linear, roughness menjadi BC4 linear, HDRI
menjadi BC6H. Pertanyaannya: siapa yang tahu?

**Slot material tahu semantiknya** — `TextureBinding::name` berisi `"tAlbedo"`,
`"tNormal"`, dan seterusnya. Tapi satu `Uuid` tekstur bisa dirujuk dua material
berbeda pada slot yang berbeda, dan saat itu tidak ada satu jawaban benar.

**Keputusan: pemakaian dicatat di aset teksturnya**, lewat berkas pengaturan di
sebelahnya, sebagaimana `MeshSettings` melakukannya untuk mesh. Ini yang
dilakukan engine lain dan yang diharapkan artis — sebuah berkas normal map
adalah normal map, di mana pun ia dipakai.

Konsekuensinya, dan cara menanganinya:

- **Konflik dideteksi, bukan diselesaikan diam-diam.** Kalau graph material
  mengikat tekstur bertanda `Color` ke slot bernama `tNormal`, panel material
  memperingatkan. Itu hampir selalu kesalahan orang, dan menebak untuknya
  menyembunyikan kesalahan itu.
- **Jalan keluar tetap ada.** Cache dikunci `(hash isi, pengaturan)`, jadi kalau
  suatu saat satu tekstur benar-benar perlu dua varian, mekanismenya sudah
  mendukung tanpa perubahan skema.

`TextureSettings` disimpan sebagai `.simtexcfg` di sebelah berkasnya, meniru
`MeshSettings` yang memakai `.simmeshcfg` — beserta alasannya yang sudah tertulis
di `MeshSettings.h:35`: `.meta` memuat identitas aset dan tidak boleh ditulis
ulang setiap kali ada pengaturan berubah.

Isi minimalnya:

| Medan | Nilai | Menentukan |
| --- | --- | --- |
| `usage` | `Color`, `NormalMap`, `Mask`, `Hdr`, `Height` | Format dan colorspace |
| `compress` | bool | Boleh dimatikan per aset untuk kasus yang butuh presisi |
| `quality` | cepat / seimbang / terbaik | Waktu encode versus PSNR |
| `generateMips` | bool | Bawaan menyala |
| `alpha` | tidak ada / punch-through / penuh | Memilih BC1 versus BC7 |

Pemetaan `usage` ke format adalah tabel, bukan tebakan:

| Usage | Format | Colorspace |
| --- | --- | --- |
| `Color` tanpa alpha | `BC7_UNORM` (atau `BC1` mode hemat) | sRGB |
| `Color` dengan alpha | `BC7_UNORM` | sRGB |
| `NormalMap` | `BC5_UNORM` | linear |
| `Mask` 1 kanal | `BC4_UNORM` | linear |
| `Hdr` | `BC6H_UFLOAT` | linear |
| `Height` | `BC4_UNORM` atau tanpa kompresi | linear |

---

## Dependensi

| Paket | Untuk apa | Catatan |
| --- | --- | --- |
| **KTX-Software (`libktx`)** | Menulis dan membaca kontainer KTX2 | Apache 2.0. KTX2 bisa membawa **blok BCn apa adanya**, bukan hanya Basis; supercompression Zstd tersedia untuk format non-ETC1S |
| **Encoder BCn** | Menghasilkan bloknya | `bc7enc`/`rgbcx` permisif dan punya RDO. ISPC Texture Compressor lebih cepat tapi menuntut compiler ISPC — keberatan yang sama yang menolak OSPRay |

Berbeda dengan backend gambar di [PLAN-IMAGEIO.md](PLAN-IMAGEIO.md) yang semuanya
opsional, **libktx wajib**: tanpa pembaca KTX2, renderer tidak bisa memuat
tekstur sama sekali begitu T3 mendarat.

---

## Milestone

### T0 — Fondasi Vulkan · ✅

`textureCompressionBC` diminta di `Device.cpp` dan ketiadaannya ditangani, bukan
diabaikan. Pembaca KTX2 di RHI yang mengunggah tiap level dengan `memcpy` ke
`VkImage`.

Bisa dikerjakan sekarang tanpa encoder apa pun: berkas uji dibuat manual dengan
CLI `ktx create`, lalu dimasukkan ke `Resources/` sebagai fixture.

**Kriteria terima**
- GPU tanpa dukungan BC tetap menjalankan editor, dan faktanya tercatat di log
  startup. ✅ `Device::SupportsBlockCompression()` menjawabnya, supaya yang
  memuat `.ktx2` ber-BC7 bisa memilih jalur lain alih-alih mengunggah dan
  berharap.
- Uji doctest membaca header KTX2 dan memeriksa `VkFormat`, jumlah level, serta
  ukuran tiap level. ✅
- `.ktx2` BC7 dengan seluruh rantai mip dimuat dan digambar benar. 🔶 pembacanya
  ada dan diuji; pengunggahannya ke `VkImage` menyusul bersama T3, karena di
  situlah ia punya pemakai.
- Nol galat validation layer. ⏸ menunggu unggahan itu.

**Pembacanya ditulis sendiri, dan libktx ditunda ke T2.** Yang dibutuhkan T0
hanya membaca: header, indeks level, lalu `memcpy` tiap level. Itu struktur
berkas yang seluruhnya dijelaskan spesifikasinya, dan menariknya lewat pustaka
penuh berarti dependensi wajib yang belum membayar apa pun — alasan yang sama
yang membuat enkoder PNG 16-bit ditulis sendiri di E7.3.

libktx tetap masuk di T2, ketika baker harus **menulis** KTX2 dan memakai
supercompression Zstd. Menulis kontainer, memilih skema supercompression, dan
menyusun DFD adalah pekerjaan yang tidak layak ditulis tangan.

**Yang tidak didukung ditolak beserta sebabnya**, bukan dibaca separuh: Basis
Universal (menuntut transcode), supercompression, larik, kubus, dan volume.
Kelimanya sah menurut spesifikasi dan tidak satu pun dipakai jalur tekstur mesin
ini hari ini — dan berkas yang diterima separuh menghasilkan gambar yang salah
tanpa satu pun pesan.

**Ujinya menyusun berkas dari tata letak spesifikasinya**, bukan lewat penulis
buatan sendiri: round-trip terhadap penulis sendiri hanya membuktikan konsistensi
dengan diri sendiri, dan ia lulus walaupun kedua sisinya salah membaca
spesifikasi dengan cara yang sama. Berkas `.ktx2` sungguhan dari CLI `ktx` tetap
layak ditambahkan sebagai fixture begitu alatnya ada.

Satu mutasi sempat lolos dan uji yang menangkapnya menyusul: **kasus persegi
tidak bisa membuktikan penjepitan ukuran mip ke satu.** Pada 16×16, kelima
levelnya berhenti tepat di 1×1 dengan sendirinya; yang membuktikannya tekstur
16×4, yang sisi pendeknya habis lebih dulu — dan ukuran nol membuat
`vkCmdCopyBufferToImage` menolak seluruh unggahan.

### T1 — `TextureSettings` dan panel Inspector · ⬜

Sidecar `.simtexcfg` mengikuti pola `MeshSettings`. Panel di Inspector dengan
command/undo. Tebakan awal `usage` saat impor dari nama berkas (`_n`, `_normal`,
`_rough`) — **sebagai tebakan yang bisa diubah, bukan sebagai kebenaran.**

**Kriteria terima**
- Mengubah `usage` menandai aset perlu di-bake ulang.
- Round-trip tulis-baca `.simtexcfg` identik; diuji doctest.
- Aset tanpa `.simtexcfg` memakai bawaan `Color`, dan tidak ada berkas yang
  ditulis sampai ada yang benar-benar mengubahnya.

### T2 — Baker: mip, kompresi, cache · ⬜ (butuh I0)

Membaca sumber lewat `Sim::ImageIO`, membangkitkan mip **di ruang linear**,
mengompresi sesuai tabel usage, menulis `.ktx2` ke cache.

Cache dikunci hash isi berkas ditambah hash pengaturan, mengikuti pola
`~/.simengine/ThumbnailCache/` yang sudah memakai FNV-1a 64-bit di
`Thumbnail.cpp:19`. Bake berjalan di `TaskPool`, hasilnya kembali lewat
`MainThreadQueue`.

**Kriteria terima**
- Bake ulang tidak terjadi kalau berkas dan pengaturannya tidak berubah —
  dibuktikan dengan menghitung panggilan encoder, bukan dengan mengamati waktu.
- Mip level 1 dari gradien sRGB **cocok dengan rata-rata yang dihitung di ruang
  linear**, bukan rata-rata nilai ter-encode. Ini uji yang menangkap kesalahan
  paling mahal di seluruh plan.
- Normal map dinormalkan ulang setelah tiap level; uji memeriksa panjang vektor
  di level terdalam.
- Tekstur dengan dimensi bukan kelipatan 4 tetap benar; kalau dipadatkan,
  padatannya tidak bocor ke tepi saat disampel.
- Waktu bake tekstur 4K tercatat; kalau melebihi beberapa detik pada kualitas
  `seimbang`, setelan bawaannya diturunkan.

### T3 — Renderer memakai KTX2 · ⬜ (mendarat bersama jalur tekstur material)

Jalur tekstur material memuat `.ktx2` dari cache, bukan mendekode PNG. Larik
bindless berisi format campuran. Tekstur yang belum di-bake memakai placeholder
yang jelas terlihat, bukan hitam.

**Ini milestone yang penjadwalannya tidak bebas.** Ia harus mendarat bersama
jalur tekstur material di E8.4, bukan sesudahnya.

**Kriteria terima**
- Material dengan albedo BC7, normal BC5, dan ORM BC4 digambar benar.
- Pemakaian VRAM tekstur pada level contoh turun terukur, dan angkanya dicatat.
- Renderer tidak pernah memanggil dekoder gambar apa pun — diuji dengan grep,
  seperti aturan `stbi_` di I0.

### T4 — HDR dan IBL ke BC6H · ⬜ (butuh I2)

`Ibl.cpp` memakai environment map BC6H. Menyambung ke I2 yang memberi
kemampuan membaca `.exr` sebagai sumbernya.

**Kriteria terima**
- IBL dari BC6H cocok dengan IBL dari sumber float dalam toleransi yang ditulis
  di test — sekali lagi, dibandingkan angkanya, bukan dilihat gambarnya.
- Environment 4096×2048 turun dari 67 MB menjadi sekitar 8 MB, terukur.
- Nilai yang sangat terang tidak terpotong; BC6H tidak bertanda, jadi masukan
  bernilai negatif ditolak atau dijepit dengan catatan di log.

### T5 — Validasi dan anggaran · ⬜

- PSNR dihitung saat bake dan disimpan di metadata KTX2; yang jatuh di bawah
  ambang dilaporkan di panel, bukan diam.
- Panel yang menampilkan anggaran VRAM tekstur per level.
- Satu uji yang mengunci kesalahan klasik: normal map tidak pernah ber-`VkFormat`
  sRGB, dan albedo tidak pernah linear.

**Kriteria terima**
- Mengubah `usage` sebuah albedo menjadi `NormalMap` membuat uji itu gagal.
- Laporan anggaran cocok dengan pemakaian sebenarnya yang dilaporkan VMA dalam
  selisih yang wajar.

---

## Lima jebakan yang tidak muncul sebagai galat

Semuanya menghasilkan gambar yang salah tanpa satu pun peringatan, dan itulah
kenapa masing-masing punya kriteria terima sendiri di atas.

1. **Mip dibangkitkan di ruang sRGB.** Merata-ratakan nilai ter-encode salah
   secara matematis; hasilnya mip yang terlalu gelap dan tekstur yang berubah
   kecerahan seiring jarak.
2. **sRGB dikira konversi.** `VK_FORMAT_BC7_SRGB_BLOCK` dan
   `VK_FORMAT_BC7_UNORM_BLOCK` berisi bit yang identik; yang berbeda hanya
   tafsir sampler. Keputusan colorspace dari I4 harus sampai ke pilihan
   `VkFormat`, bukan berhenti di lapisan I/O.
3. **Normal map dikompresi sebagai warna.** BC7 dengan metrik perseptual merusak
   normal secara halus. BC5, dan tanpa metrik warna.
4. **Kompresi dianggap murah.** Tanpa cache berkunci hash, setiap impor ulang
   membayar penuh.
5. **Blok 4×4 dilupakan.** Dimensi bukan kelipatan empat menyisakan blok tepi
   yang isinya menentukan apa yang terlihat saat disampel di tepi.

---

## Risiko

| Risiko | Tanda awal | Mundur ke |
| --- | --- | --- |
| T3 tertinggal dari jalur tekstur material | Jalur material mendarat memuat PNG | Hentikan dan sambungkan; jalur kedua yang terlanjur ada tidak akan pernah dihapus |
| Waktu bake merusak alur impor | T2 mencatat lebih dari beberapa detik per 4K | Turunkan kualitas bawaan; bake kualitas terbaik hanya saat build rilis |
| Kualitas BC7 tidak cukup untuk aset tertentu | Artis mengeluh artefak blok | `compress = false` per aset sudah ada di `TextureSettings` sejak T1 |
| Cache membengkak | Folder cache tumbuh tanpa batas | Batas ukuran dengan pembuangan terlama, seperti ThumbnailCache |
| Encoder ISPC menyeret compiler baru | Muncul saat memilih encoder di T2 | Mulai dengan `bc7enc`/`rgbcx` yang tidak menuntut apa pun |

---

## Yang tidak boleh ditunda

- **T0 sebelum jalur tekstur material ditulis.** Sama persis dengan alasan I0
  mendesak, dan keduanya menunjuk tanggal yang sama.
- **Usage di aset, diputuskan sekarang.** Memindahkannya ke slot material
  belakangan berarti mengubah skema `.simmat` yang sudah tersimpan di proyek
  orang — persis yang dihindari catatan di `MaterialParameterBlock.h` soal
  timpaan tekstur di `.simmatinst`.
- **Cache berkunci hash isi sejak awal.** Cache berkunci path akan salah setiap
  kali berkas berpindah, dan salahnya berupa tekstur basi yang terlihat benar.

---

## Yang sengaja tidak dikerjakan

- **Basis Universal dan ASTC.** Masuk kalau target mobile muncul; kontainernya
  sudah siap menampungnya.
- **Streaming tekstur / mip virtual.** Masalah scene besar, bukan masalah
  sekarang.
- **Kompresi runtime.** Tidak pernah.
- **Mengganti thumbnail dengan KTX2.** Thumbnail hidup singkat, sudah punya
  cache sendiri, dan RGBA8 sudah benar untuknya.
