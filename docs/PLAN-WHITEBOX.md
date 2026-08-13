# Plan Whitebox (W0 → W6)

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

### W5 — Penyuntingan di viewport · ⬜

Pilih sisi, dorong dengan gizmo, tetapkan material.

**Kriteria terima**
- Memilih sisi memilih **poligon**, bukan segitiga di bawah kursor.
- Satu seretan gizmo menghasilkan **satu** entri undo, aturan yang sama dengan
  seretan transform yang sudah ada.
- Menetapkan material ke sisi terpilih terlihat seketika, tanpa memuat ulang
  aset.

### W6 — Collider dan ekspor · ⬜

Whitebox yang bisa ditabrak, dan jalur keluar menuju mesh sungguhan.

**Kriteria terima**
- Whitebox menghasilkan collider yang cocok dengan yang digambar — memakai
  convex hull yang sudah ada dari `ShapeKind::Cylinder`, atau dipecah menjadi
  beberapa convex bila cekung.
- Blockout bisa diekspor menjadi aset mesh biasa, sehingga ia titik awal yang
  bisa ditinggalkan — bukan format yang mengurung pekerjaan di dalamnya.

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
