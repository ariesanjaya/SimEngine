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
| **KTX-Software (`libktx`)** | **Menulis** kontainer KTX2 (T2) | `v4.4.2`, Apache 2.0. Membacanya tidak lewat sini — `Sim::RHI` punya pembaca tulisan tangan sejak T0. Dua jebakan build-nya dicatat di docs/DEPENDENCIES.md |
| **Encoder BCn** | Menghasilkan bloknya | **`bc7enc_rdo` dipakai** (MIT): `rgbcx` untuk BC1/BC3/BC4/BC5, `bc7enc` untuk BC7, `bc7decomp` untuk sisi urainya. **Tidak ada BC6H di dalamnya** — itu yang menahan T4. ISPC Texture Compressor lebih cepat tapi menuntut compiler ISPC — keberatan yang sama yang menolak OSPRay |

Berbeda dengan backend gambar di [PLAN-IMAGEIO.md](PLAN-IMAGEIO.md) yang semuanya
opsional, **keduanya wajib** — tetapi hanya di sisi editor. Runtime yang dikirim
bersama game tidak menautkan satu pun: yang dibutuhkannya cuma pembaca KTX2
tulisan tangan di `Sim::RHI`, dan berkas yang dibacanya sudah selesai di-bake.

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

### T1 — `TextureSettings` dan panelnya · ✅

Sidecar `.simtexcfg` mengikuti pola `MeshSettings`. Panel di Inspector dengan
command/undo. Tebakan awal `usage` saat impor dari nama berkas (`_n`, `_normal`,
`_rough`) — **sebagai tebakan yang bisa diubah, bukan sebagai kebenaran.**

**Kriteria terima**
- Round-trip tulis-baca `.simtexcfg` identik. ✅ Termasuk byte-per-byte pada
  penyimpanan kedua: berkas yang urutannya berubah tiap simpan menghasilkan diff
  yang tidak membawa informasi.
- Aset tanpa `.simtexcfg` memakai bawaan, dan tidak ada berkas yang ditulis
  sampai ada yang benar-benar mengubahnya. ✅
- Mengubah `usage` menandai aset perlu di-bake ulang. ⏸ **tidak ada yang bisa
  ditandai sampai T2 ada.** Dan ketika ia ada, penandaannya tidak perlu ditulis
  sama sekali: cache dikunci `(hash isi, pengaturan)`, jadi pengaturan yang
  berubah **adalah** kunci yang berbeda. Bendera "perlu di-bake" akan menjadi
  keadaan kedua yang bisa berselisih dengan yang pertama.

**Bawaannya bukan `TextureSettings{}` melainkan bawaan berkas itu** — struct
bawaan ditambah tebakan `usage` dari namanya. Bedanya menentukan, dan versi
pertama saya salah di situ: tekstur bernama `batu_n.png` yang sengaja disetel
pengguna ke `Color` akan tersimpan sebagai "sama dengan bawaan", berkasnya
dihapus, dan tebakan namanya kembali memaksanya menjadi `NormalMap` pada
pemuatan berikutnya. Pengguna tidak punya cara menolak tebakan itu — **dan yang
tidak bisa ditolak bukan tebakan lagi.**

Akhiran dicocokkan **sebagai kata**, bukan sebagai potongan huruf: `_n` yang
dicocokkan apa adanya ikut mengenai `kayu_batan`, dan yang tertandai normal map
adalah tekstur warna yang lalu tampak biru pekat. Kedua kesalahan itu ditangkap
mutasi.

Nilainya ditulis sebagai **nama**, bukan angka. Berkas ini ikut kontrol versi dan
dibaca manusia saat menelusuri perbedaan; `"usage": 1` menuntut membuka header
untuk tahu artinya, dan nomor yang bergeser saat sebuah nilai disisipkan mengubah
arti setiap berkas yang sudah ada tanpa satu pun tanda.

**Panelnya di Asset Browser, bukan di Inspector.** Inspector menampilkan komponen
sebuah entity, dan tekstur bukan entity — yang memilih aset adalah Asset Browser,
dan panel itu sudah memuat rincian aset di sebelahnya (jalur, ukuran, GUID,
jumlah segitiga). Pengaturannya dimuat sekali per pilihan, bukan tiap frame:
membaca berkas enam puluh kali per detik untuk menggambar lima kotak pilihan akan
menimpa suntingan yang sedang berlangsung dengan isi disk.

Suntingannya lewat `CommandHistory`, **tidak seperti dokumen terrain**. Terrain
tidak bisa karena `CommandHistory` tidak punya cakupan dokumen; pengaturan
tekstur tidak punya masalah itu karena perintahnya memegang jalur berkasnya, jadi
ia berlaku pada aset yang sama betapapun jauh pengguna sudah berpindah pilihan.
Perubahan berturut-turut pada tekstur yang sama menyatu; tekstur berbeda tidak.

### T2 — Baker: mip, kompresi, cache · ✅

`Sim::ImageIO/MipChain` membangun rantainya, `Sim::Assets/BlockCompress`
mengompresinya, `Sim::Assets/Ktx2Write` menulis kontainernya, dan
`Sim::Assets/TextureBake` merangkai ketiganya di belakang cache.

**Kriteria terima**
- Bake ulang tidak terjadi kalau berkas dan pengaturannya tidak berubah —
  dibuktikan dengan **menghitung**, lewat `TextureBakeCount()`. ✅ Isi berkas
  yang berubah tanpa berganti nama juga terhitung sebagai sumber yang lain.
- Mip level 1 dari gradien sRGB cocok dengan rata-rata yang dihitung **di ruang
  linear**. ✅ Hitam dan putih bersebelahan menghasilkan 182, bukan 128; jarak
  keduanya lima puluh empat tingkat, dan uji-nya menolak 128 secara terpisah
  supaya tidak bisa lulus karena toleransi.
- Normal map dinormalkan ulang setelah tiap level. ✅ Diperiksa di level
  terdalam: panjangnya 1,0 dan bukan 0,71.
- Dimensi bukan kelipatan 4 tetap benar. ✅ Dan dibuktikan **byte per byte**,
  bukan dengan toleransi: hasil kompresi gambar 5×3 harus identik dengan hasil
  kompresi versi 8×4-nya yang tepinya sudah dijepit tangan di sisi uji.
- Waktu bake tekstur 4K tercatat, dan **setelan bawaannya memang diturunkan**.
  ✅ Angkanya di bawah.

#### Yang diukur, dan apa yang diubah karenanya

Tekstur 4096×4096, satu berkas, Release, 4096² piksel penuh sampai 1×1.

| Kualitas `seimbang` | Waktu 4K | PSNR (512², BC7) |
| --- | --- | --- |
| uber 1, partisi 16 *(bawaan pertama)* | 8,7 s | 46,5 dB |
| **uber 0, partisi 8** *(bawaan sekarang)* | **5,3 s** | **46,2 dB** |
| uber 0, partisi 8, **satu thread** | 74 s | 46,2 dB |
| uber 0, partisi 0 | 3,4 s | 41,4 dB |
| `terbaik` (uber 4, partisi 64) | 24,4 s | 46,9 dB |

Dua keputusan keluar dari tabel itu.

**Pertama, kompresinya dijadikan paralel** — dan itu memberi 8,5×, jauh melebihi
apa pun yang bisa didapat dengan menurunkan kualitas. Tiap blok 4×4 berdiri
sendiri sepenuhnya; tidak ada satu pun keputusan encoder yang menyeberang
antar-blok. Barisnya dibagi berselang-seling, bukan menjadi potongan berurutan:
thread yang kebagian daerah rata selesai jauh lebih awal daripada yang kebagian
daerah berdetail, dan yang menentukan lamanya adalah yang terakhir selesai.

**Kedua, `seimbang` diturunkan ke uber 0 / partisi 8.** Harganya 0,3 dB dan
imbalannya 3,4 detik. Yang **tidak** dilakukan adalah menolkan partisinya
sekalian, meski itu tiga detik lebih cepat lagi: harganya lima desibel, dan lima
desibel terlihat sebagai blok pada setiap tepi tajam. Batas itu sekarang dijaga
uji PSNR, bukan diingat.

Sisa 2,2 detik dari 5,3 detik itu bukan kompresi melainkan dekode PNG 38 MB dan
pembangkitan mip, yang keduanya masih satu thread. Itu batas berikutnya kalau
suatu saat perlu diturunkan lagi.

**Waktunya tidak diuji, PSNR-nya diuji.** Assertion atas jam dinding gagal ketika
mesinnya sedang sibuk dan lulus ketika sedang senggang — `SimParticleTests` sudah
memperlihatkan bentuknya. PSNR tidak bergantung pada beban, jadi ia bisa menjaga
keputusan di atas tanpa pernah gagal palsu.

#### Kontainernya ditulis libktx, pembacanya tetap tulisan tangan

Yang dibayar dari libktx adalah **Data Format Descriptor** — blok wajib di setiap
KTX2 yang menerangkan tata letak kanal dan fungsi transfernya, dan yang **tidak
dibaca pembaca kita sendiri**. Menyusunnya salah karena itu menghasilkan berkas
yang dibuka sempurna oleh mesin ini dan ditolak setiap alat lain, tanpa satu pun
tanda. Itu bentuk kesalahan yang tidak boleh dipilih sendiri.

Efek sampingnya yang paling berharga: karena penulisnya libktx sementara
pembacanya bukan, uji round-trip membandingkan **dua implementasi yang berbeda**
— bukan membuktikan satu implementasi konsisten dengan dirinya sendiri, keberatan
yang sudah tertulis di `Sim/RHI/Ktx2.h` sejak T0. Jalur mentah dan jalur blok
diuji terpisah, karena keduanya menempuh cabang yang berbeda di dalam libktx.

Supercompression Zstd sengaja tidak dipakai: pembaca di `Sim::RHI` menolaknya,
dan baker yang menghasilkan berkas yang tidak bisa dibaca runtime-nya sendiri
bukan penghematan.

#### Tabel format, dan tiga barisnya yang menyimpang dari rencana awal

| Usage | Format | Catatan |
| --- | --- | --- |
| `Color` | `BC7_SRGB` / `BC7_UNORM` | |
| `Color` tanpa alfa **dan** kualitas `cepat` | `BC1_RGB_SRGB` / `_UNORM` | inilah "mode hemat" yang disebut rencananya — setengah ukuran, gradiennya berpita |
| `NormalMap` | `BC5_UNORM` | |
| `Mask` 1 kanal | `BC4_UNORM` | |
| `Height` | `R16_UNORM` / `R8_UNORM`, **tanpa kompresi** | |
| `Hdr` | `R32G32B32A32_SFLOAT`, **tanpa kompresi** | |

- **BC1 menuntut dua hal sekaligus**, bukan hanya `alpha = tidak ada`. Tabel
  rencana menyebut BC7 untuk warna tanpa alfa dan BC1 sebagai "mode hemat", dan
  satu-satunya cara pengguna meminta mode itu adalah lewat kualitas `cepat`.
  Yang tidak memintanya mendapat BC7.
- **`Height` sengaja tidak dikompresi.** Rencananya menawarkan "BC4 atau tanpa
  kompresi"; BC4 menyimpan endpoint delapan bit, dan terasering yang
  dihasilkannya pada terrain terbaca sebagai kesalahan terrain — dicari
  berhari-hari di tempat yang salah.
- **`Hdr` menunggu T4**, dan itu bukan pilihan: `bc7enc_rdo` tidak memuat encoder
  BC6H sama sekali.

Yang tidak ada di tabel jatuh ke format tanpa kompresi, bukan ke format blok yang
kira-kira cocok: tekstur yang lebih besar dari seharusnya adalah masalah
anggaran, sedangkan tekstur yang bloknya salah adalah gambar yang salah.

**Perlindungan normal map ternyata struktural, bukan berupa bendera.** Jebakan
nomor tiga menuntut normal map dikompresi tanpa metrik warna; karena ia memakai
BC5, dan BC5 tidak punya metrik warna sama sekali, bendera `perceptual` tidak
pernah dibaca di jalur itu. Bendera itu tetap ada — dan diuji sampai ke encoder —
untuk siapa pun yang memampatkan data ke BC7, tetapi yang menjaga normal map
adalah pilihan formatnya.

#### Yang belum dikerjakan di sini, dan alasannya

- **Bake belum berjalan di `TaskPool`.** Belum ada yang memanggilnya: jalur
  tekstur renderer masih mendekode PNG, dan itu justru pekerjaan T3. Menyambung
  baker ke `TaskPool` dan `MainThreadQueue` sekarang berarti menulis pemanggil
  buatan hanya supaya ada yang memanggil.
- Ketika sambungan itu dibuat, `CompressOptions::threads` harus disetel 1:
  N tekstur yang masing-masing membuka N thread menghasilkan N² thread yang
  berebut inti yang sama.
- **Cache belum punya batas ukuran.** Ia tumbuh tanpa dibuang, sama seperti
  `ThumbnailCache` sebelum batasnya ada. Yang menyelamatkan untuk sekarang:
  kuncinya berisi versi baker, jadi berkas dari baker lama tidak pernah terpakai
  — tetapi juga tidak pernah terhapus.

### T3 — Renderer memakai KTX2 · 🔶

Jalur tekstur material memuat `.ktx2` dari cache. Berkas sumber tidak pernah
sampai ke renderer sama sekali.

**Kriteria terima**
- Renderer tidak pernah memanggil dekoder gambar apa pun. ✅ Diuji dengan
  menyisir `VulkanRenderer.cpp`, dan uji-nya membuang komentar lebih dulu —
  aturan ini soal kode, dan tanpa langkah itu catatan yang menerangkan aturannya
  sendiri menggagalkannya. `Ibl.cpp` sengaja **tidak** ikut: peta lingkungan
  masih float dari `.exr`, dan yang memindahkannya adalah T4.
- Pemakaian VRAM turun terukur, dan angkanya dicatat. ✅ Tekstur 256×256 dengan
  rantai mip penuh: **349.524 byte RGBA8 → 87.408 byte BC7**, yaitu 25,01%.
  Yang dijumlahkan muatan tiap level, bukan dimensi dikali tebakan
  bytes-per-texel.
- Tekstur yang belum di-bake memakai placeholder yang jelas terlihat. ✅ Magenta,
  dan **berbeda arti dari putih**: putih berarti "ruas ini memang tidak
  bertekstur" — nilai satuan perkalian — sementara magenta berarti "punya
  tekstur, sedang dikerjakan". Yang menunggu tanpa tahu ia menunggu akan mengira
  teksturnya hilang.
- Material dengan albedo BC7, normal BC5, dan ORM BC4 digambar benar. 🔶
  **Ketiganya di-bake, diunggah, dan tersampel dengan benar — tetapi hanya
  albedo yang sungguh dipakai menggambar.** Pass forward masih `box.frag`, yang
  tidak punya model pencahayaan untuk memakai normal maupun ORM. Yang
  menggantinya adalah pipeline material di E8.4, dan di situlah sisa kriteria ini
  selesai.

#### T0 ditutup di sini: unggahan rantai mip

`Texture2D::CreateFromKtx2` mengunggah seluruh level dalam satu staging buffer
dan satu `vkCmdCopyBufferToImage`. Tiga hal yang salahnya tidak muncul sebagai
kompilasi gagal:

- **Barrier harus menyebut seluruh level.** Yang hanya menyebut level 0
  meninggalkan sisanya di layout `UNDEFINED`.
- **`imageExtent` diambil dari dimensi level itu**, bukan dari level nol.
  Keduanya sama untuk ukuran kelipatan dua dan berbeda untuk yang lain.
- **`bufferRowLength` dibiarkan nol.** Nol berarti "rapat menurut imageExtent",
  yang benar untuk format blok maupun biasa; menghitungnya sendiri berarti
  menyalin lagi aturan pembulatan blok 4×4.

Ketiganya lolos setiap uji CPU. Yang menangkapnya adalah
`SimTextureUploadTests` — **satu-satunya uji di repo ini yang menuntut Vulkan**,
yang membaca galat validation layer dari `LogRing` dan gagal bila ada satu pun.
Dua mutasi dicoba: barrier ber-level tunggal menghasilkan tiga galat layout, dan
`imageExtent` dari level nol menghasilkan `VUID-...-imageSubresource-07971` lalu
`VK_ERROR_DEVICE_LOST`. Mesin tanpa Vulkan melewatinya alih-alih gagal.

Dan satu galat yang sudah ada sejak sebelumnya ikut ketahuan: **device headless
meminta `VK_KHR_swapchain` tanpa `VK_KHR_surface`**. Ia tidak pernah terlihat
karena tidak ada yang pernah membuat device tanpa jendela. Sekarang
`VK_KHR_swapchain` hanya diminta ketika ada ekstensi instance.

**Sampler tekstur material `REPEAT`, bukan `CLAMP_TO_EDGE`.** Yang lewat jalur
ini adalah dinding bata yang diulang sepanjang mesh; clamp membuat seluruh
permukaan di luar 0..1 memakai satu baris piksel tepinya, meregang menjadi garis.
`Create` tetap clamp karena yang lewat sana adalah thumbnail.

#### Baker dipanggil dari mana

`assets::TextureBakery` memetakan berkas sumber ke `.ktx2` di cache, dan
menjalankan bake di `TaskPool` — janji yang T2 tunda karena belum ada
pemanggilnya. Ia dimiliki `EditorApp` dan dipakai `SceneView`; **renderer tidak
pernah melihatnya**.

- Yang diminta frame ini menjawab `Pending` dan tidak memblokir apa pun.
- Permintaan kedua pada frame yang sama tidak mengantre tugas kedua. Tanpa itu,
  satu tekstur yang dipakai lima ruas mesh menjalankan lima encoder BC7 sekaligus
  untuk menghasilkan berkas yang identik.
- Kegagalan **diingat**. Diuji dengan mengganti berkas rusaknya dengan yang sah
  dan memastikan jawabannya tetap gagal — kalau tidak, berkas rusak diurai enam
  puluh kali per detik. Itu sekaligus alasan `Invalidate` ada.

#### Yang belum

- **Larik bindless berisi format campuran** belum ada. Hari ini tiap tekstur
  punya satu set descriptor sendiri, seperti sebelum T3. Itu bagian dari
  keputusan set-0 di E8.4, bukan bagian dari jalur KTX2.
- Kriteria terakhir — normal BC5 dan ORM BC4 **digambar** — menunggu pipeline
  material di pass forward. Langkah pertamanya sudah mendarat: transform instance
  kini di storage buffer, seperti yang dibaca modul material. Urutan sisanya ada
  di [PLAN-RENDER.md § E8.4](PLAN-RENDER.md).
- Cache tekstur masih tumbuh tanpa batas — sama seperti sesudah T2.

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
