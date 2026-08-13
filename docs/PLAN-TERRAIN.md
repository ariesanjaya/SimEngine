# Plan Terrain di dunia (L0 → L5)

Lanjutan **E7.3** di [PLAN-EDITOR.md](PLAN-EDITOR.md). Yang di sana sudah selesai
adalah terrain sebagai **dokumen**: penyimpanan berubin, layer material, peta
bobot dan hole, brush sculpt dan paint, undo per goresan, I/O `.simterrain` + PNG
16-bit + RAW, dan panel dengan peta 2D dari atas. Empat kriteria terimanya
terpenuhi dan terkunci 46 uji.

Yang belum ada adalah **terrain sebagai benda di dunia**. Hari ini terrain tidak
bisa dijatuhkan ke level, tidak tergambar di viewport, tidak bisa dipahat dari
sudut pandang mana pun kecuali dari atas, dan tidak bisa ditabrak. Itulah yang
dikerjakan rencana ini.

Penomoran **L** (lanskap) supaya tidak bertabrakan dengan E (editor/render), P
(fisika), C (kain), A (agentic AI), R (Embree), I (gambar), T (tekstur), M (GI),
dan W (whitebox) di [ROADMAP.md](ROADMAP.md).

---

## Yang menentukan bentuknya

**Terrain bukan mesh, dan memperlakukannya seperti mesh adalah kesalahan yang
mahal.** Sebuah terrain 4×4 km dengan sampel 0,25 m adalah 256 juta segitiga.
Tidak ada yang mengunggahnya, dan tidak ada yang menyimpannya sebagai `MeshData`.
Yang bisa digambar adalah **ubin, pada tingkat perincian yang dipilih menurut
jarak** — dan itu bukan optimasi yang ditunda melainkan syarat supaya ada sesuatu
yang tergambar sama sekali.

Konsekuensinya menyebar ke seluruh rencana ini:

- Pembangun meshnya menghasilkan **satu `MeshData` per ubin per tingkat LOD**,
  bukan satu untuk seluruh terrain.
- Fisika **tidak** menerima segitiga: 512² sampel adalah setengah juta segitiga
  per ubin. `PxHeightField` membaca kisi sampel apa adanya, dan itu jalur yang
  ada justru untuk kasus ini.
- Penyuntingan di viewport **tidak** menembak segitiga: sinar ditelusuri langsung
  terhadap heightmap, yang jauh lebih murah dan tidak bergantung pada LOD mana
  yang kebetulan sedang tergambar.

**Satu dokumen, banyak pembaca.** Panel menyunting, viewport menggambar, fisika
menabrak. Kalau ketiganya memuat sendiri-sendiri, yang tergambar adalah bentuk
sebelum goresan terakhir dan yang ditabrak adalah bentuk sebelum itu lagi.
Aturan yang sama sudah terbukti di W5: satu `TerrainStore` memegang dokumen yang
terbuka beserta penanda versinya, dan penanda itu yang membuat pembaca lain tahu
kapan harus membangun ulang.

---

## Milestone

### L0 — Terrain di dalam level · ✅

Terrain berhenti menjadi dokumen yang berdiri sendiri dan menjadi sesuatu yang
dimiliki sebuah entity.

- `TerrainComponent { AssetRef terrain; }` di `Sim::Scene`, direfleksikan seperti
  komponen lain.
- `TerrainStore` di `Sim::EditorFramework`: memuat sekali, dibagi panel dan
  viewport, dengan `Version()`/`MarkDirty()` yang sama bentuknya dengan
  `WhiteboxStore`.
- Panel Terrain berhenti memegang `Terrain` miliknya sendiri dan membaca dari
  store.
- Menjatuhkan `.simterrain` ke viewport membuat entity yang membawanya.

**Kriteria terima**
- `TerrainComponent` bolak-balik lewat simpan/muat level tanpa kehilangan
  rujukannya. ✅ Diperiksa dua kali: medannya dibaca ulang, **dan** teks simpan
  keduanya dibandingkan — medan yang hilang di perjalanan lolos dari pemeriksaan
  pertama kalau ia kebetulan bernilai bawaan.
- Dua pembaca yang meminta guid yang sama mendapat **objek yang sama**, bukan dua
  salinan. ✅ Diuji dengan menyunting lewat yang satu dan membaca lewat yang lain.
- Goresan brush menaikkan versi store; membuka ulang tidak mengulang pembacaan
  berkas. ✅ Berkas yang gagal dibaca ikut dicatat, sehingga ia tidak diurai
  ulang enam puluh kali per detik sambil membanjiri log dengan pesan yang sama.

**Menyimpan tidak menaikkan versi.** Yang berubah adalah berkasnya, bukan
bentuknya — dan menaikkannya berarti menyuruh viewport mengunggah ulang terrain
empat kilometer setiap kali seseorang menekan Save.

**Penunjuk dokumen disegarkan sekali per frame**, di awal `OnDraw` panel. Store
bisa dikosongkan di antara dua frame — project berganti, terrain ditutup — dan
penunjuk yang disimpan lintas frame akan menunjuk memori yang sudah dibebaskan
tanpa satu pun tanda. Satu pemeriksaan di satu tempat menggantikan pemeriksaan di
setiap jalur di bawahnya.

**Dokumen yang terbuka ikut dibuang saat project ditutup**, dan itu berlaku juga
untuk whitebox yang selama ini tidak dibuang. Bukan kerapian: sebuah terrain
berukuran ratusan megabyte, dan membiarkannya berarti membuka project kedua
sambil tetap membayar yang pertama sampai editor ditutup.

**Terrain berdiri di titik asal saat dijatuhkan**, bukan di bawah kursor seperti
mesh. Titik asal terrain adalah **sudut** petanya, bukan pusatnya, jadi
menjatuhkannya di tempat kursor kebetulan berada menaruh peta empat kilometer di
sembarang tempat — dan hampir setiap kali yang berikutnya dilakukan orang adalah
menolkan transformnya kembali.

### L1 — Heightmap → mesh · ⬜

`Sim::Terrain` menghasilkan geometri yang bisa digambar.

- `BuildTileMesh(terrain, tileX, tileY, lod)` → `assets::MeshData`.
- Normal dihitung dari heightmap, bukan dari segitiga: normal per-segitiga
  membuat lereng landai terlihat berundak.
- Sampel hole membuang quad-nya, bukan menurunkannya.

**Kriteria terima**
- Setiap simpul mesh sebuah ubin cocok dengan `HeightAt` pada sampel itu.
- **Jahitan antar ubin tidak berlubang**: ubin membaca satu baris dari
  tetangganya lewat `RawAt`, dan mesh dua ubin bersebelahan berbagi tepi yang
  sama persis.
- Satu sampel hole membuang tepat quad yang menyentuhnya, tidak lebih.

### L2 — LOD · ⬜

Tingkat perincian, dan jahitan di antaranya.

- Mesh pada langkah sampel 2ⁿ.
- Tepi ubin yang bertetangga dengan LOD lebih kasar **dijahit**, bukan dibiarkan:
  retakan di batas LOD adalah cacat visual paling khas terrain, dan ia muncul
  justru saat kamera bergerak.
- Pilihan LOD adalah fungsi murni dari jarak dan ukuran ubin, supaya bisa diuji
  tanpa kamera.

**Kriteria terima**
- LOD n menghasilkan (S/2ⁿ)² quad, dan batasnya tetap menyentuh tepi ubin.
- Ubin LOD 0 di sebelah ubin LOD 1 tidak meninggalkan celah: setiap simpul tepi
  yang lebih halus berada tepat pada ruas tepi yang lebih kasar.
- Pemilih LOD monoton terhadap jarak, dan tidak pernah melompat dua tingkat.

### L3 — Terrain di viewport · ⬜

Yang dipahat akhirnya terlihat dari sudut pandang mana pun.

- `SceneView` mengajukan ubin terrain yang penghuni memori, satu instance per
  ubin, lewat `AcquireMeshData` yang sudah ada sejak W5.
- LOD dipilih dari jarak kamera; ubin yang berubah diunggah ulang lewat penanda
  versi store.

**Kriteria terima**
- Terrain dengan N ubin penghuni menghasilkan N instance, dan ubin yang belum
  pernah disentuh tidak menghasilkan apa pun.
- Goresan brush menaikkan versi, dan versi itu sampai ke kunci unggahan.

### L4 — Sculpt di viewport 3D · ⬜

Menutup satu-satunya kriteria E7.3 yang tersisa selain LOD.

- `RaycastTerrain(terrain, origin, direction)` — ditelusuri terhadap heightmap,
  bukan terhadap segitiga yang kebetulan tergambar.
- Kursor brush digambar mengikuti permukaan.
- Goresan di viewport memakai jalur brush dan undo yang sama persis dengan panel;
  tidak ada jalur kedua.

**Kriteria terima**
- Sinar dari atas mengenai tinggi yang sama dengan `HeightAtWorld` dalam
  toleransi yang ditulis; sinar yang meleset menjawab "tidak kena".
- Sinar yang datang mendatar mengenai lereng di sisi yang benar, bukan menembus
  bukit.
- Goresan lewat viewport dan goresan lewat panel dengan parameter sama
  menghasilkan heightmap yang sama byte-per-byte.

### L5 — Collider heightfield (P4) · ⬜

Membuka kembali [P4 di PLAN-PHYSICS.md](PLAN-PHYSICS.md), yang memang ditunda
sampai bagian ini ada.

- `ShapeKind::HeightField` di `Sim::Physics`, dimasak menjadi `PxHeightField`.
- `ColliderShape::Terrain` di `Sim::Scene`, dipasok lewat `ColliderGeometrySource`
  yang sudah ada sejak W6 — jadi `Sim::Physics` tetap tidak membuka berkas.
- Sampelnya diberikan apa adanya. **Bukan segitiga**: 512² sampel adalah setengah
  juta segitiga per ubin, dan `PxHeightField` justru ada supaya itu tidak perlu.

**Kriteria terima**
- Benda yang dijatuhkan pada beberapa titik berhenti pada ketinggian yang sama
  dengan `HeightAtWorld`, dalam toleransi yang ditulis.
- Sampel hole benar-benar berlubang: benda yang jatuh di atasnya tidak tertahan.
- Mengubah heightmap lalu membangun ulang collider ubin itu saja terlihat oleh
  fisika, tanpa menyusun ulang seluruh scene.

---

## Risiko

**Retakan LOD.** Ini cacat terrain yang paling khas dan paling sulit dilihat di
uji headless, karena ia muncul sebagai satu baris piksel latar di antara dua
ubin. Mitigasinya menyatakan kriterianya secara geometris — setiap simpul tepi
yang lebih halus harus terletak pada ruas tepi yang lebih kasar — sehingga yang
diuji adalah posisi simpul, bukan gambar.

**Anggaran memori.** Alokasi malas `Sim::Terrain` bisa dibatalkan tanpa sengaja
oleh pembacanya: menanyakan tinggi sebuah sampel tidak mewujudkan ubin, tetapi
membangun mesh untuk seluruh ubin tentu saja mewujudkannya. Yang dibangun hanya
ubin yang **sudah** penghuni, dan `BytesResident()` tetap menjadi angka yang
diuji.

**Dua jalur penyuntingan.** Panel 2D dan viewport 3D harus memakai brush, goresan,
dan undo yang sama. Jalur kedua yang "sementara" adalah cara paling pasti
mendapatkan dua perilaku yang berselisih pada kasus tepi, dan yang berselisih di
kasus tepi tidak pernah terlihat sampai seseorang mengeluh bahwa undo-nya aneh.
