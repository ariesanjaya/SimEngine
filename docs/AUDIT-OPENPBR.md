# Audit: `openpbr.slang` terhadap OpenPBR Surface v1.1.1

Dibandingkan baris demi baris dengan panduan di `/home/arie/SDK/OpenPBR-1.1.1`
(`index.html`, bagian **Model**, dan `reference/open_pbr_surface.mtlx`).

> **Status, 23 Agustus 2026. Keempat belas temuan selesai.** Yang tersisa dari
> spesifikasi hanyalah lobe yang memang belum ada — subsurface, transmission,
> thin film, thin-walled — dan itu sudah punya rencananya sendiri di
> [`RENDER-OPENPBR.md`](RENDER-OPENPBR.md).
>
> Sejak MaterialX 1.39.6 tersedia di `/home/arie/SDK/MaterialX`, seluruh
> perbaikan dicocokkan terhadap implementasi referensinya — lihat *Pencocokan
> terhadap MaterialX* di bawah. Dua di antaranya berubah karenanya.

**Kesimpulannya: parametrisasinya sesuai, evaluasinya belum.** Kedua puluh satu
pin di `output.surface` bernama dan bernilai bawaan persis seperti spesifikasi —
tidak ada satu pun yang meleset. Yang menyimpang adalah apa yang dilakukan
terhadap nilai-nilai itu: enam parameter dibaca dengan arti yang berbeda dari
yang ditetapkan, dan dua di antaranya **tidak berpengaruh sama sekali**.

Itu bentuk ketidaksesuaian yang paling merepotkan, karena tidak terlihat sebagai
galat. Material OpenPBR dari 3ds Max atau Arnold akan terbuka tanpa keluhan,
seluruh nilainya masuk ke tempat yang benar, dan hasilnya tetap berbeda dari
tempat asalnya — tanpa satu pun petunjuk mengapa.

---

## Yang sudah sesuai

| Hal | Acuan spesifikasi |
|---|---|
| Nama dan nilai bawaan 21 pin | tabel parameter tiap bagian — **cocok seluruhnya** |
| $\alpha = r^2$ (Burley) | Microfacet model |
| Bentuk NDF GGX anisotropik | eq. NDF anisotropik |
| Smith height-correlated | Microfacet model |
| Pantulan spekular coat **tidak** diwarnai `coat_color` | Coat |
| Logam tanpa lobe difus, dicampur lewat `base_metalness` | Metal |
| Kompensasi multi-scatter di jalur IBL | Microfacet model (Kulla2017) |

Nilai bawaan yang cocok seluruhnya bukan hal sepele: itu berarti material yang
tidak menyentuh sebuah pin berperilaku sama seperti di renderer lain, dan itu
lapisan kesesuaian yang paling sering dilanggar mesin lain.

---

## Penyimpangan, diurutkan menurut seberapa terlihat

### 1. ~~Pemetaan anisotropi memakai rumus Disney, bukan rumus OpenPBR~~ ✅ diperbaiki

`Shaders/openpbr.slang:129` memakai bentuk *aspect* dari Burley 2012:

```
aspect = sqrt(1 - |a| * 0.9)      ax = alpha / aspect,  ay = alpha * aspect
```

Spesifikasi menetapkan rumus lain, dan menetapkannya sebagai persamaan bernomor:

$$\alpha_t = r^2 \sqrt{\frac{2}{1 + (1-a)^2}} \ , \qquad \alpha_b = (1-a)\,\alpha_t$$

Keduanya melebarkan lobe, tetapi mengawetkan hal yang berbeda: yang Disney
menjaga $\alpha_x \alpha_y = \alpha^2$, yang OpenPBR menjaga
$\alpha_t^2 + \alpha_b^2 = 2\alpha^2$. Spesifikasi menyebut alasannya secara
eksplisit — supaya renderer yang **mematikan** anisotropi tetap menghasilkan
highlight yang sepadan dengan yang menyalakannya.

Akibatnya: logam sikat dan serat karbon berubah bentuk highlight-nya saat
berpindah mesin. **Ini penyimpangan yang paling murah diperbaiki** — satu fungsi,
tanpa akibat ke mana-mana.

### 2. ~~`specular_color` tidak berpengaruh sama sekali pada logam~~ ✅ diperbaiki

`openpbr.slang:213` menghitung `f0 = lerp(dielectricF0, baseColor, metalness)`.
Pada `metalness = 1` seluruh `dielectricF0` — satu-satunya tempat
`specularColor` masuk — terbuang. Pin itu ada, bisa diisi, bisa dikemudikan
tekstur, dan tidak mengubah apa pun.

Spesifikasi memakai model **F82-tint** (Kutz 2021) untuk logam, dan di sana
`specular_color` justru punya peran yang tegas: ia adalah **warna di tepi
menyerempet** ($\bar\mu = 1/7$, sekitar 82°), dinyatakan sebagai pecahan dari
kurva Schlick:

$$\mathbf{F}_{82}(\mu) = \mathbf{F}_\mathrm{Schlick}(\mu) - \frac{\mu(1-\mu)^6}{\bar\mu(1-\bar\mu)^6}\Bigl(\mathbf{F}_\mathrm{Schlick}(\bar\mu) - \mathbf{F}(\bar\mu)\Bigr)$$

dengan $\mathbf{F}(\bar\mu) = \mathtt{specular\_color} \cdot \mathbf{F}_\mathrm{Schlick}(\bar\mu)$.

Model ini tereduksi tepat ke Schlick biasa pada nilai bawaan, jadi
menerapkannya **tidak mengubah satu pun material yang ada** — ia hanya
menghidupkan pin yang sekarang mati.

### 3. ~~`base_weight` tidak menskala reflektansi logam~~ ✅ diperbaiki

Spesifikasi menyatakan $\mathbf{F}_0$ logam adalah `base_weight` × `base_color`.
Di `openpbr.slang:213` yang masuk hanya `baseColor`; `baseWeight` dipakai
belakangan dan hanya untuk lobe difus (`:224`). Logam dengan `baseWeight = 0.5`
seharusnya separuh terang — sekarang tidak berubah sama sekali.

### 4. ~~`specular_weight` mengalikan lobe, seharusnya memodulasi IOR~~ ✅ diperbaiki

`openpbr.slang:219` mengalikan seluruh lobe spekular dengan `specularWeight`.
Spesifikasi menetapkan jalur yang berbeda: bobot itu memodulasi **reflektansi
pada insidensi normal** lewat rasio IOR yang diubah,

$$\eta^\prime_s = \frac{1+\epsilon}{1-\epsilon} \ , \qquad \epsilon = \mathrm{sgn}(\eta_s - 1)\sqrt{\xi_s F_s}$$

lalu $\eta^\prime_s$ itu yang dipakai di rumus Fresnel pada **setiap** sudut.

Selisihnya bukan konstanta: mengalikan lobe menurunkan pantulan menyerempet
sama besarnya dengan pantulan tegak lurus, sedangkan spesifikasi membiarkan tepi
menyerempet tetap mendekati 1. Permukaan dengan `specular_weight` rendah
karenanya kehilangan kilau tepinya di sini, padahal seharusnya tidak.

Spesifikasi juga mengizinkan $\xi_s > 1$ dengan batas $\xi_s \le 1/F_s$; pin di
katalog tidak menyatakan rentang itu.

### 5. ~~`coat_ior` tidak memengaruhi spekular di bawahnya~~ ✅ diperbaiki

Ketika ada coat, dasar dielektriknya tidak lagi bersinggungan dengan udara
melainkan dengan medium coat. Spesifikasi menyatakannya sebagai satu persamaan:

$$\eta_s = \mathrm{lerp}(n_b/n_a,\ n_b/n_c,\ \mathtt{coat\_weight})$$

`openpbr.slang:212` menghitung `dielectricF0` dari `specularIor` saja, tanpa
melihat coat. Akibatnya dasar bercoat memantul terlalu kuat: pada
`coat_ior = 1.6` dan `specular_ior = 1.5`, F0 yang benar turun dari 0,04 ke
sekitar 0,001 — dua kali lipat lebih dari sekadar "sedikit berbeda".

### 6. ~~`coat_darkening` artinya terbalik dari spesifikasi~~ ✅ diperbaiki

Ini penyimpangan yang paling mudah salah paham, jadi ditulis lengkap.

Spesifikasi: $\delta = 1$ (bawaan) berarti **penggelapan fisis terjadi apa
adanya**; $\delta = 0$ berarti albedo dasar *dinaikkan* tepat secukupnya untuk
**membatalkan** penggelapan itu. Faktornya $B(\delta) = \mathrm{lerp}(B_0, 1, \delta)$
dengan $B_0 \approx \Delta^{-1}$ dan

$$\Delta(E_b, \eta_c) = \frac{1 - K}{1 - E_b K} \ .$$

Yang digelapkan adalah efek **pantulan berulang di dalam coat**, bukan
transmisinya.

Kedua besaran yang dibutuhkannya murah. $K$ diambil dari
$K_r = 1 - (1 - E_F(\eta_c))/\eta_c^2$ untuk dasar kasar dan $K_s = F(\omega_o, \eta_c)$
untuk dasar mulus, di-lerp menurut taksiran kekasaran dasar; dan $E_F$ punya
hampiran analitik tertutup yang akurat 0,2% pada $\eta \in [1,3]$:

$$E_F(\eta) \approx \ln\biggl(\frac{10893\eta - 1438{,}2}{-774{,}4\eta^2 + 10212\eta + 1}\biggr)$$

Jadi memperbaikinya tidak menuntut LUT baru.

`openpbr.slang:245` mengerjakan sesuatu yang lain: ia me-lerp seluruh peredaman
— termasuk warna `coat_color` — antara "tanpa peredaman" dan "peredaman penuh".
Dua akibatnya:

- Pada `coatDarkening = 0`, **`coat_color` ikut hilang.** Pernis merah menjadi
  bening. Spesifikasi tetap mewarnai; yang dibatalkan hanya penggelapannya.
- Yang dimodelkan sebagai "penggelapan" adalah $(1-F)^2$, yaitu transmisi
  Fresnel — bukan $\Delta$, yaitu pantulan internal. Keduanya efek yang berbeda.

### 7. ~~Penskalaan albedo coat memakai $(1-F)^2$, bukan $(1 - E_\mathrm{coat})$~~ ✅ diperbaiki

Spesifikasi merumuskan layering sebagai penskalaan albedo dengan **albedo
terarah** $E_\mathrm{coat}(\omega_o)$ — sebuah integral atas lobe, bukan Fresnel
pada satu arah. `coatColorAttenuation` (`:184`) memakai $(1-F(v \cdot h))^2$,
yang menggelapkan lebih dari seharusnya karena dikuadratkan.

Spesifikasi juga meminta coat yang kasar **mengasarkan lobe di bawahnya**
(bagian *Roughening*). Itu belum ada.

### 8. ~~Difus memakai Oren–Nayar klasik, yang justru ditolak spesifikasi~~ ✅ diperbaiki

`diffuseLobe` (`:161`) memakai bentuk $A + B\,s/t$ dengan
$A = 1 - 0{,}5\sigma/(\sigma+0{,}33)$. Spesifikasi menyebut bentuk ini apa
adanya:

> the original model suffers from artifacts and also does not conserve energy
> (i.e. is too dark)

dan menetapkan penggantinya: **EON** — bentuk Fujii 2012 ditambah suku
kompensasi energi resiprokal. Koefisiennya jauh lebih sederhana dari yang
dipakai sekarang:

$$A = \frac{1}{1 + \left(\tfrac{1}{2} - \tfrac{2}{3\pi}\right)\sigma} \ , \qquad B = \sigma A$$

Selain itu `openpbr.slang:165` memakai $\sigma = r^2$, sedangkan spesifikasi
menyatakannya lugas — "the roughness parameter $\sigma \in [0,1]$ is given by
**`base_diffuse_roughness`**" — jadi tanpa dikuadratkan. Keduanya menggelapkan,
dan efeknya menumpuk pada `base_diffuse_roughness` tinggi.

### 9. ~~Lobe fuzz kehilangan penyebutnya, dan tidak berlapis~~ ✅ diperbaiki

Dua hal terpisah di `openpbr.slang:231`:

- `sheenDistribution` mengembalikan **D saja**. Sebuah BRDF microfacet
  membutuhkan $D \cdot G / (4\, n{\cdot}v\, n{\cdot}l)$. Tanpa penyebut itu
  lobe-nya terlalu terang pada insidensi tegak lurus dan kehilangan pemusatan
  di tepi menyerempet — justru sifat yang membuat kain terlihat seperti kain.
- Fuzz **ditambahkan**, bukan dilapiskan. Spesifikasi menetapkan
  $M_\mathrm{surface} = \mathbf{layer}(M_\textrm{coated-base}, S_\mathrm{fuzz}, \mathtt{F})$,
  jadi yang di bawahnya harus diredam. Sekarang `fuzz_weight` hanya menambah
  energi, dan permukaan berbulu bisa memantul lebih dari yang diterimanya.

Keduanya lenyap sekaligus ketika lobe-nya diganti dengan yang memang
ditetapkan spesifikasi: **microflake SGGX Zeltner 2022 lewat Linearly
Transformed Cosines**. Yang sempat membuatnya tertunda adalah anggapan bahwa
model itu menuntut tabel; MaterialX menunjukkan tidak — ada fit Gaussian untuk
albedo terarahnya dan fit rasional untuk koefisien matriks LTC-nya, keduanya
tertutup.

Lobe Zeltner **memuat kosinusnya sendiri** — spesifikasi menuliskannya sebagai
$\mu_i f_\mathrm{fuzz} = \mathbf{F}\,E_\mathrm{fuzz}\,D$ — jadi ia harus
ditambahkan *setelah* suku-suku lain dikalikan $n \cdot l$, bukan sebelum. Itu
sebabnya urutan perhitungannya ikut berubah, dan itu pula yang membongkar §14.

### 10. ~~Emisi berada di atas coat dan fuzz~~ ✅ diperbaiki

`MaterialShaderModule.cpp:294` menulis `lit += m.emissive` setelah seluruh lobe.
Spesifikasi menaruh emisi **di bawah** coat dan fuzz, dengan alasan yang
disebutkan langsung: supaya cahaya yang dipancarkan ikut diwarnai serapan kedua
lapisan itu — itulah yang membuat glow stick dan layar di balik kaca bisa
digambar tanpa memodelkan emitornya terpisah.

Spesifikasi juga memisahkan `emission_luminance` (nit) dari `emission_color`;
di sini keduanya menyatu jadi satu `float3`.

### 11. ~~Jalur cahaya langsung dan jalur IBL memakai model energi yang berbeda~~ ✅ diperbaiki

`evaluateOpenPBR_IBL` (`:316`) membagi energi tiga arah dengan kompensasi
multi-scatter, dan lulus uji white furnace. `evaluateOpenPBR` (`:223`) memakai
`(1 - F)` polos tanpa kompensasi apa pun.

Material yang sama karenanya menjawab berbeda tergantung dari mana cahayanya
datang — dan spesifikasi meminta kompensasi multi-scatter di kedua-duanya.
Pada `specular_roughness` tinggi selisihnya besar: catatan di jalur IBL sendiri
menyebut 27,6% pada kekasaran 0,6.

### 12. ~~Normal dan tangent coat tidak bisa dikemudikan terpisah~~ ✅ diperbaiki

Spesifikasi mendefinisikan `geometry_normal`, `geometry_coat_normal`,
`geometry_tangent`, dan `geometry_coat_tangent`. Katalog hanya punya `normal`.

Yang hilang bukan hanya coat bernormal sendiri, tetapi **arah anisotropi**:
tanpa `geometry_tangent`, `specular_roughness_anisotropy` selalu meregang
menurut tangent mesh, dan logam sikat melingkar tidak bisa diarahkan sama
sekali.

### 13. ~~Rentang yang lebih longgar dari spesifikasi~~ ✅ sebagian besar diperbaiki

- `specular_roughness_anisotropy` menerima nilai negatif (memutar sumbu 90°).
  Spesifikasi menetapkan $a \in [0,1]$.
- Roughness dibatasi bawah `kMinRoughness` tetapi tidak dibatasi atas;
  spesifikasi membatasi $r \in [0,1]$.

Roughness kini dijepit di kedua ujung. Anisotropi negatif **dibiarkan**: ia
perluasan yang berguna dan tidak mengubah apa pun pada rentang yang ditetapkan
spesifikasi, tetapi material yang memakainya memang berhenti bisa dipindahkan
apa adanya.

### 14. ~~Urutan lapisan terbalik: fuzz dipasang di bawah coat~~ ✅ diperbaiki

Ditemukan saat mengerjakan §9, tidak ada di pembacaan pertama.

Spesifikasi menyusun
$M_\mathrm{surface} = \mathbf{layer}(M_\textrm{coated-base}, S_\mathrm{fuzz})$ —
fuzz **di atas** coat. Kode lama menambahkan fuzz ke `result` *sebelum* blok
coat, sehingga peredaman coat ikut mengenai fuzz: lapisan paling atas diredam
oleh lapisan di bawahnya. Kain berpernis karena itu kehilangan bulunya justru
ketika pernisnya ditebalkan.

Sekarang urutannya dasar → coat → kosinus → fuzz, dan fuzz-lah yang meredam
seluruh yang di bawahnya.

---

## Lobe yang memang belum ada

`subsurface_*`, `transmission_*`, `thin_film_*`, dan `geometry_thin_walled`
belum diimplementasikan. Ini **sudah diketahui dan sudah direncanakan** —
`docs/RENDER-OPENPBR.md` memilih tekniknya dan menetapkan cara membayarnya hanya
ketika dipakai. Audit ini tidak menambah apa pun di sana.

---

## Urutan yang disarankan

Diurutkan menurut rasio antara perubahan yang terlihat dan ongkos
mengerjakannya:

| # | Perbaikan | Ongkos | Yang berubah |
|---|---|---|---|
| 1 | ✅ Rumus anisotropi OpenPBR | satu fungsi | logam sikat cocok dengan Max/Arnold |
| 2 | ✅ `base_weight` masuk ke F0 logam | satu baris | pin yang mati jadi hidup |
| 3 | ✅ F82-tint untuk logam | ~15 baris | `specular_color` jadi berarti; bawaan tidak berubah |
| 4 | ✅ `coat_ior` → rasio IOR dasar | ~5 baris | dasar bercoat berhenti terlalu terang |
| 5 | ✅ Arti `coat_darkening` diluruskan | ~20 baris | pernis berwarna berhenti hilang di δ=0 |
| 6 | ✅ Difus EON | ~30 baris | difus kasar berhenti terlalu gelap |
| 7 | ✅ Lobe fuzz Zeltner LTC + pelapisannya | ~60 baris | kain memakai model yang ditetapkan spec, dan energinya kekal |
| 8 | ✅ Kompensasi multi-scatter di jalur langsung | sedang | dua jalur cahaya sepakat |
| 9 | ✅ `specular_weight` lewat modulasi IOR | sedang | tepi menyerempet benar |
| 10 | ✅ `geometry_tangent` + `geometry_coat_normal` | besar — menyentuh katalog dan bingkai shading | anisotropi bisa diarahkan |

Nomor 1–4 seluruhnya di bawah lima puluh baris dan tidak mengubah tampilan satu
pun material yang sudah ada pada nilai bawaannya. Itu titik masuk yang paling
masuk akal — dan keempatnya sudah dikerjakan.

### Catatan pelaksanaan

**§6 — `coat_darkening`.** Spesifikasi ternyata memberi skema siap pakainya:
faktor penggelapan termodulasi $= \mathrm{lerp}(1, \Delta, \mathtt{C}\,\delta)$, dengan
cakupan coat ikut di dalamnya. Yang perlu dibangun tinggal $\Delta$, dan kedua
besaran penyusunnya murah — $E_F(\eta)$ punya hampiran tertutup, dan $K$ hanya
interpolasi antara dasar mulus dan dasar kasar. Yang lebih menentukan daripada
rumusnya adalah **pemisahannya**: serapan `coatColor`, energi yang dipantulkan
permukaan coat, dan pantulan berulang di dalamnya kini tiga faktor terpisah, dan
`coatDarkening` hanya menyentuh yang ketiga. Itu yang membuat pernis merah
berhenti menjadi bening di $\delta = 0$.

**§8 — difus EON.** Spesifikasi menunjuk Portsmouth 2024 untuk albedo terarah
$\hat{E}_\mathrm{ON}$ tanpa menuliskannya. Integralnya elementer: memisahkan
integral azimut menurut tanda $s$ memberi

$$\hat{E}_\mathrm{ON}(\mu) = A + B\,\frac{2\sin\theta}{\pi}\left[\frac{\theta - \mu\sin\theta}{2} + \frac{1 - \sin^3\theta}{3\mu} - \frac{1}{3}\right]$$

Dicocokkan terhadap integrasi numerik langsung sampai $10^{-6}$, dan albedo
rata-ratanya $\langle\hat{E}_\mathrm{ON}\rangle$ dihitung dari bentuk itu — bukan angka
yang dipungut. Energi yang dikembalikan suku kompensasinya: **5,0% pada
$\sigma$ 0,25, 9,4% pada 0,5, dan 16,7% pada 1,0**. Uji tungku putih lulus tepat
1,0000 pada seluruh $\sigma$ dan seluruh sudut pandang.

> **Satu ketidakcocokan di spesifikasinya sendiri.** Ditulis apa adanya,
> $\boldsymbol{\rho}_\mathrm{ms}$ memuat $1/\pi$ dan $f^\mathrm{comp}_\mathrm{ON}$
> membaginya dengan $\pi$ sekali lagi — dan hasilnya meleset sefaktor $\pi$ dari
> sifat yang dinyatakan spesifikasi itu sendiri dua kalimat kemudian ("as
> $\boldsymbol{\rho} \rightarrow 1$ ... the white furnace test passes"). Yang
> diimplementasikan di sini adalah versi yang benar-benar lulus uji itu: satu
> faktor $1/\pi$, bukan dua.

Kekasaran difus kini juga terlihat di jalur lingkungan, lewat albedo terarah
yang sama. Sebelumnya jalur itu Lambert murni, sehingga satu material menjawab
dua hal berbeda tergantung dari mana cahayanya datang.

**§9 — fuzz, dan apa yang sengaja tidak dikerjakan.** Penyebut yang hilang sudah
ditambahkan: lobe-nya kini $D \cdot V$ dan karenanya sebuah BRDF, bukan sekadar
distribusi. **Pelapisannya tidak.** Spesifikasi meminta
$f_\mathrm{fuzz} + (1 - E_\mathrm{fuzz})f_\textrm{coated-base}$, dan $E_\mathrm{fuzz}$
adalah besaran **tertabel**, bukan tertutup — di spesifikasi ia datang dari fit
LTC Zeltner 2022. Tempat yang benar untuknya adalah kanal ketiga LUT DFG, yang
hari ini `R32G32_SFLOAT` dan tidak punya ruang; menambahkannya menyentuh baker,
format, dan tanda tangan shader-nya.

Saya menghitung $E$ untuk lobe Charlie×Ashikhmin yang dipakai di sini secara
numerik (berkisar 0,002 sampai 0,78) tetapi **tidak** memasang hasil fitnya:
tanpa alat untuk mengukur galat fitnya, memasang kurva karangan sendiri di
tempat yang spesifikasinya menyebut tabel tertentu lebih buruk daripada
kekurangan yang disebutkan. Fuzz karena itu masih menambah energi tanpa
mengurangi energi di bawahnya, dan itu tertulis sebagai komentar di tempatnya.

**Dua Fresnel, bukan satu F0 yang di-lerp.** Kode lama menggabungkan dielektrik
dan logam di tingkat F0 lalu menjalankan satu kurva Schlick atasnya. Itu yang
membuat `specularColor` lenyap pada logam: kurva logam memang bukan Schlick,
jadi ia tidak bisa diwakili dengan menggeser F0. Sekarang keduanya dievaluasi
terpisah dan baru dicampur menurut `baseMetalness` — yang juga bentuk yang
ditetapkan spesifikasi ($\mathbf{mix}$ atas dua slab, bukan atas dua F0).

**Energi difus kini mengikuti Fresnel dielektrik saja**, bukan Fresnel gabungan.
Difus hanya ada di bawah antarmuka dielektrik; bagian logamnya sudah dipotong
faktor $(1 - \mathtt{baseMetalness})$ yang terpisah.

**F82 di jalur IBL adalah hampiran, dan disebut hampiran.** LUT DFG dibakar
untuk Schlick; membakar ulang varian F82 menuntut LUT kedua berdimensi warna
tepi. Koreksinya karena itu diterapkan sebagai rasio pada arah pandang. Pilihan
itu diambil karena alternatifnya lebih buruk: jalur lampu dan jalur lingkungan
yang berselisih tentang logam yang sama persis adalah temuan §11, dan tidak ada
gunanya memperbaiki satu temuan dengan memperparah yang lain.

**Tidak ada material yang berubah rupa pada nilai bawaannya.** F82 tereduksi
tepat ke Schlick saat `specularColor` putih, rasio IOR tereduksi ke
`specularIor` saat `coatWeight` nol, dan `baseWeight` bawaannya 1. Yang berubah
hanya material yang benar-benar menyentuh pin-pin itu — dan sebelum ini,
menyentuhnya tidak melakukan apa-apa.

Diuji lewat 16 kombinasi makro lapisan yang dikompilasi `slangc` sungguhan
(`Tests/MaterialTests.cpp`), karena lapisan yang mati di sini adalah kode yang
tidak ada, bukan cabang yang tidak diambil.


---

## Pencocokan terhadap MaterialX 1.39.6

Implementasi referensi ada di `/home/arie/SDK/MaterialX`. Seluruh perbaikan
dicocokkan terhadapnya, dan hasilnya bukan sekadar konfirmasi.

**Yang terkonfirmasi persis.** Bentuk tertutup $\hat{E}_\mathrm{ON}$ yang saya
turunkan sendiri ternyata identik dengan `mx_oren_nayar_fujii_diffuse_dir_albedo`
suku demi suku. Suku multi-scatter EON, struktur $\Delta = (1-K)/(1-E_b K)$, dan
modulasi $\mathrm{lerp}(1, \Delta, C\delta)$ juga cocok. MaterialX pun memakai
**satu** faktor $1/\pi$ pada suku kompensasi EON — menegaskan bahwa $1/\pi$ ganda
di prosa spesifikasi memang keliru tulis, bukan bacaan saya yang salah.

**Yang berubah karenanya — dua hal.**

*Pembalikan rasio IOR di §5.* MaterialX membalik rasio ketika
$n_b/n_c < 1$, dengan komentar yang menunjuk bagian TIR coat di spesifikasi.
$F_0$ sendiri kebal terhadap $\eta \rightarrow 1/\eta$, jadi pada coat penuh ini
tidak mengubah apa pun — tetapi **interpolasi menuju rasio itu berubah**, dan
selisihnya besar di tengah:

| `coat_weight` | tanpa pembalikan | dengan pembalikan |
| --- | --- | --- |
| 0,0 | 0,04000 | 0,04000 |
| 0,5 | 0,00972 | **0,01540** |
| 1,0 | 0,00104 | 0,00104 |

*Taksiran albedo dasar di §6.* MaterialX menskala albedo slab logam dengan
`specular_weight`, karena Fresnel logam memang dikalikannya. Sekarang ikut.

**Yang sengaja tetap berbeda.** Untuk koefisien pantulan internal $K$, MaterialX
memakai satu bentuk saja — $K = 1 - (1 - F_0)/\eta_c^2$ — sedangkan prosa
spesifikasi merekomendasikan $K = \mathrm{lerp}(K_s, K_r, r_b)$ dengan $E_F$
hemisferis, bukan $F_0$. Di sini yang dipakai versi prosanya, karena dokumen ini
mengaudit terhadap spesifikasi dan bukan terhadap satu implementasinya.
Selisihnya kecil: pada $\eta_c = 1{,}6$, $K$ 0,651 lawan 0,630.

**Dan satu hal yang tadinya mustahil menjadi mungkin.** Pelapisan fuzz (§9)
sempat saya tunda karena $E_\mathrm{fuzz}$ adalah besaran tertabel. MaterialX
punya **fit analitiknya** — `mx_zeltner_sheen_dir_albedo`, sebuah fit Gaussian —
beserta fit rasional untuk koefisien matriks LTC-nya. Jadi bukan hanya
pelapisannya yang jadi bisa dikerjakan: **lobe fuzz-nya sendiri kini model
Zeltner yang ditetapkan spesifikasi**, bukan sheen pengganti.
`open_pbr_surface.mtlx` menyetel `mode = "zeltner"` secara eksplisit, jadi tidak
ada ruang tafsir di sana.


---

## Catatan pelaksanaan, gelombang kedua

**§4 — `specular_weight`.** Rantainya persis seperti prosa spesifikasi, dan
implementasi referensi mengerjakannya sama:
$F_s \rightarrow \xi_s F_s \rightarrow \epsilon \rightarrow \eta^\prime_s$.
Penjepitan ke 0,99999 di tengah rantai bukan sekadar penjaga numerik — ia
**menegakkan batas $\xi_s \le 1/F_s$** yang disebut spesifikasi, karena rasio
IOR-nya meledak ke tak hingga di sana.

Selisihnya besar dan justru di tempat yang paling terlihat. Pada
`specular_weight` 0,3 dan `specular_ior` 1,5:

| $\mu$ | pengali lobe (lama) | modulasi IOR (benar) |
| --- | --- | --- |
| 1,00 | 0,0120 | 0,0120 |
| 0,50 | 0,0210 | 0,0429 |
| 0,20 | 0,1064 | 0,3357 |
| 0,05 | 0,2348 | **0,7765** |

Identik menghadap kamera, tiga kali lipat meleset di tepi. Permukaan berbobot
spekular rendah selama ini kehilangan kilau tepinya — dan kilau tepi itulah yang
membedakan plastik kusam dari plastik yang salah dimodelkan.

**§7 dan §11 tertutup oleh satu hal yang sama.** Keduanya menunggu albedo
terarah GGX, dan MaterialX punya fit rasionalnya
(`mx_ggx_dir_albedo_analytic`) — tanpa LUT. Karena itu:

- jalur cahaya langsung kini memakai kompensasi multi-scatter yang sama dengan
  jalur lingkungan, dan energi difusnya diambil dari `1 - albedo terarah`
  alih-alih `1 - F` pada satu arah. Yang dikembalikan: 1,0% pada kekasaran 0,2,
  **20,8% pada 0,6, dan 58,2% pada 1,0**;
- penskalaan albedo coat memakai albedo terarah coat yang sebenarnya, dihitung
  pada kekasaran coat-nya sendiri — bukan pada kekasaran dasar yang kebetulan
  ada di suku DFG yang diserahkan pemanggil.

Pengasaran dasar oleh coat ikut masuk, dan **satu-satunya tempat ia bisa
dipasang untuk jalur lingkungan adalah kode yang dihasilkan** — di sanalah mip
peta prafilter dan koordinat LUT dipilih. Karena itu `MaterialShaderModule`
ikut berubah, bukan hanya shader-nya.

**§10 — emisi.** Faktor transmisinya ternyata komplemen Schlick persis:
penuh menghadap kamera, nol di tepi menyerempet. Fuzz **tidak** ikut meredam —
mengikuti implementasi referensi, yang memperlakukan emisi sebagai EDF di
samping BSDF berlapis, bukan sebagai lapisan di bawahnya.


## Catatan pelaksanaan, §12

Tiga pin baru di node keluaran: `tangent`, `coatNormal`, `coatTangent`. Nilai
bawaannya **sumbu identitas, bukan nol** — nol berarti "tidak menunjuk ke mana
pun", dan itu bukan hal yang sama.

**Yang menentukan bukan pin-nya, melainkan kapan bingkainya dibangun.**
Spesifikasi memperlakukan `geometry_normal` dan `geometry_coat_normal` sebagai
dua pelekukan yang berdiri sendiri atas normal yang sama, jadi bingkai coat
harus lahir **sebelum** peta normal dasar dipasang. Membangunnya sesudah akan
membuat coat mewarisi lekukan dasarnya — persis yang tidak terjadi pada pernis
sungguhan, yang justru mengisi lekukan itu.

**Sorot coat kini membawa kosinusnya sendiri.** Dengan normal coat yang berbeda,
mengalikan seluruh hasil dengan satu $n \cdot l$ dasar membuat kulit jeruk pada
cat mobil memantul seakan permukaannya rata. Karena itu sorot coat dikumpulkan
terpisah dan ditambahkan setelah suku-suku lain dikalikan kosinus dasarnya —
struktur yang sama dengan yang sudah dipakai fuzz.

**Ongkosnya nol untuk yang tidak memintanya.** Kedua bingkai dinyalakan lewat
`SurfaceLobes`, ditanya **"dikemudikan atau tidak"** dan bukan "mungkin bukan
nol" seperti lobe yang lain — nilai bawaan pin ini sumbu identitas, jadi
membandingkannya dengan nol tidak menjawab apa pun. Material yang tidak
menyebutnya memanggil bentuk ringkas `evaluateOpenPBR(surface, frame, ...)`, dan
tidak satu baris pun kode bingkai ikut tertulis.

Itu juga jawaban atas pertanyaan kompatibilitas: coat yang tidak dikemudikan
**mengikuti** bingkai dasarnya, bukan diam-diam menjadi rata.

**Satu daftar yang tadinya dua.** Pin di luar `OpenPBRSurface` tidak punya
`defaults()` untuk bersandar, sehingga kompiler wajib menulisnya walau tidak
dikemudikan — dan uji kesamaan nilai bawaan wajib melewatinya. Kedua tempat itu
menyimpan daftarnya sendiri, dan menambah tiga pin membuat keduanya berselisih
diam-diam. Sekarang keduanya membaca `SurfacePinIsExtra`.
