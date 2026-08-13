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

### E8.3 — Lighting & shadow · ✅ selesai
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

#### Pass bayangan Vulkan

Peta bayangannya **larik berlapis, bukan atlas di satu tekstur besar**. Atlas
menuntut shader menggeser dan menskalakan koordinat per cascade, dan
penyaringan PCF-nya lalu bisa mengambil texel milik cascade tetangga di tepi
ubin. Lapisan memberi tiap cascade ruang koordinatnya sendiri, dan penjepitan
sampler bekerja apa adanya.

**Peta diimpor graph sebagai `ShaderRead`, bukan `None`.** `None` berarti "tidak
ada yang perlu ditunggu", dan itu tidak benar: frame berikutnya menulis ulang
peta yang masih dibaca fragment shader frame sebelumnya. Menyatakannya
`ShaderRead` membuat graph memancarkan barrier yang menunggu pembacaan itu
selesai — dan konsekuensinya keadaan awalnya harus benar sejak frame pertama,
sama seperti target warna.

**Bias normal, bukan bias depth konstan.** Bias depth konstan adalah angka ajaib
yang harus disetel ulang tiap cascade: yang cukup di cascade dekat menghasilkan
peter-panning di cascade jauh, dan sebaliknya menghasilkan acne. Menggeser titik
sampel sepanjang normal sebanyak beberapa texel dunia menyesuaikan diri sendiri
terhadap resolusi tiap cascade, karena `texelWorldSize` memang sudah dihitung
per cascade.

Perbandingan sampler `LESS_OR_EQUAL`, bukan `GREATER`: cascade memakai
ortografik biasa sementara target viewport reversed-Z. Keduanya hidup
berdampingan di renderer ini, dan memakai perbandingan yang salah membalik
bayangan menjadi sorotan. Filter `LINEAR` bersama `compareEnable` adalah PCF
perangkat keras — satu pengambilan mengembalikan rata-rata empat *perbandingan*,
bukan rata-rata empat kedalaman; merata-ratakan kedalaman lalu membandingkannya
sekali menghasilkan tepi yang salah di setiap permukaan miring.

**Kamera bayangan sempat ditempatkan di sisi berlawanan dari cahaya**, dan tidak
satu pun test yang sudah ada menangkapnya — bola pembatas, pengancingan texel,
dan pembagian cascade semuanya tidak menyentuh letak matanya. Yang terlihat
bukan bayangan yang bergeser melainkan adegan yang seluruhnya gelap. Sekarang
ada test yang menyatakan konvensinya secara langsung: bergerak ke arah cahaya
harus mengecilkan depth, dan seluruh isi bola cascade harus muat di kubus
satuan.

Satu galat validasi lain: descriptor menyebut `DEPTH_READ_ONLY_OPTIMAL`
sementara graph menyimpulkan `SHADER_READ_ONLY_OPTIMAL` dari `Access::ShaderRead`.
Keduanya sah untuk menyampel image depth, tapi yang berlaku adalah yang
disimpulkan graph.

Diverifikasi di GUI: kotak menjatuhkan bayangan ke bidang tanah, tidak ada acne
pada bidang seluas 40 m, tidak ada garis perpindahan cascade yang terlihat, dan
**nol pesan validation layer**.

#### Clustered lighting di pass forward

`ViewportScene` sekarang membawa `LightInstance` — bentuk yang **sengaja berbeda
dari `scene::LightComponent`**: sudah dalam ruang dunia, dan sudut kerucutnya
sudah menjadi kosinus. Renderer tidak boleh mengenal tipe komponen (seam #1 di
ARCHITECTURE.md), dan yang paling mudah bocor lewat batas itu justru
penerjemahan seperti ini: bagaimana rotasi entity menjadi arah pancar. Karena
itu penerjemahannya ada di `SceneView`, bukan di renderer.

**Directional tidak ikut penyaringan cluster.** Ia mengenai setiap cluster, jadi
memasukkannya hanya menambah satu entri ke setiap daftar; ia sudah ditangani
terpisah bersama cascade-nya. Directional pertama dari scene menjadi matahari
dan sisanya diabaikan — mengabaikannya diam-diam lebih baik daripada
menjumlahkan arah, yang menghasilkan bayangan yang tidak cocok dengan lampu mana
pun.

**Baris ubin nol adalah baris ATAS layar.** Indeks ubin adalah indeks ubin
layar — shader menghitungnya dari `gl_FragCoord`, yang sumbu Y-nya menunjuk ke
bawah — sedangkan +Y ruang pandang menunjuk ke atas. Pembalikannya terjadi
sekali, di `ClusterBounds`, di tempat yang memang tahu soal konvensi proyeksi.

Bentuk pertamanya tidak membalik, dan **test yang menyertainya ikut tidak
membalik**: ia menghitung ubin dari NDC ruang pandang, jadi ia konsisten dengan
dirinya sendiri dan buta terhadap kesalahannya. Yang menemukannya sebuah
tangkapan layar — cahaya terpotong tepat di batas ubin, persegi bertepi tegak
lurus di ruang layar, bentuk yang tidak mungkin dihasilkan geometri mana pun.
Tepinya diukur jatuh di x = 240 dan 719, dan batas ubin untuk viewport 1277
lebar dengan 16 kolom ada di 239 dan 718.

Test-nya kini menghitung ubin lewat koordinat framebuffer, seperti shader —
ditambah satu test yang menyatakan konvensinya langsung: baris atas layar harus
punya batas +Y ruang pandang, baris bawah harus −Y.

**Skala dan bias irisan dikirim dari CPU apa adanya**, bukan diturunkan ulang di
shader dari near dan far. Dua rumus yang setara secara matematis tapi ditulis
berbeda berselisih satu irisan di tepinya, dan yang terlihat adalah lampu yang
hilang tepat pada jarak tertentu.

**Peredupan jaraknya berjendela.** Kuadrat terbalik saja tidak pernah mencapai
nol, jadi setiap lampu akan menerangi seluruh dunia dengan nilai yang sangat
kecil — dan `range` tidak akan berarti apa-apa selain kebohongan yang dipakai
penyaringan cluster.

Cluster berhenti di 300 m, bukan di `farZ` kamera: irisan eksponensial sampai
dua kilometer membuat irisan pertama setipis sentimeter, dan lampu punctual
memang tidak relevan di kejauhan. Pemotongan daftar per-cluster dilaporkan lewat
log, bukan didiamkan — pemotongan yang diam-diam terlihat sebagai lampu yang
hilang di sudut tertentu saja, dan tidak ada yang akan menghubungkannya dengan
batas per-cluster.

Diverifikasi di GUI: lampu point menghasilkan kolam cahaya dengan peredupan
mulus sampai batas jangkauannya, tanpa jejak ubin cluster, berdampingan dengan
bayangan directional. **Nol pesan validation layer.**

#### Model peredupan: perbandingan dengan LightOpt (SIGGRAPH 2026)

Rumus lampu punctual kita dibandingkan dengan [LightOpt: Lights Optimization for
Real-time Rendering](https://dl.acm.org/doi/10.1145/3799902.3811072) (Chen, Cao,
Wu, Hu), yang menyatakan modelnya mengikuti Unity, yang mengikuti Frostbite.
Rumus lighting bukan kontribusi makalah itu — kontribusinya pengoptimal jumlah
lampu — tapi ia menyebutkan modelnya lengkap, jadi berguna sebagai pembanding.

Bentuk per-lampu **identik**: `E_i = V_i · c_i · I_i · D_i · A_i · (n·ω_i)`, dan
peredupan sudutnya sama persis sampai ke kuadratnya. Kita juga sama-sama
mengandaikan `V_i = 1` — bagi mereka penyederhanaan, bagi kita sementara sampai
atlas bayangan point/spot ada.

**Jendela jaraknya berbeda, dan sengaja dibiarkan berbeda.** Kita kuartik
(`(1 − (d/r)⁴)²`, Frostbite), mereka kuadratik (`(1 − (d/r)²)²`, Unity).
Selisihnya 1,56× pada setengah jangkauan dan 2,22× pada 0,7 jangkauan. Keduanya
sah dan keduanya nol tepat di `range` — dan nol itulah yang membuat penyaringan
cluster kita eksak. Yang tidak sah adalah mencampur angka intensitas yang
di-author untuk salah satunya ke yang lain, jadi hal itu ditulis di
`cluster_common.glsl` supaya tidak ada yang "memperbaikinya" belakangan.

**Yang kita adopsi dari mereka bukan rumusnya, melainkan gagasan bahwa batas
dekatnya harus sebuah parameter.** Bentuk mereka `a/(a + d²)` terbatas di mana
pun lewat parameter `a`; kita memakai `1/max(d², minDist²)` dengan `minDist`
yang dulu tertanam sebagai konstanta `1e-4` di dalam shader — yang akarnya tepat
satu sentimeter. Jadi angkanya sudah ada, hanya tersembunyi dan tidak bisa
di-author. Kini ia `LightComponent::sourceRadius`, bawaannya 1 cm, sehingga
setiap lampu yang sudah ada tampak persis seperti sebelumnya.

`a/(a + d²)` sendiri tidak diadopsi: ia ≈ `a/d²` di kejauhan, jadi `a` merangkap
skala kecerahan medan jauh — mengadopsinya berarti arti `intensity` berubah
untuk setiap lampu yang sudah ada, ditukar dengan kehalusan yang tidak terlihat
pada raster.

**Yang bisa dihitung CPU dipindahkan ke CPU.** `GpuLight` kini membawa
`1/range²` dan `sourceRadius²`, bukan `range` dan `sourceRadius`, sehingga
shader tidak punya kuadrat maupun pembagian yang bisa ditulis berbeda dari sisi
C++ — disiplin yang sama dengan skala dan bias irisan cluster.

Cermin C++ untuk rumus peredupannya **sengaja tidak dibuat**: cermin yang bukan
kode yang benar-benar berjalan adalah test yang menguji dirinya sendiri, dan ia
akan tetap hijau setelah shader-nya berubah. Yang diuji adalah bagian yang
memang hidup di C++ — penerjemahan `LightComponent` menjadi `LightInstance`,
tempat konvensi arah dan sudut kerucut diputuskan.

#### Bendera bayangan dan radiance matahari

Ketiganya ditemukan saat memeriksa ulang E8.3, dan ketiganya punya pola yang
sama: **data mengalir dari komponen ke Inspector dan ke berkas, lalu berhenti
sebelum renderer.** `MeshRendererComponent::castShadows`, `receiveShadows`, dan
`LightComponent::castShadows` semuanya tersimpan dan bisa disunting tapi tidak
mengubah apa pun — antarmuka yang berbohong, yang lebih buruk daripada tombol
yang belum ada karena pemakainya mengira sudah mematikan sesuatu.

Yang menjatuhkan bayangan diletakkan di **awal daftar buram**, sehingga pass
bayangan tinggal menggambar awalannya — tanpa atribut tambahan dan tanpa cabang
di shader. Trik yang sama dengan pemisahan buram/tembus pandang, dan urutan di
antara sesama buram memang tidak berarti apa-apa karena semuanya diuji depth.
`receiveShadows` menuntut keputusan per-fragmen, jadi ia satu-satunya yang
benar-benar butuh atribut instance — sebuah bitmask, bukan float 0/1, supaya
bendera berikutnya tinggal mengambil bit berikutnya.

**Warna dan intensitas matahari sekarang sampai ke shader.** Sebelumnya hanya
arahnya yang dipakai dan `box.frag` mengalikan angka 0,75 yang ditulis langsung
di sana.

Menyalakannya memunculkan pertanyaan yang tidak bisa dihindari: matahari yang
disemai editor berintensitas 3,0, dan target warna viewport 8-bit tanpa satu pun
operator nada di antaranya — jadi radiance sungguhan terpotong putih. Yang salah
bukan angkanya melainkan tidak adanya yang memetakannya. Jawabannya
`ViewportDesc::exposure`, sebuah **parameter bernama** yang berdiri sebagai
pengganti tone mapping sampai E8.8 — alasan yang sama dengan `sourceRadius`:
angka yang tersembunyi tidak bisa disetel siapa pun dan akan dikira bagian dari
model. Bawaannya dipilih supaya matahari bawaan menghasilkan tepat 0,75, yaitu
nilai yang dulu ditulis di shader, sehingga adegan yang ada tidak berubah rupa.

Eksposurnya **berlaku untuk kedua jalur cahaya**. Matahari dan lampu punctual
pada skala yang berbeda adalah persis jenis ketidakcocokan yang paling sulit
dilacak: setiap lampu terlihat masuk akal sendiri-sendiri.

Diverifikasi di GUI: mematikan **Cast Shadows** membuat bayangan kotak benar-
benar hilang, warna matahari mewarnai seluruh adegan, dan menurunkan
intensitasnya menggelapkannya. Nol pesan validation layer.

#### Status E8.3

| Butir | Keadaan |
|---|---|
| Cascaded shadow map (directional) | ✅ jalan di Vulkan |
| Clustered light culling | ✅ jalan di Vulkan |
| IBL (prefilter env + DFG LUT) | ✅ dibakar ke tekstur GPU dan dipakai preview material |
| Point/spot dengan shadow atlas | ✅ jalan di Vulkan |

#### IBL tersambung

`rhi::TextureCube` baru: cubemap dengan rantai mip. **Rantai mip di sini bukan
penyaringan biasa** — mip ke-*n* dibakar dengan kekasaran ke-*n*, bukan
dikecilkan dari mip sebelumnya. Membangkitkannya lewat `vkCmdBlitImage` akan
menghasilkan gambar yang mirip tapi salah: buram yang rata, bukan buram yang
mengikuti lobe GGX.

Pembakarannya di **CPU**, memakai `IEnvironmentSampler` dan fungsi-fungsi yang
sudah teruji. Membakarnya di GPU menuntut pass compute beserta rantai barrier-nya
sendiri, dan yang dibakar adalah lingkungan yang berubah paling banyak sekali per
sesi. Yang dibayar 175 ms di Release (1,8 detik di Debug — matematika ini memang
sepuluh kali lebih lambat tanpa optimasi); yang didapat adalah tidak adanya
implementasi kedua.

Lingkungannya `GradientSky` prosedural sampai importir tekstur HDR ada. Ia ada
supaya IBL punya sumber tanpa satu berkas pun — dan sekaligus lingkungan yang
jawabannya bisa diperiksa tangan di test.

Sembilan koefisien SH diunggah sebagai `float4`, bukan `float3`: std140
menjajarkan anggota larik ke 16 byte apa pun tipenya, jadi mengunggahnya rapat
membuat setiap koefisien sesudah yang pertama meleset.

Modul material yang dirakit kini mendeklarasikan cubemap prefilter, LUT DFG, dan
koefisien SH di set 0, lalu memanggil `evaluateOpenPBR_IBL`. Dua pengambilan dari
peta yang sama pada mip berbeda — base dan coat punya kekasarannya sendiri, dan
satu mip untuk keduanya membuat coat yang licin tampak sekasar lapisan di
bawahnya.

Diverifikasi di GUI: bola dielektrik menangkap gradien langit alih-alih abu-abu
rata, dan bola logam berhenti hitam — ia memantulkan langit di belahan atas dan
tanah di belahan bawah, lengkap dengan pantulan cakram mataharinya. Nol pesan
validation layer.

#### Atlas bayangan point/spot

Atlas terpisah dari cascade karena keduanya punya bentuk yang berbeda — cascade
larik berlapis dengan resolusi seragam, atlas satu bidang dengan ubin beragam
ukuran — dan menyatukannya berarti salah satunya harus mengalah pada bentuk yang
bukan miliknya.

Ubinnya pangkat dua, dialokasikan seperti quadtree tanpa pohonnya: sebuah ubin
berukuran `size` hanya boleh mulai pada kelipatan `size`, jadi keselarasan itu
sendiri yang mencegah tindihan. Ukurannya dipilih dari seberapa besar lampu itu
di layar; ukuran tetap untuk semua akan memberi lampu meja di ujung ruangan
resolusi yang sama dengan lampu sorot yang memenuhi layar.

Point membayar enam muka, jadi ubinnya diturunkan satu tingkat — tanpa itu satu
point light memakan atlas sebanyak enam spot yang sama pentingnya. Point yang
tidak muat seluruh mukanya dibatalkan seluruhnya: empat dari enam muka
menghasilkan bayangan yang berhenti di tengah udara.

Satu `vkCmdBeginRendering` untuk seluruh atlas, lalu viewport dan scissor
dipindah per ubin. Memulai rendering per ubin berarti satu clear per ubin pada
image yang sama, dan clear kedua menghapus yang pertama begitu areanya bertemu.

Koordinat dijepit di dalam ubin dengan sisa setengah texel sebelum PCF: PCF
mengambil tetangga, dan tetangga di tepi ubin adalah milik lampu lain — bayangan
lampu sebelah lalu bocor masuk sebagai garis di tepi berkas. Sampler hanya tahu
batas atlas, bukan batas ubin.

Muka +Y dan −Y memakai sumbu atas Z: keduanya memandang lurus sepanjang Y, dan
`LookAt` dengan atas yang sejajar arah pandang menghasilkan matriks yang tidak
terdefinisi — bayangannya lalu hilang hanya di atas dan di bawah lampu, gejala
yang mudah dikira masalah bias.

Bias normalnya dalam satuan dunia dan dihitung CPU dari ukuran ubin dan
jangkauan lampu, karena ubin yang lebih kecil menuntut pergeseran yang lebih
besar — dan ukuran ubin berubah setiap kali lampunya mendekat.

Diverifikasi di GUI: dengan matahari dimatikan, kotak menjatuhkan bayangan point
light yang jelas — baji gelap memanjang menjauhi lampu, tepinya lembut oleh PCF.
**Nol pesan validation layer.**

Dengan ini `V_i` pada Pers. 3 tidak lagi diandaikan 1 untuk punctual.

#### Yang tersisa dari E8.3, dan ke mana

Penyambungan IBL ke pass forward viewport menunggu pipeline material
menggantikan `box.frag` — itu pekerjaan E8.4, bukan E8.3. Preview material sudah
memakai IBL sepenuhnya.

`openpbr.slang` sendiri sudah ada sejak E8.2, tapi baru cahaya langsung: satu
arah cahaya, tanpa IBL, bayangan, maupun transmisi. Yang ditambahkan di sini
tidak mengubah antarmuka `OpenPBRSurface`, jadi material yang sudah ditulis
tidak ikut berubah.

`evalOpenPBR_IBL` menerima `prefilteredBase`, `prefilteredCoat`, dan
`irradiance` terpisah. Ketiganya di sini datang dari probe statis — dan itu juga
titik sambung untuk global illumination, yang mengganti sumbernya tanpa mengubah
satu baris pun model shading-nya. Lihat catatan GI di bawah.

### E8.4 — Mesh & animasi
Impor mesh (FBX SDK/cgltf) menggantikan importer pass-through E5, LOD dari
meshoptimizer, GPU skinning dengan skinning buffer, blend shape.
**Titik sambung:** Animation Editor memutar mesh skinned sungguhan.

**Selesai: geometri mesh sungguhan di viewport**
(`Code/Assets/{include/Sim/Assets/MeshData.h,src/MeshImport.cpp}`,
`Code/EditorFramework/src/SceneView.cpp`, `Code/Render/src/VulkanRenderer.cpp`).
Aset mesh yang ditetapkan di `MeshRendererComponent` sekarang benar-benar
digambar; sebelum ini setiap mesh renderer menggambar kubus satuan, tidak peduli
aset mana yang dirujuk.

**Rantainya putus di tiga tempat, dan ketiganya memang sengaja dibiarkan sampai
sekarang:** `.fbx` ditangani `PassThroughImporter` yang hanya memeriksa berkasnya
ada, `SceneView` tidak pernah membaca `MeshRendererComponent::mesh`, dan renderer
hanya punya satu geometri — `BuildUnitCube()` — yang digambar untuk seluruh
instance dengan satu panggilan.

- **Autodesk FBX SDK, bukan penguraian sendiri.** FBX menyimpan konvensi sumbu
  dan satuannya di dalam berkas, dan berkas dari DCC yang berbeda memakai
  konvensi yang berbeda. Mengoreksinya tangan berarti menebak konvensi sumbernya,
  dan tebakan yang salah menghasilkan mesh yang terbaring miring atau seratus
  kali terlalu besar — bukan galat. Sumbunya dikonversi SDK lewat
  `FbxAxisSystem::DeepConvertScene`; satuannya justru **tidak** (lihat bug
  konversi ruang di bawah).
- **Seluruh node digabung menjadi satu mesh**, masing-masing sudah dikalikan
  `EvaluateGlobalTransform()` beserta offset geometrinya. Yang hilang adalah
  struktur hierarkinya; yang didapat adalah satu buffer per aset alih-alih satu
  per node.
- **Vertex kembar disatukan dengan perbandingan bit-per-bit, bukan bertoleransi.**
  Toleransi menyatukan dua sisi sebuah rusuk tajam yang normalnya memang berbeda,
  dan yang terlihat adalah tepi kotak yang membulat sendiri.
- **Cache ada di renderer, bukan di editor.** Yang bisa memutuskan sebuah mesh
  sudah ada di GPU hanyalah yang memegang buffer-nya. Cache-nya wajib bukan
  optimisasi: pemanggilnya adalah pembangun daftar gambar, yang berjalan tiap
  frame untuk setiap entity — tanpa cache, FBX sebelas megabyte diurai enam puluh
  kali per detik. Jalur yang gagal dimuat dicatat gagal dan tidak dicoba lagi.
- **Draw dikelompokkan per mesh.** Instance buram diurutkan dengan dua kunci:
  yang menjatuhkan bayangan lebih dulu — sifat lama yang membuat pass bayangan
  cukup menggambar awalan daftarnya — lalu per handle mesh. Tembus pandang tetap
  diurutkan jarak saja: mengelompokkannya per mesh berarti menggambarnya di luar
  urutan, dan beberapa draw call lebih sedikit jauh lebih murah daripada yang
  hilang.
- **Kubus satuan menjadi mesh nol**, lengkap dengan buffer indeks yang tidak
  dibutuhkannya, supaya seluruh jalur gambar memakai `vkCmdDrawIndexed` tanpa
  kecuali. Ia juga tetap menjadi nilai mundur untuk setiap mesh renderer yang
  asetnya belum ditetapkan atau gagal dimuat — entity yang tidak menggambar apa
  pun adalah entity yang tidak bisa diklik, tidak bisa dipilih, dan karena itu
  tidak bisa diperbaiki.

**Satu kesalahan yang dijaga uji, dan yang sempat saya buat:** kubus satuan harus
dipetakan ke kotak batasnya — geser ke pusat lalu skala ke ukurannya — karena
vertexnya membentang -0,5..0,5 apa pun batasnya. Mesh yang diimpor vertexnya
**sudah** berada di ruang lokal yang sama dengan batasnya, jadi pemetaan yang
sama menskalakannya dua kali: shader ball setinggi 2,7 m menjadi setinggi 7,2 m,
tanpa satu pun galat yang menyertainya.

Terukur di editor (RTX 2060, viewport 1277x696):

| Yang diukur | Hasil |
| --- | --- |
| `shaderBall.fbx` | **67.832 segitiga, 35.897 vertex** |
| Batas hasil impor | 2,69 x 2,66 x 2,50 m, alas di y ~ 0,02 |
| Waktu urai (release) | **118 ms**, sekali per aset |
| `forward-opaque` | 0,014 ms |
| Galat validation layer | **0** |

**Yang belum ada dari E8.4:** LOD dan meshoptimizer, blend shape, serta glTF —
FBX SDK membaca FBX dan OBJ; glTF sejak itu mendarat lewat `cgltf`, dan USD
lewat OpenUSD. Indeks aset juga belum
mencatat jumlah segitiga: itu menuntut medan baru di `ImportResult` beserta panel
yang menampilkannya.

**Sedang berjalan: material dan tekstur dari berkasnya.** Sisi impornya sudah
mendarat (lihat di bawah); yang tersisa dua, dan yang kedua besar.

1. **Aset material yang dibangkitkan saat impor.** `MeshMaterial` yang datar
   menjadi `.simmat`, teksturnya disalin ke dalam project, dan
   `MeshRendererComponent::material` menunjuk hasilnya. Jalur teksturnya relatif
   terhadap berkas mesh dan kerap naik satu tingkat, jadi yang menyalinnya harus
   menyelesaikannya lebih dulu — `..\checkerA.tga` di sebelah `shaderBall.fbx`.
2. **Pipeline material menggantikan `box.frag` di pass forward.** Inilah yang
   ditunggu sejak E8.3 untuk menyambungkan IBL, dan yang membuat tekstur berarti
   sama sekali. `box.frag` sekarang hanya `input.color.rgb` dikali cahaya — tidak
   ada roughness, tidak ada metalness, tidak ada tekstur — jadi material yang
   diimpor tidak punya tempat untuk diperlihatkan selain warna dasarnya.

**Yang bagus, dan yang harus diperiksa sebelum mulai.** Seam-nya sudah benar:
`IMaterialPreview::SetMaterial` menerima **SPIR-V mentah** beserta ukuran blok
parameter dan jumlah teksturnya, jadi yang merakit dan mengompilasi material
adalah editor, dan `Sim::Render` tidak pernah mengenal `Sim::Material` sama
sekali. Pass forward bisa memakai bentuk yang sama persis.

Yang **tidak** bisa diangkat begitu saja adalah sisanya — dan ini yang membuat
pekerjaannya jauh lebih besar daripada "pindahkan preview ke pass forward".
Modul material dan pipeline kotak tidak sepakat pada tiga hal sekaligus:

| | modul material | `box.vert` sekarang |
| --- | --- | --- |
| Atribut vertex | 0 posisi, 1 normal, **2 tangent**, 3 uv0, 4 warna, 5–6 skin | 0 posisi, 1 normal, 2–5 kolom matriks, 6 warna, 7 bendera, 8–10 skin |
| Transform instance | storage buffer, set 0 binding 2 | atribut vertex ber-rate instance |
| Matriks kulit | set 0 binding 1 | set 1 (forward) / set 0 (bayangan) |

Tiga akibatnya, berurutan dari yang paling menentukan:

- **`MeshVertex` belum punya tangent sama sekali** — sekarang hanya posisi,
  normal, dan uv. Peta normal tidak berarti apa-apa tanpanya, jadi tangent harus
  dibangkitkan saat impor (dan disatukan dengan benar pada seam UV, yang punya
  jebakannya sendiri).
- **Set 0 milik modul material bukan set 0 milik renderer.** Modul menulis
  `FrameParams` sendiri — viewProj, kamera, satu arah cahaya, SH probe — sementara
  set 0 renderer memuat 21 binding: cascade bayangan, daftar cluster lampu, atlas
  spot/point, clipmap SDF, dan seluruh tekstur GI. Selama modul memakai
  `FrameParams`-nya sendiri, material **tidak punya akses ke bayangan, cluster
  lampu, maupun GI** — jadi mengganti `box.frag` dengannya bukan peningkatan
  melainkan kemunduran. Yang harus diputuskan lebih dulu: apakah `FrameParams`
  modul diperluas menjadi set 0 renderer seutuhnya (dan `AssembleMaterialModule`
  ikut menuliskan seluruh binding itu), atau material mendapat set keempat.
- **Transform instance pindah dari atribut ke storage buffer**, yang menyentuh
  `Gather`, buffer instance, dan keempat pipeline yang sudah ada.

**Karena itu ini bukan pekerjaan satu duduk, dan tidak boleh dimulai separuh:**
selama pass forward setengah bermigrasi, viewport tidak menggambar apa-apa.
Urutan yang masuk akal: putuskan bentuk set 0 lebih dulu → tangent di importir →
transform instance lewat storage buffer (pipeline kotak masih dipakai, jadi bisa
diuji sendiri) → baru pipeline material di pass forward, dengan `box.frag`
dipertahankan sebagai jalur mundur sampai yang baru terbukti.

**Satu keputusan yang menunggu, dan sebaiknya diambil sebelum nomor 1:** apa yang
terjadi pada `MeshRendererComponent::baseColor` begitu berkasnya membawa material
sendiri. Ia ada sebagai penambal sampai material sungguhan datang — "satu-satunya
cara sebuah adegan bisa punya permukaan yang berbeda warna" — dan begitu material
mendarat, membiarkannya tetap mengalikan warna material berarti Inspector punya
dua tempat yang mengatur hal yang sama. Membiarkannya diam-diam tidak berpengaruh
lebih buruk lagi: itu antarmuka yang berbohong.

**Selesai: impor rangka dan bobot skin dari FBX**
(`Code/Assets/{include/Sim/Assets/MeshData.h,src/MeshImport.cpp}`). Diuji
terhadap rig Mixamo `Y Bot.fbx` beserta animasi `Running.fbx` dan
`Defeated.fbx`.

- **Empat pengaruh per vertex, dan itu batas yang dipilih bukan diwarisi.**
  Pengaruh kelima dan seterusnya hampir selalu berbobot di bawah satu persen —
  yang tidak bisa dibedakan mata — sementara masing-masing menambah satu indeks,
  satu bobot, dan satu perkalian matriks per vertex per frame. Yang dibuang
  dinormalkan kembali ke empat yang tersisa: bobot yang tidak berjumlah satu
  menyusutkan vertexnya ke arah titik asal.
- **Pengaruh skin ikut menjadi bagian kunci penyatuan vertex.** Dua titik yang
  sama persis tapi berbeda bobot skin adalah dua vertex yang berbeda;
  menyatukannya berarti separuh permukaan mengikuti bone yang salah.
- **Bone diurutkan topologis dari penelusuran melebar (breadth-first) hierarki
  node**, jadi transform global bisa dihitung satu lintasan maju tanpa rekursi.
  Urutannya ditelusuri sendiri, bukan diambil dari `FbxScene::GetNode`: yang
  terakhir mengembalikan urutan objek di dalam berkasnya, dan di sana tidak ada
  yang menjamin induk mendahului anaknya. Nomor bone yang dihasilkannya juga yang
  ditunjuk `SkinInfluence`, jadi urutan itu bagian dari format aset — bukan
  detail internal importir.

**Satu bug sungguhan yang ditangkap ujinya, dan yang sempat mendarat.**
Konversi satuan yang dipanggang ke transform node root — yang dilakukan
`FbxSystemUnit::ConvertScene`, dan karena itu tidak dipakai di sini —
meninggalkan transform lokal anak-anaknya dalam satuan asli berkasnya. Terukur:
mesh Y Bot benar setinggi 1,80 m sementara translasi bone-nya 99,79 —
sentimeter. Kulit yang diulit rangka seratus kali terlalu besar tidak
menghasilkan satu pun galat; ia menghasilkan karakter yang lenyap. Bind pose
karena itu diturunkan dari transform global tiap node, bukan dari transform
lokalnya.

Uji yang menjaganya menyatakan sifat yang benar-benar berarti: **posisi global
bone harus berada di dalam kotak batas mesh-nya.** Terukur sesudah perbaikan:

| | x | y | z |
| --- | --- | --- | --- |
| Bone (global bind) | −0,973..0,973 | 0,031..1,786 | −0,109..0,213 |
| Batas mesh | −0,973..0,973 | 0,000..1,805 | −0,161..0,204 |

Terukur juga: 65 bone, 55.320 segitiga, selisih terbesar jumlah bobot dari satu
**0,000000**, nol indeks bone di luar batas. Rig-nya tidak ikut di repo, jadi
ujinya berjalan hanya bila ditunjuk: `SIM_RIG_FBX=/path/rig.fbx ctest`.

**Selesai: GPU skinning**
(`Shaders/{skin_common,instance_common,box.vert,prepass.vert,shadow.vert}.slang`,
`Code/Render/{include/Sim/Render/Types.h,src/VulkanRenderer.cpp}`,
`Code/EditorFramework/src/SceneView.cpp`,
`Code/Assets/{include/Sim/Assets/MeshData.h,src/MeshImport.cpp}`).

Bobot skin sekarang diunggah sebagai vertex buffer kedua, palet matriks kulit
sebagai storage buffer per frame, dan ketiga tahap vertex yang menggambar
geometri — forward, prepass, bayangan — menerapkannya.

- **Renderer tidak mengenal rangka sama sekali.** Yang menyeberang seam adalah
  `ViewportScene::skinMatrices` — satu larik matriks kulit yang ditunjuk tiap
  `MeshInstance` lewat `skinFirst`/`skinCount` — dan sebuah `boneCount` di
  `MeshAsset` supaya pemanggilnya tahu sepanjang apa paletnya. Nama bone,
  hierarki, dan bind pose tidak pernah lewat sini; kalau suatu saat perlu,
  berarti ada seam yang bocor.
- **Yang dikirim matriks kulit, bukan transform bone.** `global × invers bind`,
  persis keluaran `Pose::ComputeSkinning`. Mengirim transform global saja berarti
  setiap vertex harus dikembalikan ke ruang bind di dalam vertex shader: satu
  inversi matriks per vertex per frame untuk hasil yang tidak berubah selama
  rangkanya tidak berubah.
- **Dua pipeline per pass, bukan satu dengan cabang runtime.** Yang membedakan
  keduanya bukan hanya konstanta spesialisasi `kSkinned` melainkan juga stride
  binding skin — dan stride adalah bagian dari pipeline, bukan sesuatu yang bisa
  diganti per draw. Ruas gambar karena itu dikelompokkan per (mesh, berkulit),
  dan pipeline diikat di dalam `DrawRuns`, bukan sekali di awal pass.
- **Atribut skin ada di kedua varian, dan itu tuntutan Slang bukan pilihan.**
  Daftar antarmuka `OpEntryPoint` terkunci di modul sebelum konstanta
  spesialisasi dinilai — terbukti lewat `spirv-dis`: `boneIndices`,
  `boneWeights`, dan `skinBase` tetap ada dengan `kSkinned` mati, begitu pula
  descriptor `skinPalette`. Jalur statis karena itu memasang buffer pengaruh
  tiruan **ber-stride nol**, satu elemen untuk mesh apa pun, dan tetap mengikat
  set paletnya. Buffer sepanjang mesh untuk data yang tidak pernah dibaca adalah
  24 byte per vertex yang dibayar seluruh adegan statis.
- **Palet ada di set 1 pada forward dan set 0 pada bayangan.** Bukan
  ketidakrapian: set 0 milik forward berisi peta bayangan, dan pass bayangan
  sedang menulisinya — mengikat set itu di sana berarti satu image dipakai
  sebagai lampiran sekaligus descriptor pada draw yang sama. `SKIN_SET` disebut
  tiap shader supaya nomornya tetap satu tempat per pipeline layout.
- **Bayangan ikut berkulit.** Bayangan yang tetap berdiri di bind pose sementara
  karakternya bergerak adalah bayangan yang lepas dari pemiliknya — cacat yang
  jauh lebih mencolok daripada tidak ada bayangan sama sekali. Prepass juga:
  forward menguji depth dengan `EQUAL`, jadi prepass yang menaruh vertex di
  tempat yang sedikit berbeda membuat karakternya lenyap seluruhnya.
- **Rig di atas 65.535 bone ditolak, bukan dipotong.** Indeks bone-nya 16 bit,
  dan yang membungkus diam-diam adalah kulit yang mengikuti tulang acak.
- **Palet diunggah utuh, tidak dipadatkan ke instance yang lolos culling.**
  Indeksnya menunjuk larik milik pemanggil, jadi memadatkannya berarti menomori
  ulang seluruhnya — satu lintasan tambahan untuk menghemat penyalinan yang sudah
  satu `memcpy`.

**Palet yang dikirim editor masih satuan, yaitu bind pose** — matriks kulit
adalah `global × invers bind`, dan pada bind pose keduanya saling meniadakan.
Yang tergambar karena itu sama persis dengan sebelumnya, dan itu memang kriteria
terimanya: **jalur berkulit pada bind pose harus tidak memindahkan apa pun.**
Ia tetap dikirim setiap frame walaupun hasilnya sama dengan tidak dikirim —
jalur yang tidak pernah dijalankan siapa pun adalah jalur yang cacatnya baru
ditemukan pada hari klip pertama masuk. Yang menggantinya dengan pose sungguhan
adalah impor klip FBX beserta yang menjalankannya.

Diuji: palet satuan tidak menggeser titik mana pun; bobot mencampur **matriksnya**
(dua bone berlawanan arah berbobot 0,25/0,75 menaruh titik pada 2,0 dari −4 dan
+4); rotasi 90° pada sumbu Y memindahkan (1,0,0) ke (0,0,−1) — arah yang salah
kalau matriksnya tertranspose, dan panjangnya tetap sehingga tidak terlihat
sebagai cacat melainkan sebagai animasi yang terbalik; indeks bone di luar palet
jatuh ke matriks satuan alih-alih membaca memori orang lain. Dan rantai penuhnya
di `SimAnimationTests`: `Pose::ComputeSkinning` pada rantai dua bone, bone anak
diputar 90° pada Z, titik satu meter di atasnya mendarat tepat di (−1, 1, 0).

Sisi editornya diuji lewat perender palsu yang hanya menjawab jumlah bone: dua
karakter mendapat ruas palet yang berurutan dan tidak tumpang tindih — ruas yang
bertumpuk adalah dua karakter yang memakai pose yang sama, dan itu terlihat
sebagai kembar yang bergerak serempak, bukan sebagai galat — palet tidak menumpuk
antar-frame, dan mesh tanpa rangka tidak menyita satu matriks pun.

Terukur di editor (RTX 2060, rig Mixamo `Y Bot.fbx`): **65 bone, 55.320
segitiga, 35.440 vertex**, dan ruasnya benar-benar diikat ke pipeline berkulit —
diperiksa dengan pencatatan sementara di `DrawRuns`, karena bind pose yang benar
terlihat sama persis dengan jalur berkulit yang tidak pernah dijalankan. Build
Debug dengan `VK_LAYER_KHRONOS_validation` menyala: **nol galat dan nol
peringatan validation layer** sepanjang jalannya.

**Selesai: impor klip animasi dari FBX**
(`Code/Animation/{include/Sim/Animation/ClipImport.h,src/ClipImport.cpp}`,
`Code/Animation/{include/Sim/Animation/Clip.h,src/Clip.cpp}`,
`Code/Assets/src/MeshImport.cpp`). Diuji terhadap `Running.fbx` dan
`Defeated.fbx` yang dipasang ke rig `Y Bot.fbx`.

- **Impor FBX ada di `Sim::Animation`, bukan di `Sim::Assets`.** Preseden yang
  diikuti `Sim::Terrain`, yang mengurus sendiri baca-tulis heightmap PNG-nya:
  modul yang memiliki bentuk datanya juga yang memiliki penerjemahan dari
  berkasnya. Alternatifnya — bentuk klip netral di sisi aset lalu diterjemahkan
  di sini — berarti seluruh model track, kunci, dan kurva dituliskan dua kali
  untuk satu-satunya pembaca yang akan pernah ada.
- **Kunci dicuplik lewat `FbxNode::EvaluateLocalTransform`, bukan dibaca sebagai
  kurva mentah.** FBX menyimpan rotasi sebagai kurva Euler beserta urutan rotasi,
  pre-rotation, post-rotation, dan pivot yang semuanya ikut menentukan hasilnya.
  Menyusun ulang rantai itu tangan berarti setiap potong yang terlewat
  menghasilkan animasi yang *hampir* benar — jauh lebih sulit dilacak daripada
  yang jelas-jelas salah. SDK mengevaluasi rantainya pada satu waktu, dan
  importir mencuplik itu pada laju frame berkasnya.
- **Belahan kuaternion disamakan antar-frame.** Tiap frame diuraikan sendiri,
  jadi tandanya boleh berbalik di tengah klip tanpa ada yang berubah pada
  rotasinya — dan dua kunci berurutan yang berseberangan belahan di-slerp lewat
  jalan memutar, satu frame yang berputar hampir penuh lingkaran.
- **Hanya bone yang benar-benar punya kurva di take itu yang dicuplik.** Track
  rotasi tetap adalah orientasi bind rig **sumber**; memberikannya juga kepada
  bone yang take-nya tidak menyentuh sama sekali menimpa bind pose rig tujuan —
  kerusakan yang sama dengan kanal translasi tetap, lewat pintu yang lain.
- **Rotasi masuk sebagai track kuaternion tersendiri**, seperti yang sudah
  dicatat `Clip.h` sejak E7.5. Memaksanya menjadi Euler menuntut memilih satu
  dari dua cabang yang sama sahnya pada tiap kunci, dan pilihan yang salah adalah
  tulang yang berputar sepenuh lingkaran di antara dua frame yang berdampingan.
  Translasi dan skala tetap kanal skalar — keduanya memang bentuk yang keluar
  dari pemanggangan, dan keduanya bisa disunting penyunting kurva yang sudah ada.
- **Seluruh take diimpor, bukan yang pertama.** Berkas Mixamo selalu membawa
  `Take 001` yang **tidak menganimasikan satu bone pun**, dan take yang berisi
  ada di urutan kedua. Mengambil yang pertama begitu saja mengimpor klip kosong,
  dan yang terlihat adalah aset yang bisa dipilih dan tidak melakukan apa-apa.
  Take yang kosong dibuang; kalau tersisa tepat satu, klipnya dinamai menurut
  berkasnya — nama take Mixamo adalah "mixamo.com", yang tidak memberi tahu
  apa-apa, sementara "Running.fbx" justru nama yang dipilih orang.

**Dua bug sungguhan, dan keduanya ditemukan oleh pembaca kedua.**

*Pertama, di mana konversi satuan disimpan.* Yang menentukan bukan besar
faktornya melainkan tempatnya tinggal, dan hanya satu tempat yang menghasilkan
transform lokal yang benar begitu dirangkai induk-ke-anak — yaitu persis yang
dilakukan `Pose::ComputeGlobal`. Terukur dengan panjang tulang sebagai patokan,
karena ia tidak boleh berubah apa pun posenya:

| cara menyimpan konversi | skala lokal bone teratas | selisih panjang tulang terbesar |
| --- | --- | --- |
| skala pada node root (`FbxSystemUnit::ConvertScene`) | 1,0 | **9900%** — sentimeter, seratus kali |
| faktor dikalikan ke tiap transform lokal | 0,01 | **100%** — 0,01 menumpuk tiap tingkat, rangkanya runtuh |
| konjugasi `C·M·C⁻¹`, `C = s·I` (yang dipakai) | 1,0 | **0,0000%** |

Yang ketiga bekerja karena konjugasi menskalakan **translasi saja** dan
meninggalkan basis rotasi/skala apa adanya. Mengalikan dari kiri saja — cara
kedua — menskalakan keduanya, dan skala 0,01 di tiap tingkat itulah yang
meruntuhkan rangkanya.

*Kedua, dan ini yang menarik: rangka hasil impor mesh ternyata sudah salah sejak
commit sebelumnya.* `LoadMesh` membiarkan konversi satuan tinggal di node root —
bone teratas mewarisi skala 0,01 dan seluruh keturunannya bertranslasi dalam
sentimeter. Rangkanya tetap benar **secara global**, dan uji yang membandingkan
posisi bone dengan kotak batas mesh karena itu lulus dengan tenang. Yang
menangkapnya adalah pembaca kedua transform lokal itu: klip membawa translasi
dalam meter berskala satu, dan bone yang tidak dianimasikan klip tetap memakai
bind pose-nya — jadi tulang-tulang itu terlempar seratus kali terlalu jauh.
Kedua importir sekarang memakai cara yang sama. Terukur: geometrinya tidak
berubah sama sekali — batas Y Bot tetap (−0,973, 0,000, −0,161)..(0,973, 1,805,
0,204), shader ball tetap 2,69 x 2,66 x 2,50 m — sementara skala lokal bone
teratas berpindah dari 0,0100 menjadi 1,0000.

**Kanal yang tetap sepanjang klip tidak diimpor sama sekali, dan itu syarat agar
klipnya bisa dipakai rig lain.** Pada animasi rangka, satu-satunya bone yang
translasinya benar-benar bergerak lazimnya adalah pinggul; translasi bone lain
tetap, dan nilainya adalah panjang tulang rig yang mengekspor klip itu.
Terukur: `Defeated.fbx` dan `Y Bot.fbx` sama-sama rig Mixamo bernama sama, tapi
Spine2-nya 0,09322 m lawan 0,13459 m — dengan kanal tetap ikut diimpor, memasang
klip Defeated ke Y Bot memendekkan tulang itu **30,7%**. Tanpanya, bone yang
tidak dianimasikan tinggal di bind pose rig tujuan, yaitu persis arti retargeting
yang dijanjikan `RetargetMap`: sudut yang sama, bukan titik yang sama. Rotasi
tetap **tetap** diimpor — rotasi bind adalah orientasi tulang, bukan panjangnya.

Terukur pada `Running.fbx`: **52 track rotasi, tepat tiga track skalar**
(translasi pinggul, 20 kunci masing-masing), 0,633 detik pada 30 fps. Ujinya
menyatakan invarian yang tidak bergantung pose mana pun — **panjang tulang di
bawah klip harus sama dengan panjang tulang bind rignya**, diperiksa pada lima
waktu di sepanjang klip, atas 320 perbandingan: selisih terbesar di bawah 0,1%
untuk kedua klip. Berkasnya tidak ikut di repo, jadi ujinya berjalan hanya bila
ditunjuk: `SIM_RIG_FBX=/path/rig.fbx SIM_CLIP_FBX=/path/Running.fbx ctest`.

**Selesai: karakter yang benar-benar bergerak di viewport**
(`Code/EditorFramework/{include/Sim/Editor/SkinnedPreview.h,src/SkinnedPreview.cpp}`,
`Code/Scene/{include/Sim/Scene/Components.h,src/Components.cpp}`,
`Code/EditorFramework/src/SceneView.cpp`, `Code/Assets/src/MeshImport.cpp`).
Inilah yang menutup rantai E8.4: klip yang diimpor dicuplik menjadi pose, pose
menjadi palet matriks kulit, dan paletnya masuk ke jalur GPU skinning yang sudah
ada — `AnimatorComponent` di sebuah entity, dan karakternya berlari.

- **`AnimatorComponent` tidak merujuk rangka.** Rangkanya datang dari aset mesh
  di `MeshRendererComponent`, karena itu satu-satunya rangka yang indeks
  bone-nya cocok dengan bobot skin yang sudah ada di GPU. Rujukan rangka
  tersendiri berarti dua sumber yang bisa berselisih, dan selisihnya muncul
  sebagai kulit yang mengikuti tulang yang salah.
- **Berkas `.fbx` boleh dirujuk langsung sebagai klip**, bukan hanya `.simanim`.
  Memaksanya lewat langkah impor lebih dulu berarti sebuah berkas Mixamo tidak
  bisa dicoba tanpa ritual — dan mencobanya justru hal pertama yang ingin
  dilakukan orang.
- **Waktunya keadaan runtime, tidak ikut diserialisasi.** Medan yang berubah tiap
  frame di dalam berkas level berarti level yang mengotori dirinya sendiri tanpa
  ada yang menyuntingnya, dan tanda "ada perubahan belum disimpan" yang menyala
  sendiri adalah tanda yang berhenti berarti. Ia juga dibungkus ke dalam durasi
  klipnya tiap frame: waktu yang tumbuh tanpa batas kehilangan presisi float-nya
  sesudah beberapa jam, dan yang terlihat adalah animasi yang makin tersendat
  pada editor yang dibiarkan terbuka.
- **Berjalan di editor, bukan hanya saat Play.** Animasi yang hanya bergerak di
  Play adalah animasi yang tidak bisa disetel — dan menyetelnya justru yang
  dilakukan orang di editor.
- **`LoadSkeleton` yang baru membaca rangka tanpa geometrinya.** Yang memutar
  klip butuh nama dan hierarki bone untuk memasang track, bukan vertexnya — dan
  vertexnya sudah ada di GPU, diurai perender. Terukur: rig Y Bot terbaca
  **6 ms** lewat jalur ini, terhadap 118 ms untuk mengurai mesh penuh. Ujinya
  menyatakan sifat yang benar-benar berarti: **urutan bone-nya harus sama persis
  dengan yang dihasilkan `LoadMesh`** — rangka yang sama tapi berurutan lain
  menghasilkan pose yang benar untuk tulang yang salah.
- **Palet yang panjangnya tidak cocok ditolak, bukan dipotong.** Panjang yang
  berbeda berarti pose itu milik rangka lain; bind pose yang jelas-jelas diam
  jauh lebih mudah dilacak daripada kulit yang terpelintir.

Diuji ujung ke ujung: entity ber-`MeshRenderer` dan ber-`Animator` yang menunjuk
`Y Bot.fbx` dan `Running.fbx` menghasilkan palet 65 matriks yang **bukan** bind
pose pada frame pertama — palet satuan terlihat persis sama dengan animasi yang
berjalan benar, jadi itu diperiksa tersendiri — berubah sesudah seperempat detik,
benar-benar diam saat `playing` dimatikan, dan waktunya tetap terbungkus di dalam
durasi klip sesudah 200 langkah. Animator pada mesh tanpa rangka tidak
menghasilkan palet sama sekali.

**Selesai: sistem task pose, mengikuti Esoterica**
(`Code/Animation/{include/Sim/Animation/PoseTask.h,src/PoseTask.cpp}`, acuan
`/home/arie/SDK/Esoterica/Code/Engine/Animation/TaskSystem`).

**Inti rancangan yang membedakan Esoterica dari runtime animasi yang lazim:
simpul graph tidak menghasilkan pose.** Ia mendaftarkan sebuah *task* dan
mengembalikan indeksnya; pose baru terbentuk saat daftar task dijalankan. Yang
didapat ada tiga, dan ketiganya tidak mungkin didapat kalau pencampuran terjadi
di dalam simpul:

- Pembaruan graph menjadi murah dan bebas pose. Simpul yang bobotnya nol tidak
  pernah mendaftarkan task, jadi cabang yang tidak terlihat tidak pernah
  dicuplik — sementara runtime yang mencampur di tempat sudah terlanjur mencuplik
  sebelum tahu bobotnya nol.
- Daftar task bisa dijalankan pada rangka LOD yang berbeda, atau tidak
  dijalankan sama sekali untuk karakter di kejauhan, tanpa menyentuh graph.
- Daftar task adalah data: bisa diserialisasi, dikirim lewat jaringan, atau
  direkam untuk diputar ulang saat mencari sebab sebuah pose yang salah.

Bentuknya **DAG, bukan pohon.** Sebuah task boleh dirujuk lebih dari satu task
lain, dan itulah yang membuat satu klip yang dicuplik sekali bisa masuk ke dua
blend tanpa dicuplik dua kali — sesuatu yang tidak punya tempat untuk dinyatakan
pada runtime yang mencampur pose di dalam simpulnya. Diuji: task bersama
dijalankan tepat sekali, dan task yang tidak menyumbang ke akar tidak dijalankan
sama sekali.

**Kolam buffer pose, dan pelepasan tepat waktu.** Blend menulis ke buffer masukan
pertamanya, dan `TaskSystem` menghitung berapa banyak task yang membutuhkan hasil
tiap task supaya buffer dilepas begitu pemakai terakhirnya selesai. Terukur:
rantai sembilan blend berturut-turut memakai **puncak dua buffer**, bukan
sembilan — kolam yang tumbuh linear terhadap kedalaman graph adalah kolam yang
tidak melepas apa pun. Sesudah eksekusi, pemakaiannya kembali nol.

Akar yang tidak sah menghasilkan **bind pose, bukan pose kosong**: karakter yang
runtuh ke titik asal jauh lebih sulit dilacak daripada karakter yang berdiri diam,
yang langsung terbaca sebagai "graph-nya tidak menghasilkan apa-apa".

**Yang belum ada, dan urutannya sudah jelas.** Sistem task adalah lantainya;
di atasnya menyusul simpul graph berpasangan Definition/instance — definisi yang
tidak berubah dan dibagi seluruh karakter, instance yang dialamati indeks dan
di-resolve menjadi pointer sekali saat instantiasi — beserta pemisahan
`PoseNode`/`ValueNode` dan `GraphPoseNodeResult` yang mengembalikan indeks task,
delta root motion, dan rentang event. Sesudahnya sync track dan state machine
sebagai satu jenis simpul di antara yang lain.

`AnimationGraph` bergaya Mecanim yang sudah ada — parameter, lapis, state,
transisi, blend tree — **sengaja belum disentuh.** Ia berjalan, punya ujinya,
punya panelnya, dan punya serialisasinya; menggantinya sebelum runtime penggantinya
lengkap berarti mematikan satu-satunya jalur animasi yang bekerja demi jalur yang
belum bekerja.

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

**Langitnya mengikuti `/home/arie/SDK/atmosphere-bac`** — implementasi
Bruneton-Hillaire (transmittance LUT, multiscattering LUT, sky-view LUT, aerial
perspective) beserta awan volumetriknya. Referensi lokal, bukan dependensi; lihat
`docs/DEPENDENCIES.md`.

**Ditambah editor Time-of-Day ala CryEngine**: kurva per-parameter atmosfer
terhadap jam, dan siklus siang-malam yang berjalan real-time sehingga mataharinya
benar-benar berputar. Ini yang menyambungkan langit ke sistem yang sudah ada —
arah dan radiance matahari sudah mengalir dari `LightComponent` ke cascade
bayangan sejak E8.3, jadi memutar matahari otomatis memutar bayangannya.

**Selesai** (`Code/Render/{include/Sim/Render/,src/}TimeOfDay.{h,cpp}`,
`Code/Editor/src/TimeOfDayPanel.cpp`). Yang ada sekarang:

- **Geometri matahari sungguhan**, bukan busur yang digambar tangan: deklinasi
  dari hari ke berapa dalam setahun, sudut jam dari jamnya, lintang dari
  tempatnya. Busur yang digambar tangan tidak bisa menyatakan musim, dan bayangan
  tengah harinya jatuh ke arah yang salah untuk setiap lokasi kecuali yang
  kebetulan dipakai saat menggambarnya. Terukur di editor: Jakarta (−6,2°) pada
  titik balik Juni menunjukkan altitude tengah hari 60,4° terhadap 60,35° dari
  hitungan tangan, dan 16:28 menunjukkan 18,3° terhadap 18,26°.
- **Vektornya dihitung langsung dalam kerangka horizontal**, bukan lewat azimut
  lalu dikembalikan ke vektor. Rumus azimut punya pembagian yang tidak
  terdefinisi tepat di zenit — dan di lintang rendah zenit adalah keadaan yang
  pasti dilewati. `LookRotation` menangani hal yang sama untuk rotasi entity-nya.
- **Kurva siklis atas 24 jam.** Ruas dari kunci terakhir ke kunci pertama
  melintasi tengah malam, dan itulah ruas yang paling mudah terlupa; kurva yang
  berhenti di jam 24 menjadikan tengah malam sebuah loncatan.
- **Jam terpisah dari kurvanya.** Kurva adalah data yang disunting dan disimpan,
  jam adalah keadaan yang berjalan — menyatukannya berarti menggulung waktu ikut
  mengubah berkas. Terukur: 10 menit per hari memajukan jam 16:32 → 16:42 dalam
  empat detik nyata.
- **Peredam terbit-terbenam ±5,7°**, bukan pemutus mendadak: matahari yang
  dimatikan tepat saat menyentuh horizon membuat seluruh adegan berkedip dalam
  satu frame, tepat pada saat yang paling diperhatikan orang.
- **Sakelar "Drive the sun" mati secara bawaan.** Editor yang diam-diam menimpa
  nilai yang baru saja disetel tangan adalah editor yang tidak bisa dipakai
  menyetel apa pun.

Kurva yang ada baru yang benar-benar tersambung ke sesuatu — warna dan intensitas
matahari, warna langit di zenit dan horizon. Parameter Bruneton (Rayleigh, Mie,
ketinggian lapisan) menyusul bersama pass langitnya: kurva tanpa pembaca adalah
kurva yang tidak pernah diuji.

**Selesai: rantai HDR, ACES, dan eksposur otomatis**
(`Code/Render/{include/Sim/Render/ToneMap.h,src/ToneMap.cpp,src/PostProcess.{h,cpp}}`,
`Shaders/{fullscreen.vert,tonemap_common,lum_seed.frag,lum_reduce.frag,exposure.frag,tonemap.frag}.slang`).

`ViewportDesc::exposure` **sudah dihapus**, seperti yang direncanakan di sini
sejak E8.3. Yang menggantikannya:

- **Target warna adegan menjadi `R16G16B16A16_SFLOAT`.** Seluruh pass adegan
  menulis radiance apa adanya ke sana; yang memetakannya ke layar adalah pass
  `tonemap` di ujung graph. Lampu tidak lagi dikalikan pengali eksposur di dua
  tempat.
- **Encode sRGB, yang sebelumnya tidak dilakukan siapa pun.** Swapchain-nya
  UNORM — sengaja, karena ImGui menggambar UI dengan warna yang sudah dalam
  ruang sRGB — jadi tidak ada satu pun tempat lain yang meng-encode. Sebelum
  pass ini viewport menampilkan nilai linier apa adanya: seluruh adegan lebih
  gelap dan lebih kontras daripada yang dimaksud, merata, sehingga tidak
  terlihat sebagai kesalahan melainkan sebagai gaya. Terukur pada adegan bawaan:
  muka kotak yang tersinari 98/255 menjadi 191/255, dan kedua mukanya tetap
  terbedakan (191 lawan 171) alih-alih terpotong putih.
- **Pengukuran luminansi lewat rantai reduksi grafis, bukan histogram compute.**
  Bukan karena histogram lebih buruk melainkan karena belum ada satu pun compute
  pipeline di engine ini, dan yang pertama akan membawa serta storage buffer,
  barrier compute, dan seluruh jalur yang belum pernah dijalankan sekali pun.
  Rantai reduksi memakai bentuk yang sudah berjalan di `DepthPyramid`, dan
  rata-rata geometrik yang dihasilkannya justru yang diinginkan: satu sorotan
  spekular tidak menggelapkan seluruh adegan.
- **Petak awal 256×256 berukuran tetap, bukan seukuran viewport.** Penurunan 2×2
  atas ukuran ganjil harus membuang satu baris atau menimbang tiga texel, dan
  keduanya memasukkan bias yang bergantung ukuran jendela — eksposur yang
  bergeser sedikit saat pemisah dock digeser tidak akan pernah dicurigai siapa
  pun.
- **Dua tetapan waktu adaptasi**, bukan satu: satu tetapan memaksa memilih antara
  kedipan saat berbalik menghadap matahari dan keterlambatan saat masuk ke
  lorong.

Diverifikasi di editor, bukan hanya di uji:

| Yang diukur | Hasil |
| --- | --- |
| Enam stop EV manual (−3…+3), piksel terhadap model CPU | selisih terbesar **1/255** |
| Adaptasi cepat (τ = 0,4 s) sesudah jeda masukan 0,15 s | selisih terbesar **0,02** dari pecahan |
| Galat validation layer | **0** |

**Selesai: langit Bruneton-Hillaire**
(`Code/Render/src/SkyAtmosphere.{h,cpp}`,
`Shaders/sky_{common,transmittance.frag,multiscatter.frag,view.frag,draw.frag}.slang`),
mengikuti `/home/arie/SDK/atmosphere-bac`.

**Tiga LUT dengan tiga umur yang berbeda, dan itu inti rancangannya.**
Transmitansi hanya bergantung pada parameter atmosfer, jadi ia dibangun sekali.
Multiscattering bergantung pada matahari, jadi ia dibangun ulang saat matahari
bergeser lebih dari setengah derajat — bukan "saat berubah sama sekali", karena
matahari yang digerakkan slider bergeser sedikit tiap frame dan membangun ulang
untuk pergeseran sekecil itu berarti membangun ulang tiap frame. Sky-view
bergantung pada matahari **dan** ketinggian kamera, jadi ia dibangun tiap frame —
tapi ukurannya 192×108, sepersekian dari satu layar.

Cakram matahari digambar di dalam pass ini, bukan sebagai objek: ia berada di
jarak tak hingga, dan objek di jarak tak hingga adalah objek yang setiap sistem
culling, bayangan, dan depth harus punya kekecualian untuknya.

Terukur di editor, dengan matahari disetir panel Time-of-Day:

| Jam | Nisbah merah/biru di horizon |
| --- | --- |
| 12:00 | 0,52 (biru) |
| 16:00 | 0,70 |
| 17:30 | **1,30** (jingga) |

Biaya pass `sky`: **0,29 ms**. Nol galat validation layer.

**Sakelarnya sekarang menyala secara bawaan.** Ia sempat mati, dan alasannya
benar pada waktunya: langit menggantikan warna latar yang disetel pemakai, dan
editor yang mengganti latar sendiri tanpa diminta sulit dibedakan dari editor
yang rusak. Syarat yang dicatat waktu itu — "sampai matahari benar-benar disetir
Time-of-Day" — sudah dipenuhi. Yang membalik timbangannya: langit yang mati
secara bawaan adalah langit yang tidak pernah dilihat siapa pun, dan cacat pada
sesuatu yang tidak pernah terlihat adalah cacat yang tidak pernah ditemukan.

Terukur pada kamera bawaan yang diarahkan ke atas: zenit (11, 33, 68) dengan
nisbah merah/biru **0,16**, melunak mulus ke kabut horizon (140, 154, 159)
bernisbah **0,88**, lalu gelap di bawah horizon. Kamera bawaan sendiri menunduk
26°, jadi yang terlihat tanpa memutar pandangan hanyalah pita 0–7° di atas
horizon — dan pita itu memang berkabut putih, bukan biru. Langit birunya ada di
atas, seperti seharusnya.

**Selesai: aerial perspective** (`Code/Render/{include/Sim/Render/Atmosphere.h,
src/Atmosphere.cpp,src/SkyAtmosphere.{h,cpp}}`,
`Shaders/sky_aerial{,_apply}.frag.slang`). LUT 3D 32×32×32: dua sumbu layar, satu
sumbu jarak.

- **Satu draw per slice, bukan satu compute dispatch.** Acuannya memakai compute
  shader; engine ini sengaja belum punya satu pun compute pipeline, dan yang
  pertama akan membawa serta storage image, barrier compute, dan seluruh jalur
  yang belum pernah dijalankan sekali pun. `VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT`
  membuat tiap slice sebuah lampiran warna yang sah, sementara view 3D di atas
  image yang sama tetap memberi penyaringan trilinear — termasuk antar-slice,
  yang justru arah tempat kabut paling mudah menjadi pita.
- **Kompositnya blending, bukan salinan ping-pong.** Yang harus terjadi tiap
  piksel adalah `warna × transmitansi + hamburan`, dan itu persis
  `dst = src + dst × src.a` asalkan transmitansinya satu skalar di kanal alfa.
  Bentuk mana pun yang membaca gambar HDR lewat descriptor sambil menulisinya
  adalah perilaku yang tidak terdefinisi, dan menghindarinya menuntut gambar HDR
  kedua seukuran penuh beserta satu salinan tiap frame. Yang ditukar: pemerahan
  pada jalur yang sangat panjang. Acuan Bruneton-Hillaire menukar yang sama.
- **Jangkauan LUT mengikuti bidang jauh kamera, bukan 4 km per slice.** Angka
  acuan itu masuk akal untuk adegan seluas planet; untuk adegan sepanjang dua
  kilometer ia menaruh seluruh yang terlihat di dalam slice pertama — kabut lalu
  menjadi satu nilai tetap yang tidak berubah terhadap jarak sama sekali, yang
  tampak seperti kabut yang "tidak bekerja" alih-alih seperti LUT yang salah
  skala.
- **Sebaran slice kuadratik**, dan ia harus membalik dengan tepat di kedua arah.
  Pemetaan yang meleset tidak menghasilkan galat apa pun, hanya kabut yang
  pekatnya benar pada jarak yang salah.

Uji yang menentukan ada di acuan CPU-nya: **integral 0→D harus sama dengan
rantai 0→d lalu d→D**. LUT ini berhenti di jarak maksimumnya dan yang di baliknya
diwarnai pass langit; kalau integral yang dipotong dua tidak sama dengan yang
utuh, kedua bagian itu tidak akan bertemu — dan yang terlihat adalah garis
jahitan tepat di tempat terrain menyentuh langit. Terukur: transmitansi cocok
dalam 0,2%, hamburan dalam 1%.

Terukur di editor:

| Yang diukur | Hasil |
| --- | --- |
| Biaya pass `aerial` (LUT + komposit) | **0,117 ms** |
| Galat validation layer | **0** |
| Siluet geometri terhadap latar, pada uji gerbang depth | 9/26/53 lawan 47/68/103 |

**Satu hal yang harus dikatakan apa adanya: di adegan bawaan editor, kabut ini
nyaris tak terlihat, dan itu benar.** Yang tampak sebagai "tanah" di viewport
adalah pass grid, dan grid sengaja tidak menulis depth sama sekali — jadi pass
kabut, yang melewati piksel bidang jauh, tidak menyentuhnya. Yang tersisa untuk
dikabuti hanyalah dua mesh berjarak beberapa meter, dan udara sepanjang beberapa
meter memang tidak mengubah apa pun. Terukur pada haze 20×: hanya 3.703 piksel
berubah, seluruhnya pada geometri, seluruhnya ke arah yang benar (lebih terang
dan lebih biru). Pass ini baru punya sesuatu untuk dikerjakan begitu E8.5 membawa
terrain.

**Selesai: awan volumetrik**
(`Code/Render/{include/Sim/Render/CloudNoise.h,src/CloudNoise.cpp,src/SkyAtmosphere.{h,cpp}}`,
`Shaders/sky_clouds.frag.slang`). Raymarch melalui cangkang bola antara dua
ketinggian, disinari matahari lewat light march pendek, dikomposit dengan
blending yang sama dengan kabut.

- **Derau 3D-nya dibangkitkan di CPU, bukan di compute shader.** Alasan
  langsungnya sama dengan aerial perspective; yang didapat lebih dari sekadar
  menghindari jalur baru. Derau menjadi kode biasa yang punya nilai balik, jadi
  sifat yang menentukan benar-tidaknya bisa dinyatakan sebagai uji: **ia harus
  menyambung di ketiga tepinya.** Lapisan awan membentang puluhan kilometer dan
  volumenya 64 texel, jadi ia diulang berkali-kali; derau yang tidak menyambung
  menaruh tepi tajam pada setiap batas pengulangan, dan yang terlihat adalah
  kisi garis lurus di langit — teratur sempurna, dan tidak mungkin dikira awan.
- **Dibagi ke 24 thread.** 64³ satu thread berharga 1,8 detik yang dibayar
  setiap kali editor dibuka; dibagi per irisan z ia menjadi 218 ms, dan hasilnya
  sama persis karena tiap texel berdiri sendiri.
- **64³, bukan 128³ seperti acuannya.** Di compute shader 128³ berharga beberapa
  milidetik; di CPU ia berharga 1,3 detik. Yang hilang pada awan berskala
  kilometer adalah rincian yang toh sudah dikikis volume rincian.
- **Cangkang bola, bukan kotak.** Kotak sejajar sumbu punya tepi, dan tepi itu
  jatuh di dalam pandangan sebagai garis lurus tempat awan berhenti mendadak —
  tepat di dekat horizon, yaitu tempat lapisan awan paling banyak terlihat.

**Dua kesalahan yang keduanya muncul sebagai langit yang cerah**, bukan sebagai
galat — dan keduanya baru ketahuan setelah sebaran nilainya diukur, bukan
ditatap:

- Bentuk pertama `PerlinWorley` membagi dengan Worley. Perlin dan Worley
  sama-sama bermean sekitar 0,5, jadi pembilangnya sama seringnya negatif:
  **median kanal itu nol**, separuh volume terjepit habis. Bentuk yang benar
  memetakan Perlin ke rentang yang dasarnya digeser Worley.
- Derau FBM memusat di sekitar mean-nya. Terukur sebelum peregangan ada, kanal
  gabungannya membentang 0,09–0,71 bermedian 0,24 — sementara cakupan 0,45
  berarti ambang 0,55, yang hanya dilewati **0,2%** volume. Sesudah tiap kanal
  diregangkan ke seluruh rentangnya: median 0,53 dan 40% melewati ambang. Acuan
  itu menormalkan deraunya lewat pass tersendiri, dan alasannya sama.

Terukur di editor (RTX 2060, viewport 1277×541):

| Yang diukur | Hasil |
| --- | --- |
| Biaya pass `clouds` | **1,534 ms** |
| Biaya pass `sky` / `aerial` di frame yang sama | 0,131 / 0,133 ms |
| Pembangkitan volume derau saat start (release) | **259 ms** |
| Galat validation layer | **0** |

**Mati secara bawaan, dan alasannya angka di atas.** Langit dan kabut berharga
sepersepuluh milidetik; awan berharga sepuluh kali lipatnya, dan pada 1080p
penuh ia mendekati 4–5 ms. Sakelar yang menyala sendiri membuat seseorang
membayar harga itu tanpa pernah memintanya.

**Yang belum benar dan sudah terlihat:** geseran acak per piksel menukar pita
dengan bintik, dan bintiknya terlihat pada awan yang tipis. Yang menghilangkannya
adalah blue noise beserta akumulasi temporal — dan akumulasi temporal adalah
pekerjaan yang sama dengan TAA, yang masih ada di daftar E8.8. Menyelesaikannya
dua kali adalah menyelesaikannya sekali dengan cara yang salah.

**Selesai: skybox HDRI sebagai sumber langit kedua**
(`Code/Render/{include/Sim/Render/Ibl.h,src/Ibl.cpp,src/SkyAtmosphere.{h,cpp}}`,
`Shaders/sky_hdri.frag.slang`). `ViewportDesc::skySource` memilih antara atmosfer
Bruneton dan sebuah berkas `.hdr` equirectangular.

**Alasannya biaya, dan biayanya terukur.** Pada viewport 1277×541 yang sama:

| Pass | Atmosfer + awan | HDRI |
| --- | --- | --- |
| `sky` | 0,131 ms | **0,047 ms** |
| `clouds` | 1,534 ms | — |
| `aerial` | 0,133 ms | — |
| **Total GPU frame** | **2,221 ms** | **0,429 ms** |

- **HDRI mematikan kabut dan awan dengan sendirinya, bukan lewat sakelar
  terpisah.** Keduanya milik model atmosfer prosedural: menumpuk kabut Bruneton
  di atas foto langit berarti menghitung udara yang sama dua kali dengan dua
  warna yang berlainan, dan yang terlihat adalah kabut yang warnanya tidak cocok
  dengan langit di belakangnya. Editor mengatakannya, tidak menyembunyikannya —
  sakelar yang hilang tanpa penjelasan terbaca sebagai editor yang rusak.
- **Pengalinya sendiri, bukan `skyIntensity`.** Atmosfer Bruneton menghasilkan
  angka dalam satuannya sendiri sehingga pengalinya berguna di sekitar 20;
  berkas HDR sudah berisi radiansi sehingga pengalinya berguna di sekitar 1.
  Satu angka untuk keduanya berarti berpindah sumber diam-diam mengalikan langit
  dua puluh kali — yang tidak terlihat sebagai pengali yang salah melainkan
  sebagai eksposur otomatis yang "menggelapkan segalanya".
- **RGBA16F, bukan RGBA32F.** Peta 4096×2048 berharga 64 MB sebagai half dan
  128 MB sebagai float penuh, dan yang dibedakan mata pada radiansi langit jauh
  di bawah ketelitian half.
- **`SetHdri` tidak melakukan apa-apa bila jalurnya tidak berubah**, karena ia
  dipanggil tiap frame. Tanpa penjagaan itu, editor mendekode berkas enam
  megabyte enam puluh kali per detik — yang muncul bukan sebagai galat melainkan
  sebagai editor yang berjalan tiga frame per detik.

**Sekaligus menutup lubang yang dicatat `Ibl.h` sejak awal.** Komentar di atas
`GradientSky` menyebut "peta lingkungan sungguhan datang bersama importir tekstur
HDR"; `EquirectEnvironment` adalah importir itu, dan karena ia
mengimplementasikan `IEnvironmentSampler` yang sama, seluruh rantai IBL — SH
iradiansi, prefilter spekular — menerimanya tanpa satu baris pun berubah. Diuji
begitu: SH yang diproyeksikan dari peta terang-di-atas menghasilkan iradiansi
yang lebih besar di normal ke atas daripada ke bawah.

Uji yang menentukan tetap pemetaannya: **arah→uv dan uv→arah harus saling
membalik.** Yang meleset tidak menghasilkan galat apa pun, hanya matahari di
dalam HDRI yang muncul di arah berbeda dari matahari yang menyinari adegan — dan
itu terlihat sebagai bayangan yang arahnya aneh, bukan sebagai peta yang
terpasang terbalik. Diuji juga bahwa U membungkus sementara V menjepit: menjepit
U meninggalkan jahitan tegak selebar satu texel dari zenit ke nadir, dan
membungkus V mengambil warna kutub seberang tepat di zenit.

**Dua hal yang belum ada, dan keduanya disengaja.** HDRI belum menyalakan IBL —
ia baru menjadi latar, bukan sumber cahaya, jadi objek masih disinari matahari
directional saja; jalurnya sudah terbuka lewat `IEnvironmentSampler` tapi
pembakaran IBL-nya pekerjaan tersendiri. Dan jalur berkasnya belum tersimpan
antar-jalan, sama dengan seluruh pengaturan langit yang lain.

**Selesai: bloom** (`Code/Render/{include/Sim/Render/Bloom.h,src/Bloom.cpp}`,
`Shaders/{bloom_down,bloom_up}.frag.slang`). Penurunan 13 cuplikan (Jimenez)
lalu penaikan tenda 3×3, dengan pembobotan Karis pada penurunan pertama.

Dua keputusan yang punya kasus gagalnya sendiri, dan keduanya sempat saya buat
terbalik:

- **Di dalam rantai, penaikan memadu.** Yang menambahkan tiap tingkat menaikkan
  energi seluruh gambar, dan energi itu lalu diukur eksposur otomatis, yang
  menurunkan eksposur, yang menurunkan bloom — keduanya saling mengejar dan yang
  terlihat adalah kecerahan yang bergoyang pelan tanpa sebab. Ujinya: gambar
  konstan lewat seluruh rantai tanpa berubah.
- **Di ujung, komposit menambahkan.** Ini keharusan yang mengikuti dari
  ambangnya: rantai ini berambang, jadi di seluruh bagian yang tidak berpendar
  isinya nol. Bentuk pertama saya memadu di sini juga, dan hasilnya terukur di
  editor — latar 62/255 menjadi 46/255 tanpa satu pun halo yang muncul, yaitu
  seluruh adegan meredup begitu bloom dinyalakan.

Satu lagi yang hanya terlihat sebagai gambar, bukan sebagai galat: uv segitiga
penutup layar membentang 0..1 atas petak yang **digambar**, sedangkan petak
terpakai sumbernya hanya sebagian dari alokasinya — `RenderTarget` memang
mengalokasi lebih besar supaya menyeret pemisah dock tidak mengalokasi ulang.
Menjepit uv ke batas petak (bukan menskalakannya) membuat separuh keluaran
membaca baris yang sama berulang-ulang: adegan teregang dan terulang di dalam
pendarannya.

Terukur di editor pada ambang 1,0: tepat di luar tepi kotak terang, bloom
menambahkan **+22/255**, meluruh mulus ke +1 dalam 30 piksel, dan latar di bawah
ambang tidak tersentuh sama sekali.

**Satu hal yang terukur saat langit dinyalakan, dan belum dikerjakan: rantai
eksposur dan ACES belum dikalibrasi terhadap satu sama lain.** `AutoExposure`
menyasar luminansi rata-rata 0,104 — angka yang benar, dan memang angka
pengukur cahaya sungguhan (K = 12,5, ISO 100, formulasi berbasis kejenuhan).
Tapi kurva ACES memetakan 0,104 menjadi **0,045**, yaitu sRGB 59/255. Abu-abu
tengah karena itu mendarat sekitar dua stop lebih gelap daripada tempat yang
lazim (0,18 → 0,106, atau sRGB 93/255).

Terukur pada adegan bawaan dengan langit menyala: rata-rata geometrik luminansi
tampilan **0,0463**, terhadap ACES(0,104) = 0,0452 dari model CPU. Selisihnya 2%
— artinya rantainya bekerja persis seperti yang dirancang, dan yang salah bukan
implementasinya melainkan **sambungan antara sasaran eksposur dan kurva nada**.
Perbaikan yang lazim adalah mengalikan masukan ACES dengan tetapan kalibrasi
sebelum kurvanya. Dicatat di sini, bukan dikerjakan sekarang: ia menyentuh setiap
piksel yang sudah diukur di tabel-tabel di atas, jadi ia layak menjadi
perubahannya sendiri beserta pengukurannya sendiri.

**Satu hal lagi yang belum benar dan sudah tercatat:** lampu belum memakai satuan
fotometrik. Matahari bawaan beradiansi 3,0, bukan sepuluh ribu cd/m², jadi EV100
yang berguna di sini berada di sekitar nol alih-alih 13–15. Nilai bawaan
`manualEv100` pertama saya adalah 13, dan hasilnya viewport hitam pekat tanpa
satu pun galat. Label "EV100" baru berarti harfiah begitu lampu memakai satuan
sungguhan.

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

**Urutan yang diminta: GI lebih dulu, lalu E8.8.**

Ketiga hal di bawah sudah terjawab, dan dua di antaranya bermuara ke tempat yang
sama — M0.

**Anggaran tidak bisa didamaikan di atas kertas; ia harus diukur.** 3,0 ms dari
frame 16,67 ms menyisakan 13,67 ms untuk terrain, 200 ribu instance vegetasi, 20
lampu berbayang, karakter, partikel, dan seluruh post-process. Apakah itu cukup
bukan hal yang bisa disimpulkan dengan menalar — dan adegan ujinya belum ada,
karena E8.4–E8.7 belum dikerjakan. Jadi yang bisa diputuskan sekarang bukan
angkanya melainkan **alatnya**: pengukuran GPU per pass, yang memang sudah
diminta M0 dan kini ada. Angka yang tidak bisa diukur bukan anggaran melainkan
harapan.

Pengukuran pertama pada adegan hampir kosong (RTX 2060, 1277×614): total
**0,315 ms** — shadow-cascades 0,151, grid 0,070, shadow-atlas 0,052,
forward-opaque 0,025, depth-prepass 0,014, sisanya di bawah 0,005. Itu garis
dasarnya; yang menarik adalah bagaimana ia tumbuh saat E8.4 ke atas mendarat.

**Pemilih backend harus terlihat sejak M0, bukan M7.** Rencana menaruh override
manual di M7, dan itu terlambat: begitu `RayQueryBackend` ada, pengujian
sehari-hari di mesin ini otomatis pindah ke sana — dan jalur SDF, yang justru
harus bekerja di GPU tanpa RT core, berhenti dijalankan siapa pun tanpa ada yang
menyadarinya. Urutan milestone-nya sendiri sudah benar (SDF dulu di M1–M2, ray
query terakhir di M7); yang ditambahkan hanyalah bahwa pemilihnya wajib eksplisit
dan tampak, sejak awal.

Kekhawatirannya terkonfirmasi begitu deteksinya jalan: `VK_KHR_ray_query` dan
`VK_KHR_acceleration_structure` keduanya tersedia di mesin ini, jadi **Auto
memilih ray query** — dan tanpa sakelar paksa, jalur SDF tidak akan pernah
dijalankan sekali pun selama pengembangannya.

#### M0 — apa yang sudah ada

- **`rhi::GpuProfiler`** dan tabel waktu per pass. Lihat catatan anggaran di atas.
- **`ITraceBackend`** beserta `NullTraceBackend` yang selalu meleset. Null bukan
  penambal sementara: ia yang membuat seluruh sistem di atasnya bisa dibangun dan
  diuji sebelum ada satu pun ray yang ditembakkan, dan ia jawaban yang benar saat
  GI dimatikan.
- **Pemilih backend** dengan Auto / Force SDF / Force ray query, yang melaporkan
  backend yang **akhirnya** dipakai beserta alasannya. Permintaan dan hasil
  sengaja dua hal yang berbeda: memaksa ray query di perangkat tanpa dukungan
  diturunkan ke SDF — editor yang menolak jalan tidak bisa dipakai menyunting
  data — tapi penurunannya disebutkan, tidak didiamkan.
- **Daftar debug view** (albedo, normal, iradiansi mentah, ray count, heatmap
  langkah march), terdaftar sekarang walau isinya menyusul. Alat diagnostik yang
  ditambahkan belakangan adalah alat yang tidak dipakai saat ia paling
  dibutuhkan, yaitu ketika sistemnya masih salah dan belum jelas salahnya di mana.

`TraceResult::steps` sudah ada sejak sekarang dan bernilai nol pada backend null
— heatmap yang kosong adalah jawaban benar untuk backend yang memang tidak
melangkah, bukan angka yang mengarang.

#### M1 — SDF clipmap global · ✅ selesai

**Sudah ada:** `SdfClipmap` — pengalamatan, pemilihan kaskade, toroidal scroll,
dan penyandian jarak. Seluruhnya teruji tanpa GPU, mengikuti pola yang sama
dengan cascade bayangan, cluster, dan IBL.

**Teksturnya tidak bergerak; titik asalnyalah yang bergerak.** Kamera yang maju
satu voxel hanya menuntut satu lempeng tepi ditulis ulang. Tanpa itu, sebuah
kaskade 128³ berarti 2 juta voxel ditulis ulang setiap frame kamera bergerak,
dikali tiga kaskade — dan anggaran 0,4 ms untuk pembaruan clipmap habis sebelum
satu ray pun ditembakkan. Diuji langsung: gerak satu voxel menghasilkan 32×32
voxel basi, bukan 32³.

**Tiap kaskade dikancing ke ukuran voxelnya sendiri**, bukan ke yang terhalus.
Mengancing semuanya ke voxel terhalus membuat kaskade kasar bergeser pecahan
voxelnya sendiri, dan seluruh isinya jadi basi setiap frame. Diuji: gerak 0,1 m
menggeser kaskade 0 dan tidak menyentuh kaskade 1 sama sekali.

**Pengali antar-kaskade dipaksa pangkat dua.** Bukan kerapian: kisi kaskade
kasar harus selaras dengan yang halus, kalau tidak sebuah titik yang berpindah
kaskade jatuh di tempat yang berbeda, dan jahitannya bergerak saat kamera maju.

Dua jebakan bilangan bulat yang masing-masing baru muncul setelah kamera
melewati titik nol dunia, dan keduanya punya test sendiri:

- `%` C++ memberi sisa **bertanda** — `−1 % 128` adalah `−1`, bukan `127` — dan
  indeks negatif membaca di luar tekstur.
- Konversi float→int memotong **ke arah nol**, jadi −0,05 m dan +0,05 m jatuh ke
  voxel yang sama. Kedua sisi origin lalu berbagi satu voxel.

**Yang disimpan hanya pita tipis di sekitar permukaan**, bukan seluruh
jangkauan. Delapan bit yang dipaksa mewakili seluruh kaskade menghasilkan
langkah kuantisasi hampir satu meter di kaskade terkasar — sphere tracing dengan
langkah sebesar itu menembus dinding. Jaraknya **bertanda**, dengan nol di
tengah rentang: jarak tak bertanda membuat sphere tracing tidak bisa tahu ia
sudah di dalam benda, dan ray yang mulai di dalam dinding tidak pernah keluar.

Lompatan yang lebih jauh daripada lebar kaskade menulis ulang seluruh volume
sekali, bukan tiga lempeng yang saling tumpang tindih penuh — yang justru lebih
mahal.

**Sebuah lempeng di dunia bisa terbelah di dalam tekstur.** Pengalamatan
toroidal berarti wilayah yang bersambung di dunia melompati tepi tekstur dan
muncul kembali di sisi seberangnya — sampai delapan potongan bila ketiga
sumbunya membelah. Menyalinnya sebagai satu kotak akan menimpa texel milik
bagian dunia yang sama sekali lain.

Bentuk pertama `SplitWrapped` punya cabang khusus untuk wilayah selebar
teksturnya, dan cabang itu menghasilkan kotak di luar batas: texel 32 ditambah
64 adalah 96 pada tekstur selebar 64. **Segfault, bukan gambar yang salah** —
dan test yang menangkapnya adalah yang mengisi seluruh volume, yaitu jalur yang
dijalankan setiap kali kaskade ditempatkan pertama kali. Cabangnya dibuang;
lingkaran pemecahnya sudah menangani kasus itu dengan benar tanpa cabang apa pun.

**`SdfVolume` adalah acuan kebenaran, bukan yang dipakai menggambar.** Yang
menggambar membaca tekstur volume di GPU; yang di CPU menyimpan byte yang sama
dengan rumus yang sama, cukup lambat untuk test dan cukup sederhana untuk
dibaca. Kriteria selesai M2 menuntut backend SDF lulus uji ray tunggal terhadap
referensi CPU — dan referensi itu tidak ada gunanya kalau ia ditulis dari
pemahaman yang berbeda.

Volume yang belum diisi bernilai jenuh **positif**, bukan nol. Nol berarti
"permukaan ada di mana-mana" dan membuat sphere tracing berhenti di langkah
pertama; jenuh positif berarti ruang kosong, yang memang keadaan awal yang
benar.

**Sphere tracing, bukan langkah tetap.** Langkah tetap menuntut langkah sekecil
voxel supaya tidak menembus dinding tipis — ratusan langkah untuk melintasi satu
kaskade. Diuji: menemukan bola pada jarak 1,5 m dengan voxel 5 cm dalam kurang
dari 20 langkah, bukan 30. Ambang "sudah sampai" diikat ke ukuran voxel kaskade
tempat titiknya berada, bukan ke satu angka tetap: ambang yang cukup halus untuk
kaskade terdekat membuat kaskade terkasar berputar ratusan langkah.

`TraceResult::steps` dilaporkan juga saat meleset — heatmap-nya yang menunjukkan
ray mana yang mahal, dan ray yang mahal justru yang tidak mengenai apa pun.

#### Kaskade di GPU

`rhi::Texture3D` baru. **Yang membedakannya dari `Texture2D` bukan dimensinya
melainkan pola tulisnya:** clipmap memperbarui lempeng tepi saja, jadi unggahan
sub-wilayah adalah operasi utamanya, bukan kasus khusus. Mengunggah ulang
seluruh volume setiap kamera bergerak meniadakan seluruh gunanya pengalamatan
toroidal.

Sampler-nya `REPEAT`, bukan `CLAMP`. Pengalamatan clipmap memang toroidal:
koordinat yang melewati tepi harus muncul kembali di sisi seberangnya, dan
sampler yang menjepit akan mengoles texel tepi sepanjang seluruh sisi.

`UploadRegion` **menolak** sub-wilayah yang melewati tepi alih-alih menjepitnya
— pemanggil yang belum memecah wilayah toroidalnya harus tahu, bukan mendapat
gambar yang hampir benar.

**Unggahannya direkam ke command buffer frame, bukan disubmit sendiri.**
`UploadRegion` menyubmit lalu menunggu queue idle — bentuk yang benar untuk
pengisian sekali di waktu setup dan salah untuk apa pun yang berulang tiap
frame. Clipmap menulis belasan wilayah per frame, jadi bentuk pertama saya
menunggu queue idle belasan kali per frame: **3,98 ms untuk 16 ribu voxel,
hampir seluruhnya menunggu.** Sekarang seluruh wilayah dikemas ke satu buffer
staging per slot frame, dan `RecordRegionCopy` merekam salinannya ke command
buffer frame di depan pass mana pun. Barrier-nya per tekstur, bukan per
wilayah.

Buffer staging-nya **per slot frame**: isinya masih dibaca GPU sampai submit
slot itu selesai, dan satu buffer bersama akan ditimpa frame berikutnya di
tengah salinan frame ini.

**Komposit dan penyandiannya masih di CPU, dan itu batas yang disengaja untuk
M1.** Bake per-mesh menjadi brick sparse menuntut importir mesh yang baru datang
di E8.4; selama geometrinya masih kotak, medan jaraknya punya bentuk analitik
yang tepat — jadi yang diuji benar-benar clipmap dan sphere tracing-nya, bukan
ketelitian sebuah baker yang belum ada.

Jarak kotak berskala tak seragam dikalikan skala **terkecil**: jarak yang diukur
di ruang lokal berpadanan dengan antara d·min(skala) dan d·maks(skala) di dunia,
dan yang terkecil tidak pernah melebih-lebihkan ruang kosong. Bentuk pertama
saya memakai yang terbesar — arah yang justru menembus dinding.

#### Komposit CPU: dari 48 ns menjadi 12 ns per voxel

Anggaran 0,4 ms untuk pembaruan clipmap tidak bisa dipenuhi dengan medan jarak
berbentuk `std::function` yang dipanggil sekali per voxel. Empat perubahan,
masing-masing diukur, bukan diperkirakan:

**Medan jaraknya kelas, bukan `std::function`.** Pemanggilan tak langsung per
voxel menghalangi inlining seluruh rumus jaraknya. 48 → 15 ns/voxel.

**Basis lokal tiap mesh dihitung per kotak texel, bukan per baris.** Sebuah
kotak di dunia tetap kotak di ruang lokal — transformasinya affine — jadi satu
titik asal dan tiga vektor langkah menjangkau seluruh isinya. Bentuk per baris
membayar dua perkalian matriks 4×4 per mesh untuk setiap baris.

**Baris mengikuti sumbu terpanjang kotak, bukan selalu X.** Ini yang paling
besar dampaknya, dan yang paling tidak terlihat dari kode: gerakan kamera satu
voxel menghasilkan lempeng setebal satu voxel, dan pembagian toroidalnya
memecah lempeng itu menjadi kotak seperti **1×56×9**. Dengan baris dipatok ke X,
itu 504 baris berisi satu voxel — seluruh biaya per baris tanpa satu pun voxel
untuk membaginya. Dicatat dengan mencetak bentuk tiap kotak yang benar-benar
dikirim, bukan dengan menebak bentuk apa yang mungkin muncul.

**Penyandian jaraknya dijabarkan di jalur per voxel.**
`SdfClipmap::EncodeDistance` ada di unit terjemahan lain, jadi ia tidak bisa
di-inline — dan sebuah panggilan yang tak bisa di-inline, dikali puluhan ribu
voxel, berharga lebih mahal daripada rumus di dalamnya. Pengemasan ke buffer
staging juga berhenti memanggil `At` per voxel: kotak texel rapat pada sumbu X,
jadi tiap barisnya satu salinan.

Membuang mesh yang tidak mungkin menyentuh sebuah baris memakai kotak batas
dunianya, dikali **min(skala)/maks(skala)**. Faktor itu bukan kerapian: nilai
yang disimpan adalah d·min(skala), sedangkan jarak ke kotak batas adalah jarak
dunia — dan tanpa faktornya, mesh yang diskala tak seragam akan dibuang padahal
jaraknya di ruang lokal masih di dalam pita. Lubang di medan jarak adalah
dinding yang bisa ditembus sphere tracing.

Resolusinya **64³, bukan 128³** yang diminta rencana. Batasnya bukan memori
melainkan komposit CPU: medan jaraknya dievaluasi per voxel, dan 128³ berarti
delapan kali pekerjaan itu. Sesudah empat perubahan di atas, frame terberat saat
kamera terbang penuh memakai **0,49 ms** — di dalam anggaran, tapi hanya dengan
sisa 0,01 ms. Delapan kali pekerjaan itu jelas tidak muat; 128³ menunggu komposit
compute, dan keputusan itu sekarang punya angka untuk bersandar.

**Biaya pembaruan clipmap terlihat di panel Statistics**, sebagai angka frame ini
dan puncak dua detik terakhir. Puncaknya bukan kemewahan: yang dibatasi anggaran
adalah frame saat kamera melintasi batas voxel, dan frame itu satu di antara
belasan — angka sesaat memperlihatkannya hanya kalau kebetulan terbaca pada frame
yang tepat. Angka ini di CPU, jadi ia tidak muncul di tabel pass GPU.

**Satu frame masih jauh di luar anggaran, dan itu disengaja:** menyalakan GI atau
melompatkan kamera lebih jauh daripada lebar clipmap menulis seluruh isi ketiga
kaskade sekaligus — 786 ribu voxel, **8,5 ms**. Menyicilnya ke beberapa frame
berarti beberapa frame pertama menelusuri medan yang separuh kosong, dan itu
menukar satu hentakan yang terlihat dengan cahaya yang salah yang tidak terlihat.
Pilihan itu bisa ditinjau ulang setelah M5 punya denoiser yang menyembunyikan
transisi.

#### Pass debug: sphere tracing di shader

Pass full-screen yang menelusuri kaskade dan menggambar hasilnya. **Rumusnya
harus sama dengan `SdfTraceBackend` di sisi C++** — yang di CPU adalah acuan yang
sudah diuji terhadap medan analitik, yang di shader adalah yang benar-benar
dipakai, dan dua rumus yang ditulis terpisah berselisih tepat di kasus yang
paling sulit dilihat.

Pass-nya **bersyarat**: ia hanya ada di graph saat debug view menyala. Graph yang
dibangun ulang tiap frame membuat itu sekadar sebuah `if` — dan pass bersyarat
justru alasan graph ini ada.

Sampler dipilih lewat cabang, bukan ternary: GLSL tidak mengizinkan sampler
sebagai nilai ekspresi. Larik sampler akan lebih rapi tapi menuntut indeks yang
seragam di seluruh subgroup, dan indeks kaskade memang berbeda antar-piksel.

**Heatmap-nya langsung memberi tahu sesuatu.** Seluruh layar hijau, artinya
setiap ray memakai 15–24 dari 48 langkah — termasuk ray yang tidak mengenai apa
pun. Sebabnya pita jarak yang sempit: nilai di luar pita jenuh pada `bandVoxels`
× ukuran voxel, jadi langkah terpanjang di ruang kosong adalah 0,4 m di kaskade
terhalus. Melintasi kaskade 0 sejauh 6,4 m karena itu butuh belasan langkah, dan
tidak ada sphere tracing yang bisa melompatinya. Itu pertukaran yang melekat
pada SDF berpita, dan ia terlihat pada alat pertama yang dibuat untuk
melihatnya — persis gunanya.

Biaya terukur: total GPU **1,348 ms** dengan pass debug aktif versus **0,433 ms**
tanpa, di 1277×614 dengan anggaran 48 langkah. Jadi sphere tracing layar penuh
satu ray per piksel ≈ **0,9 ms**. Sebagai pembanding, anggaran screen probe di
rencana adalah 1,4 ms untuk 16 ray per probe pada ubin 16×16 — yaitu seperenam
belas jumlah ray ini.

#### Kriteria selesai M1 — terpenuhi

**Depth sphere tracing cocok dengan raster: sebagai angka, bukan penilaian
mata.** Rencana menyebut "uji visual side-by-side"; yang dipakai adalah
perpotongan sinar-kotak analitik — kebenaran yang sama dengan yang dirasterkan,
tanpa perlu membaca depth buffer. 25 sinar menembus SDF sebuah kotak 2×2×2 pada
voxel 0,05 m, dan tiap sinar harus sepakat soal kena/meleset serta berselisih
**kurang dari dua voxel**. Dua, bukan satu: satu untuk kuantisasi 8-bit, satu
untuk ambang berhenti sphere tracing yang memang setengah voxel di depan
permukaan. Ada juga sinar yang lewat di samping kotak — kesepakatan soal meleset
tidak lebih murah daripada kesepakatan soal kena.

**Biaya pembaruan < 0,5 ms saat kamera bergerak cepat:** frame terberat 0,49 ms
pada adegan uji, terbaca langsung di panel Statistics. Angka itu berasal dari
0,44 ms nilai tengah dan 0,49 ms puncak — bukan dari satu tangkapan yang
kebetulan bagus.

**Belum ada:** bake SDF per-mesh menjadi brick sparse (menunggu importir mesh
E8.4) dan komposit compute yang membuka 128³. Keduanya di luar kriteria selesai
M1 dan tercatat di M2 dan seterusnya.

#### M2 — Lapis screen-space · ✅ selesai

**Depth buffer ditanya lebih dulu, lalu SDF, lalu langit.** Urutannya bukan soal
biaya melainkan ketelitian: depth buffer punya resolusi geometri sungguhan —
satu piksel — sedangkan voxel SDF terhalus sepuluh sentimeter, dan yang hilang di
SDF justru detail yang paling terlihat, yaitu di dekat perpotongan permukaan.

**Meleset di lapis pertama bukan jawaban.** Sinar yang keluar layar, atau yang
tersembunyi di balik permukaan lain, tidak berarti "tidak ada apa-apa di sana" —
ia berarti "layar tidak tahu". Membedakan keduanya adalah seluruh gunanya
jenjang ini; menyamakannya menghasilkan lubang gelap tepat di tepi layar, cacat
khas GI screen-space. `ScreenTraceResult::leftScreen` yang membawa perbedaan itu,
dan ia diuji langsung.

**Piramida HiZ menyimpan yang TERBESAR, bukan terkecil.** Reversed-Z: depth
terbesar adalah permukaan yang paling dekat, dan itulah yang harus diketahui
penelusur — sebuah sel boleh dilompati hanya kalau sinarnya masih di depan
permukaan terdekat di dalamnya. Menyimpan yang terkecil membuat tiap sel
melaporkan permukaan terjauhnya sebagai penghalang, dan tiap sinar menembus
geometri yang justru paling dekat.

**Ukuran tiap tingkat mengikuti aturan mip Vulkan: dibulatkan ke bawah.** Bentuk
pertama saya membulatkannya ke atas supaya baris terakhir dari ukuran ganjil
tidak hilang, dan itu menghasilkan satu tingkat lebih banyak daripada yang boleh
dimiliki sebuah image — 1280×768 memberi 12 sementara Vulkan mengizinkan 11, dan
`vkCreateImage` menolaknya. Barisnya tetap tidak boleh hilang, jadi yang berubah
bukan ukurannya melainkan cakupannya: texel **terakhir** sebuah baris merangkum
tiga texel sumber, bukan dua.

**Uji ketebalan dilakukan di tempat sinar MASUK sebuah piksel, bukan keluarnya.**
Depth buffer hanya menyimpan permukaan terdepan, jadi sinar yang lewat di
belakang sebuah benda akan melaporkan kena di siluetnya. Yang membedakan
"memotong permukaan" dari "lewat di belakangnya" adalah keadaan saat masuk: sinar
yang masuk sudah di belakang permukaan tidak memotongnya. Bentuk pertama saya
mengujinya di tempat keluar, dan itu menolak setiap perpotongan yang sah pada sel
yang lebar — termasuk seluruh sinar yang mengarah lurus menjauhi kamera, yang
bayangannya di layar adalah satu titik.

Ketebalannya **diukur dalam meter lewat `invViewProj`**, bukan dalam satuan
depth. Depth reversed-Z sangat tidak linear: satu ambang tetap dalam satuan depth
berarti sentimeter di dekat kamera dan ratusan meter di kejauhan.

Sinar didorong maju 2 cm dari titik asalnya sebelum ditelusuri, dan **dorongan
itu dalam meter, bukan piksel**: sinar yang mengarah lurus menjauhi kamera tidak
bergerak satu piksel pun di layar, dan dorongan yang diukur di layar tidak
menggerakkannya sama sekali.

**Penelusuran berjalan lurus di ruang NDC.** Bayangan sebuah ruas garis di layar
tetap ruas garis lurus, dan depth NDC ikut linear terhadap parameter yang sama —
sifat proyeksi perspektif yang juga dipakai rasterizer saat menginterpolasi
depth. Jadi tidak ada langkah yang perlu dikembalikan ke ruang dunia di tengah
lingkaran.

**Debug view "Trace layer" ada sejak lapis ini ada.** Hijau untuk layar, biru
untuk SDF, abu-abu untuk langit. Jenjang yang tidak terlihat adalah jenjang yang
diam-diam berhenti dipakai — dan lapis screen-space yang tidak pernah mengenai
apa pun tampak persis sama dengan yang bekerja sempurna. Terbukti langsung di
editor: kotak uji berwarna hijau dengan tepi biru setipis siluetnya, yaitu
piksel tempat depth buffer kehilangan permukaannya dan SDF mengambil alih.

Sakelar "Screen-space layer" **bukan tombol kualitas melainkan alat ukur**:
mematikannya membuat seluruh gambar menjadi biru, dan itu satu-satunya cara
membuktikan lapis pertama benar-benar yang menjawab. Pass `hiz-build` ikut hilang
dari graph saat lapisnya mati — pass yang hasilnya tidak dibaca siapa pun justru
yang frame graph ini ada untuk mencegahnya.

Biaya terukur di Release, 1277×431: `hiz-build` **0,07–0,12 ms** untuk sebelas
tingkat, dan `gi-sdf-debug` **0,42–0,56 ms**. Selisih antara jenjang penuh dan
SDF saja **tenggelam di dalam sebaran itu** pada adegan uji yang isinya dua
kubus: kebanyakan sinar tidak mengenai apa pun di kedua lapis. Angka yang jujur
di sini adalah "belum terukur", bukan angka yang dipilih dari satu tangkapan yang
kebetulan mendukung.

#### Dua kesalahan yang tidak muncul sebagai galat

**`std::array<Recorder, 8>` untuk graph yang punya sembilan pass.** Angka delapan
itu dihitung tangan dan cocok sampai `hiz-build` lahir; pass kesembilan menulis
di luar larik. **Stack corruption, bukan galat** — editornya mati dengan SIGSEGV
di tempat yang tidak ada hubungannya. Sekarang ukurannya diambil dari
`graph_.PassCount()`, karena angka yang harus diperbarui setiap kali sebuah pass
ditambahkan adalah angka yang suatu saat lupa diperbarui.

**Pass tanpa keluaran dibuang graph.** `hiz-build` tidak menulis satu pun resource
yang dilacak graph — piramida mengurus perpindahan layout tiap mip-nya sendiri,
karena graph melacak resource sebagai satu kesatuan sementara pembangunan
piramida membaca satu mip sambil menulis mip berikutnya. Graph menyimpulkan pass
itu tidak menghasilkan apa pun dan membuangnya; penelusuran lalu membaca piramida
frame sebelumnya **tanpa satu pun galat**, dan gejalanya cuma satu: lapis
screen-space tidak pernah menjawab. `SetSideEffect` yang menandainya — dan
mekanisme itu memang sudah ada di graph justru untuk pass semacam ini.

#### Kriteria selesai M2 — terpenuhi

Rencana menyebutnya langsung: **`SdfTraceBackend` lengkap dan lulus uji ray
tunggal terhadap referensi CPU.** "Lengkap" berarti jenjangnya utuh — jalur yang
disebut rencana sebagai SDF memang "screen-space HiZ + SDF clipmap global", bukan
SDF saja. Enam test baru menguncinya: dua untuk aturan reduksi mip, satu untuk
dinding di depan kamera (lurus dan miring, dengan jarak dicocokkan ke 1/cos),
satu untuk perbedaan antara meleset-keluar-layar dan meleset-karena-kosong, satu
untuk sinar yang lewat di belakang papan (dengan ketebalan besar sebagai kontrol,
supaya gagalnya terbukti karena ketebalan dan bukan karena sinarnya tidak pernah
sampai), dan satu untuk lapis mana yang menjawab — dengan geometri SDF sengaja
diletakkan di luar layar supaya lapisnya terbaca dari lapisnya sendiri, bukan
disimpulkan dari jaraknya.

#### M3 — Screen probe · ✅ selesai

**Sudah ada: acuan CPU-nya, lengkap dan teruji.** Pemetaan oktahedral, kisi
probe, arah ray berjitter, integrasi iradiansi, proyeksi SH, akumulasi temporal,
dan bobot interpolasi bilateral. Sepuluh test, ditulis sebelum satu baris shader
pun ada — pola yang sama dengan M1 dan M2, dan alasannya sama: yang di shader
harus cocok dengan yang di CPU texel demi texel, dan acuan yang ditulis
belakangan hanya mengukuhkan kesalahan yang sudah ada.

**Oktahedral, bukan kubus atau bola.** Pemetaan kubus menuntut enam sisi dan
karena itu enam kali pemilihan sisi di setiap pembacaan; latitude/longitude
memusatkan hampir seluruh texel-nya di kutub. Oktahedral memetakan seluruh bola
ke satu kotak dengan luasan hampir seragam — dan keseragaman itulah yang membuat
16 texel per probe cukup mewakili seluruh arah.

**Ray menelusuri seluruh bola, bukan setengahnya.** Sebuah probe melayani ubin
16×16 piksel, dan piksel di dalam satu ubin bisa punya normal yang sangat
berbeda — di tepi geometri bahkan berlawanan. Probe yang hanya menelusuri
setengah bola di sekitar satu normal tidak punya apa pun untuk diberikan kepada
piksel yang menghadap ke arah lain.

**Satu ray per sel oktahedral; yang berjitter hanya letaknya di dalam sel.**
Tanpa stratifikasi itu, separuh bola bisa tidak tersampel sama sekali pada sebuah
frame — dan yang kosong muncul sebagai iradiansi yang berdenyut walaupun
adegannya diam, yaitu tepat yang dilarang kriteria selesai M3. Jitternya berbeda
tiap probe lewat rotasi Cranley–Patterson: jitter yang seragam di seluruh probe
membuat polanya terlihat sebagai kisi ubin.

**Radiansi probe disimpan sebagai SH orde satu, bukan 16 texel oktahedral.** Yang
dibutuhkan piksel adalah iradiansi — integral radiansi terhadap cosinus — dan
integral itu memangkas seluruh frekuensi tinggi. Menyimpan arahnya utuh berarti
setiap piksel membaca 16 texel dari masing-masing empat probe tetangganya lalu
menjumlahkannya kembali menjadi angka yang hanya punya empat derajat kebebasan.
Orde satu, bukan dua: dengan 16 ray per frame, koefisien orde dua lebih banyak
berisi derau daripada isyarat. Tata letaknya satu `vec4` per kanal warna — bentuk
yang muat persis di tiga lampiran RGBA16F.

**Jarak pada bobot interpolasi diukur tegak lurus bidang piksel.** Dua titik di
lantai yang sama boleh berjauhan; dua titik yang terpisah dinding tidak boleh
berdekatan. Memakai jarak lurus membuat cahaya merembes menembus sudut ruangan —
cacat yang paling sering dikira kebocoran denoiser.

Uji tungku yang mengunci normalisasinya: radiance seragam harus menghasilkan
iradiansi tepat π kali radiance-nya untuk normal mana pun, karena ∫cos dω atas
setengah bola persis π. Dua jalur diuji terhadap angka yang sama — integrasi
langsung dan lewat SH — sehingga faktor konvolusi cosinus SH tidak bisa
menyimpang diam-diam. Kesalahan normalisasi muncul sebagai adegan yang terlalu
terang atau terlalu gelap secara merata, dan itu paling mudah dikira masalah
eksposur.

**Penelusur berjenjang dipindah ke `Shaders/gi_trace.glsl`.** Pass probe dan pass
debug memakai satu implementasi. Dua salinan rumus yang sama adalah dua rumus
yang akan berselisih — dan selisih antara apa yang dilihat alat diagnostik dan
apa yang dipakai menggambar adalah selisih yang paling mahal, karena alatnya lalu
berbohong justru saat paling dibutuhkan. `invViewProj` ikut pindah ke UBO supaya
penelusurnya tidak bergantung pada push constant pemakainya.

#### Porting shader ke Slang

Dua belas entry point dan enam berkas bersama pindah dari GLSL ke Slang dalam
satu sapuan, sebelum sisi GPU M3 menambah shader baru — memporting dua belas
berkas lebih murah daripada lima belas, dan yang lebih penting: shader probe
lahir dalam bahasa yang benar alih-alih ditulis dua kali.

**Tidak ada satu pun penghalang teknis.** Semua yang dipakai punya padanan:
`gl_VertexIndex`→`SV_VertexID`, `texelFetch`→`.Load()`, `sampler2DArrayShadow`→
`Sampler2DArrayShadow` dengan `SampleCmp`, `sampler3D`→`Sampler3D`, push
constant→`[[vk::push_constant]]`, storage buffer→`StructuredBuffer`. Yang
menahannya selama ini hanyalah jalur GLSL yang sudah ada dan terus diperpanjang.

**Perkalian matriks adalah jebakan yang paling berbahaya, dan Slang justru
menangkapnya.** GLSL `M * v` dengan matriks kolom-mayor menjadi `mul(M, v)` di
Slang; menuliskannya sebagai `M * v` **tidak dikompilasi** — Slang tidak punya
overload `matrix * vector`. Itu keberuntungan yang layak dicatat: kesalahan yang
seharusnya muncul sebagai seluruh dunia tertranspose diam-diam malah muncul
sebagai galat kompilasi di tiga baris yang tepat.

**`row0..row3` sebenarnya kolom, dan namanya berbohong.** Sisi C++ menuliskan
`model[0..3]`, dan indeks glm memilih kolom. GLSL `mat4(a,b,c,d)` juga
kolom-mayor, jadi keduanya cocok tanpa transpose. Slang mengikuti HLSL:
`float4x4(a,b,c,d)` menyusunnya sebagai **baris**. Transpose-nya sekarang
disebut sekali di `instance_common.slang`; melewatkannya tidak menghasilkan galat
apa pun — hanya setiap kotak tergambar di tempat dan orientasi yang salah.

**ABI `ShadowParams` selamat karena bentuknya, bukan karena keberuntungan.**
Seluruh anggotanya `float4`, `float4x4`, atau larik keduanya — semuanya
berukuran dan berjajar kelipatan 16 byte. Pada tipe-tipe itu std140, std430, dan
tata letak skalar sepakat persis, jadi tidak ada aturan tata letak yang bisa
menggeser satu medan pun. Menambahkan `float3` atau skalar tunggal ke sana akan
mematahkan sifat itu, dan `static_assert` di sisi C++ tidak bisa melihatnya.

**Slang membuang atribut vertex yang tidak dipakai; glslang tidak.** Pass
bayangan hanya membaca posisi dan keempat kolom matriks, jadi normal, warna, dan
bendera hilang dari antarmuka entry point-nya — dan pipeline yang mendeklarasikan
atribut yang tidak dikonsumsi shader-nya adalah peringatan validasi di setiap
pembuatan pipeline. Yang dilakukan bukan meredam peringatannya melainkan
menyaring daftar atributnya: yang tidak diambil memang tidak perlu diambil.

Pelacakan dependensi lewat `-depfile`: menyentuh `shadow_common.slang` membangun
ulang tepat dua shader yang menyertakannya, bukan seluruhnya dan bukan tidak
sama sekali.

#### Sisi GPU-nya

**Depth prepass sekarang menulis normal, dan itu membalik keputusan lama.**
Prepass sebelumnya berjalan tanpa tahap fragment sama sekali — bentuk yang benar
selama tidak ada yang membutuhkan keluarannya. Screen probe membutuhkannya:
penempatan probe dan bobot interpolasinya sama-sama menuntut normal per piksel
dari permukaan yang benar-benar terlihat, dan menurunkannya dari turunan depth
menghasilkan normal yang ngawur tepat di siluet — yaitu tempat probe paling
sering salah tempat. Yang hilang jalur depth-only; yang didapat normal tanpa satu
pun draw tambahan. Disandikan oktahedral ke dua kanal 16-bit: normal satuan hanya
punya dua derajat kebebasan.

Prepass punya tahap vertex sendiri, bukan milik pass forward. Menyalurkan warna
dan bendera lewat sana berarti tahap vertex mengeluarkan varying yang tidak ada
yang membaca — dan itu peringatan validasi di setiap pembuatan pipeline. Tiap
pipeline sekarang menyebutkan atribut vertex mana yang benar-benar dibacanya
lewat sebuah masker, menggantikan pengendusan "ini pipeline bayangan" yang
sempat ada.

**Akumulasi temporalnya dikerjakan blend unit, bukan ping-pong tekstur.** Bentuk
yang lazim menyimpan dua salinan dan bergantian membaca yang satu sambil menulis
yang lain — enam tekstur, dua set descriptor, dan satu pertanyaan "yang mana yang
berlaku sekarang" di setiap pass. Yang dibutuhkan hanyalah `dst = src·c + dst·
(1−c)`, dan itu persis yang dilakukan blending dengan konstanta. Konstantanya
disetel per frame lewat `vkCmdSetBlendConstants`, jadi ia mengikuti
`AccumulateProbe` di sisi C++ angka demi angka — termasuk 1,0 pada frame pertama
dan setiap kali kamera berpindah, yang artinya "buang seluruh riwayat".
Reproyeksi datang di M5; sampai saat itu membuang riwayat adalah satu-satunya
jawaban yang jujur, karena riwayat probe terikat ke piksel dan piksel yang sama
menunjuk permukaan yang berbeda begitu kamera bergerak.

Tidak ada pass resolve tersendiri: tampilan Irradiance menginterpolasi probe ke
piksel langsung di dalam pass debug. Pass resolve baru berguna saat shading
memakainya, dan itu M6.

**Sinar yang radiansinya tidak diketahui dibuang, bukan dihitung nol.** Ini
perubahan yang paling menentukan, dan ia ditemukan lewat pengukuran, bukan lewat
membaca kode. Sinar yang mengenai lewat lapis SDF tidak punya warna sampai hash
grid M4. Menghitungnya sebagai hitam bukan sekadar membuat gambarnya lebih gelap
— **ia menjadikan jenjang penelusuran itu sendiri sumber derau**: permukaan yang
sama mengembalikan warnanya kalau lapis layar menjawab dan nol kalau SDF yang
menjawab, dan mana yang menjawab berubah tiap frame karena arahnya berjitter.
Terukur: dengan sinar SDF dihitung nol, **25,5% piksel berubah lebih dari 4/255
antar-frame** walaupun kamera diam. Dengan sinar itu dibuang dari penaksir,
**0,0%**.

#### Kriteria selesai M3 — terpenuhi

Rencana menyebutnya: "Cornell box menunjukkan color bleeding yang benar dan
stabil (tidak berdenyut) saat kamera diam."

Cornell box-nya berkas data, bukan kode: `Resources/Levels/cornell.simlevel`,
tujuh kotak dan satu lampu titik. Itu menuntut satu hal yang belum ada — warna
per-mesh. `MeshRendererComponent::baseColor` mengisinya sampai pipeline material
menggantikan shader kotak; tanpa itu seluruh dunia berwarna sama, dan tidak ada
adegan uji GI yang bisa memperlihatkan color bleeding sama sekali.

**Color bleeding: benar.** Dinding kiri merah membias merah ke lantai dan sisi
kiri kotak; dinding kanan hijau membias hijau ke sisi kanannya.

**Stabil: diukur, bukan dinilai mata.** Dua tangkapan berjarak 1,5 detik dengan
kamera diam, dibandingkan piksel demi piksel: selisih rata-rata **0,02/255**,
maksimum 5, dan **nol persen** piksel berubah lebih dari 4/255.

**Yang masih terlihat kasar adalah ubin probe**, dan itu memang keadaan yang
benar untuk M3: satu probe per 16×16 piksel tanpa penyaring spasial apa pun.
Yang menghaluskannya A-trous di M5, dan menambahkannya sekarang akan
menyembunyikan justru apa yang perlu dilihat saat menyetel probe.

#### M4 — Hash grid radiance cache · ✅ selesai

**Sudah ada: acuan CPU-nya, lengkap dan teruji, termasuk uji tungku yang menjadi
kriteria selesainya.**

Cache ini yang memberi warna pada sinar yang mengenai lewat lapis SDF — sinar
yang di M3 terpaksa dibuang dari penaksir karena clipmap hanya menyimpan jarak,
bukan radiansi. Membuangnya adalah jawaban yang benar selama tidak ada yang tahu
warnanya; cache inilah yang tahu.

**Arah ikut ke dalam kunci, bukan hanya posisi.** Sebuah titik di lantai
memancarkan radiansi yang sangat berbeda ke atas dan ke samping; menyimpan satu
angka per posisi berarti merata-ratakan keduanya, dan hasilnya cahaya yang bocor
menembus permukaan tipis — sisi gelap sebuah dinding menerima rata-rata sisi
terangnya. Enam arah cukup: yang dicatat radiansi permukaan, dan enam sudah
membedakan lantai dari langit-langit dan keempat dinding.

**Sel membesar mengikuti jarak ke kamera**, sama seperti kaskade SDF. Detail
radiansi pada jarak lima puluh meter tidak terlihat sama sekali, dan menyimpannya
sehalus yang di depan mata menghabiskan seluruh cache untuk hal yang tidak ada
yang melihat. Tingkatnya ikut ke kunci: sel halus dan sel kasar yang kebetulan
bertumpuk tidak boleh berbagi entri, karena isinya beda arti.

**Kapasitasnya tetap, bukan tumbuh.** Cache yang tumbuh mengikuti adegan adalah
cache yang biayanya tidak bisa disebut di muka — dan yang harus dialokasi ulang
tepat saat kamera memasuki ruangan baru, yaitu frame yang paling tidak punya
waktu luang. Yang tetap kadang membuang entri, dan `DroppedSamples` menghitungnya
supaya cache yang terlalu kecil terlihat sebagai angka, bukan sebagai gambar yang
agak salah.

**Query yang tidak ketemu menjawab "tidak tahu", bukan hitam.** Pelajaran yang
sama dengan sinar SDF di M3, dan alasannya sama: nilai yang tidak diketahui dan
nilai nol adalah dua hal berbeda, dan menyamakannya menggelapkan gambar sambil
menambah derau.

Bit kuncinya dicampur, bukan dipakai apa adanya. Dua sel bertetangga berbeda satu
pada satu sumbu; tanpa pencampuran keduanya mendarat di slot bertetangga, dan
probing linear lalu langsung menabrak tetangganya sendiri — cache penuh jauh
sebelum entrinya habis. Diuji: 8000 kunci bertetangga ke 65536 slot dengan empat
langkah probing menyisipkan lebih dari 7900.

**Uji tungku sebagai kriteria selesai.** Adegan albedo 1,0 di bawah pencahayaan
seragam L harus tetap L setelah berapa pun pantulan. Yang diuji lingkarannya,
bukan salah satu bagiannya: radiansi masuk ke cache, dibaca kembali sebagai
radiansi permukaan, lalu masuk lagi — persis jalur multi-bounce. Dua ratus
iterasi atas sepuluh permukaan, dan hasilnya tetap L dalam 10%. Yang
menggelapkan adalah faktor yang hilang — pembagi π yang lupa, iradiansi yang
dikira radiansi, atau entri yang belum terisi dianggap hitam — dan tidak satu pun
dari ketiganya muncul sebagai galat.

**Entri basi direbut, dan yang masih terpakai tidak.** Tanpa daur ulang, kamera
yang berkeliling meninggalkan entri untuk setiap tempat yang pernah dilihatnya —
entri yang tidak pernah dibaca lagi tapi tetap memakai slotnya, sampai seluruh
cache penuh oleh masa lalu dan setiap penyisipan baru terbuang. Gejalanya GI yang
bekerja saat editor baru dibuka lalu **diam-diam berhenti bekerja setelah
beberapa menit menjelajah** — kegagalan yang tidak akan pernah terlihat pada
tangkapan layar mana pun. Penyisipannya dua lintasan: yang pertama mencari slot
kosong atau kunci yang sama, yang kedua baru merebut yang basi. Merebut di
lintasan pertama akan membuang entri hidup yang kebetulan ditemui lebih dulu
daripada slot kosong di belakangnya.

#### Sisi GPU-nya

Satu storage buffer device-local, 2²⁰ entri × 32 byte = 32 MB, dibersihkan sekali
dengan `vkCmdFillBuffer` — bukan diunggah dari CPU, karena menyalin 32 MB nol
melewati PCIe untuk sesuatu yang bisa dituliskan perangkat sendiri dalam sekali
perintah adalah pekerjaan yang tidak ada yang memintanya. Device-local, bukan
host-visible: ia dibaca dan ditulis GPU tiap frame.

**Kuncinya disimpan sebagai sidik jari 32-bit, bukan sebagai sel utuh.** Sel utuh
menuntut tiga `int` lagi per entri, dan dua kunci berbeda yang sidik jarinya
kebetulan sama hanya menghasilkan satu entri berisi campuran dua tempat — cacat
yang sudah melekat pada cache lossy, bukan cacat baru. Nol dipakai sebagai
"kosong", jadi sidik jarinya dipaksa bukan nol.

**Slotnya direbut dengan atomik; isinya ditulis biasa.** Merebut slot harus
atomik — dua invokasi yang mengklaim slot yang sama bersamaan akan menghasilkan
dua entri yang saling menimpa kuncinya. Isinya tidak: yang terburuk adalah satu
sampel dari probe lain hilang, dan itu memang perilaku cache lossy. Atomik float
menuntut `VK_EXT_shader_atomic_float`, yaitu syarat perangkat baru demi mencegah
kehilangan yang tidak ada yang bisa melihatnya.

`fragmentStoresAndAtomics` harus dinyalakan di pembuatan device. Pass probe
menulis dari tahap fragment, dan tanpa fitur itu setiap storage buffer yang tidak
ditandai `NonWritable` ditolak di pembuatan pipeline — ditemukan validation
layer, bukan dengan membaca kode.

**Yang dilihat layar disimpan supaya bisa dipakai saat ia tidak lagi terlihat.**
Sinar yang mengenai lewat lapis layar menyisipkan radiansi permukaan yang
dikenainya, dengan normal dibaca dari G-buffer di piksel yang sama — bukan
ditebak dari arah sinar, karena permukaan menghadap ke arahnya sendiri dan arah
sinar hanya kebetulan datang dari sana. Sinar yang mengenai lewat SDF membacanya
kembali. Yang belum pernah terlihat tetap dibuang: "tidak tahu" masih bukan
"hitam".

Diperiksa di Cornell box: iradiansinya jauh lebih terisi daripada M3 — lantai dan
kedua kotak menerima cahaya yang di M3 hilang bersama sinar SDF yang dibuang —
dan tetap **0,0% piksel berubah** antar-frame dengan kamera diam (rata-rata
0,02/255, maksimum 3). Tanpa satu pun pesan lapisan validasi.

**Belum ada:** angka diagnostik dari sisi GPU. `DroppedSamples` dan
`EvictedEntries` ada di acuan CPU dan diuji di sana, tapi membacanya dari GPU
menuntut readback — dan cache yang terlalu kecil karena itu masih terlihat
sebagai gambar yang agak salah, bukan sebagai angka. Itu yang pertama dibutuhkan
kalau kapasitasnya harus disetel.

#### M5 — Denoise & temporal · ✅ selesai

**Sudah ada: acuan CPU-nya — reproyeksi, kernel à-trous, dan penjepitan riwayat —
beserta angka yang mengubah kriteria selesainya menjadi anggaran.**

**Kriteria M5 sudah menjadi angka sebelum satu shader pun ditulis.** Rencana
menuntut GI merespons lampu dinyalakan-matikan di bawah 200 ms; pada 60 Hz itu
dua belas frame. `FramesToRespond` menyimulasikan rata-rata berjalan dari riwayat
yang **sudah penuh** — bukan dari kosong, karena yang ditanyakan adalah respons
terhadap perubahan di tengah adegan yang sudah menyatu, dan memodelkannya dari
kosong membuat jawabannya selalu satu frame. Hasilnya:

| jendela akumulasi | frame sampai 90% | milidetik pada 60 Hz |
|---|---|---|
| 16 (dipakai M3 dan M4) | **36** | 600 |
| 5 | **11** | 183 |

Jadi M5 bukan sekadar menambah penyaring: **jendela temporalnya harus dipendekkan
tiga kali lipat**, dan penyaring spasial serta penjepitan riwayatlah yang membuat
jendela sependek itu bisa ditoleransi. Itu fakta yang jauh lebih baik diketahui
sebelum menulis pass daripada sesudahnya.

**Reproyeksi mengikat riwayat ke dunia, bukan ke piksel.** Sampai M4 riwayat
probe dibuang seluruhnya begitu kamera berpindah — riwayatnya terikat ke piksel,
dan piksel yang sama menunjuk permukaan yang berbeda. Reproyeksi mencari titik
dunia yang sama di tempatnya berada frame lalu. Titik yang tidak ada di layar
frame lalu mengembalikan "tidak ada riwayat", dan itu berbeda dari riwayat yang
bernilai nol — pelajaran yang sama dengan sinar SDF di M3 dan query cache di M4.

**À-trous, bukan Gaussian bertingkat.** Lintasan ke-n melompat 2ⁿ piksel dengan
kernel yang sama, jadi dua lintasan menjangkau 20 piksel dengan 50 pengambilan —
sementara satu Gaussian selebar itu menuntut ratusan. Yang membuat lompatannya
sah adalah bobot bilateralnya: bobot yang melintasi permukaan bernilai nol.
Kernelnya B-spline dan jumlah bobotnya tepat satu; kernel kotak menyebarkan tepi
menjadi tangga selebar kernelnya, dan tangga itu terlihat lebih buruk daripada
derau yang dihilangkannya. Dinormalkan oleh bobot yang benar-benar dipakai, bukan
oleh satu: tetangga yang ditolak tidak boleh menggelapkan hasilnya.

**Penjepitan riwayat memutus pertukaran respons lawan derau.** Jendela pendek
merespons cepat tapi berderau; jendela panjang sebaliknya. Menjepit riwayat ke
sekitar rerata tetangga memutus pertukaran itu: selama sampel barunya sejalan
dengan tetangganya, riwayat panjang dipertahankan dan deraunya hilang; begitu
adegannya benar-benar berubah, riwayat yang sudah tidak sejalan dijepit masuk ke
rentang baru dalam satu frame. Diuji langsung pada kasus lampu dimatikan.

#### Sisi GPU-nya

**Akumulasi pindah dari blend unit ke shader, membalik keputusan M3.** M3 memadu
di tempat lewat `vkCmdSetBlendConstants` — tiga tekstur, tanpa ping-pong, tanpa
pertanyaan "yang mana yang berlaku sekarang". Reproyeksi mematahkannya: blending
hanya bisa memadu di texel yang sama, sedangkan titik dunia yang sama ada di
**ubin yang berbeda** begitu kamera bergerak. Riwayatnya disalin ke tekstur
tersendiri sesudah tiap pass, bukan dibaca lewat set descriptor kedua — salinan
empat tekstur 80×45 lebih murah daripada satu pertanyaan yang harus dijawab di
setiap pass yang membacanya.

Riwayat disalin **sebelum** disaring. Menyaring lalu menyimpannya sebagai riwayat
membuat penyaring memakan keluarannya sendiri frame demi frame, dan hasilnya
kabur yang terus melebar tanpa batas.

**Penyaringnya bekerja di kisi probe, bukan di resolusi layar.** Derau yang
terlihat adalah derau per-probe, dan setiap piksel toh sudah menginterpolasi
empat probe. Menyaring di resolusi layar berarti menyaring 800 ribu piksel untuk
menghapus derau yang hanya punya 3600 derajat kebebasan.

**Normal probe dikemas ke mantissa sebuah float, bukan lewat `f32tof16`.**
Intrinsik itu memancarkan kapabilitas SPIR-V Float16 dan Int16, dan keduanya
menuntut fitur perangkat yang tidak ada di baseline — syarat perangkat baru demi
mengemas sebuah normal yang hanya dipakai sebagai bobot. Ditemukan validation
layer. Nilainya selalu di [1,2): eksponennya tetap, jadi tidak ada pola bit yang
bisa menjadi inf atau NaN, yang payload-nya tidak dijamin selamat melewati
tekstur.

#### Periode jitter dipisahkan dari jendela akumulasi

Keduanya sempat satu angka, dan itu tampak masuk akal — sampai jendelanya
dipendekkan dari 16 ke 5 demi kriteria respons, dan urutan jitternya ikut runtuh
menjadi lima pola yang berulang selamanya. **Uji tungku yang menangkapnya:**
iradiansi meleset 10% dari πL karena arah yang tersedia tidak lagi menutupi bola
dengan merata. Keduanya menjawab pertanyaan yang berbeda — yang satu seberapa
cepat GI merespons perubahan, yang lain berapa banyak pola sampel berbeda yang
ada sebelum berulang — dan menyatukannya berarti tidak bisa menyetel salah
satunya tanpa merusak yang lain.

#### Kriteria selesai M5 — terpenuhi

Rencana menyebutnya: "lampu dinyalakan-matikan, GI merespons < 200 ms tanpa
ghosting yang terlihat."

**Stimulusnya bukan lampu, dan alasannya adalah temuan tersendiri.** Menyalakan
dan mematikan lampu titik di Cornell box hampir tidak menggerakkan iradiansi sama
sekali — terukur, bukan diperkirakan: lampunya dihapus dan angkanya bergeser
0,5 dari 52. Sebabnya suku ambient tetap `0.25` di `box.frag.slang`, yang
mendominasi warna yang dibaca probe. Suku itu memang penambal sementara sampai
pipeline material menggantikannya, dan **GI-lah yang seharusnya menggantikannya**
— yaitu M6. Sampai saat itu, perubahan pencahayaan tidak bisa dipakai sebagai
stimulus di adegan ini.

Yang dipakai sebagai gantinya: **menghapus dinding hijau**, yaitu menghilangkan
permukaan terang yang memantul — perubahan mendadak pada radiansi yang masuk,
persis jenis perubahan yang ditanyakan kriterianya. Diukur pada lantai tepat di
depannya, memisahkan rembesan warna dari terang biasa lewat selisih kanal hijau
dan merah:

| waktu | rembesan (G−R) |
|---|---|
| 0 ms | −4,96 |
| 32 ms | −4,96 |
| **48 ms** | **0,00** |
| 592 ms | 0,00 |

**48 milidetik, dan tanpa ekor.** Anggarannya 200. Yang membuatnya jauh lebih
cepat daripada 183 ms yang diramalkan jendela lima frame adalah penjepitan
riwayat: perubahan sebesar itu tidak diluruhkan pelan-pelan, ia dijepit masuk ke
rentang baru dalam satu frame. Nilainya langsung ke nol dan tinggal di sana —
tidak ada ghosting yang meluruh, yang justru gejala yang dilarang kriterianya.

Diperiksa juga bahwa penyaringnya benar-benar bekerja: ubin probe yang masih
terlihat kasar di M3 dan M4 hilang, dan kestabilan kamera-diam tetap **0,0%
piksel berubah** antar-frame. Tanpa satu pun pesan lapisan validasi.

#### M6 — Integrasi ke shading OpenPBR (berjalan)

**Kriteria selesainya terpenuhi di model shading, tapi gambarnya belum benar —
dan itu dua hal yang berbeda.**

**Uji white furnace lulus untuk seluruh rentang roughness dan metalness.** Yang
menghalanginya adalah energi yang hilang, dan hilangnya besar: terukur dengan LUT
DFG engine ini sendiri, logam putih kehilangan 1,9% energinya pada kekasaran 0,2,
27,6% pada 0,6, dan **63,9% pada 1,0**. Itu bukan kesalahan kecil — itu logam
kasar yang tampak abu-abu kotor, dan tidak ada penyetelan material yang bisa
memperbaikinya karena energinya hilang sebelum sampai ke penyetelan.

Kompensasinya membagi energi lingkungan menjadi tiga suku yang **berjumlah tepat
satu menurut konstruksi**: pantulan tunggal, pantulan lanjutan, dan sisa untuk
difus. Sisanya ditulis sebagai pengurangan, bukan sebagai rumus tersendiri —
itulah yang membuat jumlahnya satu karena konstruksi, bukan karena kebetulan.
Diuji pada 7 kekasaran × 5 metalness × 3 sudut pandang.

Aturan "metal mengambil dari lapis spekular, bukan iradiansi difus" **tidak
ditulis sebagai cabang**. Untuk logam putih, sisa untuk difus jatuh ke nol dengan
sendirinya dari rumusnya. Cabang berarti dua tempat yang bisa berselisih.

#### Satu bug yang membatalkan sebagian penilaian M3–M5

`tMax` yang dikirim ke pass probe **selalu nol sejak pass itu lahir**, dan nol
tidak menghasilkan galat apa pun. `traceScreen` menyerah seketika; lalu
`traceSdf` "mengenai" permukaan tempat sinarnya berangkat pada langkah nol —
karena titik asalnya memang di permukaan. Setiap probe karena itu mengembalikan
**warna pikselnya sendiri**.

Yang membuatnya mahal adalah hasilnya tampak masuk akal: kabur, berwarna benar di
dekat dinding berwarna, stabil terhadap waktu, dan merespons perubahan adegan.
Seluruh bukti visual M3–M5 konsisten dengannya. Yang tidak dilakukannya hanyalah
memantulkan cahaya — dan itu satu-satunya hal yang seharusnya dilakukannya.

Pengukuran yang tetap berlaku: kestabilan temporal, waktu respons, hilangnya ubin
probe oleh à-trous, dan seluruh test acuan CPU — semuanya menguji mekanisme yang
memang bekerja. Yang **tidak** lagi berlaku adalah penilaian "color bleeding
benar" pada M3: yang terlihat merah di dekat dinding merah adalah piksel dinding
merah itu sendiri, bukan cahaya yang memantul darinya.

#### Yang masih salah

Dengan `tMax` diperbaiki dan iradiansi GI menggantikan ambient tetap `0.25` di
`box.frag.slang`, dinding Cornell box masih **hitam**. Lantainya menerima
matahari dan memantul, tetapi pantulan itu tidak sampai ke dinding. Sebabnya
belum ditemukan, dan menebaknya lebih jauh tanpa alat ukur baru hanya akan
menambah tebakan.

Yang dibutuhkan berikutnya adalah alat, bukan tebakan: tampilan debug yang
memperlihatkan **isi sebuah probe** — arah dan radiansi keenam belas sinarnya —
bukan hasil interpolasinya. Tampilan lapis dan heatmap langkah menjawab
pertanyaan tentang penelusuran; tidak satu pun menjawab "apa yang dilihat probe
ini".

**GI tetap mati secara bawaan**, jadi viewport tanpa GI tidak berubah sama sekali:
`kFallbackAmbient` yang berlaku, persis seperti sebelumnya.

- **Anggarannya belum didamaikan dengan kriteria terima E8.** 3,0 ms adalah 18%
  dari frame 60 fps, sementara adegan uji E8 — terrain 2×2 km, 200 ribu instance
  vegetasi, 20 lampu berbayang — sudah menuntut sisanya. Keduanya ditulis
  terpisah dan belum pernah dijumlahkan.
- ~~**Ia bergantung pada E8.3, bukan hanya E8.1.**~~ **Sudah lepas.** E8.3
  selesai: prefilter env, DFG LUT, dan `evaluateOpenPBR_IBL` semuanya ada dan
  dipakai preview material. Titik sambung M6 tinggal mengganti sumber
  `irradiance` dan `prefilteredBase` dari probe statis ke cache radiance — dan
  antarmuka `evaluateOpenPBR_IBL` sengaja dibuat menerima ketiganya sudah jadi
  supaya penggantian itu tidak menyentuh satu baris pun model shading.
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
4. **Shader**: sumbernya Slang, dikompilasi lewat `slangc` dari Vulkan SDK.
   **Seluruhnya, tanpa kecuali.**

   Aturan ini sempat berbunyi "GLSL masih diterima untuk shader utilitas", dan
   pengecualian itu menelan dirinya sendiri: dua belas shader pipeline ditulis
   GLSL karena masing-masing terasa "utilitas" saat ditulis — termasuk penelusur
   GI, pembangun piramida HiZ, dan pass forward. Sebuah pengecualian yang
   batasnya ditentukan perasaan penulisnya bukan pengecualian melainkan aturan
   kedua. Sekarang tidak ada lagi berkas `.glsl` di `Shaders/`.

   Berkas diberi nama `<nama>.<tahap>.slang`; `sim_compile_shaders` membuang
   akhiran `.slang` dari nama keluarannya, sehingga sisi C++ tetap memuat
   `box.vert.spv` dan tidak ikut berubah hanya karena bahasa sumbernya berganti.

   **`-matrix-layout-column-major` wajib**, dan harus sama dengan argumen yang
   dipakai pipeline material di `ShaderCache`. Tanpanya Slang memakai tata letak
   baris, dan setiap matriks di seluruh engine tertranspose tanpa satu pun galat
   kompilasi.
5. **Vulkan 1.3** sebagai baseline (dynamic rendering, synchronization2,
   timeline semaphore), dengan fallback render pass tradisional bila perangkat
   hanya 1.2.
