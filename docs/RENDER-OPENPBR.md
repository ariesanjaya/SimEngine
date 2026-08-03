# Lobe OpenPBR yang belum ada, dan cara menjalankannya real time

`openpbr.slang` menjalankan reduksi OpenPBR Surface v1.1 ke campuran lobe
analitik: base, specular, coat, fuzz. Tiga kelompok parameter spesifikasi belum
ada di sana, dan ketiganya justru yang paling sering dianggap "mahal":

| Kelompok | Parameter |
|---|---|
| `subsurface_*` | weight, color, radius, radius_scale, scatter_anisotropy |
| `transmission_*` | weight, color, depth, scatter, scatter_anisotropy, dispersion_scale, dispersion_abbe_number |
| `thin_film_*` | weight, thickness, ior |

Dokumen ini memilih teknik untuk masing-masing, dan — yang lebih menentukan —
menetapkan **cara membayarnya hanya ketika dipakai**.

---

## Yang sebenarnya membuat fitur ini mahal

Bukan instruksi per-pikselnya. Yang mahal adalah **membayarnya untuk material
yang tidak memakainya**: satu cabang subsurface di dalam shader tunggal membuat
setiap dinding beton ikut menanggung register dan cabang yang tidak pernah
menyala, dan penurunan occupancy itu berlaku untuk seluruh layar.

Karena itu urutan keputusannya terbalik dari yang biasa. Yang pertama ditetapkan
bukan tekniknya, melainkan **penyaringnya**:

### 1. Permutasi shader dari graph, bukan dari cabang runtime

`CompileMaterial` sudah tahu pin mana yang benar-benar dikemudikan — ia memakai
pengetahuan itu untuk memutuskan baris mana yang ditulis. Pengetahuan yang sama
bisa keluar sebagai **penanda fitur**:

```
SIM_FEATURE_THIN_FILM     thin_film_weight dikemudikan
SIM_FEATURE_TRANSMISSION  transmission_weight dikemudikan
SIM_FEATURE_SUBSURFACE    subsurface_weight dikemudikan
```

Material yang tidak menyentuh subsurface tidak mengandung satu instruksi
subsurface pun — bukan cabang yang selalu bernilai false, melainkan kode yang
tidak ada. Ini lever terbesar, dan setengahnya sudah terbangun.

Yang perlu dijaga: jumlah permutasi. Tiga penanda = delapan varian, dan itu
batas yang sehat. Setiap penanda baru menggandakannya, jadi penanda hanya boleh
diberikan pada fitur yang benar-benar mengubah bentuk shader — bukan pada setiap
pin.

### 2. Gerbang di tingkat pass, dari topeng fitur per-view

`MaterialCompileResult` membawa topeng fitur; renderer menggabungkannya untuk
seluruh objek yang terlihat. Tidak ada material transmisif di layar berarti
salinan buffer warna dan rantai mip-nya **tidak dibuat sama sekali**. Tidak ada
material subsurface berarti dua pass blur tidak dijadwalkan.

Ini yang membuat sebuah level tanpa kaca dan tanpa kulit berjalan persis secepat
sebelum ketiga fitur ini ada.

### 3. Tingkat kualitas dipilih proyek, bukan per-material

Setiap fitur punya tingkat murah dan tingkat mahal. Yang memilih adalah setelan
proyek atau platform — **bukan artis di dalam material**. Kalau per-material,
satu aset bisa membuat frame tiga kali lebih mahal tanpa ada yang menyadari
sampai profiler dibuka.

---

## thin_film_* — analitik, kerjakan langsung

**Teknik.** Belcour & Barla 2017, dalam bentuk yang sudah dibakukan Khronos
sebagai `KHR_materials_iridescence`. Bentuk itu dipakai Blender, Unity HDRP,
glTF, dan OpenPBR sendiri, jadi memilihnya juga berarti material kita cocok
dengan yang datang dari sana.

**Cara kerjanya.** Interferensi lapisan tipis mengubah **Fresnel**, bukan
NDF maupun visibility. Jadi yang berubah hanya satu pemanggilan:

```
F_Schlick(f0, VoH)  →  F_Iridescent(f0, VoH, thickness, filmIor)
```

Berlaku untuk Fresnel dielektrik maupun logam, karena OpenPBR menempatkan
lapisan tipis pada antarmuka specular — di bawah coat, di atas base.

**Ongkos.** Sekitar 40–60 ALU, tanpa pass tambahan, tanpa buffer tambahan, tanpa
tekstur tambahan. Belcour & Barla sendiri menyebut overhead-nya "reasonable
enough for production" dibanding Fresnel biasa.

**Jebakan yang sudah diketahui.** Penjumlahan Airy pada tiga panjang gelombang
RGB tetap menghasilkan pita warna berfrekuensi tinggi. Bentuk yang benar
memakai pra-integrasi di ranah Fourier, yang juga membuat film sangat tebal
meluruh ke abu-abu netral alih-alih berkilau selamanya. Jangan tulis versi RGB
naif lalu berharap memperbaikinya belakangan — hasilnya akan terlihat salah
persis pada nilai yang paling sering dipakai artis.

**Putusan: kerjakan bersama E8, tingkat tunggal.** Tidak perlu tingkat murah;
versi penuhnya sudah murah.

---

## transmission_* — refraksi ruang layar, dua tingkat

**Teknik.** Refraksi ruang layar: material transmisif digambar setelah opaque,
membaca salinan buffer warna opaque. Ini yang dipakai Filament, Unreal, dan
Godot 4. Tidak ada alternatif real time yang sepadan — diskusi Godot yang
dirujuk di bawah menyimpulkan hal yang sama, berikut kenyataan bahwa artefaknya
melekat pada tekniknya.

**Tingkat A — thin-walled** (`transmission_depth == 0`). Tidak ada pembelokan
sinar: hanya tint, Fresnel, dan pengaburan menurut roughness lewat pemilihan
mip. Satu sampel. Ini yang benar untuk jendela, gelembung, kain tipis.

**Tingkat B — solid** (`transmission_depth > 0`). Vektor pandang dibelokkan
menurut `specular_ior`, ditelusuri sejauh tebal benda, lalu UV keluarnya dipakai
menyampel salinan buffer. Serapan Beer-Lambert memakai `transmission_color` dan
`transmission_depth`.

**Sumber ketebalan** — ini bagian yang menentukan, dan tiga pilihannya berbeda
jauh ongkosnya:

1. **Tekstur tebal yang di-bake per-mesh.** Termurah saat runtime, dan paling
   bisa diprediksi. Butuh langkah bake di importer mesh (E8).
2. **Prepass kedalaman back-face.** Tepat untuk mesh tertutup, satu pass ekstra.
3. **Perkiraan dari bounding volume.** Gratis, dan cukup untuk benda cembung
   sederhana seperti botol.

Mulai dari (3), tambahkan (1) ketika importer mesh ada. (2) hanya kalau ada yang
membutuhkannya.

**Dispersi.** `transmission_dispersion_scale` > 0 berarti tiga sampel dengan IOR
per-kanal yang diturunkan dari bilangan Abbe. Digerbangi terpisah — ia melipat-
tigakan sampel, dan sebagian besar kaca tidak membutuhkannya.

**Ongkos.** Satu salinan buffer warna + rantai mip per frame, **hanya bila ada
material transmisif yang terlihat**. Per-piksel 1–3 sampel tekstur.

**Batas yang harus dinyatakan ke artis, bukan disembunyikan.** Refraksi ruang
layar tidak bisa melihat apa yang di luar layar maupun yang tertutup benda lain.
Yang di belakang objek transmisif yang tidak tergambar di buffer opaque tidak
akan muncul. Ini bukan bug yang menunggu diperbaiki; ini sifat tekniknya, dan
material yang mengandalkannya perlu ditata dengan itu di kepala.

**`transmission_scatter` / `transmission_scatter_anisotropy` tidak dipetakan.**
Keduanya hamburan di dalam volume, yang menuntut integrasi volumetrik — persis
bagian yang membuat implementasi Adobe bukan untuk real time. Diserap sebagai
warna serapan Beer-Lambert, dan selisihnya dinyatakan di dokumen material, bukan
dibiarkan pengguna menemukannya sendiri.

---

## subsurface_* — tiga tingkat, dua di antaranya nyaris gratis

**Tingkat A — pra-integrasi** (Penner & Borshukov). Satu LUT 2D pada
(N·L, kelengkungan). Tanpa pass tambahan, tanpa buffer tambahan. Cukup untuk
kulit pada jarak menengah dan untuk perangkat lemah. Tidak menangani cahaya yang
menembus bagian tipis.

**Tingkat A2 — translusensi belakang** (Barré-Brisebois & Bouchard, Frostbite).
Beberapa ALU ditambah tekstur tebal. Inilah yang membuat telinga, daun, dan
tirai menyala saat dilihat berlawanan cahaya — hal yang justru paling dikenali
mata, dan yang tidak diberikan tingkat A. Pasangkan keduanya.

**Tingkat B — difusi ruang layar** (Burley / separable, Jimenez). Buffer diffuse
terpisah, dua pass blur separable, bertopeng stencil hanya pada piksel
subsurface. Ini yang dipakai Unreal sebagai *Subsurface Profile*. Kualitas
tertinggi yang masih real time; ongkosnya nyata tapi terbatas pada piksel yang
memang subsurface.

**Pemetaan parameter OpenPBR.** `subsurface_radius × subsurface_radius_scale`
adalah jarak bebas rata-rata per kanal — itu **persis** parameter profil Burley,
jadi pemetaannya langsung, bukan penyetelan yang dikira-kira.
`subsurface_scatter_anisotropy` tidak bertahan melewati aproksimasi difusi;
lipat menjadi radius efektif, dan nyatakan bahwa ia tidak berpengaruh penuh.

**Putusan: A + A2 sebagai tingkat bawaan, B sebagai pilihan proyek.** Sebagian
besar benda subsurface di sebuah level bukan wajah pemeran utama.

---

## Urutan mengerjakannya

1. **Topeng fitur di `MaterialCompileResult`** — kecil, dan pintu bagi semua
   yang lain. Bisa dikerjakan sekarang, di dalam E7.1, karena kompilernya sudah
   tahu pin mana yang dikemudikan.
2. **`thin_film_*`** — analitik murni, tanpa pass, tanpa buffer. Bisa masuk
   bersama shader E8 pertama.
3. **`transmission_*` tingkat A** — butuh salinan buffer warna, yaitu keputusan
   struktur render pertama yang nyata.
4. **`subsurface_*` tingkat A + A2** — butuh LUT dan tekstur tebal, yaitu
   keputusan pipeline aset.
5. Tingkat B masing-masing, kalau memang ada yang menuntutnya.

Langkah 1 tidak menunggu E8. Sisanya menunggu, dan itu wajar: keempatnya
menyentuh struktur render yang belum ada.

---

## Rujukan

- [Belcour & Barla 2017 — *A Practical Extension to Microfacet Theory for the Modeling of Varying Iridescence*](https://belcour.github.io/blog/research/publication/2017/05/01/brdf-thin-film.html)
- [Khronos `KHR_materials_iridescence`](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_materials_iridescence/README.md) — bentuk Belcour-Barla yang sudah dibakukan
- [Jimenez — *Screen-Space Subsurface Scattering*](https://www.iryoku.com/screen-space-subsurface-scattering/) dan [separable-sss](https://github.com/iryoku/separable-sss)
- [Unreal — *Subsurface Profile Shading Model*](https://dev.epicgames.com/documentation/en-us/unreal-engine/subsurface-profile-shading-model-in-unreal-engine)
- [MJP — *An Introduction To Real-Time Subsurface Scattering*](https://therealmjp.github.io/posts/sss-intro/)
- [Godot — diskusi refraksi ruang layar dan batasnya](https://github.com/godotengine/godot-proposals/discussions/9580)
- [*OpenPBR: Novel Features and Implementation Details*](https://arxiv.org/abs/2512.23696) — fisika coat darkening dan interferensi lapisan tipis, serta alasan pilihan parameterisasi subsurface
