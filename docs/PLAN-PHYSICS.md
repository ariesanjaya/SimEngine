# Plan Fisika PhysX 5 (P0 → P8)

Menjadikan PhysX 5 tulang punggung simulasi E9: rigid body, scene query, joint,
articulation, dan kendaraan — semuanya di CPU lebih dulu, dengan CUDA sebagai
lapisan tambahan yang bisa dimatikan.

Penomoran **P** supaya tidak bertabrakan dengan E (editor/render), A (agentic
AI), C (kain), R (Embree), I (gambar), dan M (GI) di [ROADMAP.md](ROADMAP.md).

---

## Yang harus diketahui sebelum apa pun direncanakan

**Tiga dari sepuluh fitur di daftar PhysX tidak punya jalur CPU sama sekali.**
Ini bukan pendapat tentang performa; ia terbaca di tanda tangan API-nya:

```cpp
createPBDParticleSystem(PxCudaContextManager& ...)   // referensi, bukan pointer
createDeformableVolume  (PxCudaContextManager&)      // soft body FEM
createDeformableSurface (PxCudaContextManager&)      // kain deformable
```

Sebuah `&` tidak bisa bernilai null. Ketiganya **menuntut** CUDA — bukan
memakainya bila ada. `README_LINUX.md` menegaskan sisi sebaliknya: *"CUDA
Toolkit 12.8 (Not required for CPU only builds)"* — build CPU-only memang ada,
ia hanya tidak memuat fitur-fitur itu.

| Fitur | Mode CPU | Tinggal di mana |
| --- | --- | --- |
| Rigid Body Dynamics | ✅ | PhysX core |
| Scene Queries | ✅ | PhysX core |
| Joints | ✅ | PhysX core |
| Custom Geometries | ✅ | PhysX core |
| Reduced Coordinate Articulations | ✅ | PhysX core |
| Vehicle Dynamics | ✅ | PhysX core (di atas rigid body) |
| Character Controller | ✅ | PhysX core (ekstensi) |
| Fracture — **Blast** | ✅ | **SDK terpisah** (`/blast`) |
| PBD (liquid/cloth/inflatable) | ❌ **CUDA wajib** | PhysX core, solver GPU |
| Soft Body FEM | ❌ **CUDA wajib** | PhysX core, solver GPU |
| Smoke & Fire — **Flow** | ❌ **GPU** | **SDK terpisah** (`/flow`) |

**Dan CUDA bukan sekadar "belum dipasang" bagi sebagian target.**
[rencana-implementasi-gi.md](rencana-implementasi-gi.md) menyebut **RDNA1**
sebagai baseline — itu AMD, dan tidak ada versi driver yang membuatnya
menjalankan CUDA. Di mesin itu ketiga fitur tersebut tidak pernah ada, bukan
"mati sementara". Jadi mereka tidak bisa direncanakan sebagai sakelar yang
sebagian orang nyalakan; mereka fitur yang **hilang untuk satu kelas perangkat
keras**, dan setiap permainan yang bergantung padanya kehilangan kelas itu.

Konsekuensi yang menentukan seluruh urutan di bawah: **P0–P7 adalah rencana yang
sesungguhnya**, dan P8 adalah lapisan yang bagusnya-ada.

---

## Keputusan pokok

**PhysX opsional, dicari, tidak dibangun.** Pola yang sudah dipakai OpenUSD,
OpenImageIO, dan OpenVDB di `cmake/SimDeps.cmake`. Membangunnya dari nol
memakan puluhan menit; menjadikannya syarat berarti setiap orang yang hanya
ingin mengubah satu panel editor membayar itu pada build bersih pertamanya.
Yang melewatinya tetap bisa membangun seluruh mesin — yang hilang adalah
simulasi, dan entity berfisika jatuh ke perilaku statis yang **dikatakan di
log**, bukan diam-diam.

**Sakelar CPU/GPU-nya satu flag, di sisi PhysX.** Preset
`linux-clang-cpu-only` sudah ada dan memakai toolchain yang sama dengan proyek
ini; satu-satunya beda dengan preset penuh adalah
`PX_GENERATE_GPU_PROJECTS=True`. Jadi `SIM_WITH_PHYSX_GPU` tidak menambah jalur
kode kedua — ia memilih pemasangan PhysX yang mana yang ditautkan, dan menyalakan
fitur yang memang cuma ada di sana.

**Tipe PhysX tidak pernah muncul di header publik `Sim::Physics`.** Aturan yang
sama yang menjaga OpenImageIO dan OpenVDB di luar jalur runtime, dan yang sudah
terbukti tiga kali: backend gambar berganti pustaka dua kali tanpa satu pun
titik panggil berubah. Yang menyeberang batas modul adalah `Vec3`, `Quat`, dan
handle — bukan `PxRigidActor*`.

**Simulasi berlangkah tetap, terpisah dari frame render.** Fisika yang berlangkah
mengikuti waktu frame menghasilkan simulasi yang berbeda di mesin yang lebih
cepat — dan "menara balok saya runtuh di laptop tapi tidak di desktop" adalah
laporan bug yang tidak bisa ditindaklanjuti. Render menginterpolasi di antara dua
langkah; itu yang membuat 60 Hz fisika terlihat mulus di 144 Hz layar.

**Determinisme yang dijanjikan dibatasi dan ditulis.** PhysX CPU deterministik
untuk masukan yang sama pada build, platform, dan jumlah thread yang sama. Ia
**tidak** deterministik lintas-platform, dan tidak lintas jumlah thread. Janji
yang lebih besar dari itu akan runtuh tepat ketika seseorang mengandalkannya
untuk replay atau lockstep multiplayer.

---

## Dua tabrakan dengan rencana yang sudah ada

Keduanya nyata, dan keduanya harus diputuskan — bukan dibiarkan menumpuk sampai
ada dua sistem yang mengerjakan satu pekerjaan.

**1. Kain: PBD PhysX melawan `Sim::Cloth`.**
[PLAN-CLOTH.md](PLAN-CLOTH.md) sudah merencanakan solver kain GPU dari XRTailor
sebagai modul `Sim::Cloth`, lengkap dengan pengukuran di RTX 2060. PBD PhysX juga
mensimulasikan kain, juga di GPU, juga hanya dengan CUDA. **Dua solver kain
adalah dua tempat yang harus dipelihara** untuk satu fitur.

**Sudah dianalisa: [ANALISA-KAIN.md](ANALISA-KAIN.md).** Hasilnya membalik premis
pertanyaannya — PBD kain **sudah usang** di PhysX 5.6.1 yang kita pakai, jadi ia
gugur sebelum dibandingkan. Perbandingan yang tersisa adalah
`PxDeformableSurface` melawan XRTailor, dan keduanya sama-sama menuntut CUDA
sehingga sama-sama absen di RX 5600 XT — separuh baseline yang tertulis di
rencana GI. Rekomendasinya: ukur `PxDeformableSurface` di P8 sebelum
mengekstraksi 17 ribu baris XRTailor.

**Diputuskan sesudah P7, bukan sekarang.** Alasannya bertahan diperiksa: tidak
satu pun milestone CPU menyentuh kain, dan kain PBD hidup di P8 yang menuntut
CUDA — jadi menunda keputusannya tidak menghalangi apa pun dan tidak membuat
siapa pun mengerjakan yang salah. Yang berbahaya adalah menundanya sampai
**sesudah** salah satunya dibangun; sampai P7 selesai, itu tidak mungkin terjadi.

Bahan untuk keputusan itu sudah lengkap sekarang, dan dicatat di sini supaya
tidak perlu digali ulang: rencana C sudah punya angka terukur di RTX 2060, jalur
bake Alembic, dan rancangan untuk bereaksi terhadap karakter ber-skin. Keduanya
menuntut CUDA, jadi keduanya sama-sama absen di RDNA1. Yang tersisa dari PBD —
cairan, inflatable, shape matching — tidak dikerjakan rencana mana pun, jadi
tidak ikut bertabrakan apa pun yang diputuskan.

**2. Scene query: PhysX melawan Embree.**
[PLAN-EMBREE.md](PLAN-EMBREE.md) R2 merencanakan picking presisi segitiga lewat
Embree, R3 merencanakan query authoring. PhysX juga menyediakan raycast, sweep,
dan overlap.

Usulan: **keduanya ada, dan batasnya jelas.** Embree melayani *authoring dan
offline* — picking di editor, path tracer acuan, bake. Ia bekerja pada geometri
render yang sebenarnya, termasuk mesh yang tidak punya collider sama sekali.
PhysX melayani *gameplay runtime* — apa yang dilihat peluru, apa yang dipijak
karakter. Ia bekerja pada bentuk tabrakan, yang memang sengaja lebih sederhana
daripada mesh yang digambar. Menyatukan keduanya berarti picking editor
berhenti bekerja untuk objek tanpa collider, atau peluru menabrak segitiga
dekoratif.

---

## Arsitektur

```
Code/Physics/
  include/Sim/Physics/
      PhysicsTypes.h      handle, deskriptor bentuk, hasil query — tanpa tipe PhysX
      PhysicsWorld.h      lifecycle scene, langkah tetap, sinkronisasi transform
      PhysicsQuery.h      raycast/sweep/overlap untuk gameplay
  src/
      PhysicsWorld.cpp    satu-satunya TU yang melihat PxPhysics
      ShapeCook.cpp       cooking convex & triangle mesh dari MeshData
      Backend*.cpp        dikompilasi hanya bila SIM_WITH_PHYSX
```

**`Sim::Physics` tidak bergantung pada `Sim::Render`.** Ia menghasilkan
transform; yang menggambarnya adalah pemanggilnya. Aturan yang sama menjaga
`SimHeadless` — dan karena itu server dedicated — tetap bisa mensimulasikan
tanpa satu pun perangkat grafis.

**Komponen scene**, mengikuti pola `LightComponent` dan `SkyComponent` yang sudah
ada: `RigidBodyComponent` (statik/kinematik/dinamis, massa, damping),
`ColliderComponent` (box/sphere/capsule/convex/mesh + material), dan nanti
`JointComponent`, `VehicleComponent`, `CharacterControllerComponent`. Semuanya
lewat refleksi, jadi Inspector menyuntingnya tanpa kode panel tambahan.

**Cooking adalah pengondisian aset, bukan pekerjaan runtime.** Convex hull dan
triangle mesh PhysX dimasak sekali dan disimpan di samping asetnya — memasaknya
saat level dimuat menambah detik ke waktu muat untuk hasil yang selalu sama.
Aturan yang sama dengan bake SDF di [PLAN-IMAGEIO.md](PLAN-IMAGEIO.md) dan
`Sim::Volume`.

### Model thread

Simulasi berjalan di `TaskPool` lewat `PxCpuDispatcher`, tapi `fetchResults` dan
penulisan transform ke `World` terjadi di main thread. Alasannya sama dengan
seluruh editor: `World` tidak dilindungi mutex, dan menambahkannya sekarang
berarti membayar penguncian di setiap pembacaan komponen demi satu penulis.

---

## Milestone

### P0 — Build CPU-only, dependensi opsional, kerangka modul · ✅

PhysX dibangun dengan preset `linux-clang-cpu-only`, hasilnya disalin ke
`Third-Party/PhysX/` seperti OpenVDB. `SIM_WITH_PHYSX` mendeteksinya dua sisi —
header **dan** pustaka — karena header tanpa pustaka kompilasi dengan sukses lalu
gagal saat link. Modul `Sim::Physics` berdiri dengan `PhysicsWorld` yang bisa
dibuat dan dihancurkan, belum mensimulasikan apa pun.

**Kriteria terima**
- Build tanpa PhysX sukses dan seluruh test lulus; `PhysicsWorld` melaporkan
  dirinya tidak tersedia dengan pesan yang menyebut apa yang kurang.
- Build dengan PhysX sukses, versi SDK tercatat di log startup.
- Tidak ada satu pun tipe `Px*` di header publik — diuji dengan menyisir
  `Code/`, seperti uji `stbi_` di `SimImageIOTests`.

### P1 — Rigid Body Dynamics · ✅

Komponen rigid body dan collider, langkah tetap, sinkronisasi transform dua arah.
Bentuk primitif lebih dulu: box, sphere, capsule, plane.

Jembatannya, `physics::PhysicsScene`, tinggal di `Sim::Physics` dan bukan di
`Sim::EditorFramework`. Ia terlihat seperti kode editor — membaca komponen,
menulis transform — tetapi `Sim::EditorFramework` menarik `Sim::Render`, dan
menaruhnya di sana berarti tidak ada yang bisa mensimulasikan tanpa perangkat
grafis. Arahnya juga disengaja: `Sim::Physics` yang melihat `Sim::Scene`, bukan
sebaliknya, supaya importir aset dan tool yang tidak pernah mensimulasikan apa
pun tidak ikut menautkan PhysX.

`PhysicsScene.cpp` tidak memuat satu pun `#if SIM_WITH_PHYSX`: ia hanya memakai
API publik `PhysicsWorld`, yang sudah menolak secara terbuka. Berkasnya karena
itu dikompilasi **dan diuji** sama persis di kedua build, sehingga jalur yang
hanya pernah dikompilasi di satu konfigurasi — kelas cacat yang paling sering
lolos dari dependensi opsional — tidak punya tempat bersembunyi.

**Play tidak lagi menuntut Lua.** Seluruh `EditorApp::Play` dulu dipagari
`#if SIM_WITH_LUA` dan langsung kembali bila runtime skrip null, jadi build tanpa
Lua tidak akan pernah menjalankan simulasinya — dua fitur opsional yang saling
mengunci tanpa alasan. Sekarang cuplikan level, fisika, dan skrip masing-masing
berdiri sendiri.

**Kriteria terima**
- Benda jatuh dan berhenti di lantai pada ketinggian yang **dihitung**, bukan
  yang "kelihatan benar" — bola berjari-jari r berhenti di y = r.
- Simulasi dengan seed dan urutan yang sama menghasilkan posisi yang sama
  bit-per-bit pada dua kali jalan di mesin yang sama.
- Langkah tetap terbukti tetap, dengan batas yang benar: rentang waktu yang sama
  yang dipecah menjadi frame berbeda-beda menghasilkan jumlah langkah yang
  berselisih **paling banyak satu**, selisih itu tidak menumpuk sepanjang
  simulasi, dan keadaan yang sudah mengendap sama persis.

  Kriteria ini semula ditulis sebagai "hasil yang sama" tanpa syarat, dan itu
  **tidak bisa dipenuhi siapa pun** — P0 mengukurnya: satu detik sebagai 20 frame
  @50 ms panjangnya 59,999998 langkah sementara sebagai 60 frame @16,67 ms tepat
  60, karena `3 × float(1/60)` benar-benar lebih besar daripada `float(0.05)`.
  Sebabnya masukan, bukan akumulator, jadi tidak ada implementasi yang
  menghilangkannya. Yang penting bagi pemain tetap terjaga: selisihnya terbatas,
  tidak tumbuh, dan hilang begitu adegannya diam.
- Entity kinematik digerakkan transform-nya, bukan gaya; entity statis tidak
  pernah dibangunkan.
- Entity berinduk ditulis balik di ruang induknya, bukan ruang dunia. Ditambahkan
  saat P1 dikerjakan karena ia cacat yang hanya muncul di level sungguhan — di
  sana entity fisika hampir selalu berada di dalam grup — sementara setiap uji
  yang menaruh benda di akar akan lulus tanpa menyentuhnya.
- Skala entity ikut menentukan ukuran bentuk tabrakan. Bentuk yang mengabaikannya
  tidak pernah tampak sebagai galat: bendanya digambar besar, berhenti seolah
  kecil, dan yang terlihat hanya benda yang tenggelam ke dalam lantai.

### P2 — Scene Queries · ✅

Raycast, sweep, dan overlap untuk gameplay, dengan filter layer.

#### Batasnya terhadap Embree

Ditulis di sini dan di `PhysicsQuery.h`, bukan hanya dipahami — keduanya
menembakkan ray, dan itu satu-satunya kemiripannya.

| | PhysX scene query | Embree ([PLAN-EMBREE.md](PLAN-EMBREE.md)) |
|---|---|---|
| Menjawab tentang | bentuk tabrakan, keadaan simulasi frame ini | setiap segitiga yang digambar |
| Umur jawabannya | kedaluwarsa satu langkah kemudian | tetap, selama geometrinya tidak berubah |
| Dipanggil | ratusan kali per frame | sekali, saat bake |

Memakai yang satu untuk pekerjaan yang lain **gagal secara diam-diam**, dan itu
yang membuat batas ini perlu tertulis: raycast fisika terhadap dunia yang penuh
hiasan tanpa collider menembusnya seolah tidak ada, sementara bake yang memakai
bentuk tabrakan membakar bayangan kotak untuk pohon.

**Kriteria terima**
- Raycast terhadap bentuk analitik menjawab jarak dan normal yang benar.
- Filter layer benar-benar menyaring: ray yang diberi mask kosong tidak pernah
  mengembalikan hit.
- Query dari beberapa thread `TaskPool` tidak merusak apa pun — PhysX
  mengizinkannya selama simulasi tidak sedang berjalan, dan batas itu ditegakkan
  dengan assert, bukan dengan harapan.

Penegakannya berupa bendera atomic yang menyala sepanjang `Step`, dan query yang
menabraknya dijawab penolakan beserta satu baris log alih-alih keadaan setengah
diperbarui. **Lebih ketat daripada yang dituntut PhysX** — sela di antara dua
langkah sebenarnya sah — karena selanya berdurasi mikrodetik sementara salah
menempatkan batasnya berakibat kerusakan senyap.

Diuji dengan thread pembaca yang menembak terus-menerus sementara main thread
melangkah 200 kali: 98 query tertolak, **nol dijawab salah**. Uji itu sengaja
tidak menuntut jumlah tertentu tertolak — yang bergantung pada penjadwalan bukan
uji — melainkan menuntut tidak ada jawaban yang salah, dan memeriksa dari main
thread bahwa penjagaannya terbuka lagi sesudah langkah terakhir.

### P3 — Joints · ✅

Fixed, revolute, prismatic, spherical, D6. `JointComponent` merujuk dua entity.

Rujukannya **GUID, bukan `Entity`**: indeks entity dipakai ulang setelah entity
dihapus, jadi sendi yang menyimpannya akan menunjuk benda yang salah begitu level
dimuat ulang. GUID kosong berarti dunia — pintu dan bandul memang tergantung pada
titik tetap di ruang, dan tanpa itu setiap adegan harus menyediakan benda statis
pura-pura untuk digantungi.

Sendinya **milik entity yang bergerak, bukan yang menahan**. Keduanya sama masuk
akal dilihat dari luar; yang terbalik membuat menghapus bandul meninggalkan poros
dengan sendi ke sesuatu yang tidak ada.

Sendi dibangun di **sapuan kedua**, sesudah seluruh benda ada. Membangunnya
bersamaan membuat sendi berhasil atau gagal tergantung urutan entity di berkas —
sesuatu yang tidak pernah dipikirkan orang saat menyusun levelnya.

**Kriteria terima**
- Bandul revolute berayun pada bidang yang benar dan tidak menyimpang keluar
  bidang itu setelah 10.000 langkah.

  Terukur **2,4 × 10⁻¹⁴ m** keluar bidang — derau `float`, bukan penyimpangan —
  dan lengannya meleset paling jauh 2,6 mm. Uji itu juga menuntut bandulnya
  benar-benar berayun: yang lulus karena bandulnya tidak pernah bergerak tidak
  menguji apa pun.
- Limit ditegakkan: sendi berlimit tidak pernah melewati batasnya.

  Diperiksa dua kali dengan cara yang berbeda — lewat sudut yang dilaporkan
  sendinya, dan lewat posisi bebannya, karena yang pertama saja hanya menguji
  sendi itu melaporkan dirinya sendiri secara konsisten.
- Joint yang salah satu ujungnya dihapus tidak meninggalkan dangling actor —
  diuji dengan menghapus entity, bukan dengan membaca kode.

  PhysX menuntut sendi dilepas **sebelum** benda yang dipegangnya; melepas aktor
  lebih dulu meninggalkan sendi yang menunjuk memori bebas, dan itu tidak crash
  di tempat kejadian melainkan pada langkah berikutnya yang kebetulan
  menyentuhnya. `RemoveBody` karena itu menyapu sendi yang memegang aktornya
  lebih dulu. Uji dibuktikan bisa gagal dengan melumpuhkan penyapuan itu.

### P4 — Custom Geometries · ✅ untuk heightfield terrain

**Dikerjakan sebagai [L5 di PLAN-TERRAIN.md](PLAN-TERRAIN.md)**, setelah terrain
punya bentuk untuk diuji. `ShapeKind::HeightField` memasak `PxHeightField` dari
sampel enam belas bit `Sim::Terrain` tanpa satu pun pembulatan, dan
`ColliderShape::Terrain` menyambungkannya lewat `ColliderGeometrySource` yang
sudah ada sejak W6 — sehingga `Sim::Physics` tetap tidak membuka berkas.

`PxCustomGeometry` sendiri **tidak** dipakai, dan tidak dibutuhkan: PhysX sudah
punya kisi tinggi bawaan, dan yang bawaan lebih cepat daripada callback ke kode
kita. Yang tersisa dari P4 adalah bentuk kustom yang benar-benar tidak ada di
daftar bawaan — dan sampai ada yang memintanya, itu tidak ada.

Catatan aslinya, disimpan karena alasannya masih berlaku:

**Ditunda sampai Terrain Editor dikerjakan.** Seluruh nilai P4 ada pada
heightfield yang membaca `Sim::Terrain` apa adanya, dan menyambungkan fisika
ke terrain yang belum bisa disunting berarti menguji integrasi terhadap data
yang belum berbentuk. P5 dikerjakan lebih dulu; keduanya tidak saling
bergantung.

`PxCustomGeometry` untuk bentuk yang tidak ada di daftar bawaan — yang paling
berguna di sini: heightfield terrain yang membaca `Sim::Terrain` apa adanya,
tanpa menyalin heightmap ke bentuk PhysX kedua.

**Kriteria terima**
- Benda menggelinding di atas terrain dan mengikuti ketinggian yang sama dengan
  yang dilaporkan `Terrain::HeightAtWorld`, dalam toleransi yang ditulis.
- Mengubah heightmap saat runtime terlihat oleh fisika tanpa membangun ulang
  seluruh scene.

### P5 — Reduced Coordinate Articulations · ✅

Rantai sendi untuk ragdoll dan robot. Dibangun **setelah** joint, karena
articulation adalah jawaban untuk kasus di mana rantai joint biasa terlalu
lembek.

Pembangun ragdoll menerima **daftar tulang generik**, bukan `animation::Skeleton`.
Membuat `Sim::Physics` melihat `Sim::Animation` berarti setiap yang menautkan
fisika ikut menautkan importir FBX dan USD di baliknya, dan server dedicated tidak
punya alasan membawa keduanya. Lintasan pengubahnya sepuluh baris dan dimiliki
pemanggil — di pohon ini, dijalankan oleh uji.

**Kriteria terima**
- Ragdoll dari rangka `.simskel` berdiri tanpa meledak pada langkah pertama.

  Terukur: link bergerak **2,72 mm** pada langkah pertama, yaitu tepat jarak
  jatuh bebas satu langkah 60 Hz (g·dt² = 2,725 mm). Bukan "cukup kecil"
  melainkan angka yang sudah diketahui sebelum simulasinya dijalankan.

  Diuji juga dengan rig Mixamo sungguhan (65 tulang → 25 link, 40 dilipat), yang
  menemukan dua cacat yang tidak muncul pada rangka karangan — lihat di bawah.
- Rantai 20 tautan tidak melar di bawah beban — inilah yang membedakannya dari
  rantai joint biasa, dan karena itu inilah yang diuji.

  Rantai 10 m yang digantungi 200 kg melar **0,00095 mm**. Rantai sendi biasa
  dengan beban dan bentuk yang sama, dibangun dengan cara paling lurus, melar
  **13.727 mm** — ia praktis terurai. Keduanya diuji berdampingan supaya angka
  yang pertama tidak terbaca sebagai sesuatu yang diberikan solver secara cuma-
  cuma.

#### Dua cacat yang hanya ditemukan rig sungguhan

**Massa turunan volume merusak articulation.** Kerapatan terdengar lebih fisis
daripada massa total, tetapi ia menyerahkan massa tiap bagian kepada bentuk yang
disusun otomatis — dan bentuk itu tidak tahu apa-apa tentang anatomi. Pada Y Bot,
tulang pinggul yang pendek menghasilkan kapsul berjari-jari 2,8 cm sementara paha
menghasilkan 10,2 cm, sehingga **akar tubuh menjadi 50 kali lebih ringan daripada
anaknya**. Hasilnya bukan goyangan melainkan koordinat NaN pada langkah ke-55.
Sekarang massa dibagikan dari `totalMass` menurut volume, lalu perbandingannya
dibatasi pada 0,5×–2,5× rata-rata.

**Tulang ujung tidak boleh mewarisi panjang induknya.** Aturan itu menyelamatkan
tangan dan kepala, tetapi ikut menyelamatkan tulang bantu sepanjang nol — yang
letaknya persis di induknya — sehingga ia mendapat kapsul kembar di ruang yang
sama. Dua bentuk yang saling menembus sejak langkah nol adalah bentuk paling umum
dari ragdoll yang meledak. Yang dipakai sekarang adalah jarak tulang itu sendiri
dari induknya, yang nol untuk tulang bantu dan karena itu melipatnya.

Tabrakan antar-link **mati secara bawaan**, dan itu bukan jalan pintas: bentuk
yang disusun otomatis pasti bertumpuk di percabangan — dua paha berjarak 20 cm
dengan jari-jari 10,5 cm sudah saling menembus sebelum langkah pertama.
Menyalakannya menuntut bentuk yang disetel tangan.

### P6 — Vehicle Dynamics · ✅

`PxVehicle` di atas rigid body. Roda, suspensi, mesin, transmisi.

**Dipecah menjadi enam sub-fase, dan pemecahannya sendiri adalah alat debug.**
P6 dikerjakan sekali sebagai satu bongkahan dan hasilnya adalah kendaraan yang
terbangun, tertaut, masuk scene — lalu diam saja diberi gas penuh. Gejala
sebesar itu tidak menunjuk apa pun: ia bisa berarti roda tidak menemukan tanah,
torsi tidak sampai ke roda, ban tidak menghasilkan gaya, atau gaya tidak sampai
ke bodi. Urutan di bawah menyusunnya menjadi rantai sebab-akibat yang tiap
matanya bisa dijawab benar atau salah sendiri-sendiri.

`vehicle2` adalah kerangka perakitan komponen — 56 header, dan referensi
direct-drive NVIDIA sendiri ~1100 baris — jadi ini memang milestone terbesar di
rencana ini.

#### P6a — Kendaraan berdiri di suspensinya · ✅

Tidak ada gas, tidak ada rem, tidak ada kemudi. Hanya: aktor terbentuk dengan
bentuk chassis dan roda, query jalan menemukan bidang di bawah tiap roda, dan
bodinya turun ke ketinggian diam lalu berhenti di sana.

**Kriteria terima**
- Keempat roda melaporkan `onGround` sesudah mengendap. **Inilah mata rantai
  pertama**, dan selama ia salah tidak ada gunanya menguji yang lain — ban yang
  tidak menyentuh apa pun tidak akan pernah menghasilkan gaya betapapun besar
  gasnya.
- Ketinggian diam bodinya **dihitung lebih dulu**, bukan dibaca dari hasil:
  pegas menahan massa tertopang pada tekanan x = mg/k, jadi bodinya duduk
  setinggi itu di bawah titik gantung suspensinya.
- Suspensi tidak berosilasi tak berhingga di atas permukaan datar — amplitudonya
  meluruh, dan pegas bawaan memang diberi redaman kritis c = 2·√(k·m) supaya ia
  kembali tanpa melewati sama sekali.

#### P6b — Gas menghasilkan gerak · ✅

Torsi direct-drive masuk ke roda penggerak, roda berputar, ban menghasilkan gaya
memanjang, bodi bergerak.

**Kriteria terima**
- Kecepatan naik secara monoton pada gas tetap di permukaan datar. Terukur: 20
  cuplikan, **nol** yang menurun.
- Kecepatan putar roda cocok dengan laju maju: ω·r ≈ v — **diperiksa saat
  meluncur bebas, bukan saat digas**.

  Di bawah gas penuh roda penggerak memang *harus* selip; itulah cara ban
  memindahkan gaya, dan direct drive memperparahnya karena torsinya tetap berapa
  pun laju rodanya — tidak ada kurva mesin yang meredamnya di putaran tinggi.
  Terukur 39% selip pada 53 m/s, dan itu sifat modelnya, bukan cacat; yang
  memperbaikinya P6f.

  Yang sesungguhnya diuji adalah **kopel** roda ke tanah: lepas gas, dan slipnya
  harus hilang. Terukur ω·r = 53,05 m/s terhadap v = 53,38 m/s — **selip 0,6%**.

#### P6c — Rem · ✅

**Kriteria terima**
- Kendaraan berhenti dari 100 km/jam dalam jarak yang masuk akal dan tercatat.
  Batas bawahnya fisika, bukan selera: dengan gesekan μ dan gravitasi g, jarak
  terpendek yang mungkin adalah v²/(2·μ·g) = 27,8²/(2·1,0·9,81) ≈ 39 m. Yang
  jauh lebih pendek berarti ban menggigit lebih dari yang mungkin.
- Rem tangan mengunci hanya roda yang ditandai `handbraked`.

#### P6d — Kemudi · ✅

**Kriteria terima**
- Radius belok pada kemudi penuh sepadan dengan sudut kemudi dan jarak sumbu
  roda: R ≈ wheelbase / tan(δ). Diuji terhadap rumus itu, bukan terhadap
  "kelihatan berbelok". Terukur **5,03 m** terhadap rumus **4,39 m** — 15% lebih
  lebar, dan itu justru yang diharapkan: rumusnya mengabaikan sudut selip ban,
  yang selalu melebarkan lingkarannya sedikit.
- Mobil tidak terguling pada kemudi penuh di kecepatan sedang — titik beratnya
  memang disetel di bawah pusat kotak justru untuk ini.

Diuji pada laju rendah dengan sengaja: pada 3–4 m/s percepatan menyampingnya
sekitar 0,2 g, jauh di bawah cengkeraman ban. Pada laju tinggi mobil menyapu
keluar dan radiusnya melebar — perilaku yang benar, tetapi ia menguji model ban,
bukan geometri kemudi.

#### P6e — Komponen scene dan prefab · ✅

`VehicleComponent` lewat refleksi, dijembatani `PhysicsScene`, plus satu prefab
kendaraan yang bisa dijatuhkan ke level seperti Physics Box.

**Yang disebut komponennya adalah ukuran, bukan daftar roda.** Keempat roda
diturunkan dari jarak sumbu dan jarak jejak — dua angka yang dipikirkan orang saat
merancang mobil, sedangkan empat koordinat lepas adalah empat kesempatan menaruh
roda tidak simetris tanpa menyadarinya. Yang butuh susunan tak lazim — tiga roda,
enam roda, roda miring — memakai `physics::VehicleDesc` langsung.

**`VehicleComponent` menggantikan `RigidBodyComponent`, bukan melengkapinya.**
Kendaraan sudah satu benda tegar di dalam dirinya. Entity yang membawa keduanya
tetap dibangun sebagai kendaraan — itu yang jelas diminta — dan benda tegar
keduanya dilaporkan lewat `PhysicsSceneStats::vehiclesWithRigidBody`, karena dua
benda di tempat yang sama saling mendorong dengan cara yang tidak bisa dijelaskan
siapa pun.

Kendaraan disusun di sapuan tersendiri, sesudah benda biasa dan **sebelum** sendi:
sendi boleh menunjuk chassis sebuah kendaraan, jadi chassis-nya harus sudah
terdaftar. Chassis itu ikut dilacak seperti benda dinamis biasa, sehingga
transform entity-nya ditulis balik tiap langkah tanpa jalur terpisah.

**Kriteria terima**
- Kendaraan dari komponen berdiri di suspensinya dengan keempat roda menapak, dan
  transform entity-nya ikut diperbarui.
- Prefab `Physics/Vehicle.simprefab` diuji **lewat berkasnya** — nama komponen dan
  ejaan enum yang salah hanya ketahuan di sana. Terukur menempuh **44,1 m** dalam
  4 detik gas penuh.

#### P6f — Mesin, kopling, girboks · ✅

`PxVehicleEngineDrive` di atas P6a–P6d. **Sengaja terakhir**: direct drive sudah
cukup untuk seluruh kriteria terima di atas, dan menambahkan kurva torsi mesin
di atas rantai yang belum terbukti berarti dua lapis yang belum terbukti
sekaligus.

Dipasang sebagai **mode**, bukan pengganti: `VehicleDriveModel::DirectDrive` tetap
jalan dan tetap diuji. Urutan komponennya bercabang di dua tempat — respons
perintah dan drivetrain — dan sisanya dipakai bersama.

**Kriteria terima**
- Putaran mesin tetap di dalam batasnya. Terukur **111,9–559,5 rad/s** dengan idle
  105 dan redline 630. Yang berputar mundur atau melewati redline bukan mesin
  melainkan angka yang lepas kendali, dan gejalanya muncul jauh kemudian sebagai
  torsi yang tidak masuk akal.
- Girboks otomatis berpindah naik. Terukur melalui **gigi 1 → 6**.
- **Roda berhenti selip di laju tinggi** — inilah yang tidak bisa dilakukan direct
  drive, dan alasan P6f ada.

  Terukur berdampingan pada 25 m/s dengan gas penuh: direct drive **40,8%** selip,
  engine drive **16,0%**. Torsi direct drive tetap berapa pun laju rodanya sehingga
  ban tetap jenuh; kurva torsi mesin turun menjelang redline, jadi roda berhenti
  memaksa dan mulai menggelinding. Bedanya berupa angka, bukan pendapat.

#### Cacat yang menghentikan P6 pada percobaan pertama

**Kerangka sumbu.** Bawaan `PxVehicleFrame` adalah **Z-atas**; mesin ini Y-atas.
Suspensi karena itu menembakkan sinarnya mendatar, tidak pernah menemukan tanah,
dan mobil duduk di atas bak chassis-nya dengan roda menggantung. Gejalanya
"kendaraan tidak jalan" — yang menunjuk ke mana-mana kecuali ke sumbu. Kerangkanya
sekarang disusun satu fungsi, `SimVehicleFrame()`, dipakai backend **dan** konteks
simulasi dunia: dua tempat yang menyusunnya sendiri-sendiri adalah dua tempat yang
bisa berbeda.

**Bendera bentuk nol.** `PxVehiclePhysXActorCreate` memanggil `setFlags` apa
adanya, jadi `PxShapeFlags(0)` berarti benar-benar tanpa `eSIMULATION_SHAPE` —
chassis jatuh menembus lantai alih-alih menabraknya. Roda sengaja tetap tanpa
bentuk simulasi: yang menahan mobil adalah suspensinya, dan bentuk roda yang ikut
menabrak akan melawan suspensi itu sendiri.

**Chassis terdaftar dua kali.** Ia sengaja masuk daftar benda biasa supaya scene
query dan sendi bisa menyebutnya, sementara pemiliknya tetap kendaraan. Yang
melepasnya hanya boleh satu; tanpa pencabutan saat dunia ditutup, aktornya dilepas
dua kali — dan itu muncul sebagai segfault di penutupan, bukan di tempat sebabnya.

**Pose roda di kerangka pusat massa.** Menyusunnya langsung dengan pose aktor
menggeser seluruh roda sejauh offset titik berat — di sini 0,35 m, yang kebetulan
persis jari-jari roda, sehingga hasilnya terbaca seolah mobil melayang setinggi
satu roda.

**Dan satu cacat di uji, bukan di mesin.** `doctest::Approx::epsilon` mengalikan
toleransinya dengan (1 + nilai), jadi untuk besaran sekitar satu meter epsilon
0,25 berarti ±0,45 m. Uji tinggi diam sempat lulus dengan mobil yang duduk di atas
bak chassis-nya. Sekarang toleransinya 1% dan tingginya dihitung penuh dari titik
sentuh ke atas.

#### Keadaan sekarang

Kerangkanya berdiri: `PhysicsVehicle.h` (API publik tanpa tipe PhysX),
`VehicleBackend.h/.cpp` dengan sebelas antarmuka komponen dan urutan
komponen bersubstep 3×, serta penyambungan ke `PhysicsWorld` — termasuk
pendaftaran chassis sebagai benda biasa supaya scene query dan sendi bisa
menyebutnya, dan pelangkahan kendaraan **sebelum** `simulate` di langkah yang
sama. Semuanya terbangun di ketiga konfigurasi.

**Keenam sub-fase lulus**, semuanya berjalan tanpa syarat di suite.

Dua cacat lain diperbaiki lebih dulu, keduanya sah dan keduanya bukan penyebab
utamanya:

- **Urutan taut**: `PhysXVehicle2` harus mendahului `PhysXExtensions`, karena
  `PxVehiclePhysXActorCreate` memakai `PxDefaultMemoryOutputStream` yang tinggal
  di Extensions. Lolos selama lima milestone karena belum ada yang memanggilnya.
- **Mesh silinder sapuan dibangun dari kerangka sumbu yang belum diisi**.
  Jounce sekarang dihitung dengan raycast, yang tidak menuntut mesh itu sama
  sekali; sapuan lebih tepat untuk roda lebar di tepi trotoar dan bisa dinaikkan
  nanti, tetapi rantai dasarnya harus benar lebih dulu.

**Jarak berhenti terukur 34,3 m, di bawah batas fisika 39 m** — dan itu bukan
kesalahan pengukuran. PhysX memasang kendala "ban lengket" pada laju rendah untuk
membawa kendaraan benar-benar berhenti alih-alih merayap; kendala itu bukan
gesekan dan tidak tunduk pada lingkaran gesekan, sehingga beberapa meter terakhir
ditempuh lebih cepat daripada yang bisa dilakukan ban. Dicatat sebagai sifat yang
diketahui, bukan sebagai toleransi yang dilonggarkan diam-diam.

### P7 — Fracture: Blast · ⬜

SDK terpisah, dibangun terpisah, opsional terpisah. Aset retak dimasak offline —
ia pengondisian aset, sama dengan cooking convex.

**Kriteria terima**
- Sebuah bongkahan pecah menjadi pecahan yang jumlah dan massanya kekal.
- Tanpa Blast, aset yang merujuknya ditolak dengan pesan yang menyebut SDK yang
  dibutuhkan.

### P8 — Jalur GPU: PBD, Soft Body, Flow · ⬜

**Hanya bila `SIM_WITH_PHYSX_GPU` dan perangkat kerasnya CUDA.** Tiga fitur yang
tidak punya jalur CPU, ditambah Flow yang SDK-nya sendiri.

**Ini lapisan tambahan, bukan bagian dari rencana inti.** Permainan yang
bergantung padanya tidak berjalan di RDNA1 sama sekali, dan itu harus menjadi
keputusan sadar yang tercatat di project — bukan akibat sampingan dari sebuah
sakelar build.

**Kriteria terima**
- Tanpa CUDA, seluruhnya tidak muncul di UI mana pun — bukan muncul lalu gagal.
- Dengan CUDA, level yang memakainya berjalan; tanpa, level yang sama memuat
  dengan peringatan yang menyebut fitur mana yang hilang.
- Kain PBD **tidak** dikerjakan — lihat tabrakan #1 di atas.

---

## Risiko

| Risiko | Tanda awal | Mundur ke |
| --- | --- | --- |
| Fisika dan render memakai transform yang berbeda | Objek bergetar saat diam | Satu sumber kebenaran: `World` transform ditulis sesudah `fetchResults`, dan render menginterpolasi — tidak pernah dua arah di frame yang sama |
| Cooking mesh dilakukan saat muat | Waktu muat level bertambah detik | Dimasak offline dan disimpan di samping aset, seperti bake SDF |
| Determinisme dijanjikan lebih dari yang bisa ditepati | Replay menyimpang di mesin lain | Janjinya dibatasi sejak awal ke build+platform+thread yang sama, dan ditulis di dokumentasi |
| Skala unit tidak cocok | Benda jatuh seperti di bulan, atau bergetar | `PxTolerancesScale` disetel dari ukuran adegan sekali, di satu tempat, dan diuji |
| Dua solver kain hidup berdampingan | Dua panel yang mengatur hal yang sama | Diputuskan di P8: kain PBD tidak dikerjakan |
| Build PhysX menua terhadap toolchain | Galat kompilasi di header PhysX | Ia dicari, tidak dibangun — versi yang dipakai tercatat, dan menggantinya adalah menyalin folder |

---

## Yang tidak boleh ditunda

- **Batas modul sejak P0.** Tipe PhysX yang bocor ke header publik akan menyebar
  ke setiap pemanggil, dan menariknya kembali setelah lima modul memakainya jauh
  lebih mahal daripada menjaganya sejak awal.
- **Langkah tetap sejak P1.** Fisika yang berlangkah mengikuti frame terasa benar
  di mesin pengembang dan salah di mana pun selainnya; memperbaikinya kemudian
  berarti menyetel ulang setiap nilai yang sudah terlanjur disetel.
Keputusan soal kain **tidak** ada di daftar ini, dan itu disengaja — lihat di
bawah.

## Yang sengaja tidak dikerjakan

- **Kain PBD PhysX** — `Sim::Cloth` sudah memilikinya.
- **Fisika deterministik lintas-platform** — PhysX tidak menjanjikannya, dan
  membangunnya di atas yang tidak dijanjikan adalah membangun di atas pasir.
- **Menggantikan picking Embree** — batasnya ditulis di atas; keduanya melayani
  konsumen yang berbeda.
- **Physics authoring lengkap di editor** — panel khusus menunggu sampai ada yang
  benar-benar menyetel fisika setiap hari. Sampai itu, Inspector lewat refleksi
  sudah cukup.
