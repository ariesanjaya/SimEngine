# Analisa: menutup celah "belum bertekstur"

Ditulis setelah L6 (cat layer terrain) dan L7 (decal) sama-sama berhenti pada
kalimat yang sama: *warnanya rata, belum bertekstur, karena renderer ini belum
menggambar tekstur pada mesh sama sekali.*

Dokumen ini menjawab satu pertanyaan: **apa yang harus dikerjakan supaya kalimat
itu tidak perlu ditulis lagi**, dan dalam urutan apa.

---

## Yang ternyata sudah ada

Celahnya jauh lebih sempit daripada bunyi kalimatnya. Diperiksa di kode, bukan
diingat:

| Bagian | Keadaan |
| --- | --- |
| Graph material → Slang | ✅ `MaterialCompiler`, termasuk node `input.texture` |
| Slang → modul utuh yang bisa dikompilasi | ✅ `MaterialShaderModule` |
| Slang → SPIR-V, di-cache | ✅ `ShaderCache`, varian lewat konstanta spesialisasi |
| Tata letak blok parameter | ✅ `MaterialParameterBlock` |
| Konvensi binding set 2 | ✅ params di binding 0, tekstur+sampler berselang mulai 1 |
| Berkas gambar → `VkImage` | ✅ `rhi::Texture2D::CreateFromRgba`, dipakai ThumbnailCache dan IblBaker |
| Perender yang memakai semuanya | ✅ **tetapi hanya untuk bola preview** (`VulkanMaterialPreview`, 856 baris) |
| Seam-nya | ✅ `IMaterialPreview::SetMaterial` menerima SPIR-V mentah; `Sim::Render` tidak mengenal `Sim::Material` |
| Pass forward viewport | ❌ dua set (bayangan, kulit), warna rata, UV tidak dideklarasikan |

Artinya **tidak ada satu pun bagian besar yang harus ditemukan**. Yang belum ada
adalah penyambungannya di pass forward.

Analisanya pun sudah ada, dan masih tajam:
[PLAN-RENDER.md § E8.4](PLAN-RENDER.md) sudah mendaftar tiga ketidaksepakatan
antara modul material dan pipeline kotak, dan sudah menyimpulkan urutan
kerjanya. Dokumen ini **tidak mengulanginya** — ia mengoreksi yang sudah usang,
memutuskan satu hal yang digantung di sana, dan menambahkan jalan pintas yang
baru menjadi masuk akal setelah L6/L7.

---

## Tiga koreksi terhadap E8.4

Analisa itu ditulis sebelum beberapa hal mendarat. Yang berubah:

1. **"`MeshVertex` belum punya tangent sama sekali" — sudah tidak berlaku.**
   `MeshVertex` sekarang membawa posisi, normal, uv, tangent, dan (sejak L6)
   warna. Tangent sudah dibangkitkan importir. Rintangan yang di sana disebut
   "paling menentukan" sudah lewat.

2. **UV sudah ada di GPU, hanya tidak dideklarasikan.** Buffer vertexnya
   di-`memcpy` dari `MeshVertex` apa adanya, jadi uv dan tangent sudah terunggah
   di offset 24 dan 32 — yang belum ada hanya `VkVertexInputAttributeDescription`
   dan varying-nya. Biayanya nol byte tambahan.

3. **Set 2 memang kosong di pass forward.** `forwardSets` berisi dua layout,
   dan modul material menulis ke set 2. Keduanya sudah sepakat tanpa
   direncanakan — konvensi binding modul material disalin ke preview dan dikunci
   uji di `MaterialTests.cpp`.

---

## Keputusan yang digantung E8.4, dan jawabannya

> *"Yang harus diputuskan lebih dulu: apakah `FrameParams` modul diperluas
> menjadi set 0 renderer seutuhnya, atau material mendapat set keempat."*

**Rekomendasi: perluas set 0 renderer, jangan beri material set keempat.**

Alasannya bukan kerapian. Set 0 renderer memuat 21 binding — cascade bayangan,
cluster lampu, atlas spot/point, clipmap SDF, tekstur GI. Material yang memakai
`FrameParams`-nya sendiri **tidak bisa melihat satu pun dari itu**. Mengganti
`box.frag` dengan material yang buta terhadap bayangan dan GI bukan peningkatan
melainkan kemunduran, dan kemunduran yang akan dilaporkan sebagai "bayangannya
hilang sejak material dipasang".

Set keempat menghindari itu tetapi memindahkan persoalannya: modul material
tetap harus tahu binding mana yang harus dibacanya untuk bayangan dan GI, jadi
`AssembleMaterialModule` tetap harus menuliskan seluruh deklarasi itu. Yang
dihemat hanya nomor set — dan yang dibayar adalah dua sumber kebenaran tentang
apa isi set 0.

Ongkos memperluas set 0: `AssembleMaterialModule` menuliskan 21 binding alih-alih
lima, dan `VulkanMaterialPreview` harus menyediakan yang tidak dimilikinya
(preview tidak punya cascade bayangan maupun clipmap SDF) — dengan tekstur
tiruan 1×1, persis seperti `fallbackTexture_` yang sudah ada di sana.

---

## Rekomendasi: dua jalur, dan yang kecil dikerjakan lebih dulu

E8.4 menyimpulkan bahwa migrasi pass forward "bukan pekerjaan satu duduk dan
tidak boleh dimulai separuh" — selama setengah bermigrasi, viewport tidak
menggambar apa-apa. Itu benar, dan itu sebabnya ia terus ditunda.

Tetapi yang dibutuhkan decal dan layer terrain **bukan** OpenPBR penuh. Yang
dibutuhkan satu tekstur albedo yang disampel dengan UV yang sudah ada. Itu
subhimpunan yang bisa mendarat sendiri.

### Jalur A — slot albedo di pass forward yang sekarang (kecil)

Satu set deskriptor per material di **set 2**, dengan **konvensi binding yang
sama persis** dengan modul material: parameter di binding 0, tekstur dan sampler
berselang mulai binding 1. `box.frag` mengalikan warnanya dengan sampel albedo;
material tanpa tekstur mendapat tekstur putih 1×1, yang sudah menjadi pola di
`VulkanMaterialPreview`.

Yang dibutuhkan:

- Atribut uv (satu baris) dan varying uv (dua baris).
- Cache tekstur di renderer: `AcquireTexture(path)` → `rhi::Texture2D`, dengan
  bentuk yang sama seperti `AcquireMesh` yang sudah ada. `ThumbnailCache` sudah
  membuktikan jalurnya.
- Satu descriptor set per material, dialokasikan saat material dipakai pertama
  kali; draw run sudah dikelompokkan per material lewat `partColors`, jadi
  tempat mengikatnya sudah ada.
- `SceneView` menyelesaikan material → jalur tekstur. Ia **sudah** menyelesaikan
  material → warna hari ini, jadi ini memperluas fungsi yang ada.

Yang didapat: decal bertekstur, layer terrain bertekstur, dan mesh impor yang
akhirnya memperlihatkan albedonya.

**Risikonya jujur:** ini jalur tekstur kedua di samping yang akan datang, dan
jalur kedua "sementara" adalah jalur yang tidak pernah mati. Mitigasinya bukan
disiplin melainkan bentuk: dengan nomor set dan konvensi binding yang sama,
Jalur B **mengganti shader-nya, bukan pipa-nya**. Cache tekstur, alokasi set,
dan pengelompokan draw dipakai ulang apa adanya.

### Jalur B — pipeline material di pass forward (besar)

Persis yang sudah diuraikan E8.4, dengan urutan yang disebut di sana: bentuk
set 0 diputuskan (§ di atas) → transform instance pindah ke storage buffer,
sementara pipeline kotak masih dipakai sehingga bisa diuji sendiri → pipeline
material masuk, dengan `box.frag` dipertahankan sebagai jalur mundur sampai yang
baru terbukti.

Jalur A **memperkecil** Jalur B, tidak menambahnya: begitu A mendarat, satu-satunya
yang tersisa di B adalah set 0 dan transform instance — dan keduanya soal
renderer murni yang bisa diuji tanpa menyentuh material.

---

## Urutan yang disarankan

1. **Jalur A**, dipecah tiga: (a) atribut+varying uv, (b) cache tekstur, (c) set
   material dan sampel albedo di `box.frag`. Masing-masing bisa diuji sendiri;
   (a) dan (b) tidak mengubah satu piksel pun sampai (c) mendarat.
2. **Decal dan layer terrain memakainya.** `DecalComponent` mendapat
   `AssetRef material`; `TerrainLayer::material` akhirnya dibaca — medan yang
   sudah tersimpan dan diserialisasi sejak E7.3 tanpa pernah dipakai.
3. **Keputusan set 0**, lalu transform instance lewat storage buffer.
4. **Jalur B.**
5. **[PLAN-TEXTURE.md](PLAN-TEXTURE.md) (T0–T5)** — BCn/KTX2 — kapan saja
   sesudah 1(b). Ia mengganti *format* yang diunggah cache tekstur, bukan siapa
   yang mengikatnya, jadi ia tegak lurus terhadap seluruh urutan ini.

**Yang tidak disarankan: bindless sekarang.** Ia jawaban yang benar untuk adegan
dengan ribuan material, dan ia memang tercatat sebagai yang tersisa dari E8.1.
Tetapi ia menyelesaikan masalah *batching*, sedangkan yang dihadapi sekarang
masalah *ketiadaan*. Mendahulukannya berarti menunda tekstur pertama demi
optimasi yang tidak bisa diukur sampai ada yang dioptimasi.

---

## Yang harus diperiksa sebelum mulai

**`MeshRendererComponent::baseColor` menjadi antarmuka yang berbohong.** Sudah
dicatat E8.4 dan masih menunggu: ia ada sebagai penambal sampai material
sungguhan datang. Begitu material membawa albedonya sendiri, membiarkannya tetap
mengalikan berarti Inspector punya dua tempat yang mengatur satu hal —
dan membiarkannya diam-diam tidak berpengaruh lebih buruk lagi.

Hal yang sama kini berlaku untuk **warna simpul yang ditambahkan L6**. Ia
mengalikan, jadi ia hidup berdampingan dengan tekstur tanpa bertabrakan — cat
layer terrain memodulasi tekstur layernya, dan itu memang yang diinginkan. Tetapi
begitu layer terrain punya tekstur sungguhan, warna simpul berhenti menjadi
"warna layer" dan menjadi "pemadu antar layer". Namanya harus ikut berubah, atau
ia akan dibaca sebagai yang pertama oleh orang berikutnya.
