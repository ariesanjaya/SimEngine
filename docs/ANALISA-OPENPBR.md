# Analisa: OpenPBR versus metallic-roughness untuk render real-time

Ditulis untuk menjawab satu pertanyaan yang wajar: **metallic-roughness masih
dipakai Unreal Engine 5 dan terbukti relevan — apakah memilih OpenPBR sebuah
kesalahan?**

Jawaban singkatnya: keduanya bukan pilihan yang saling meniadakan, dan
pertanyaannya sendiri mengandung satu kesalahan kategori yang perlu diluruskan
lebih dulu.

---

## Keduanya tidak berada di lapisan yang sama

**Metallic-roughness adalah sebuah parameterisasi.** Ia menyatakan satu
permukaan mikrofaset dengan tiga angka — base color, metalness, roughness — dan
menyerahkan sisanya (distribusi, geometri bayangan, Fresnel) kepada
implementasinya. Itu inti glTF 2.0, dan itu pula bentuk *default lit* Unreal.

**OpenPBR Surface adalah sebuah model permukaan berlapis** yang *memuat*
parameterisasi itu di lapisan dasarnya. Kalau `coatWeight`, `fuzzWeight`,
`baseDiffuseRoughness`, dan anisotropi semuanya nol, yang tersisa persis
metallic-roughness: satu lobe difus, satu lobe GGX, F0 dari IOR.

Jadi pertanyaannya bukan "OpenPBR **atau** metallic-roughness". Yang benar:
**"apakah kita membayar lapisan yang tidak dipakai?"** — dan itu pertanyaan
implementasi, bukan pertanyaan spesifikasi.

Bahwa Unreal memakai metallic-roughness juga tidak berarti Unreal berhenti di
situ. Ia menambahkan *shading model* terpisah untuk clear coat, cloth,
subsurface, hair, dan eye — yaitu hal-hal yang sama yang dijadikan OpenPBR
sebagai lapisan di dalam satu model. Bedanya cara menyusun, bukan cakupan.

---

## Yang sudah ada di mesin ini, diperiksa di kode

`Shaders/openpbr.slang` (348 baris) mengimplementasikan:

| Lapisan | Keadaan |
| --- | --- |
| Base — difus Oren–Nayar, metalness sebagai interpolasi | ✅ `baseDiffuseRoughness` 0 jatuh persis ke Lambert |
| Specular — GGX anisotropik, Smith height-correlated, F0 dari IOR | ✅ anisotropi 0 adalah kasus khusus, bukan cabang lain |
| Coat — lobe GGX kedua beserta peredaman lapisan di bawahnya | ✅ termasuk `coatDarkening` |
| Fuzz — sheen distribusi invers-roughness | ✅ |
| IBL — irradiance SH + DFG LUT split-sum | ✅ `evaluateOpenPBR_IBL` |
| Subsurface, transmission, thin film | ❌ tidak ada, dan tidak dibutuhkan siapa pun hari ini |

Parameternya delapan belas pin di `output.surface`, dan **nama medannya sama
persis dengan nama pin** — kompiler graph menulis `result.surface.<nama pin>` apa
adanya.

Ongkosnya, dibaca dari `evaluateOpenPBR`:

- Material dengan `coatWeight = 0` dan `fuzzWeight = 0` mengeksekusi **satu lobe
  difus + satu lobe GGX** — yaitu metallic-roughness, ditambah dua perbandingan
  cabang.
- Yang memakai coat membayar lobe GGX kedua; yang memakai fuzz membayar satu
  distribusi sheen.

Artinya biaya OpenPBR di mesin ini **sudah** sebanding metallic-roughness untuk
material yang tidak memakai lapisan tambahan. Yang belum benar adalah *bagaimana*
lapisan itu dimatikan — lihat rekomendasi.

---

## Alasan yang mendorong pertanyaan ini: interop 3ds Max

Ini alasan yang kuat, dan ia berdiri sendiri terlepas dari perdebatan di atas.

3ds Max kini mengirimkan material OpenPBR, dan Arnold serta MaterialX
mengimplementasikan model yang sama. Kalau mesin ini memakai parameterisasi yang
sama, **pemetaan ekspor menjadi satu-ke-satu**: `base_color` ke `baseColor`,
`specular_roughness` ke `specularRoughness`, `coat_weight` ke `coatWeight`.
Artis melihat hasil yang sama, dan pemetaan yang tidak perlu ditebak adalah
pemetaan yang tidak bisa salah diam-diam.

Kalau mesin ini memakai metallic-roughness, material OpenPBR dari Max harus
**diproyeksikan**: coat dilipat ke roughness, fuzz dibuang, IOR dipaksa menjadi
0,04. Proyeksi itu bekerja — banyak pipeline melakukannya — tetapi hasilnya
selalu "mirip", tidak pernah "sama", dan selisihnya baru terlihat sesudah aset
masuk.

**Yang perlu ditegaskan:** interop itu urusan *parameter*, bukan *BRDF*. Mesin
metallic-roughness pun bisa mengimpor material OpenPBR — ia hanya kehilangan
lapisan yang tidak bisa diwakilinya. Nilai memilih OpenPBR adalah **tidak
kehilangan apa pun saat impor**, dan itu bergantung pada lapisan mana yang
benar-benar diimplementasikan. Coat dan fuzz sudah ada di sini; subsurface dan
transmission belum, jadi material Max yang memakainya tetap akan kehilangan
sesuatu.

---

## Yang harus jujur disebut sebagai risiko

**Spesifikasinya muda.** Metallic-roughness sudah satu dekade di glTF, punya
ribuan implementasi yang saling mengoreksi, dan perilakunya di kasus tepi sudah
diperdebatkan habis. OpenPBR belum. Revisi 1.x masih bisa menggeser tampilan
material yang sudah dibuat orang.

Mitigasinya sudah terpasang dan bukan kebetulan: **model shadingnya tinggal di
satu berkas**, dan material hanya menjawab "berapa nilai tiap parameter di titik
ini". Revisi spesifikasi mengubah `openpbr.slang` dan tidak menyentuh satu pun
`.simmat`. Itu keputusan E7.1, dan ia terbayar tepat pada risiko ini.

**Isi dunia masih metallic-roughness.** glTF, Substance, dan hampir setiap
pustaka aset berbicara metallic-roughness. Jadi jalur impor **metallic-roughness
→ OpenPBR** bukan jalur pinggiran melainkan jalur utama, dan ia harus tetap
tepat. Yang sudah ada dan diuji:

- `specularRoughness` perseptual, disalin apa adanya dari `roughnessFactor` —
  mengkuadratkannya di importir membuat setiap permukaan terlalu mengkilap.
- `specularIor` 1,5 menghasilkan F0 0,04, yaitu nilai dielektrik yang
  diandaikan metallic-roughness.
- `baseMetalness` disalin apa adanya.

**Referensi real-time lebih sedikit.** Kompensasi energi dan albedo berarah untuk
model **berlapis** lebih sulit daripada untuk GGX tunggal, dan yang salah muncul
sebagai material yang terlalu gelap di roughness tinggi — bukan sebagai galat.

---

## Rekomendasi

**Tetap OpenPBR.** Alasannya bukan bahwa ia lebih baru, melainkan tiga hal yang
bisa diperiksa: ia memuat metallic-roughness sebagai kasus khususnya, jalur
impor dari metallic-roughness sudah ada dan diuji, dan alasan interop 3ds Max
yang mendorong pertanyaan ini justru terpenuhi olehnya.

Tetapi tiga hal perlu dikerjakan supaya pilihan itu tidak menjadi ongkos yang
tidak perlu:

### 1. Lapisan dimatikan saat **kompilasi**, bukan saat menggambar · ✅ selesai

Hari ini `if (surface.coatWeight > 0.0)` adalah cabang runtime. Pada GPU, cabang
yang diambil sebagian lane dalam satu warp membayar **kedua** sisinya — jadi satu
material bercoat di layar membuat tetangganya ikut membayar, dan material yang
`coatWeight`-nya literal nol tetap membawa kode coat di dalam SPIR-V-nya.

Mekanismenya sudah ada: `ShaderVariant` sudah memasang konstanta spesialisasi
lewat `constant_id`, dan kompiler graph sudah tahu pin mana yang tersambung dan
mana yang literal. Material yang `coatWeight`-nya literal nol seharusnya
menghasilkan modul **tanpa** lobe coat sama sekali.

Ini yang membuat "OpenPBR mahal" berhenti benar. Sesudahnya, material
metallic-roughness biasa menghasilkan kode yang sama dengan mesin
metallic-roughness — bukan mirip, sama.

**Dikerjakan.** Bentuknya bukan konstanta spesialisasi melainkan `#define` yang
ditulis `AssembleMaterialModule` **sebelum** prelude-nya. Konstanta spesialisasi
disetel saat pipeline dibuat, sementara yang menentukan lapisan adalah
materialnya sendiri — dan SPIR-V-nya memang sudah per-material. Dengan nilai
yang diketahui `slangc`, kodenya tidak pernah menjadi instruksi; dengan
konstanta spesialisasi ia tetap ada dan hanya tidak diambil.

`openpbr.slang` memasang bawaan lewat `#ifndef`, jadi berkas itu tetap sah
dibaca sendirian dan yang tidak menyebut apa-apa mendapat perilaku sebelum
penyaringan ini ada.

Kompiler menjawab **"mungkin dipakai"**, bukan "dipakai": pin yang tersambung ke
sesuatu bisa bernilai apa saja saat menggambar, dan literal yang tidak terbaca
sebagai angka juga dijawab "mungkin". Tebakan ke arah itu hanya membuat material
membayar lobe yang tidak dipakainya; ke arah sebaliknya ia menghilangkan lapisan
tanpa satu pun galat.

**Terukur, dan itu yang membuktikannya.** Bendera di `MaterialCompileResult` dan
`#define` di dalam teks modul keduanya bisa benar sementara `slangc` tetap
menghasilkan instruksi yang sama, jadi yang diuji adalah SPIR-V-nya: material
polos yang sama menghasilkan **2.588 byte** dengan lapisan yang benar-benar
dipakainya, terhadap **4.067 byte** dengan seluruh lapisan dinyalakan — 36% lebih
kecil.

Mutasi yang memindahkan `#define` ke *sesudah* prelude ikut menggugurkan uji.
Itu kesalahan yang paling mudah dibuat di sini: modulnya tetap terbentuk,
`#ifndef` di dalam prelude sudah terlanjur memasang bawaannya, dan penyaringannya
diam-diam tidak berpengaruh sama sekali.

### 2. Uji kesetaraan terhadap metallic-roughness

Sebuah uji yang menyatakan: material OpenPBR dengan `coatWeight = 0`,
`fuzzWeight = 0`, `baseDiffuseRoughness = 0`, anisotropi 0, dan
`specularIor = 1.5` menghasilkan radiansi yang sama — dalam toleransi yang
ditulis — dengan referensi metallic-roughness GGX/Smith/Lambert yang dihitung
terpisah di dalam ujinya.

Itu mengunci klaim "OpenPBR memuat metallic-roughness" sebagai sifat yang
diperiksa, bukan sebagai keyakinan. Dan ia yang akan menangkap revisi
spesifikasi yang menggeser lapisan dasarnya.

### 3. Jangan kejar lapisan yang belum ada isinya

Subsurface, transmission, dan thin film adalah bagian OpenPBR yang belum
diimplementasikan — dan **belum ada satu pun aset di repo ini yang memakainya**.
Menambahkannya sekarang berarti membangun tiga jalur yang tidak bisa diuji
dengan data sungguhan, dan yang tidak bisa diuji dengan data sungguhan hampir
selalu salah pada kasus tepi yang baru muncul bersama datanya.

Syarat masuknya bisa ditulis sekarang: **sebuah aset dari 3ds Max yang benar-benar
memakainya**, seperti rig Mixamo yang menjadi acuan skinning.

---

## Ringkasnya

| | metallic-roughness | OpenPBR di mesin ini |
| --- | --- | --- |
| Lapisan | difus + spekular | difus + spekular + coat + fuzz |
| Biaya material biasa | 1 difus + 1 GGX | sama, ditambah dua cabang runtime (§1 menghapusnya) |
| Impor dari glTF/Substance | langsung | lewat pemetaan yang sudah diuji |
| Impor dari 3ds Max/Arnold | proyeksi, kehilangan coat dan fuzz | satu-ke-satu untuk lapisan yang ada |
| Kematangan spesifikasi | satu dekade | muda, masih berevisi |
| Ongkos revisi spesifikasi | — | satu berkas, tidak menyentuh material |

Pilihan yang sudah diambil E7.1 tetap benar. Yang belum selesai bukan
pilihannya, melainkan **membuat lapisan yang tidak dipakai benar-benar tidak
dibayar** — dan itu pekerjaan kompiler graph, bukan pekerjaan model shadingnya.
