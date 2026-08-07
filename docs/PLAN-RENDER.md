# Plan Rendering & Runtime (E8 → E9)

Fase ini baru dimulai setelah E7 selesai. Ditulis sekarang supaya keputusan yang
dibuat di fase editor tidak menutup pintu ke arah yang kita tuju.

## Apa yang sudah tersedia saat E8 dimulai

Dari fase editor kita sudah punya, dan sudah teruji dipakai:

- `RHI` Vulkan yang bekerja: instance, device, swapchain, VMA, render target
  offscreen, descriptor allocator, frame in-flight (E1).
- Pipeline kompilasi shader GLSL/Slang → SPIR-V yang terhubung ke build (E0).
- Format aset yang stabil: material graph, definisi partikel, terrain, vegetasi,
  klip animasi — semuanya sudah dipakai nyata oleh penulis konten (E7).
- Antarmuka `IViewportRenderer` yang sudah dipanggil dari lima panel berbeda.

Artinya E8 adalah menulis implementasi kedua dari satu antarmuka yang sudah mapan.

## E8 — Renderer

### E8.1 — Render graph & pass dasar · 🔨 semuanya kecuali bindless
Frame graph sederhana (deklarasi pass + resource, penyusunan barrier otomatis),
depth prepass, forward opaque, transparan tersortir, resolve. Kamera & frustum
culling. Bindless descriptor untuk tekstur (`VK_EXT_descriptor_indexing`).

**Sudah ada:** `render::FrameGraph` dan `render::Frustum`, keduanya **bebas
Vulkan dengan sengaja** dan karena itu diuji tanpa GPU — 22 test. Keduanya
tinggal di header publik `Sim::Render`, yang memang sudah bebas Vulkan sejak E1
supaya modul Editor yang me-link Render tidak ikut melihatnya.

**Barrier tidak ditulis tangan.** Barrier yang ditulis tangan benar pada hari ia
ditulis dan salah setelah pass ketiga disisipkan di antaranya — dan salahnya
tidak muncul sebagai kesalahan melainkan sebagai kedipan di satu kartu grafis
saja. Tiap pass hanya menyatakan apa yang dibaca dan ditulisnya; perpindahan
keadaannya diturunkan dari deklarasi itu. Barrier hanya dipancarkan saat keadaan
benar-benar berpindah: dua pembacaan berurutan dengan cara yang sama tidak
dipisahkan apa pun, karena memancarkan barrier per pass memang benar tapi
memaksa GPU menjalankannya berurutan tanpa ada yang menuntutnya.

**Barrier `CompiledPass` selalu berarti "sebelum pass ini".** Satu arah saja, dan
transisi yang memang harus terjadi sesudah seluruh pass punya tempatnya sendiri
(`finalBarriers`). Versi pertamanya menempelkan transisi keluaran ke barrier pass
terakhir — yang berarti target diserahkan ke ImGui sebelum pass itu sempat
menggambarnya. Ditemukan saat memeriksa ulang kontraknya, bukan oleh test:
test-nya sendiri ikut salah karena ditulis dari asumsi yang sama.

**Pass yang hasilnya tidak dibaca siapa pun dibuang, dan pembuangannya menular ke
belakang.** Tanpa itu, tiap fitur yang bisa dimatikan — bayangan, SSAO, garis
bantu debug — menuntut `if` di dalam kode frame. Dengan pembuangan otomatis,
mematikan sebuah fitur cukup berarti tidak ada yang membaca keluarannya.

**Alias memori ditentukan lewat selang umur, bukan lewat kolam.** Kolam
membagikan apa pun yang sedang bebas, jadi jumlah memori yang terpakai bergantung
pada urutan permintaan dan berubah-ubah antar-frame tanpa ada yang mengubahnya —
"kadang kehabisan memori" adalah bug yang paling mahal dicari. Selang umur memberi
jawaban yang sama untuk graph yang sama, dan itu dikunci test.

**Reversed-Z** (`PerspectiveReversedZ`, plus varian bidang-jauh-tak-hingga) — sudah
keputusan terkunci sejak fase editor. Frustum-nya sendiri tidak perlu tahu:
bidangnya diturunkan dari matriks view-proj apa adanya, jadi yang berubah hanya
isi matriksnya sementara volume yang dibatasi tetap sama. Itu dikunci test yang
membandingkan keputusan "di dalam/di luar" kedua proyeksi pada ribuan titik.

**`VulkanRenderer` menjalankannya.** Lima pass lewat graph — grid, depth
prepass, forward opaque, transparan tersortir, garis bantu — dengan dynamic
rendering dan `synchronization2` (keduanya sudah dinyalakan `Device` sejak E1).
Ia dipilih lebih dulu di composition root, dan `StubRenderer` menjadi jalur
mundurnya kalau perangkatnya bukan Vulkan 1.3.

**Yang digambar masih kotak, dan itu bukan penambal yang malas.**
`ViewportScene::MeshInstance` belum membawa geometri sama sekali — hanya
transform, kotak batas, dan warna — karena importir mesh baru datang di E8.4.
Yang dibuktikan E8.1 bukan "mesh terlihat benar" melainkan bahwa graph
benar-benar menjalankan pass Vulkan dengan barrier yang disimpulkannya sendiri.
Kotak cukup untuk itu; begitu mesh masuk, yang berganti hanya sumber vertex-nya.

**Prepass jalan tanpa fragment shader sama sekali**, dan opaque mengujinya
dengan `EQUAL`. Itulah guna prepass yang sebenarnya: bukan menghemat depth test,
melainkan menghemat shading yang akan ditimpa.

**Tiga bug ditemukan dengan menjalankannya, bukan dengan membaca kode:**

1. *Arah muka.* `VK_FRONT_FACE_CLOCKWISE` terlihat benar di atas kertas — matriks
   proyeksi membalik Y, jadi lilitan segitiga ikut terbalik. Salah: arah muka
   dinilai di ruang framebuffer, yang sumbu Y-nya memang sudah menghadap ke
   bawah, jadi pembalikan di proyeksi justru meniadakannya. Terlihat sebagai
   kotak yang memperlihatkan sisi dalamnya.
2. *Layout awal target.* Graph menyatakan target warna dimulai dalam keadaan
   `Present`; itu benar untuk setiap frame kecuali yang pertama, karena image
   yang baru dibuat berada di UNDEFINED. Ditemukan validation layer, dan hanya
   muncul kalau panel Viewport kebetulan tertutup jendela lain pada frame
   pertama.
3. *Prepass menyatakan lampiran warna yang tidak dipasangnya.* Tanpa validation
   layer, yang terlihat adalah prepass yang jalan di satu driver dan tidak di
   driver lain.

Sesudah ketiganya: **nol pesan validation** pada build Debug, termasuk saat
kamera diputar dan viewport diubah ukurannya.

**Grid dan garis bantu ikut dibawa**, bukan ditinggalkan. Renderer baru yang
menghapus cara utama orang membaca skala dan orientasi bukan kemajuan. Shader
grid kini menerima nilai depth bidang dekat sebagai push constant — 0 pada
proyeksi biasa, 1 pada reversed-Z — sehingga kedua renderer memakai shader yang
sama tanpa satu pun menebak.

**Belum ada:** bindless descriptor (`VK_EXT_descriptor_indexing`), yang baru
berarti begitu ada tekstur untuk diindeks — yaitu E8.2. Alias memori transien
sudah ada di graph dan teruji, tapi belum ada pemakainya: pass pertama yang
benar-benar menuntut target antara adalah post-process di E8.8.

### E8.2 — Material runtime · ✅ selesai
Kompilasi graph `.simmat` → kode shader → SPIR-V, cache di disk berbasis hash graph.
Material instance jadi uniform buffer + indeks tekstur. Varian shader (skinned,
instanced, alpha-test) lewat spesialisasi konstanta. **Titik sambung:** Material
Editor mulai menampilkan preview PBR sungguhan.

**Sudah ada:** jalur lengkap dari graph sampai SPIR-V untuk kedua tahap —
`MaterialParameterBlock` (tata letak blok uniform dan tabel slot tekstur),
`openpbr.slang` (model shading-nya), `AssembleMaterialModule` (perakit modul,
tahap vertex dan fragment), dan `ShaderCache` (kompilasi `slangc` beserta cache
disk). Graph → Slang sendiri sudah selesai di E7.1.

**Ini ABI antara kode Slang yang dihasilkan dan sisi C++.** Kedua sisi
menghitung offset dari daftar parameter yang sama, dan selisih satu sisipan
tidak menghasilkan galat apa pun — hanya material yang warnanya salah dengan
cara yang sulit dilacak. Karena itu tata letaknya dihitung di satu tempat dan
diuji terhadap contoh yang offset-nya dihitung tangan, bukan disalin dari
keluaran kodenya sendiri.

**`float3` pada std140 punya dua angka yang berbeda: penjajaran 16 tapi ukuran
12.** Keduanya harus dipegang sekaligus, dan salah satunya saja meleset ke arah
yang berlawanan — lupa penjajarannya menaruh `float3` di offset 4, sedangkan
mengira ukurannya 16 mendorong `float` berikutnya melewati celah yang justru
boleh diisinya. Yang kedua sempat ditulis terbalik, dan test yang menemaninya
ikut salah karena ditulis dari keyakinan yang sama; keduanya sudah dikoreksi.

**Urutan slot mengikuti urutan deklarasi, bukan urutan yang dioptimasi.**
Menyusun ulang menurut ukuran menghemat sisipan, tapi membuat tata letak
berubah saat sebuah parameter diganti tipenya — dan setiap material instance
yang sudah tersimpan menunjuk offset yang lama.

**Sisipan selalu nol**, supaya dua blok yang isinya sama berbanding sama byte
demi byte: itulah yang dipakai renderer untuk memutuskan apakah sebuah blok
perlu diunggah ulang.

#### Kompilasi dan cache

**Keluaran kompiler graph belum bisa diberikan ke `slangc`.** Ia berisi
`evalMaterial()` dan tidak punya entry point, varying, maupun yang memanggil
model shading-nya. `AssembleMaterialModule` yang melengkapinya: konstanta
spesialisasi, prelude, cbuffer per-frame, struct varying, kode material, lalu
entry point fragment.

**Prelude ditanam, bukan di-`import`, dan itu keputusan demi cache-nya.** Kunci
cache adalah hash teks sumber yang diberikan ke kompilator; sumber yang cuma
menulis `import openpbr;` menghasilkan kunci yang **tidak berubah ketika
`openpbr.slang` berubah** — dan cache akan dengan patuh menyerahkan SPIR-V yang
dibangun terhadap model shading yang sudah tidak ada. Menanamnya membuat setiap
perubahan model shading otomatis membatalkan seluruh material, tanpa satu pun
daftar dependensi yang harus dipelihara tangan. Harganya modul yang panjang dan
prelude yang diparse ulang per material — ongkos sekali per material per
perubahan, dibayar oleh cache yang justru jadi benar.

**Identitas kompilator ikut kunci.** Tanpanya cache menyerahkan SPIR-V yang
dihasilkan `slangc` yang sudah tidak terpasang, dan bug itu muncul sebagai
shader yang jalan di satu mesin dan tidak di mesin lain.

**Varian TIDAK ikut kunci.** Skinned, instanced, dan alpha-test dipasang sebagai
konstanta spesialisasi saat pipeline dibuat, jadi satu modul melayani kedelapan
kombinasinya. Menjadikannya bagian kunci akan melipatgandakan modul secara
kombinatorial — tiga sakelar menjadi delapan modul yang masing-masing harus
dikompilasi, disimpan, dan dibatalkan sendiri-sendiri.

**Entri diperiksa, bukan dipercaya.** Berkas yang terpotong dibaca sebagai cache
miss. Penulisannya lewat berkas sementara lalu `rename` di direktori yang sama —
rename lintas sistem berkas bukan operasi tunggal, dan justru di situlah keadaan
setengah tertulis yang ingin dihindari muncul.

**Kuncinya 128 bit.** Bukan demi kekuatan kriptografis: kunci cache yang
bertabrakan muncul sebagai material yang memakai shader milik material lain —
bug yang tidak akan pernah dicurigai orang berasal dari cache.

#### Tahap vertex

**Satu modul, dua entry point, dua entri cache.** Sumbernya sama persis untuk
kedua tahap; yang membedakan kuncinya adalah tahap dan nama entry point-nya.
Merakit dua modul terpisah akan membuat struct varying tertulis dua kali — dan
struct varying yang berbeda antara vertex dan fragment adalah kegagalan link
pipeline, bukan sesuatu yang bisa ditemukan dengan membaca salah satunya.

**Lokasi atribut dan nomor binding ditulis eksplisit, tidak dinomori otomatis.**
Penomoran otomatis mengikuti urutan yang *bertahan*: dengan `kSkinned` mati
kedua atribut tulang bisa dibuang, dan seluruh atribut sesudahnya bergeser
sementara `VkVertexInputAttributeDescription` di sisi C++ tidak ikut. Hal yang
sama berlaku untuk tekstur di set material, yang bergeser saat sebuah parameter
tidak terpakai dibuang. Keduanya tidak menghasilkan galat validasi — hanya mesh
yang membaca UV sebagai warna, dan albedo yang ternyata peta normal.

Pembagian set mengikuti seberapa sering isinya berubah: set 0 per-frame
(FrameParams, matriks tulang, transform instance), set 1 per-objek, set 2 milik
material. Nomornya tertulis sekali di `MaterialBindings` dan
`MaterialVertexLocation`, dan diuji terhadap teks yang benar-benar dihasilkan.

**Atribut vertex selalu tujuh, apa pun nilai konstanta spesialisasinya.**
Spesialisasi terjadi saat pipeline dibuat, sedangkan daftar antarmuka
`OpEntryPoint` sudah terkunci di modul — terbukti lewat `spirv-dis`:
`boneIndices` dan `boneWeights` tetap ada meski `kSkinned` mati. Mesh tanpa data
skin karena itu tetap harus menyediakan keduanya, biasanya lewat binding
ber-stride nol.

**Argumen `slangc` ikut identitas kompilator.** Ini sempat terlewat.
`-matrix-layout-column-major` menyamakan tata letak matriks dengan glm; kalau ia
tidak ikut kunci cache, mengubahnya meninggalkan seluruh entri lama yang tampak
sah dan ternyata mentranspose setiap matriks — mesh yang terpelintir, bukan
pesan galat.

**Bobot skinning dinormalisasi, bukan dipercaya.** Bobot yang jumlahnya 0.98
karena dikuantisasi ke 8 bit di importer menyusutkan mesh 2% secara merata —
cacat yang terbaca sebagai "model ini agak kecil", bukan sebagai bug skinning.
Arah (normal, tangent) memakai bagian 3x3 matriks saja; ini benar untuk tulang
rotasi + translasi + skala seragam, dan skala tak seragam akan menuntut invers
transpose. Hal yang sama berlaku untuk transform instance, dan penabur vegetasi
E7.4 memang hanya menghasilkan yaw + skala seragam.

`SV_InstanceID` dihitung relatif terhadap `firstInstance`, jadi draw yang
memakai firstInstance bukan nol tetap mengindeks buffer transform dari nol —
firstInstance tidak bisa dipakai sebagai offset ke dalamnya.

#### Preview PBR di Material Editor

**Seam kedua, bukan instance kedua `IViewportRenderer`.** Catatan E7.1 menyebut
jalan keluarnya "membuat instance KEDUA lewat pabrik yang sudah ada". Itu
menyelesaikan tabrakan target render, tapi tidak menyelesaikan gunanya:
`ViewportScene` tidak punya tempat untuk sebuah material — ia daftar
`MeshInstance` berwarna solid — jadi instance kedua akan menggambar bola abu-abu,
yaitu persis kekosongan yang membuat preview ditunda sejak awal. Yang dibutuhkan
preview memang bukan "satu scene lagi" melainkan satu mesh, satu material, satu
cahaya, dan `IMaterialPreview` menyebutkan tepat itu — tetap bebas Vulkan seperti
seam pertama.

**Materialnya diserahkan sebagai SPIR-V, bukan sebagai graph.** Preview yang
mengompilasi sendiri akan menjadi implementasi kedua dari jalur graph → shader,
dan seluruh E7.1 dipegang satu alasan: model shading hanya boleh punya satu
implementasi. Yang dilihat preview adalah shader yang sama persis dengan yang
nanti dipakai renderer.

**Tekstur material sementara memakai tekstur putih 1×1.** Putih, bukan magenta:
tekstur yang belum dimuat harus membuat material terlihat seperti material tanpa
tekstur, bukan membuat seluruh preview berteriak. Pemuatan tekstur sungguhan
masuk bersama sistem tekstur E8.

**Logam tampak hitam kecuali di sorotannya, dan itu benar.** `openpbr.slang`
baru cahaya langsung; logam tidak punya lobe difus, jadi tanpa IBL tidak ada apa
pun yang mengisi bagian yang tidak memantulkan cahaya. IBL datang di E8.3.

Tiga hal ditemukan dengan menjalankannya, bukan dengan membaca kode:

- **Depth image tidak pernah dipindahkan layout-nya.** Warna yang salah layout
  terlihat langsung; depth tidak — preview tetap tergambar benar dan
  satu-satunya yang memberi tahu adalah validation layer.
- **Kapabilitas SPIR-V `DrawParameters` tidak diaktifkan device.** Asalnya
  keputusan yang sengaja diambil: `SV_InstanceID` berarti nomor instance relatif
  terhadap `firstInstance`, jadi slangc menerjemahkannya jadi
  `gl_InstanceIndex - gl_BaseInstance`. Kini `shaderDrawParameters` diaktifkan.
- **`ImGui::Image` bukan item yang bisa diaktifkan**, jadi seret untuk mengorbit
  kamera tidak pernah terbaca. Kodenya terbaca benar dan slider di sebelahnya
  bekerja; yang gagal diam-diam justru satu-satunya interaksi di panel itu.
  Sekarang `InvisibleButton` yang menerima seretnya, dan gambarnya digambar
  lewat draw list di rect yang sama.

**Preview bukan tab, melainkan area tetap di bawah tab.** Sebagai tab ia hilang
tepat ketika ia paling berguna — saat orang menyunting parameter di Details atau
menelusuri galat di Compiled Slang — dan menyunting material tanpa melihat
akibatnya adalah bolak-balik yang tidak perlu dilakukan siapa pun.

**Kiri menggerakkan cahaya, kanan mengorbit kamera; objeknya tidak pernah
diputar.** Keduanya menjawab pertanyaan yang berbeda: memindahkan cahaya
memperlihatkan bentuk sorotan sebuah material, sedangkan mengorbit
memperlihatkan bagaimana rupanya berubah terhadap sudut pandang — dan untuk
material anisotropik atau ber-coat, yang kedua tidak bisa disimpulkan dari yang
pertama. Memutar objek terlihat sama dengan mengorbit kamera tapi ia menggeser
bingkai tangent bersamanya, jadi arah anisotropi ikut berputar dan justru
menyembunyikan hal yang sedang diperiksa.

Kedua sumbu cahaya sempat terbalik — seret ke kiri memindahkan sorotan ke kanan.
Itu bukan kesalahan yang terlihat dari kode: yang menentukan arahnya adalah letak
kamera, bukan rumus di baris yang bersangkutan. Orbit memang berlawanan dengan
kursor (yang diseret adalah objeknya), cahaya harus mengikutinya.

Diverifikasi lewat XTEST pada Debug build: material dibuat, preview menampilkan
bola ber-shading dan tetap terlihat saat tab berpindah ke Compiled Slang,
mengubah **Base Metalness** 0 → 1 lewat node Constant mengubah bola menjadi
logam, mengganti bentuk bekerja, seret kiri memindahkan sorotan searah kursor,
seret kanan mengorbit kamera, dan cache disk terisi dua berkas per material.
**Nol pesan validation layer.**

### E8.3 — Lighting & shadow · 🔨 matematika cascade, cluster, dan IBL selesai
IBL (prefilter env + DFG LUT), directional light dengan cascaded shadow map,
point/spot dengan shadow atlas, clustered light culling untuk banyak lampu.

**Sudah ada:** `ShadowCascades`, `LightCluster`, dan `Ibl` — seluruh
matematikanya, teruji tanpa GPU — ditambah `evaluateOpenPBR_IBL` di
`openpbr.slang`. Sama seperti E8.1, yang diputuskan lebih dulu adalah bagian
yang bisa dibuktikan salah di test; pass Vulkan-nya menyusul.

#### Cascade

**Tiap cascade dipas pada bola, bukan pada kotak yang ketat.** Kotak ketat
berubah ukuran saat kamera berputar, dan ukuran yang berubah berarti
dunia-per-texel yang berubah — tepi bayangan lalu berkilat setiap kali kamera
bergerak sedikit. Bola pembatas irisan frustum tidak bergantung orientasi, jadi
ukurannya tetap untuk sebuah belahan. Yang dibayar peta bayangan yang sedikit
lebih longgar daripada perlunya. Diuji dua arah: bola benar-benar memuat
kedelapan sudut irisan, dan jari-jarinya tidak berubah untuk dua belas orientasi
kamera.

**Titik asalnya lalu dikancing ke kelipatan texel.** Bola menghilangkan kilatan
akibat rotasi, pengancingan yang akibat pergeseran — salah satunya saja masih
menyisakan kilatan, dan keduanya sering dikira satu perbaikan yang sama.
Kisinya diambil dari rotasi cahaya dengan titik asal di origin dunia; memakai
matriks pandang yang titik asalnya ikut bergerak bersama bola membuat pusatnya
selalu jatuh tepat di (0,0), pengancingannya tidak melakukan apa pun, dan
kilatannya tetap ada sementara kodenya tampak sudah menanganinya. Itu bentuk
pertama yang saya tulis.

Test-nya pun mula-mula salah: ia mengukur pergeseran di ruang **dunia**,
padahal yang dikancing hanya X dan Y di ruang cahaya. Z di sana menunjuk
sepanjang arah cahaya — menggesernya memindahkan rentang kedalaman, bukan kisi
texel — jadi pengukuran yang mencampur ketiganya membuat pengancingan yang benar
terlihat gagal.

**Belahannya campuran logaritmik dan seragam.** Keduanya sendirian salah ke arah
berlawanan: logaritmik murni menaruh hampir seluruh resolusi di beberapa meter
pertama sehingga cascade terakhir jadi kotak-kotak, seragam murni memberi jarak
dekat porsi yang sama dengan jarak jauh padahal di dekatlah tepi bayangan
diperhatikan.

**Bidang dekat cahaya ditarik mundur** supaya benda di belakang kamera tetap
menjatuhkan bayangan ke dalam pandangan — tanpa itu yang hilang justru bayangan
benda tinggi, yang paling diperhatikan.

Ortografiknya biasa, bukan reversed-Z: depth ortografik linier, jadi menukarnya
tidak memindahkan presisi ke mana pun — hanya menambah satu aturan yang harus
diingat.

#### Clustered culling

**Eksponensial di kedalaman, seragam di layar.** Irisan seragam memberi beberapa
meter pertama satu irisan dan ratusan meter terakhir sisanya, padahal lampu
berkerumun justru di dekat kamera.

**Cluster diuji sebagai kotak, bukan sebagai enam bidang.** Uji bola-terhadap-
bidang menganggap bola di dalam selama ia di sisi dalam setiap bidang — benar
untuk volume cembung tak terbatas, bukan untuk cluster yang terbatas: bola yang
lewat di dekat sudut memenuhi keenam bidang tanpa pernah menyentuh cluster-nya.
Ada test khusus untuk kasus sudut itu.

**Spot diuji terhadap kerucut sungguhan**, bukan bola pembatasnya: spot sempit
yang panjang punya bola pembatas jauh lebih besar daripada kerucutnya. Puncak
kerucut digeser mundur `r/sin θ` supaya bola yang menyerempet sisi ikut
terhitung — tanpa itu lampu sorot memotong benda tepat di tepi berkasnya.

**Daftar per cluster dipotong dan pemotongannya dilaporkan.** Daftar tanpa batas
membuat satu cluster buruk — sudut ruangan dengan dua puluh lampu — menentukan
biaya seluruh frame, dan yang terlihat adalah frame rate yang jatuh di satu
tempat tanpa sebab yang jelas.

Rumus irisan disimpan sebagai skala dan bias supaya CPU dan GPU memakai bentuk
yang sama persis; dua rumus yang setara secara matematis tapi ditulis berbeda
akan berselisih satu irisan di tepinya, dan yang terlihat adalah lampu yang
hilang tepat pada jarak tertentu.

#### IBL

Sumber lingkungan diambil lewat antarmuka `IEnvironmentSampler`, bukan lewat
cubemap konkret. Pembakaran IBL adalah pekerjaan sekali per environment, jadi
biaya panggilan virtual tidak berarti apa-apa — sementara yang didapat besar:
seluruh matematikanya bisa diuji terhadap lingkungan yang jawabannya diketahui
secara analitis, tanpa satu berkas gambar pun.

**LUT DFG menyimpan skala dan bias, bukan hasil jadi untuk sebuah F0.** F0
keluar dari integralnya sebagai faktor linear, jadi satu LUT melayani setiap
material; menyimpan hasil jadi berarti satu LUT per material, dan LUT-nya
berukuran sama dengan tekstur.

**Sampelnya deterministik.** Urutan Hammersley, bukan RNG — alasan yang sama
dengan penabur vegetasi E7.4: LUT yang berbeda antar-jalan membuat perbandingan
gambar tidak bisa dipakai sebagai test, dan cache apa pun yang menyimpannya
tidak pernah sah.

**Irradiance sebagai sembilan koefisien SH, bukan cubemap.** Kernel lobe kosinus
meredam pita di atas orde dua sampai di bawah satu persen, jadi cubemap
irradiance membayar memori dan satu pengambilan tekstur untuk ketelitian yang
tidak terlihat. Konvolusinya (π, 2π/3, π/4) adalah yang membedakan irradiance
dari radiance rata-rata; melewatkannya menghasilkan angka yang tetap masuk akal
— halus dan berwarna benar — tapi salah sebesar faktor yang berbeda per pita,
dan biasanya "diperbaiki" dengan menaikkan intensitas lampu sampai tidak ada
lagi yang cocok. Diuji sebagai tungku putih: radiance konstan L menghasilkan
irradiance π·L pada normal mana pun.

**Prefilter mengandaikan N = V = R**, pendekatan baku sejak Karis 2013.
Harganya jelas: pantulan yang seharusnya memanjang pada sudut serong menjadi
bundar. Menyimpan variasi terhadap sudut pandang menuntut dimensi ketiga pada
petanya — memori yang belum ada yang menuntutnya.

Beberapa hal kecil yang masing-masing punya kegagalannya sendiri: bingkai sampel
memilih sumbu bantu dari komponen normal terkecil (sumbu tetap menghasilkan
cross product nol tepat pada arah "atas", yang muncul di setiap permukaan
datar); konstanta k Smith untuk IBL berbeda dari yang untuk cahaya langsung
(`alpha/2` lawan `(r+1)²/8`, dan yang salah membuat logam kasar terlalu gelap);
LUT dibakar di tengah texel, bukan di tepinya; kekasaran nol memakai
pengambilan tunggal alih-alih integral yang sampelnya menyebar karena penjepitan.

`evaluateOpenPBR_IBL` menerima irradiance, kedua radiance prefilter, dan suku
DFG **sudah jadi** — ia tidak tahu apakah sumbernya probe statis, SH, atau nanti
global illumination. Itulah titik sambungnya: mengganti sumbernya tidak
menyentuh satu baris pun model shading.

**Belum ada:** pembakaran peta prefilter dan LUT ke tekstur GPU, pass bayangan
Vulkan beserta atlas untuk point/spot, dan penyambungan ketiganya ke pipeline
material.

`openpbr.slang` sendiri sudah ada sejak E8.2, tapi baru cahaya langsung: satu
arah cahaya, tanpa IBL, bayangan, maupun transmisi. Yang ditambahkan di sini
tidak mengubah antarmuka `OpenPBRSurface`, jadi material yang sudah ditulis
tidak ikut berubah.

`evalOpenPBR_IBL` menerima `prefilteredBase`, `prefilteredCoat`, dan
`irradiance` terpisah. Ketiganya di sini datang dari probe statis — dan itu juga
titik sambung untuk global illumination, yang mengganti sumbernya tanpa mengubah
satu baris pun model shading-nya. Lihat catatan GI di bawah.

### E8.4 — Mesh & animasi
Impor mesh (ufbx/cgltf) menggantikan importer pass-through E5, LOD dari
meshoptimizer, GPU skinning dengan skinning buffer, blend shape.
**Titik sambung:** Animation Editor memutar mesh skinned sungguhan.

### E8.5 — Terrain
Rendering terrain ter-tile dengan LOD berbasis jarak (clipmap atau quadtree),
sampling splat map, blending material layer, culling per-tile, hole.
**Titik sambung:** Terrain Editor melukis pada permukaan yang di-shading penuh.

### E8.6 — Vegetation
Instanced rendering dengan GPU culling (compute + indirect draw), LOD & transisi
billboard, animasi angin di vertex shader, impostor untuk jarak jauh.
**Titik sambung:** Vegetation Editor menampilkan sebaran sesungguhnya.

### E8.7 — Partikel
Simulasi GPU (compute shader) dengan modul yang sama seperti definisi editor,
sorting untuk transparansi, soft particle, ribbon/trail.
**Titik sambung:** Particle Editor memutar simulasi yang identik dengan runtime.

### E8.8 — Post-process & langit
Sky atmospheric, tone mapping (ACES), bloom, SSAO, TAA atau FXAA, depth of field,
motion blur, color grading LUT, exposure otomatis.

**Kriteria terima E8 (keseluruhan).** Scene uji berisi terrain 2×2 km, 200 ribu
instance vegetasi, 20 lampu berbayang, karakter ber-animasi, dan tiga sistem partikel
berjalan ≥ 60 fps pada GPU target, tanpa error validation layer, dan tampilan di
Editor identik dengan tampilan di SimRuntime.

## Global illumination

**Belum masuk daftar pass di atas, dan itu disengaja sampai ada keputusan.**
Rencana terpisah ada di `/home/arie/SDK/rencana-implementasi-gi.md`: screen probe
+ hash grid radiance cache di belakang satu antarmuka `ITraceBackend`, dengan dua
implementasi — SDF clipmap untuk GPU tanpa RT core, ray query untuk yang punya.
Anggarannya 3,0 ms per frame di 1080p, ±16 minggu untuk satu orang.

Tiga hal yang perlu diputuskan sebelum ia masuk roadmap:

- **Anggarannya belum didamaikan dengan kriteria terima E8.** 3,0 ms adalah 18%
  dari frame 60 fps, sementara adegan uji E8 — terrain 2×2 km, 200 ribu instance
  vegetasi, 20 lampu berbayang — sudah menuntut sisanya. Keduanya ditulis
  terpisah dan belum pernah dijumlahkan.
- **Ia bergantung pada E8.3, bukan hanya E8.1.** Milestone M6-nya menyambung ke
  `evalOpenPBR_IBL`, yang menuntut prefilter env dan DFG LUT sudah ada.
- **Baseline-nya tidak bisa diuji di mesin ini.** Mesin pengembangan punya RT
  core (RTX 2060), yaitu tier atas rencana itu. Jalur SDF — yang justru harus
  bekerja di semua GPU — hanya bisa diuji lewat override backend manual, jadi
  override itu bukan kemudahan melainkan syarat.

Kabar baiknya: **sisi shader M6 sudah selesai.** `evalOpenPBR_IBL` sudah
menerima `irradiance` terpisah, sudah mengalikannya dengan `(1 - E_spec)`, dan
sudah mengecualikan logam dari lobe difus lewat `lerp(..., metalness)`. Yang
tersisa hanya menyambungkan sumbernya.

## E9 — Runtime & distribusi

- **SimRuntime**: player tanpa editor yang memuat level + menjalankan Lua.
- **Cook/packaging**: konversi aset ke format biner siap-pakai, pak arsip,
  pemangkasan aset yang tidak terpakai lewat graf ketergantungan dari E5.
- **PhysX 5** (`/home/arie/SDK/PhysX-main`): rigid body, collider, character
  controller, raycast, dengan komponen dan visualisasi debug di editor.
- **Audio** (OpenAL Soft di `/home/arie/SDK/openal-soft-1.25.2`): sumber suara 3D,
  bus, mixing.
- **Play-in-Editor** yang sesungguhnya: menjalankan world sungguhan dalam proses
  editor, dengan pemisahan state agar Stop mengembalikan scene ke keadaan awal.
- Profiler (Tracy), build Windows, dan skrip rilis.

## Keputusan yang sudah dikunci sejak fase editor

Dicatat supaya tidak diperdebatkan ulang saat E8:

1. **Koordinat**: Y-up, tangan-kanan, satuan meter. Depth Vulkan `[0,1]` dengan
   reversed-Z (near = 1.0) untuk presisi jarak jauh — penting karena ada terrain.
2. **Warna**: seluruh perhitungan di ruang linear; sRGB hanya di titik input tekstur
   dan output akhir. Editor sudah menandai color space per tekstur sejak E5.
3. **Material**: **OpenPBR Surface v1.1**, bukan metallic-roughness sederhana
   maupun specular-glossiness. Node keluaran graph E7.1 sudah mencerminkan
   `OpenPBRSurface` pin per pin, dengan nilai bawaan yang dikunci test terhadap
   berkas normatif OpenPBR. `base_metalness` tetap ada di dalamnya — yang
   bertambah adalah lapis specular terpisah, coat, dan fuzz.

   Tiga kelompok parameter spesifikasi sengaja belum ada: `subsurface_*`,
   `transmission_*`, `thin_film_*`. Tekniknya sudah dipilih di
   [RENDER-OPENPBR.md](RENDER-OPENPBR.md).
4. **Shader**: sumbernya Slang, dikompilasi lewat `slangc` dari Vulkan SDK. GLSL
   masih diterima untuk shader utilitas.
5. **Vulkan 1.3** sebagai baseline (dynamic rendering, synchronization2,
   timeline semaphore), dengan fallback render pass tradisional bila perangkat
   hanya 1.2.
