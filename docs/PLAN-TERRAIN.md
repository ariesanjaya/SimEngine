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

### L1 — Heightmap → mesh · ✅

`Sim::Terrain` menghasilkan geometri yang bisa digambar.

**Kriteria terima**
- Setiap simpul mesh sebuah ubin cocok dengan `HeightAt` pada sampel itu. ✅
  Diuji pada bentuk yang tidak rata, supaya "cocok" bukan pernyataan yang bidang
  datar mana pun akan lolos.
- **Jahitan antar ubin tidak berlubang.** ✅ Ubin membentang satu sampel lebih
  jauh daripada miliknya, sampai sampel pertama tetangganya, dan membacanya
  lewat `RawAt`. Yang dibaca hanya dibaca — tidak ada baris tepi yang disalin,
  jadi tidak ada dua salinan yang bisa berbeda. Memotong bentangan itu satu
  sampel menggugurkan dua uji.
- Satu sampel hole membuang tepat satu quad. ✅ Lubang di mesin ini memang
  **per-quad**, diindeks sampel kiri-bawahnya — jadi "tepat satu" adalah
  pernyataan yang bisa diucapkan tanpa syarat.

**Normalnya beda tengah heightmap, bukan normal segitiga dan bukan
`NormalAtWorld`.** Dua alasan, keduanya tentang jahitan. Normal segitiga membuat
lereng landai terlihat berundak, karena dua segitiga satu quad punya normal
berbeda sementara permukaannya mulus. Dan beda tengah adalah fungsi murni dari
koordinat sampel **global**, jadi simpul yang dimiliki dua ubin mendapat normal
yang sama persis dari keduanya — sementara `NormalAtWorld` menjawab gradien sel
bilinear, yang tepat di titik sampel memilih selnya secara asimetris. Benar,
tetapi tidak simetris, dan yang tidak simetris di tepi ubin adalah garis terang
yang membelah peta.

Ujinya memeriksa **lerengnya**, bukan komponen normal yang sudah ternormalisasi.
Versi pertamanya memeriksa `normal.x` dan gagal terhadap implementasi yang benar:
pada lereng curam, normalisasi memampatkan selisihnya sampai lereng 8 dan lereng
9 hanya berbeda 0,0016 — di bawah toleransi mana pun yang masuk akal. Lerengnya
sendiri berbeda 12%. Dan lereng **tetap** tidak bisa membedakan beda tengah dari
beda maju sama sekali; yang membedakannya permukaan melengkung, yang justru
keadaan biasa sebuah terrain.

**Ubin terakhir peta berhenti di sampel terakhir**, bukan membentang satu sampel
lebih jauh seperti tetangganya. Yang melampauinya menumbuhkan jalur datar di
luar peta — dan itu paling terlihat, karena di situlah orang berdiri untuk
melihat batas dunia. Konsekuensinya ubin terakhir punya S−1 quad, bukan S, dan
S−1 tidak habis dibagi langkah LOD mana pun kecuali satu; karena itu kolom
terakhir selalu diikutkan walaupun langkahnya tidak mendarat tepat padanya.

**Membangun mesh tidak mewujudkan ubin mana pun.** Ia hanya membaca lewat
`RawAt`, yang menjawab tinggi dasar untuk ubin yang belum pernah ditulis.
Alokasi malas adalah seluruh alasan terrain sebesar ini muat di memori, dan
pembaca yang mewujudkan ubin hanya dengan membacanya membatalkannya diam-diam —
yang terlihat bukan galat melainkan editor yang menghabiskan RAM saat membuka
peta. Diuji, dan mutasi yang menulis balik sampel yang baru dibacanya
menggugurkan ujinya.

**UV dalam meter**, bukan 0..1 per ubin. Layer terrain menyebut ukurannya dalam
meter per pengulangan (`TerrainLayer::tileSize`), dan UV yang diregangkan per
ubin membuat pengulangan itu berubah ukuran setiap kali seseorang mengganti
jumlah ubin.

### L2 — LOD · ✅

Tingkat perincian, dan jahitan di antaranya.

**Kriteria terima**
- LOD n menghasilkan (S/2ⁿ)² quad, dan batasnya tetap menyentuh tepi ubin. ✅
  Jangkauannya diperiksa terpisah dari jumlah quad-nya: ubin yang ikut menyusut
  saat kamera menjauh meninggalkan celah yang tepat sebesar ubinnya.
- **Tepi halus berada tepat pada ruas tepi yang kasar.** ✅ Retakan LOD adalah
  satu baris piksel latar di antara dua ubin — mustahil dilihat uji headless
  sebagai gambar, tetapi persis terukur sebagai posisi simpul. Ujinya juga
  memeriksa bahwa tanpa penjahitan tepinya memang **tidak** berimpit (selisih di
  atas 0,1 m), sehingga ia tidak lolos untuk implementasi yang tidak menjahit
  apa pun. Mematikan penjahitan menggugurkan 13 pernyataan.
- Pemilih LOD monoton terhadap jarak, dan tidak pernah melompat dua tingkat. ✅
  Diperiksa pada 401 jarak berurutan, ditambah ambang yang disebutkan angkanya.
  Membulatkan ke terdekat alih-alih ke bawah menggugurkan ujinya.

**Yang halus menjahit ke yang kasar, dan hanya satu sisi yang mengerjakannya.**
Kalau keduanya menyesuaikan diri, keduanya bergerak dan tidak ada yang menjadi
acuan — retakannya berpindah alih-alih tertutup.

Daftar simpul tepi yang kasar dibangun lewat `Columns` yang **sama persis**
dengan yang dipakai ubin kasar itu untuk menyusun simpulnya. Dipakai bersama,
bukan dihitung ulang: dua rumus yang "seharusnya sama" adalah dua rumus yang
suatu saat tidak sama lagi, dan yang tidak sama di sini adalah retakan yang
justru sedang ditutup.

Tinggi **dan** normalnya diinterpolasi. Tinggi saja menutup retakannya; normal
yang tidak ikut meninggalkan garis terang di tempat retakan tadi — cacat yang
lebih halus, dan karena itu lebih lama dicari.

Sudut ubin sengaja tidak dijahit dua kali. Ia ujung dua tepi sekaligus, dan yang
menggesernya dua kali memindahkannya ke tempat yang bukan milik tepi mana pun —
retakan di sudut adalah lubang, bukan garis.

**Syarat `tetangga > lod` adalah penghematan, bukan syarat kebenaran.** Ini
ditemukan lewat mutasi, bukan dari membaca kode: menggantinya dengan `!= lod` —
sehingga penjahitan ikut berjalan terhadap tetangga yang lebih halus — tidak
menggagalkan satu pun dari 1292 pernyataan. Sebabnya struktural: daftar simpul
tetangga yang lebih halus selalu superset daftar milik ubin ini, jadi setiap
simpul mendarat tepat pada titik acuannya dan penjahitannya berakhir sebagai
operasi kosong. Ujinya tetap ada untuk mengunci perilakunya, dan komentarnya
menyebut bahwa ia mengunci, bukan menjaga.

**Pemilih LOD-nya fungsi murni** dari jarak, ukuran ubin, dan sebuah pengali
kualitas — tanpa kamera dan tanpa keadaan. Ambangnya diukur terhadap ukuran ubin
itu sendiri, bukan jarak mutlak: yang menentukan seberapa kasar sebuah ubin boleh
digambar adalah berapa piksel yang ditempatinya, dan ubin sepuluh meter tidak
boleh berbagi ambang dengan ubin satu kilometer. Dibulatkan **ke bawah**, karena
yang membulatkan ke terdekat menaruh ambangnya di tengah penggandaan — sehingga
ubin berganti perincian saat ia masih menempati piksel dua kali lebih banyak
daripada yang dianggarkan.

Masukan yang tidak masuk akal — jarak NaN, ukuran ubin nol — menjawab perincian
**penuh**, bukan terkasar. Perbandingannya karena itu ditulis `!(d > ambang)`
dan bukan `d <= ambang`: ubin di depan hidung yang tiba-tiba menjadi empat
segitiga jauh lebih terlihat daripada ubin jauh yang terlalu halus.

### L3 — Terrain di viewport · ✅

Yang dipahat akhirnya terlihat dari sudut pandang mana pun.

**Kriteria terimanya diubah, dan itu keputusan.** Rencananya semula berbunyi
"ubin yang belum pernah disentuh tidak menghasilkan apa pun". Itu salah:
terrain baru **tidak punya satu pun ubin penghuni**, jadi aturan itu membuatnya
tak terlihat sama sekali — dan yang tak terlihat tidak bisa diklik, sehingga L4
tidak punya apa pun untuk dipahat. Ubin yang datar setinggi `baseHeight` tetap
tanah yang harus terlihat.

Alokasi malas yang sebenarnya perlu dijaga bukan "jangan gambar ubin kosong"
melainkan "**menggambar tidak boleh mewujudkan ubin**", dan itu sudah dikunci
uji di L1. Ubin jauh murah karena LOD, bukan karena tidak digambar.

**Kriteria terima**
- Satu instance per ubin, lewat `AcquireMeshData` yang sudah ada sejak W5. ✅
  Kuncinya menyebut ubinnya (`guid#tx,ty`), bukan terrainnya: satu kunci untuk
  seluruh peta berarti keenam puluh empat ubin saling menimpa di cache renderer,
  dan yang tergambar adalah ubin mana pun yang kebetulan terakhir diunggah.
- Goresan menaikkan penanda unggahan, dan penanda itu sampai ke kunci
  unggahan. ✅ Termasuk lewat undo: undo yang membetulkan data sambil
  meninggalkan layar menggambar bentuk yang sudah tidak ada adalah bug yang
  tampak seperti "undo tidak bekerja".

**Meshnya di-cache, dan itu syarat bukan kemewahan.** `SceneView::Build`
berjalan tiap frame; membangun ulang ubin 512² per frame adalah ratusan ribu
simpul yang dihitung untuk hasil yang sama persis. Pada peta sungguhan itu bukan
pemborosan melainkan editor yang tidak bisa dipakai.

**Yang menentukan "berubah" adalah revisi per ubin, bukan versi dokumen.** Versi
dokumen naik untuk seluruh terrain, jadi menggores satu sudut peta akan
mengunggah ulang keenam puluh empat ubinnya — tiap frame selama tombol ditahan.
`Terrain::TileRevision` naik hanya pada ubin yang benar-benar berubah bentuknya.

Revisinya angka yang **hanya naik**, bukan penanda "kotor" yang harus dibersihkan
seseorang. Penanda begitu menuntut satu pemilik yang menghapusnya, dan pembaca
kedua yang datang belakangan menemukannya sudah bersih padahal ia belum
membangun apa-apa. Angka yang hanya naik bisa dibandingkan siapa pun, sebanyak
apa pun, tanpa saling meniadakan.

Bentuk saja yang menaikkannya: tinggi dan lubang, **bukan bobot layer**.
Mengecat tidak mengubah satu pun simpul, dan menaikkannya di sana berarti terrain
empat kilometer dibangun ulang setiap sapuan kuas. Menulis nilai yang sama juga
tidak menaikkannya — brush berkekuatan nol tidak boleh menyibukkan GPU.

**Yang dibandingkan adalah revisi ubin beserta kedelapan tetangganya.** Meshnya
membaca satu baris dari tetangga sisi, dan normal beda tengah di simpul pojok
membaca satu sampel lagi secara diagonal — jadi menyunting ubin sebelah menggeser
tepi ubin ini. Yang menghemat tanpa memperhitungkan itu meninggalkan retakan
yang hanya muncul di dekat batas ubin, dan hanya kadang-kadang. Dijumlah, bukan
diambil maksimumnya: maksimum tidak berubah ketika sebuah tetangga naik dari 3 ke
4 sementara yang lain sudah 7.

**LOD dihitung dua lintasan**: seluruh ubin dulu, baru meshnya. Menjahit tepi
menuntut LOD tetangga, dan yang menghitungnya sambil jalan hanya tahu LOD ubin
yang sudah lewat — separuh jahitannya akan memakai angka yang belum ada.

Posisi kamera dipindahkan ke ruang terrain sekali, bukan tiap ubin dipindahkan ke
ruang dunia: satu matriks kali satu titik, bukan sekali per ubin.

Tiga mutasi membuktikan pembatalan cache-nya memegang: membandingkan revisi ubin
saja tanpa tetangga, membiarkan bobot layer menaikkan revisi, dan menghilangkan
kenaikan revisi pada undo — ketiganya menggugurkan uji.

### L4 — Sculpt di viewport 3D · ✅

Menutup satu-satunya kriteria E7.3 yang tersisa selain LOD.

**Kriteria terima**
- Sinar dari atas mengenai tinggi yang sama dengan `HeightAtWorld`; sinar yang
  meleset menjawab "tidak kena". ✅ Diperiksa di 81 titik, bukan satu: satu
  titik bisa kebetulan benar pada implementasi yang menjawab tinggi rata-rata
  seluruh peta.
- Sinar mendatar mengenai lereng di sisi depan, bukan menembus bukit. ✅ Titik
  tembusnya **diketahui sebelum uji dijalankan** — lereng linear dari 0 di
  x = 27 ke 20 di x = 28 memotong ketinggian 9 tepat di x = 27,45.
- Goresan lewat viewport dan lewat panel menghasilkan heightmap yang sama
  byte-per-byte. ✅ dengan catatan di bawah.

**Ditelusuri terhadap heightmap, bukan terhadap segitiga yang tergambar.** Dua
alasan, keduanya menentukan. Segitiga yang tergambar bergantung pada LOD, dan
LOD bergantung pada jarak kamera — jadi menembak segitiga membuat titik yang
dipahat berpindah ketika kamera maju-mundur, tanpa satu pun yang berubah di
terrainnya. Dan segitiganya tidak ada di sisi engine yang bisa ditanya: mereka
penghuni memori GPU, dan menyalinnya kembali untuk sebuah klik jauh lebih mahal
daripada menelusuri heightmap yang sudah di tangan.

Langkahnya setengah jarak sampel, lalu perpotongannya dipersempit dengan bagi
dua. Heightmap tidak bisa menyatakan fitur yang lebih sempit daripada satu
sampel, jadi langkah setengahnya tidak melewatkan apa pun yang benar-benar ada
di dalam datanya — sementara langkah sebesar satu sampel bisa menyeberangi
punggungan secara diagonal.

**Uji ketelitiannya semula tidak menguji apa pun.** Ini ketahuan lewat mutasi:
membuang seluruh bagi dua tetap lolos. Sebabnya dua — tinggi hasilnya dibaca
ulang dari `HeightAtWorld` sehingga selalu tepat di permukaan apa pun yang
terjadi pada x/z-nya, dan pemeriksaan x-nya memakai `epsilon` doctest, yang
mengalikan toleransi dengan (1 + nilai) sehingga "epsilon 0,002" pada x ≈ 27
berarti 0,057 m — lebih longgar daripada satu langkah penuh. Diganti dengan
toleransi mutlak terhadap titik tembus yang dihitung tangan, dan mutasi yang
sama kini menggugurkannya.

**Sinar yang berpangkal di bawah permukaan menjawab "tidak kena."** Yang di
bawah tanah tidak bisa melihat apa yang dipahatnya, dan mengembalikan titik
tembus dari sisi bawah berarti kursor melompat ke tempat yang tidak ditunjuk
siapa pun.

**Kursornya mengikuti permukaan, bukan lingkaran datar.** Lingkaran datar
berbohong justru di tempat ia paling dibutuhkan: di lereng, jangkauan kuas yang
sebenarnya membentang jauh lebih panjang menuruni bukit daripada yang digambar,
dan yang memahat tepi jurang terus-menerus mengenai lebih banyak daripada yang
dimaksudnya. Lingkaran keduanya menandai tempat falloff mulai melembut — tanpa
itu, "seberapa lembut" hanya bisa diketahui dengan mencoba lalu membatalkan.

**Kuasnya milik store, bukan milik panel.** Jari-jari yang disetel di panel lalu
tidak berlaku di viewport adalah dua alat yang kebetulan bernama sama.
Pengubah papan ketiknya — Ctrl melembutkan, Shift membalik — dipisah menjadi
fungsi murni `EffectiveSculptBrush` yang dipanggil keduanya; menuliskannya dua
kali adalah dua tempat yang suatu saat tidak sepakat tentang apa arti Shift, dan
yang membaca ImGui langsung tidak bisa diuji tanpa menjalankan ImGui.

**Catatan tentang kriteria ketiga.** "Tidak ada jalur kedua" di sini dijamin
**oleh konstruksi**, bukan oleh perbandingan: viewport memanggil
`terrain::BrushStroke` yang sama persis dengan yang dipanggil panel, jadi
membandingkan hasil keduanya berarti membandingkan sebuah fungsi dengan dirinya
sendiri. Yang benar-benar bisa berbeda adalah **dari mana koordinat dunianya
datang**, dan itulah yang diuji: goresan yang titiknya ditemukan lewat sinar
menghasilkan heightmap yang sama byte-per-byte dengan goresan yang titiknya
diberikan langsung — dengan pemeriksaan tambahan bahwa goresannya memang
mengubah sesuatu, karena dua peta yang sama-sama kosong juga akan lulus
perbandingan itu.

Memahat mengambil alih tombol kiri sepenuhnya: gizmo tidak digambar dan klik
tidak memilih apa pun. Alat yang menyeret tanah sambil sesekali memindahkan
objek yang kebetulan di bawah kursor adalah alat yang tidak bisa dipercaya.
Seretan yang kursornya keluar peta di tengah jalan menyentuh di tempat terakhir
yang sah alih-alih berhenti diam-diam — goresan yang putus di tengah
menghasilkan dua entri undo untuk satu sapuan tangan.

### L5 — Collider heightfield (P4) · ✅

Membuka kembali [P4 di PLAN-PHYSICS.md](PLAN-PHYSICS.md), yang memang ditunda
sampai bagian ini ada.

- `ShapeKind::HeightField` di `Sim::Physics`, dimasak menjadi `PxHeightField`.
- `ColliderShape::Terrain` di `Sim::Scene`, dipasok lewat `ColliderGeometrySource`
  yang sudah ada sejak W6 — jadi `Sim::Physics` tetap tidak membuka berkas.
- Sampelnya diberikan apa adanya. **Bukan segitiga**: 512² sampel adalah setengah
  juta segitiga per ubin, dan `PxHeightField` justru ada supaya itu tidak perlu.

**Kriteria terima**
- Benda yang dijatuhkan pada beberapa titik berhenti pada ketinggian yang sama
  dengan datanya. ✅ Tiga teras berbeda, karena satu titik bisa kebetulan benar
  pada kisi yang sumbunya tertukar — dan menukar baris dengan kolom memang
  menggugurkan ujinya.
- Sampel hole benar-benar berlubang. ✅ Dengan bola kedua di atas tanah utuh,
  yang membuktikan lubangnya tidak melubangi seluruh peta.
- Mengubah heightmap terlihat oleh fisika tanpa menyusun ulang seluruh scene.
  ⏸ **Tidak dikerjakan, dan sengaja.** Editor ini membuang seluruh suntingan
  yang dilakukan selama Play begitu Stop ditekan — itu keputusan yang sudah
  tercatat di `PhysicsScene.h`, karena keadaan solver tidak bisa dibangun ulang
  di tengah jalan tanpa membuang momentum setiap benda. Memahat sambil Play
  berjalan karena itu tidak punya arti hari ini, dan membangun mesin untuk
  memperbarui collider di tengah simulasi berarti membangun untuk keadaan yang
  belum ada.

**Kisi tinggi, bukan segitiga.** Ubin 512² adalah setengah juta segitiga; kisi
yang sama adalah 262 ribu angka enam belas bit. Solver menurunkan segitiganya
sendiri saat dibutuhkan, dan itulah seluruh alasan bentuk ini ada.

**Sampelnya berpindah tanpa satu pun pembulatan.** `Sim::Terrain` menyimpan
tinggi sebagai `uint16` di dalam `[minHeight, maxHeight]`; PhysX menyimpannya
sebagai `int16` dikali sebuah skala. Keduanya kisi enam belas bit, jadi
perpindahannya adalah penggeseran titik nol sebesar 32768 — bukan pembulatan.
Titik nolnya dikembalikan lewat pose lokal bentuknya, dan menghilangkan
penggeseran itu menggugurkan empat dari lima uji.

Melewatkan float di antaranya akan menghasilkan collider yang tidak pernah
persis sama dengan yang digambar, dengan selisih yang terlalu kecil untuk dicari
dan terlalu besar untuk diabaikan ketika sebuah benda berhenti setengah
tenggelam. Ujinya membandingkan terhadap tinggi yang **benar-benar tersimpan**
di kisi, bukan terhadap angka bulat yang dimasukkan — yang membandingkannya
dengan angka bulat sedang menguji pembulatan ujinya sendiri.

**Skalanya masuk ke jarak sampel dan skala tinggi, bukan ke jutaan sampelnya.**
Kisi beraturan menyimpan jarak sampel sebagai satu angka; menskalakan isinya
berarti menyentuh setiap sampel untuk hasil yang satu perkalian sudah
memberikannya.

**Terrain dinamis dilewatkan, bukan dimundurkan ke kotak seperti whitebox.**
Kotak setengah-ukuran 0,5 di tempat terrain empat kilometer bukan hampiran
melainkan sesuatu yang lain sama sekali, dan benda yang bertumpu padanya
melayang di udara.

**Satu kisi untuk seluruh peta, dengan batas yang disebutkan.** Peta 4 km pada
jarak sampel 0,25 m adalah 268 juta sampel, dan tidak ada yang memasaknya. Yang
melampaui 4096² ditolak beserta angkanya alih-alih menjadi alokasi satu gigabyte
yang diam-diam. Collider per-ubin adalah langkah berikutnya ketika peta sebesar
itu benar-benar ada; menambahkannya sekarang berarti mengubah `BodyDesc` menjadi
berbentuk-jamak untuk kasus yang belum pernah diminta.

**Kisi tinggi tidak punya tebal, dan benda cepat menembusnya.** Ini ditemukan
lewat uji yang gagal, bukan dari membaca dokumentasi: versi pertama uji ini
menjatuhkan bola berjari-jari 0,25 m dari 45 m, dan ia lulus **hanya pada teras
tertinggi** — yang jatuhnya paling pendek. Pada 45 m bolanya bergerak 0,44 m tiap
langkah, hampir sepanjang diameternya, dan deteksi tabrakan diskret
melewatkannya. Itu sifat kisi tinggi di mesin fisika mana pun, bukan cacat yang
bisa dibetulkan di sini — yang butuh benda cepat menyalakan CCD. Ujinya kini
menjatuhkan dari lima meter, sehingga yang diukur adalah tinggi istirahatnya dan
bukan batas deteksi tabrakan PhysX.

Kisi ujinya juga sempat salah bentuk: ia lereng tetap, jadi setiap bola yang
mendarat menggelinding turun lalu jatuh dari tepi peta — yang terbaca sebagai
"collidernya tidak ada" padahal ia bekerja. Diganti teras datar, dan ditambah
pemeriksaan bahwa bolanya tidak bergeser: permukaan yang miring padahal datanya
rata akan terlihat di situ.

### L6 — Cat yang terlihat di viewport · ✅

Smoke test sesudah L5 menemukan celah yang tidak terlihat selama viewport 3D
belum ada: **mengecat layer bekerja sempurna sebagai data dan berhenti di
situ.**

Yang sudah ada dan sehat — dikunci 66 uji, dan diperiksa ulang lewat smoke test
dari kuas sampai berkas: bobot per layer, invarian total 255, undo per goresan,
peta bobot PNG 8-bit per layer, alokasi malas, dan pratinjau di peta 2D panel.

Yang tidak ada: **satu pun jalur dari bobot itu ke layar 3D.** Diverifikasi,
bukan diduga:

- Mesh ubin menghasilkan **satu ruas bermaterial −1**. Bobot layer tidak
  memecahnya.
- `WeightAt`/`ReadWeights` dipanggil di tiga tempat di luar `Sim::Terrain` —
  panel (peta 2D dan status) dan `Sim::Vegetation` (aturan sebar). Nol di jalur
  render.
- `TerrainLayer::material` hanya pernah **ditulis** panel dan **diserialisasi**
  `TerrainIo`. Tidak ada yang membacanya untuk menggambar.
- Viewport menggambar terrain dengan satu warna rata.

#### Kenapa ini bukan "texture splatting", dan kenapa itu benar

Renderer ini belum menggambar tekstur pada mesh **sama sekali**. Atribut vertex
yang sampai ke GPU hanya posisi dan normal; UV dan tangent ikut terunggah di
dalam `MeshVertex` tetapi tidak pernah dideklarasikan sebagai atribut. Warna
sebuah permukaan datang dari satu `float4` — per ruas lewat push constant, atau
per instance.

Splatting bertekstur karena itu menuntut, berurutan: UV sebagai atribut, jalur
unggah tekstur dari editor, pengelolaan descriptor, dan varian shader yang
menyampel N tekstur layer. Itu **milestone renderer**, bukan fitur terrain — dan
ia prasyarat setiap mesh bertekstur, bukan hanya terrain. Menyelundupkannya ke
dalam rencana terrain berarti mengerjakan E8 dengan nama yang salah.

Yang dikerjakan L6 adalah lapisan di bawahnya, yang tetap berguna sesudah
tekstur ada: **warna layer dipadukan per simpul menurut bobotnya.** Pada LOD 0
resolusinya persis satu sampel heightmap — sama dengan resolusi catnya sendiri.
Ia informasi yang sama dengan yang sudah ditampilkan peta 2D, kini di ruang tiga
dimensi, dan ia tetap menjadi warna dasar ketika material bertekstur menyusul.

Alternatif yang ditolak: memecah mesh menjadi satu ruas per layer dominan.
Itu tidak menuntut satu baris pun perubahan renderer — tetapi batas antar layer
menjadi tajam per segitiga, dan yang dilihat perancang adalah mosaik alih-alih
peralihan. Cat yang tidak bisa dipadukan bukan cat.

### L6a — Warna simpul dari bobot layer · ✅

**Rencananya menyebut larik sejajar `MeshData::colors`; yang dikerjakan sebuah
medan di dalam `MeshVertex`.** Alasan yang ditulis di rencana ternyata tidak
berlaku: larik sejajar menghemat memori **CPU** milik importir, sementara yang
mahal adalah memori GPU — dan di sana strukturnya seragam untuk seluruh mesh
betapapun ia disimpan di sisi CPU. Binding vertex terpisah menuntut stride yang
berbeda untuk mesh yang punya warna dan yang tidak, dan stride adalah sifat
**pipeline**: ia akan melipatgandakan varian pipeline yang sudah ada demi
penghematan yang tidak pernah terjadi. Medan di dalam vertex membuat perubahan
renderernya tiga baris.

Putih adalah nilai satuannya, bukan "belum diisi": importir yang tidak
menyentuhnya menghasilkan mesh yang tergambar persis seperti sebelum medan ini
ada.

**Kriteria terima**
- Sampel yang dicat penuh berwarna layer itu persis; yang tidak tercat berwarna
  **layer dasar** — bukan putih, dan bukan warna yang dikarang renderer. ✅
- Padanan setengah-setengah jatuh di tengah keduanya. ✅ Inilah yang membedakan
  cat yang bisa dipadukan dari mosaik; mengabaikan bobotnya menggugurkan enam
  pernyataan.
- Seperempat cat menyisakan tiga perempat dasar — layer dasar adalah **sisa**,
  bukan angka tersimpan. ✅
- LOD kasar tetap berwarna. ✅

**Aturan L3 dibalik, dan itu temuan dari uji sendiri.** L3 menetapkan bahwa
bobot layer bukan bentuk, sehingga mengecat tidak membangun ulang mesh. Sesudah
warna dipanggang ke simpul, kalimat itu tidak lagi benar — dan membiarkannya
berarti setiap sapuan kuas cat tidak pernah tergambar. Yang sebenarnya ingin
dijaga aturan lama tetap dijaga: cat tidak boleh membangun ulang collider empat
kilometer.

Jawabannya **dua revisi terpisah** per ubin. `TileRevision` untuk bentuk —
tinggi dan lubang — yang dibaca fisika dan mesh. `TilePaintRevision` untuk bobot
layer, yang dibaca mesh saja. Menyatukannya berarti salah satu dari dua
kesalahan: cat yang membangun ulang collider, atau cat yang tidak pernah
terlihat.

### L6b — Renderer menggambar warna simpul · ✅

Satu atribut baru di binding 0, bersama posisi dan normal — bukan binding
sendiri, karena alasan stride di atas. Shader **mengalikan**, tidak
menggantikan: yang menggantikan akan membuat setiap mesh tanpa warna simpul
menjadi hitam pekat pada hari atribut ini ditambahkan.

Alpha-nya ikut dikalikan supaya sisi yang sengaja tembus pandang tetap tembus
pandang. Sorotan seleksi memakai jalur `forceInstanceColor` yang sudah ada, jadi
ia tidak ikut teredam warna simpul — objek terpilih tetap berubah warna
seluruhnya.

**Kriteria terima**
- Mesh tanpa warna simpul tergambar persis seperti sebelumnya. ✅ Putih adalah
  nilai satuan perkalian, dan kubus satuan bawaan renderer ikut diisi putih.
- Terrain yang dicat memperlihatkan peralihan halus. 🔶 belum dinilai mata —
  menunggu Anda menjalankan editornya, seperti W5 dan L3.

### L6c — Material layer terhitung terpakai · ✅

Ditemukan pada smoke test yang sama: `AssetUsage` tidak mengenal terrain, jadi
material yang dirujuk sebuah layer **tidak terhitung terpakai** — ia bisa
dilaporkan yatim lalu dihapus, dan yang menghapusnya tidak akan pernah tahu apa
yang hilang sampai terrainnya dibuka lagi.

Sebabnya ternyata bukan `AssetUsage` melainkan **importir**: `DocumentImporter`
mengumpulkan GUID dari Level, Prefab, Material, Graph, dan Json — dan tidak dari
Terrain. `.simterrain` karena itu jatuh ke importir pass-through dan menyatakan
nol dependensi, sehingga `UsersOf()` tidak pernah menyebutnya. Satu baris.

**Kriteria terima**
- Material yang dipakai sebuah layer terrain terhitung terpakai. ✅ Mengeluarkan
  Terrain dari daftar itu lagi menggugurkan ujinya.

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
