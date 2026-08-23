# Plan Whitebox (W0 → W7)

Blok yang bisa disunting langsung di viewport untuk merancang level — dorong
sebuah sisi dan ruangan bertambah panjang, bukan kembali ke DCC untuk mengekspor
kubus lagi.

**Dengan material berbeda per sisi**, yang justru tidak dimiliki WhiteBox O3DE
yang mengilhaminya.

Penomoran **W** supaya tidak bertabrakan dengan E (editor/render), P (fisika),
C (kain), A (agentic AI), R (Embree), I (gambar), T (tekstur), dan M (GI) di
[ROADMAP.md](ROADMAP.md).

---

## Yang membuatnya bukan sekadar "mesh kubus"

Whitebox terasa seperti alat rancang, bukan penyunting segitiga, karena satu
gagasan: **poligon, bukan face.**

Sebuah kubus punya 12 segitiga tetapi 6 sisi. Yang didorong perancang adalah
sisinya. Jadi struktur datanya harus bisa mengatakan "enam segitiga ini satu
bidang" dan memperlakukannya sebagai satu benda — itulah yang dilakukan
`HideEdge` di O3DE: menyembunyikan rusuk di antara dua segitiga sebidang
menggabungkan keduanya menjadi satu poligon.

Tanpa lapisan itu, yang didapat pengguna adalah penyunting segitiga, dan
mendorong "sisi" berarti memilih dua segitiga dan berharap keduanya bergerak
bersama.

**Dan poligon itu pula yang menjadi satuan material.** Satu sisi = satu poligon =
satu slot material. Itu yang membuat permintaan "tiap face bisa dipasang material
berbeda" punya satuan yang masuk akal — bukan per segitiga, yang tak seorang pun
ingin menyetelnya satu per satu.

---

## Yang sudah ada di mesin ini, dan itu menentukan bentuknya

**Material per-ruas sudah terpasang di seluruh jalur.** `MeshData::parts` adalah
daftar `SubMesh { firstIndex, indexCount, material }`, dan
`MeshRendererComponent::materials` adalah daftar `AssetRef` per slot. Keduanya
sudah dipakai mesh impor sejak E8.

Konsekuensinya: **material per-sisi tidak menuntut satu pun perubahan renderer.**
Poligon dikelompokkan menurut slot materialnya, tiap kelompok menjadi satu
`SubMesh`, dan jalur gambar yang sudah ada mengerjakan sisanya.

Ini kebalikan dari keadaan di O3DE, yang `WhiteBoxRenderData`-nya memegang satu
`WhiteBoxMaterial` untuk seluruh benda — di sana material per-sisi berarti
membongkar jalur rendernya. Di sini ia justru jalur yang sudah ada.

---

## Keputusan yang harus diambil: struktur half-edge

Penyuntingan topologi — ekstrusi, belah rusuk, gabung sisi — menuntut adjacency
dua arah. Array segitiga tidak cukup: menjawab "siapa tetangga sisi ini" dengan
menyisir seluruh segitiga membuat tiap operasi kuadratik dan tiap bug topologi
diam-diam.

Dua jalan, dan ini perlu Anda putuskan sebelum W1:

### Opsi 1 — vendor OpenMesh

Yang dipakai O3DE. **BSD-3-Clause** sejak versi 4.0 — sudah diperiksa, aman untuk
ditautkan statis dan dipakai komersial dengan atribusi.

Matang, teruji bertahun-tahun, dan menyelesaikan seluruh kasus tepi topologi yang
memang di situlah bug hidup.

Harganya: pustaka template besar untuk mengambil sekitar sepersepuluh isinya, dan
waktu kompilasi yang ikut naik di setiap TU yang menyentuhnya.

### Opsi 2 — half-edge ringkas, ditulis sendiri

Sekitar 800–1.500 baris untuk yang benar-benar dibutuhkan: simpul, half-edge,
rusuk, face, adjacency, tambah/hapus face, dan penjagaan invarian.

**Yang membenarkannya: ukurannya.** Mesh whitebox adalah blockout level — puluhan
sampai ratusan sisi, bukan jutaan. Struktur yang tidak perlu cepat pada satu juta
segitiga boleh jauh lebih sederhana.

**Yang menentangnya: kasus tepi topologi.** Menggabungkan dan membelah face
dengan benar pada geometri non-manifold adalah tempat bug bersembunyi, dan
OpenMesh sudah membayarnya.

### Rekomendasi

**Opsi 2, dengan syarat.** Lapisan poligon — yang membuat whitebox terasa seperti
whitebox — harus ditulis sendiri betapapun pilihannya, karena ia bukan bagian
OpenMesh. Dan mesh sekecil blockout tidak menuntut struktur data seukuran itu.

Syaratnya: **invariannya diuji, bukan diasumsikan.** Setiap operasi topologi
berakhir dengan pemeriksaan menyeluruh — tiap half-edge punya pasangan, tiap face
tertutup, tiap simpul mencapai seluruh tetangganya. Itu yang menggantikan
kematangan OpenMesh, dan itu yang membuat opsi 2 bukan penghematan palsu.

Kalau pemeriksaan itu ternyata terus gagal di W1, jalur mundurnya jelas dan
murah: ganti isi `HalfEdgeMesh` dengan OpenMesh, karena lapisan poligon di atasnya
tidak berubah.

---

## Arsitektur

```
Code/Whitebox/
  include/Sim/Whitebox/
      HalfEdgeMesh.h    simpul, half-edge, face — adjacency dua arah
      Polygon.h         kelompok face sebidang, satuan yang disunting pengguna
      WhiteboxMesh.h    mesh + poligon + slot material; ini yang disimpan aset
      Operations.h      ekstrusi, belah, gabung, geser
  src/
      HalfEdgeMesh.cpp
      Polygon.cpp
      Operations.cpp
      MeshBuild.cpp     whitebox → MeshData ber-SubMesh per slot material
Code/Editor/src/
      WhiteboxTool.cpp  pemilihan sisi, gizmo dorong, penetapan material
```

**`Sim::Whitebox` tidak bergantung pada `Sim::Render` maupun `Sim::Editor`.** Ia
menghasilkan `MeshData`; yang menggambarnya dan yang menyuntingnya adalah
pemanggilnya. Aturan yang sama dengan `Sim::Physics` dan `Sim::Cloth`.

**Asetnya menyimpan topologi, bukan segitiga.** `.simwhitebox` berisi simpul,
rusuk, poligon, dan slot material — bukan mesh hasilnya. Yang tersimpan harus
yang bisa disunting lagi; segitiga adalah keluaran, dan menyimpannya berarti
blockout yang tidak bisa diubah setelah disimpan.

---

## Milestone

### W0 — Half-edge dan invariannya · ✅

Struktur data saja. Belum ada poligon, belum ada UI.

**Kriteria terima**
- Kubus satuan dibangun dari enam quad: 8 simpul, 12 rusuk, 6 face.
- **Pemeriksa invarian menyisir seluruh mesh** — tiap half-edge punya pasangan
  yang pasangannya kembali ke dirinya, tiap face tertutup melingkar, tiap simpul
  mencapai seluruh half-edge keluarnya. Dipanggil di akhir **setiap** uji operasi,
  bukan sekali di awal.
- Pemeriksa itu dibuktikan bisa gagal **per klausa**, bukan sekali. Pemeriksa yang
  satu klausanya diam adalah pemeriksa yang menjaga sebagian, dan yang tidak
  dijaganya justru bagian yang tak seorang pun tahu tidak dijaga.

  Empat kerusakan diuji lewat pintu belakang khusus uji: pasangan diputus,
  pasangan diarahkan salah, lingkaran face diputus, dan half-edge yang mengaku
  milik face lain. Keempatnya ditolak beserta pesan yang menyebut letaknya.

**Yang dikerjakan melebihi kriterianya**, karena keduanya murah dan tajam:
rumus Euler V − E + F = 2 diperiksa pada kubus (8 − 12 + 6), dan **arah tiap
normal sisi dihitung** terhadap vektor dari pusat kubus ke pusat sisinya — satu
quad yang urutan simpulnya terbalik tidak terlihat sebagai galat melainkan
sebagai lubang di dinding.

Mesh terbuka ikut disahkan sejak awal, bukan ditunda: satu quad menghasilkan
lingkaran batas sepanjang empat, dan blockout memang dimulai dari satu sisi
sebelum diekstrusi.

Normal memakai Newell, bukan hasil kali silang dua rusuk pertama — sesudah
beberapa kali ekstrusi poligon jarang sebidang sempurna, dan dua rusuk pertama
bisa hampir sejajar.

### W1 — Poligon · ✅

Kelompok face sebidang, dan operasi gabung/pisah rusuk yang membentuknya.

**Kriteria terima**
- Kubus satuan punya **6 poligon**, bukan 12 face. Inilah yang membedakan alat
  rancang dari penyunting segitiga.

  Kriteria ini semula tidak bisa diuji apa adanya: `MakeUnitCube` membangun enam
  **quad**, bukan dua belas segitiga, jadi ia sudah enam face sejak awal. Yang
  ditambahkan adalah `MakeUnitCubeTriangulated` — dan itu bukan versi yang lebih
  rendah melainkan **keadaan yang datang dari luar**: mesh impor selalu
  tersegitigakan, dan whitebox harus bisa mengelompokkannya kembali menjadi sisi.
  Dua belas face → enam poligon, enam diagonal disembunyikan.
- Menyembunyikan rusuk di antara dua face sebidang menggabungkan poligonnya;
  memulihkannya memisahkannya kembali, dan pengelompokannya kembali **persis**
  seperti semula — bukan sekadar berjumlah sama. Diuji lima putaran
  gabung-pulihkan berturut-turut, karena pengelompokan yang bocor sedikit tiap
  putaran hanya terlihat sesudah beberapa kali.
- Yang menyudut menolak digabung, dan penolakan itu dibuktikan berasal dari
  toleransinya: dengan toleransi 91° kubus yang sama menggabung semuanya.

**Identitas poligon kanonik, bukan dialokasikan.** Sebuah poligon selalu dikenali
oleh face bernomor terkecil di dalamnya. Percobaan pertama mengalokasikan nomor
baru saat poligon terbelah, dan ujinya menangkapnya: memulihkan rusuk
mengembalikan pengelompokan yang benar tetapi dengan nomor berbeda — yang berarti
seleksi yang dipegang penyunting akan putus setiap kali undo ditekan. Perwakilan
terkecil selalu sama berapa pun urutan penggabungannya.

**Topologi meshnya tidak disentuh sama sekali.** Menyembunyikan rusuk hanya
mengubah pengelompokan, dan itulah yang membuat pemulihannya persis. Rusuk yang
benar-benar dihapus harus dibangun ulang dari ingatan tentang bentuknya dahulu,
dan ingatan itu selalu kurang sesuatu.

`PolygonSet::CheckInvariants` menyisir empat klausa, termasuk yang paling halus:
**tiap poligon harus terhubung lewat rusuk tersembunyi**. Poligon yang terbelah
menjadi dua kepingan terpisah adalah sisi yang separuhnya tidak ikut bergerak
saat didorong — dan itu terlihat sebagai bug ekstrusi, bukan sebagai bug
pengelompokan.

### W2 — Ekstrusi dan geser · ✅

Operasi yang membuatnya berguna: dorong sebuah sisi keluar dan mesh bertambah.

**Kriteria terima**
- Mengekstrusi satu sisi kubus sejauh d menghasilkan volume yang **dihitung**:
  balok 1×1×(1+d). Diperiksa terhadap rumus, bukan terhadap tangkapan layar.

  Oracle volumenya ditulis di dalam ujinya lewat teorema divergensi, **bukan
  dipanggil dari pustakanya**: uji yang membandingkan hasil terhadap fungsi milik
  pustaka yang sama hanya menguji bahwa pustaka itu konsisten dengan dirinya
  sendiri.

  Diuji juga empat kali berturut-turut, karena sekali bisa benar karena
  kebetulan — yang diuji putaran kedua adalah bahwa hasil putaran pertama
  benar-benar bisa dipakai lagi.
- Invarian W0 **dan** W1 tetap berlaku sesudah setiap operasi. Ini yang menangkap
  bedah topologi yang menghasilkan bentuk benar tetapi struktur rusak — dan bentuk
  benar adalah persis yang dilihat orang saat memutuskan sesuatu sudah selesai.
- Ekstrusi nol tidak mengubah apa pun — bukan menghasilkan face berluas nol yang
  merusak normal di kemudian hari.
- Sisi yang sudah digabung dari beberapa segitiga **tetap satu sisi** sesudah
  didorong. Tanpa ini, mendorong sisi kubus impor akan memecahnya kembali menjadi
  segitiga di tangan pengguna.

**Meshnya dibangun ulang, bukan disulam.** Menyulam pointer half-edge di tempat
adalah tempat bug topologi hidup, dan mesh blockout berukuran puluhan sampai
ratusan sisi — membangunnya ulang memakan mikrodetik. Yang ditukar adalah
kerumitan dengan waktu, dan pada ukuran ini waktunya tidak terasa.

Konsekuensi yang menopang W5: **nomor face dipertahankan**, jadi poligon dan
seleksi bertahan melewati operasi. Face baru selalu ditambahkan di belakang,
tidak pernah disisipkan.

### W3 — Material per-sisi · ✅

Slot material per poligon, dan pembangunan `MeshData` yang mengelompokkannya.

**Kriteria terima**
- Kubus dengan tiga sisi bermaterial A dan tiga bermaterial B menghasilkan
  **tepat dua** `SubMesh`, bukan enam — poligon sematerial digabung.
- Jumlah indeks seluruh `SubMesh` sama dengan tiga kali jumlah segitiga. Tidak
  ada segitiga yang hilang maupun terhitung dua kali.
- Poligon tanpa material menghasilkan ruas ber-`material` -1, mengikuti aturan
  yang sudah dipakai mesh impor. Nol bukan penggantinya — nol adalah slot pertama
  yang sah.
- **Material bertahan melewati ekstrusi.** Sisi yang kehilangan materialnya
  sesudah didorong adalah kejutan yang menyalahkan operasi dorongnya.

**`WhiteboxMesh` memediasi setiap perubahan, dan itu ditemukan lewat uji yang
gagal.** Percobaan pertama mengekspos mesh dan pengelompokannya sebagai referensi
yang bisa diubah, sehingga topologi berubah di belakang punggung pemiliknya dan
daftar materialnya menjadi basi. Sekarang keduanya hanya bisa dibaca, dan
ekstrusi, geser, sembunyikan-rusuk, serta gabung-sebidang semuanya lewat kelas
itu — yang memindahkan materialnya mengikuti poligon sesudahnya.

Simpul **tidak dibagi antar sisi**: whitebox digambar rata, dan dua sisi yang
bertemu di sebuah rusuk punya normal berbeda. Simpul yang dibagi hanya bisa
membawa satu di antaranya, dan hasilnya rusuk yang membulat — yang justru
menghapus tampilan blockout.

Ruas kosong tidak diterbitkan: ia satu panggilan gambar yang tidak menggambar apa
pun, dan satu slot material yang menyesatkan.

### W4 — Aset `.simwhitebox` dan komponen · ✅

Topologi tersimpan, `WhiteboxComponent` lewat refleksi.

**Kriteria terima**
- Simpan-muat-simpan menghasilkan byte yang sama, aturan yang sama dengan E3.
  Berkas yang isinya bergeser tanpa ada yang menyuntingnya menghasilkan diff
  palsu di kontrol versi, dan diff palsu membuat yang sungguhan tidak terbaca.
- Yang dimuat bisa disunting lagi: ekstrusi sesudah muat ulang menghasilkan
  **berkas yang sama persis** dengan ekstrusi yang sama sebelum disimpan — bukan
  sekadar "berhasil dijalankan".
- Berkas yang rusak ditolak beserta sebabnya, bukan "gagal memuat".

**Rusuk tersembunyi disimpan sebagai pasangan simpul, bukan nomor rusuk.** Nomor
rusuk lahir dari urutan pembangunan, dan berkas yang isinya bergantung pada
urutan pembangunan akan rusak diam-diam begitu pembangunannya diperbaiki.

**Material disimpan per face**, sehingga bentuk berkasnya tidak perlu tahu apa
itu poligon sama sekali — pengelompokan dipulihkan dari rusuk tersembunyi, dan
materialnya dibaca dari face perwakilannya.

`WhiteboxComponent` terpisah dari `MeshRendererComponent` dan keduanya dipakai
bersama: yang satu menyimpan rujukan ke topologi yang bisa disunting, yang lain
menggambar segitiga hasilnya beserta material per ruasnya. Menyatukannya berarti
setiap mesh impor ikut membawa medan whitebox yang tidak pernah dipakainya.

### W5 — Penyuntingan di viewport · ✅

Pilih sisi, dorong dengan gizmo, tetapkan material.

**Kriteria terima**
- Memilih sisi memilih **poligon**, bukan segitiga di bawah kursor. ✅

  Diuji dengan dua sinar yang mengenai **dua segitiga berbeda** pada sisi atas
  kubus tersegitigakan: face-nya berlainan, poligonnya sama. Sisi belakang ikut
  bisa diklik — perancang kerap bekerja dari dalam ruangan yang baru dibuatnya,
  dan dinding yang tidak bisa dipilih dari dalam berarti dinding yang tidak bisa
  dipindahkan tanpa memutar kamera keluar.
- Satu seretan gizmo menghasilkan **satu** entri undo, aturan yang sama dengan
  seretan transform yang sudah ada. ✅

  Diuji dengan empat puluh frame seretan: satu entri, dan membatalkannya sekali
  mengembalikan berkas yang byte-nya sama persis dengan sebelum seretan.
- Menetapkan material ke sisi terpilih terlihat seketika, tanpa memuat ulang
  aset. ✅ untuk perintahnya — penetapan bisa dibatalkan, dan **sisi berbeda
  tidak digabung** karena berpindah sisi adalah keputusan baru pengguna.

- Sorotan sisi menggambar **batas poligon**, bukan batas face. ✅

  Diuji pada kubus tersegitigakan yang sudah digabung: dua segitiga isi, tetapi
  empat rusuk batas — diagonalnya tidak muncul. Membalik uji "seberangnya
  poligon yang sama" membuat delapan pernyataan gagal, jadi uji ini benar-benar
  memegang aturannya alih-alih kebetulan lolos.
- Gizmo berdiri di **titik berat berbobot luas** sisi itu. ✅

  Diuji pada persegi panjang 4×1 yang terbelah tidak rata: rata-rata simpulnya
  menjawab x = 5/3, titik beratnya x = 2. Mengganti pembobotannya dengan
  rata-rata biasa membuat uji ini gagal.

**Jalan masuknya.** Asset Browser membuat `.simwhitebox` baru lewat "New
whitebox" — berisi kubus satuan, bukan berkas kosong: blockout dimulai dengan
mendorong sisi, dan tidak ada sisi yang bisa didorong pada mesh tanpa isi.
Menjatuhkannya ke viewport membuat entity yang membawanya.

Entity berwhitebox tergambar tanpa `MeshRenderer`. Sebelumnya keduanya wajib —
whitebox untuk bentuk, `MeshRenderer` untuk material — sehingga blok yang baru
dijatuhkan tidak terlihat sama sekali sampai seseorang menebak bahwa ia butuh
komponen kedua yang tidak menunjuk mesh apa pun. Yang tidak terlihat juga tidak
bisa diklik, jadi tidak ada jalan memperbaikinya dari viewport. Diuji lewat
`SceneView`: satu entity berwhitebox menghasilkan satu pickable.

**Bentuk alatnya.** Tombol "Edit sides" (B) muncul di bilah viewport hanya
ketika entity terpilih membawa whitebox — tombol yang selalu ada tetapi hampir
selalu tidak melakukan apa-apa mengajari orang bahwa menekannya percuma. Selama
mode itu menyala, klik memilih sisi dan gizmo berpindah dari entity ke sisinya.

Sumbu gizmonya **mengikuti sisi, bukan dunia**: sumbu ketiganya menghadap keluar,
sehingga mendorong sisi selalu memakai pegangan yang sama betapapun bloknya
diputar. Menahan **Shift** saat seretan dimulai menumbuhkan sisi baru
(ekstrusi); tanpanya sisi yang ada bergeser. Shift dibaca sekali di awal — yang
dibaca tiap frame berarti arti sebuah gerakan bisa berubah di tengah jalan.

Tiap frame seretan **membangun ulang dari keadaan awal seretan**, bukan dari
frame sebelumnya. Menumpuknya menghasilkan satu lapis dinding per frame:
seretan sepanjang satu meter meninggalkan puluhan dinding tersembunyi di dalam
bloknya, dan tak satu pun terlihat sampai bloknya dipotong.

Seleksi sisi disimpan di `WhiteboxStore`, bukan di panel maupun di dalam
meshnya. Panel memilih lewat daftar dan viewport memilih lewat sinar; dua
salinan berarti dua sorotan berbeda, dan yang digerakkan gizmo bukan yang
tersorot di daftar. Di dalam mesh pun salah — seleksi bukan bagian bentuk, dan
akan ikut tersimpan ke berkas.

Perintahnya menyebut sasaran lewat **store dan guid**, bukan lewat pointer mesh.
Bukan kerapian: store yang memegang penanda versi, dan versi itulah yang membuat
viewport mengunggah ulang geometrinya. Perintah yang memegang mesh langsung
membatalkan bentuknya dengan benar sambil meninggalkan layar menggambarkan
bentuk yang sudah tidak ada — bug yang tampak seperti "undo tidak bekerja"
padahal datanya sudah benar. Diuji: `Undo()` menaikkan versi store.

Cuplikan undo-nya **utuh, bukan tambalan**: operasi whitebox membangun ulang
meshnya (keputusan W2), jadi tidak ada tambalan kecil yang bisa disimpan. Mesh
blockout berukuran puluhan sampai ratusan sisi — cuplikannya beberapa kilobyte,
dan ia persis terbalikkan. `MemoryCost` melaporkan ukuran sebenarnya supaya batas
memori riwayat tetap berarti.

Penetapan material dipisahkan dari perubahan bentuk: menyimpan cuplikan mesh utuh
untuk mengganti satu bilangan bulat berarti riwayat yang penuh oleh salinan
geometri yang tidak berubah.

### W6 — Collider dan ekspor · ✅

Whitebox yang bisa ditabrak, dan jalur keluar menuju mesh sungguhan.

**Kriteria terima**
- Whitebox menghasilkan collider yang cocok dengan yang digambar. ✅

  `ColliderShape::Whitebox` mengambil bentuknya dari komponen Whitebox entity
  itu. Diuji: kubus satuan berskala 4 × 3 × 4 menahan bola berjari-jari 0,25
  pada y = 1,75, dan bola kedua di x = 1,5 ikut tertahan — dua angka yang
  keduanya salah bila skalanya diabaikan. Mengabaikan skalanya menggugurkan uji
  ini.
- Blockout bisa diekspor menjadi aset mesh biasa. ✅

  "Export as mesh" di panel Whitebox menulis `.obj` beserta `.mtl` di sebelah
  asetnya. Diuji dengan memuatnya kembali lewat `LoadMesh` mesin ini sendiri:
  bentuknya sampai, dan pembagian materialnya ikut sampai sebagai dua ruas
  alih-alih runtuh menjadi satu.

**Bukan convex decomposition, dan itu keputusan.** Kriteria semula menyebut
"dipecah menjadi beberapa convex bila cekung". Yang dikerjakan bukan itu:

- **Statis dan kinematik memakai segitiganya apa adanya** (`PxTriangleMesh`).
  Untuk blockout — yang hampir seluruhnya geometri level yang diam — ini bukan
  hampiran melainkan **persis**, dan cekungan sedalam apa pun terjaga. Convex
  decomposition justru akan menukar bentuk yang persis dengan hampiran.
- **Dinamis memakai selubung cembungnya**, karena PhysX menolak mesh segitiga
  yang bergerak: mesh segitiga tidak punya bagian dalam, jadi tidak ada yang
  bisa menjawab "seberapa dalam benda ini menembus" — pertanyaan yang harus
  dijawab setiap kontak dinamis. Blok cekung yang dinamis **dilaporkan**, bukan
  didiamkan: lekukannya terisi, dan yang tidak diberitahu akan mengira
  solvernya rusak.

Yang tersisa bagi blok cekung yang harus bergerak adalah memecahnya menjadi
beberapa entity — atau mengekspornya, yang kini ada jalannya. V-HACD bisa
menyusul kalau kebutuhannya muncul; menambahkannya sekarang berarti satu
dependensi lagi untuk kasus yang belum pernah diminta.

**Uji yang memisahkan kedua pilihan itu.** Seluruh uji lain memakai bentuk
cembung, yang selubungnya sama persis dengan segitiganya — jadi tak satu pun
membedakan `TriangleMesh` dari `ConvexHull`. Yang membedakannya sebuah palung:
lantai di y = 1 dengan bibir di y = 2. Bola jatuh ke dasarnya (1,25) dengan
segitiga, dan tertahan di bibirnya (2,25) dengan selubung. Memaksa yang statik
memakai selubung menggugurkan uji itu.

**Fisika tidak membuka berkas.** Bentuknya dipasok lewat `ColliderGeometrySource`
— sebuah callback yang menjawab "bentuk entity ini apa?". Editor memasangnya di
atas `WhiteboxStore`; pemuat level memasang miliknya sendiri. Menariknya ke
dalam `Sim::Physics` berarti modul itu ikut bergantung pada `Sim::Assets`,
`Sim::Whitebox`, dan setiap format yang menyusul — dan `SimHeadless` ikut
membayarnya.

Bentuk yang tidak bisa diambil **mundur ke kotak**, bukan menghilangkan
bendanya: benda yang dilewatkan simulasi terlihat sebagai benda yang jatuh
menembus lantai — gejala yang mengarahkan orang mencari bug solver — sementara
kotak yang salah ukuran terlihat sebagai kotak yang salah ukuran. `Stats`
menghitungnya, dan notifikasi menyebutkannya.

Bentuk tabrakan dibangun **terpisah dari mesh yang digambar**. Yang digambar
dipecah per material supaya renderer mengganti material sekali per ruas, dan
pemecahan itu menggandakan simpul di setiap batas ruas — kubus menjadi 24 simpul
alih-alih 8. Solver tidak peduli material sama sekali.

Ekspornya memakai OBJ karena tiga alasan sekaligus: mesin ini sudah bisa
membacanya kembali, setiap DCC bisa membukanya, dan ia teks — yang berarti
hasilnya bisa diperiksa mata dan diuji tanpa memuat pustaka apa pun. `.mtl`-nya
bukan hiasan: pembaca OBJ mengelompokkan segitiga menurut material yang
**terdaftar**, jadi berkas tanpanya kembali sebagai satu ruas dan enam sisi
bermaterial berbeda menjadi satu. Angkanya ditulis dengan `%.6g`, bukan
`std::to_string`: yang kedua menghormati locale, dan locale berkoma menghasilkan
`v 0,5 0,5 0,5` — berkas yang tampak wajar dan ditolak setiap pembaca OBJ di
dunia, dengan kegagalan yang bergantung pada mesin yang mengekspor sehingga tak
pernah muncul di mesin yang mengujinya.

---

### W7 — Penyuntingan sub-objek: simpul, rusuk, sisi · ✅

W5 mengirim seleksi **poligon** dan tidak lebih. Yang belum ada sama sekali:
melihat simpul dan rusuknya, memilihnya, dan memindahkannya. Untuk blockout itu
batas yang terasa cepat — sebuah ruangan yang sudah berdiri hampir selalu perlu
satu sudutnya digeser, bukan seluruh sisinya.

**Lapisan mesh sudah menyediakan topologinya.** `EdgeCount`, `GetEdge`,
`EdgeHalfEdges`, `EdgeFaces`, dan `VertexOutgoing` semuanya sudah ada sejak W0;
yang tidak ada adalah operasi yang menyuntingnya dan cara melihatnya. Jadi W7
tidak menyentuh `HalfEdgeMesh` sama sekali.

#### W7.1 — Operasi simpul dan rusuk · ✅

`TranslateVertices`, `AlignVertices`, `SlideVertexAlongEdge`, dan `SplitEdge`,
semuanya mengikuti pola `Operations.cpp` yang sudah ada: ekstrak face, sunting,
`Rebuild`, `RegroupPolygons`. **Nomor face dipertahankan**, jadi seleksi poligon
bertahan melewati operasi simpul — aturan yang sama yang membuat ekstrusi tidak
menghilangkan sorotan.

**Kriteria terima**
- Menggeser satu simpul menggeser **setiap** face yang menyentuhnya, dan
  invarian half-edge tetap lulus sesudahnya.
- Meratakan simpul pada satu sumbu membuat koordinat sumbu itu sama persis untuk
  seluruh simpul terpilih; dua mode lain — ke minimum dan ke maksimum — diuji
  terpisah, karena "rata" yang berarti rata-rata dan "rata" yang berarti sejajar
  dinding adalah dua permintaan berbeda.
- Menggeser simpul sepanjang rusuk menaruhnya **tepat di garis rusuk itu** untuk
  `t` berapa pun, dan menjepit di kedua ujungnya alih-alih melewatinya.
- Memecah rusuk menambah **satu** simpul dan menaikkan derajat kedua face yang
  bertetangga padanya satu-satu; poligon yang memuat keduanya tidak pecah.

**Penyisipan rusuk dipilih bentuknya: `ConnectEdges`, bukan pemotong loop.**
Yang diminta adalah menghubungkan rusuk-rusuk yang benar-benar ditunjuk, berhenti
di situ — bukan merambat sendiri menelusuri strip quad sampai mentok. Untuk
blockout itu yang benar: dinding yang diam-diam ikut terbelah sampai ujung
ruangan adalah operasi yang harus dibatalkan, bukan disyukuri. Face yang
menyentuh lebih dari dua rusuk terpilih **ditolak seluruhnya**, karena
menghubungkan empat rusuk pada satu quad tidak punya satu jawaban; kisi dibuat
dengan memanggilnya berulang.

**Hasil.** Kelima operasi ada di `Operations.h` beserta pembungkusnya di
`WhiteboxMesh` — dan pembungkus itu bukan kerapian: ia yang memanggil
`RemapMaterials`, dan memanggil fungsi bebasnya langsung akan membuat material
per-sisi berpindah sendiri sesudah sebuah simpul digeser. Tiga belas test case baru, 51/51 lulus di `SimWhiteboxTests` — termasuk alur
yang menjadi alasan operasi ini ada: belah sebuah quad, kedua belahannya menjadi
poligon terpisah, lalu salah satunya diekstrusi sendiri.

Yang ikut terbukti dan tidak diminta: menggeser simpul kubus tersegitigakan
mempertahankan keenam poligon hasil penggabungan sebidang, karena `Rebuild`
mempertahankan nomor face dan `RegroupPolygons` memulihkan pengelompokannya.

#### W7.2 — Melihat dan memilih sub-objek · ✅

Mode sub-objek — simpul / rusuk / sisi — beserta gambarannya di viewport: titik
untuk simpul, garis untuk rusuk, batas poligon untuk sisi (yang terakhir sudah
ada sejak W5). Seleksi menjadi **himpunan**, bukan satu handle: `SideSelection`
hari ini memuat satu `PolygonHandle`, dan setiap operasi di W7.3 menuntut lebih
dari satu.

Pemilihannya **di ruang layar, bukan dengan sinar**. Sebuah simpul tidak punya
luas untuk ditembak dan sebuah rusuk hanya setebal nol; yang menentukan mana yang
terpilih adalah jaraknya dari kursor dalam piksel — aturan yang sama dengan ikon
entity, yang sudah menang atas geometri di belakangnya karena alasan itu.

**Kriteria terima**
- Simpul di belakang geometri tidak ikut terpilih saat mode tembus-pandang mati.
- Kotak seleksi mengambil seluruh sub-objek di dalamnya, bukan yang terdekat.
- Berpindah mode **tidak** mengosongkan bentuk yang sedang disunting, dan
  seleksi lama yang tidak berlaku di mode baru dibuang alih-alih ditafsirkan
  ulang menjadi sub-objek yang tidak pernah ditunjuk siapa pun.

#### W7.3 — Operasi yang muncul menurut yang terpilih · ✅

Gizmo memindahkan apa pun yang terpilih. Di samping itu, opsi yang hanya masuk
akal untuk jenisnya: perataan dan geser-sepanjang-rusuk untuk simpul, ekstrusi
untuk sisi (sudah ada, tinggal dipanggil dari seleksi baru), dan penyisipan
rusuk untuk rusuk.

**Satu seretan tetap satu entri undo**, aturan yang sudah dipegang W5.

**Kriteria terima**
- Satu seretan gizmo atas lima simpul menghasilkan satu entri undo, dan
  membatalkannya mengembalikan berkas yang byte-nya sama persis.
- Opsi yang tidak berlaku untuk jenis yang terpilih tidak ditampilkan
  setengah-aktif — ia tidak ada.

**Hasil W7.2 & W7.3.**

- Simpul dan rusuk digambar di ruang layar lewat `ImDrawList`, sehingga
  penandanya tetap seukuran itu berapa pun jarak kamera. Rusuk yang disembunyikan
  penggabungan sebidang tidak digambar dan tidak bisa dipilih.
- Pemilihan memakai jarak piksel, bukan sinar. Jangkauan kliknya sengaja lebih
  besar daripada penandanya.
- Gizmo sub-objek berdiri di **rata-rata simpul** dan memakai sumbu **dunia** —
  sekumpulan simpul tidak punya luas untuk dibobot maupun normal untuk memberi
  sumbu ketiga arti, dan mengarang keduanya lebih sulit dipakai daripada sumbu
  yang jujur.
- Perataan dan penyisipan rusuk tinggal di panel Whitebox, muncul hanya untuk
  jenis yang terpilih. Keduanya menolak sebelum tombolnya bisa ditekan bila
  seleksinya kurang.
- Enam test case baru di `SimLevelEditorTests` (61/61): ketiga jenis tersimpan
  berdampingan, toggle, berpindah aset mengosongkan, **mode bertahan melewati
  pengosongan**, empat puluh frame seretan simpul menjadi satu entri undo, dan
  operasi yang mengubah topologi tetap satu entri yang membatalkannya utuh.

**Satu cacat ditangkap uji regresinya sendiri:** `ClearSelection()` semula
melakukan `selected_ = {}`, yang ikut mengembalikan mode ke `Face`. Artinya satu
klik meleset saat menyunting simpul melempar perancang kembali ke mode sisi, dan
klik berikutnya mengenai jenis yang salah. Mode sekarang dipertahankan.

**Yang belum terverifikasi dengan mata:** gambar dan pemilihannya. Mode blockout
hanya menyala lewat tombol B atau tombol overlay, dan tidak ada jalan otomatis
menekannya — jadi titik, garis, dan gizmo sub-objek belum pernah dilihat berjalan.
Yang sudah pasti: seluruhnya kompilasi dengan `-Werror`, model seleksinya benar
lewat uji, dan editor berjalan tanpa assert dengan level whitebox terbuka.

---

## Risiko

**Kasus tepi topologi.** Ini alasan utama opsi 1 masih layak dipertimbangkan.
Mitigasinya bukan kehati-hatian melainkan pemeriksa invarian yang dijalankan
setiap uji.

**Cekung dan collider.** Whitebox yang dicekungkan tidak bisa menjadi satu convex
hull. W6 memecahnya, dan kalau pemecahan itu meleset, jalur mundurnya adalah
triangle mesh statis — sah untuk level, tidak untuk benda bergerak.

**Godaan menjadikannya modeler.** Whitebox berguna justru karena terbatas. Setiap
operasi yang ditambahkan di luar daftar W2 harus membenarkan dirinya terhadap
"kembali ke DCC" — dan kebanyakan tidak bisa.

---

## Yang sengaja tidak dikerjakan

**CSG (boolean).** O3DE memakainya lewat Manifold. Ia berguna, tetapi ia
pekerjaan tersendiri dengan kasus tepinya sendiri, dan blockout bisa berjalan
jauh tanpanya.

**UV mapping yang bisa disunting.** Whitebox memakai proyeksi planar per poligon —
cukup untuk melihat skala tekstur benar saat merancang. UV yang digarap adalah
pekerjaan DCC, dan W6 menyediakan jalan keluarnya.
