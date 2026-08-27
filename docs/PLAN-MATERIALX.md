# Impor material dari 3ds Max lewat MaterialX

Membuat material yang dibuat artis di 3ds Max masuk ke mesin ini **apa adanya**,
bukan diproyeksikan ke lima angka — dengan dokumen MaterialX sebagai bentuk yang
menang bila ada, dan blok properti kustom di dalam FBX sebagai cadangannya.

**Sudah mendarat.** Diperiksa terhadap ekspor 3ds Max sungguhan
(`NewSponza_Main_Yup_003.fbx`, 28 material Physical Material) dan terhadap
dokumen `.mtlx` yang memakai tekstur, nodegraph, dan lapisan yang mesin ini belum
punya.

---

## Apa yang hilang sebelumnya

Impor FBX membaca material lewat `ReadMaterial()` di `MeshImport.cpp`, dan yang
dibacanya properti Lambert/Phong: `sDiffuse`, `sShininess` diturunkan menjadi
roughness, `sTransparencyFactor`, dan `metalness` **dipatok nol**. Hasilnya
diratakan menjadi `MeshMaterial` — lima skalar — lalu ditulis sebagai
`.simmatinst` di atas satu induk yang hanya mengekspos enam parameter.

Sementara itu `output.surface` mesin ini punya delapan belas pin OpenPBR yang
namanya **sudah** sejalan dengan `open_pbr_surface.mtlx`. Itu keputusan E7.1, dan
[ANALISA-OPENPBR.md](ANALISA-OPENPBR.md) menyebut interop 3ds Max sebagai salah
satu alasan yang mendorongnya:

> Kalau mesin ini memakai parameterisasi yang sama, **pemetaan ekspor menjadi
> satu-ke-satu**. […] Nilai memilih OpenPBR adalah **tidak kehilangan apa pun
> saat impor**.

Alasan itu belum pernah ditagih. Sebelum perubahan ini, sebuah material OpenPBR
dari Max kehilangan coat, fuzz, anisotropi, `base_weight`, `specular_ior`,
Oren–Nayar, dan **empat dari lima teksturnya** — peta normal, kekasaran,
kelogaman, dan emisi seluruhnya terbaca lalu dibuang, karena induk impornya
tidak punya slot untuk mereka.

---

## Transport: tiga tingkat, dan yang lebih lengkap menang

FBX tidak punya slot material PBR. Jadi pertanyaannya bukan "bagaimana membacanya
dari FBX" melainkan "di mana datanya sebenarnya berada", dan jawabannya ada tiga,
dicoba berurutan:

| # | Sumber | Yang bisa dinyatakannya | Menang atas |
|---|---|---|---|
| 1 | Dokumen `.mtlx` di sebelah berkas mesh | seluruh input OpenPBR, **beserta node yang mengemudikannya** | semuanya |
| 2 | Blok `3dsMax\|Parameters` di dalam FBX | daftar angka + peta per parameter | Lambert/Phong |
| 3 | Properti Lambert/Phong FBX | lima angka | — |

Urutan ini bukan preferensi melainkan akibat: yang di atas bisa menyatakan hal
yang tidak bisa dinyatakan yang di bawahnya. Blok Max adalah daftar angka; ia
tidak punya cara mengatakan "input ini dikemudikan gambar ini lewat node itu".

**Dokumen dicari begini**, dan berhenti di yang pertama terbaca:

1. Jalur `.mtlx` yang **disebut berkas mesh itu sendiri** — dikumpulkan dari
   setiap properti bertipe string di tiap materialnya. Bukan dari satu nama
   properti yang dipatok: nama parameter itu berbeda antara MaterialX Map dan
   material OpenPBR Max, sedangkan ekstensinya tidak.
2. `<nama berkas mesh>.mtlx` di folder yang sama.
3. Satu-satunya `.mtlx` di folder itu — **dan hanya bila memang cuma ada satu.**
   Dua berkas berarti tidak ada jawaban yang bisa ditebak, dan menebaknya
   menghasilkan material yang salah tanpa satu pun tanda.

Pemasangan material ke material lewat nama, tanpa peduli besar-kecil huruf.
Perkecualian tunggal: **satu material di mesh lawan satu di dokumen** dipasangkan
walau namanya berbeda — tidak ada pasangan lain yang mungkin, dan nama material
memang kerap berganti melewati eksportir. Dua lawan dua tidak: di sana menebak
berarti separuh kemungkinan memasang material yang salah.

---

## Yang diperiksa terhadap berkas sungguhan

Tebakan atas nama parameter Max diperiksa, bukan didiamkan. Alatnya dibuat
bersamaan dan ikut dikirim:

```
SimHeadless --project P --no-render --dump-fbx-material <berkas.fbx>
```

Ia mencetak setiap properti tiap material apa adanya — nama hierarkis, tipe,
nilai, dan tekstur yang tersambung. Dijalankan atas `NewSponza_Main_Yup_003.fbx`,
ia memperlihatkan:

```
=== arch_stone_wall_01 [unknown] ===
  3dsMax|ORIGINAL_MTL : KString = "PHYSICAL_MTL"
  3dsMax|Parameters|base_weight : Float = 1.000000
  3dsMax|Parameters|base_color : ColorAndAlpha = (0.500000, 0.500000, 0.500000)
  3dsMax|Parameters|roughness : Float = 0.000000
  3dsMax|Parameters|roughness_inv : Bool = 0.000000
  3dsMax|Parameters|metalness : Float = 0.000000
  3dsMax|Parameters|trans_ior : Float = 1.100000
  3dsMax|Parameters|emission : Float = 1.000000
  3dsMax|Parameters|emit_color : ColorAndAlpha = (0.000000, 0.000000, 0.000000)
  3dsMax|Parameters|emit_luminance : Float = 1500.000000
  3dsMax|Parameters|base_color_map : Reference  <- textures\arch_stone_wall_01_BaseColor.png
  3dsMax|Parameters|roughness_map : Reference   <- textures\arch_stone_wall_01_Roughness.png
  3dsMax|Parameters|metalness_map : Reference   <- textures\arch_stone_wall_01_Metalness.png
  3dsMax|Parameters|bump_map : Reference        <- textures\arch_stone_wall_01_Normal.png
```

Tiga hal berubah karenanya:

- **`ORIGINAL_MTL` menjadi penentu keluarga nama.** Max menyebutkan sendiri
  material apa yang diekspornya. Itu jawaban yang jauh lebih baik daripada
  menebak dari nama parameter — nama boleh bertambah di rilis berikutnya,
  pernyataan eksplisit tidak berubah artinya. Tebakan tetap ada sebagai cadangan
  untuk versi yang tidak menulis baris itu.
- **`sheen` adalah fuzz.** Physical Material menyebutnya sheen, OpenPBR
  menyebutnya fuzz; satu lobe, dua nama. Ia terlewat di tebakan awal.
- **Emisi tidak boleh memakai bobotnya sendirian.** Ekspor nyata menulis
  `emission = 1` bahkan untuk dinding batu; yang membuatnya gelap adalah
  `emit_color` hitam. Memakai bobot itu sebagai emisi menyalakan seluruh adegan.

Dan satu bug ditemukan justru karena daftarnya dicetak: `GetNextProperty` sudah
menelusuri properti bersarang, jadi penelusuran keturunan di atasnya menghasilkan
setiap parameter **tiga kali**.

---

## Pemetaan

### Dokumen MaterialX → pin `output.surface`

Satu-ke-satu; yang berbeda hanya ejaannya. Tidak ada satu pun besaran yang
dikonversi, dan itu memang seluruh alasan mesin ini memilih OpenPBR.

| `open_pbr_surface.mtlx` | pin mesin | catatan |
|---|---|---|
| `base_weight`, `base_color`, `base_metalness`, `base_diffuse_roughness` | `baseWeight`, `baseColor`, `baseMetalness`, `baseDiffuseRoughness` | |
| `specular_weight`, `specular_color`, `specular_roughness`, `specular_roughness_anisotropy`, `specular_ior` | `specularWeight`, `specularColor`, `specularRoughness`, `specularRoughnessAnisotropy`, `specularIor` | kekasaran perseptual di kedua sisi — **tidak** dikuadratkan di importir |
| `coat_*` | `coat*` | termasuk `coat_darkening` |
| `fuzz_*` | `fuzz*` | |
| `emission_luminance` × `emission_color` | `emissive` | dikalikan sekali, di pembacanya |
| `geometry_opacity` | `opacity` | |
| `geometry_normal` ← `normalmap` ← `image` | `normalTexture` | ditembus lewat node `normalmap` |

### Physical Material 3ds Max → pin `output.surface`

Di sini pemetaannya terjemahan, bukan ejaan.

| Parameter Max | pin mesin | catatan |
|---|---|---|
| `base_weight`, `base_color` | `baseWeight`, `baseColor` | |
| `metalness` | `baseMetalness` | |
| `diff_roughness` | `baseDiffuseRoughness` | |
| `reflectivity` | `specularWeight` | |
| `refl_color` | `specularColor` | |
| `roughness` (+ `roughness_inv`) | `specularRoughness` | `_inv` menyala berarti angkanya **glossiness**; yang halus menjadi kasar tanpa memeriksanya |
| `trans_ior` | `specularIor` | Physical Material memakai satu IOR untuk seluruh permukaan; tidak ada parameter IOR lain di dalam bloknya |
| `coating`, `coat_color`, `coat_roughness` (+ `_inv`), `coat_ior` | `coat*` | |
| `sheen`, `sheen_color`, `sheen_roughness` | `fuzz*` | |
| `emission` × `emit_color` × `emit_luminance` | `emissive` | |
| `transparency` | `opacity` | **dibalik**: `1 − transparency` |
| `base_color_map`, `roughness_map`, `metalness_map`, `emit_color_map`, `cutout_map`, `bump_map` | slot tekstur | nama slot terverifikasi dari ekspor nyata |

**`anisotropy` sengaja tidak dipetakan.** Ekspor Sponza menulis `anisotropy = 0`
pada seluruh 28 materialnya sementara `anisoangle` tetap 0,25 di semuanya, dan di
sebelahnya ada `aniso_mode` serta `aniso_channel` yang ikut menentukan artinya.
Nol memang sejajar dengan nol OpenPBR, tetapi skala nilai yang bukan nol tidak
bisa disimpulkan dari itu. Dua arah kesalahannya tidak setara: tidak memetakannya
membuat material anisotropik masuk sebagai isotropik — satu efek yang hilang;
memetakannya dengan skala yang salah membuat material isotropik keluar
anisotropik — **setiap** pantulan di adegan memanjang. Yang kedua jauh lebih
mahal, jadi ia menunggu sebuah berkas yang benar-benar memakainya.

---

## Dua induk, dan kenapa bukan satu

`DetectLobes` membaca **graph induk**, bukan nilai instance: sebuah pin
`coatWeight` yang dikemudikan parameter selalu terbaca "mungkin bukan nol", jadi
setiap material yang memakai induk ber-coat ikut membawa kode coat di dalam
SPIR-V-nya. Itu mekanisme yang sengaja dibangun — rekomendasi 1 di
[ANALISA-OPENPBR.md](ANALISA-OPENPBR.md), "lapisan dimatikan saat kompilasi" —
dan satu induk lengkap untuk semua material impor akan membatalkannya untuk
setiap material glTF di dunia, yang tidak punya coat maupun fuzz sama sekali.

Jadi induknya dua:

| Induk | Untuk | Lapisan yang ikut dikompilasi |
|---|---|---|
| `Materials/Sistem/Material Impor.simmat` | glTF, USD, FBX Lambert/Phong | tidak ada — base + specular saja |
| `Materials/Sistem/Material Impor OpenPBR.simmat` | sumber yang **menyatakan dirinya** OpenPBR | coat, fuzz, anisotropi, Oren–Nayar |

Yang memilih adalah ada-tidaknya `MeshMaterial::openPbr`, bukan tebakan atas isi
materialnya. Yang memakai induk kedua membayar keempat lapisan itu — dan itu
memang lapisan yang materialnya sendiri sebutkan. Induk pertama tidak berubah
sedikit pun.

> **Yang bisa dikerjakan nanti, dan syaratnya.** Ladder bertingkat — induk
> OpenPBR tanpa coat/fuzz untuk material yang keempat bobotnya nol — akan
> menghapus biaya itu untuk material OpenPBR yang sederhana. Ia menunggu sebuah
> pengukuran: berapa ongkos nyata coat + fuzz + anisotropi pada adegan yang
> materialnya memang tidak memakainya. Menambah induk ketiga sebelum angka itu
> ada berarti menggandakan permukaan aset demi tebakan.

### Peta normal butuh sakelar, dan saluran lain tidak

Saluran tekstur disusun sebagai `skalar × tekstur`: tekstur yang tidak diisi
terbaca **putih**, yaitu identitas perkalian, jadi material tanpa tekstur memakai
skalarnya apa adanya tanpa cabang dan tanpa graph kedua. Untuk input yang
seluruhnya dikemudikan gambar, importir menyetel skalarnya ke 1 supaya
`1 × tekstur` benar-benar sama dengan gambarnya.

Peta normal tidak bisa begitu: `2 × putih − 1` adalah (1, 1, 1), sebuah normal
miring 45° di **setiap** piksel. Induknya karena itu menyusunnya sebagai
`lerp(datar, terdekode, normalTextureAmount)`, dan importir menyalakan sakelar
itu hanya ketika peta normalnya benar-benar **terpasang** — bukan ketika
materialnya menyebut satu. Peta yang disebut tapi berkasnya hilang menghasilkan
tekstur putih, dan sakelar yang terlanjur menyala jauh lebih buruk daripada
sekadar kehilangan detailnya.

---

## Kegunaan tekstur ditulis dari slotnya

`TextureResolver` kini menerima `TextureUsage`, dan editor menuliskannya ke
`TextureSettings` saat menyalin berkasnya.

**Sebelumnya kegunaannya ditebak dari nama berkas.** `GuessUsageFromName` benar
untuk `batu_n.png` dan meleset untuk `T_Wall_02.png` — dan yang menulis material
tahu persis slot mana yang diisi berkas itu. Peta normal yang salah didekode
sebagai sRGB tidak memunculkan galat; ia memunculkan permukaan yang cekung di
tempat yang seharusnya cembung.

Ditulis lewat `SaveTextureSettings`, yang menghapus berkasnya bila hasilnya sama
dengan bawaan berkas itu — jadi tekstur yang tebakan namanya memang sudah benar
tidak menambah satu berkas pun ke dalam kontrol versi.

Satu berkas tekstur tetap satu aset dengan satu `TextureSettings`: berkas yang
sama dirujuk dua material di slot berbeda memakai yang pertama, karena menyalinnya
dua kali berarti dua GUID untuk gambar yang sama.

---

## Yang tetap hilang, dan disebut namanya

Transmission, subsurface, dan thin film ada di OpenPBR dan di Physical Material,
dan **tidak ada** di `openpbr.slang` — [RENDER-OPENPBR.md](RENDER-OPENPBR.md)
membahas ketiganya beserta cara menjalankannya real time. Material kaca dari Max karena itu masuk
sebagai permukaan buram. Itu batas yang sah; yang tidak sah adalah membiarkannya
lewat tanpa suara — yang mengimpornya lalu mencari sebabnya di pencahayaan, di
tekstur, dan di eksportirnya, karena tidak ada satu pun baris yang menyebut bahwa
lapisannya memang dibuang di sini.

Jadi keduanya mencatat: pembaca `.mtlx` mengumpulkannya di
`MaterialXDocument::notes`, pembaca blok Max menulisnya sebagai peringatan log.
Begitu pula input yang dikemudikan node di luar gambar — sebuah `noise2d`, sebuah
`mix`, sebuah nodegraph utuh: mesin ini menyambungkan satu tekstur ke satu
saluran, dan memanggang graph orang lain menjadi berkas baru adalah pekerjaan
yang tidak diminta siapa pun.

**Satuan emisi belum dikalibrasi.** `emission_luminance` bersatuan nit (cd/m²)
dan bawaannya di Max 1500; pin `emissive` mesin ini satu float3 radiansi yang
ditambahkan sesudah BRDF. Yang dilakukan importir adalah perkalian apa adanya —
`emission_color × emission_luminance` — bukan sebuah faktor karangan yang tidak
bisa dijelaskan siapa pun. Mesin ini punya auto-eksposur dan ACES, jadi angka
sebesar itu tidak langsung salah; tetapi hubungan antara nit Max dan satuan lampu
di sini belum pernah diukur, dan itu pekerjaan tersendiri.

---

## Yang diperiksa uji

`SimAssetTests`, delapan test case:

- setiap input `open_pbr_surface` sampai ke pin yang namanya sama, dan input yang
  tidak disebut dokumennya memakai bawaan nodedef — yang sama persis dengan
  bawaan pin di `openpbr.slang`;
- input yang dikemudikan `image` menjadi tekstur dengan skalar identitas; yang
  dikemudikan `noise2d` tidak, dan alasannya tercatat;
- pemasangan dokumen pendamping menurut nama, satu-lawan-satu tanpa nama, dan
  penolakan menebak ketika ada dua dokumen di satu folder;
- blok `3dsMax|Parameters` menjadi material OpenPBR utuh — lewat sebuah FBX
  **ASCII** yang ditulis tangan di dalam berkas ujinya, karena fixture biner
  adalah berkas yang tidak bisa dibaca, di-diff, maupun diperbaiki siapa pun yang
  menemukan ujinya gagal;
- `shaderBall.fbx` dari Maya **tidak** mendadak membawa material OpenPBR — jalur
  Lambert/Phong lamanya tidak berubah;
- setiap parameter yang ditimpa instance benar-benar ada di induknya, dan setiap
  slot tekstur benar-benar bertipe Texture. Timpaan untuk parameter yang tidak ada
  tidak menghasilkan galat: `ResolveParameters` membuangnya diam-diam, dan
  materialnya memakai bawaan seolah impornya tidak pernah terjadi.

Induk `Material Impor OpenPBR.simmat` juga ikut lintasan `SimMaterialTests` yang
memuat, memvalidasi, dan membolak-balikkan **setiap** material bawaan.

Jalur tanpa MaterialX (`-DSIM_WITH_MATERIALX=OFF`) ikut dibangun dan diuji:
uji `.mtlx` melewatkan dirinya sendiri, dan jalur blok Max tetap berjalan karena
ia tidak menyentuh pustaka itu sama sekali.
