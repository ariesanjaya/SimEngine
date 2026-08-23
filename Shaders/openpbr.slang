// Model permukaan OpenPBR Surface v1.1, bagian yang dipakai material graph.
//
// **Ini yang dipanggil kode hasil graph, bukan sebaliknya.** Graph hanya
// menjawab "berapa nilai tiap parameter permukaan di titik ini"; seluruh
// perhitungan cahaya tinggal di berkas ini. Pemisahan itu yang membuat mengubah
// model shading tidak menyentuh satu pun material, dan mengubah material tidak
// bisa merusak model shading.
//
// **Hanya cahaya langsung.** IBL, bayangan, dan transmisi belum ada di sini;
// yang dibutuhkan preview material dan pass forward pertama adalah evaluasi
// terhadap satu arah cahaya. Menambahkannya nanti tidak mengubah antarmuka
// `OpenPBRSurface`, jadi material yang sudah ditulis tidak ikut berubah.
//
// Nama medannya sama persis dengan nama pin di Surface Output. Itu bukan
// kebetulan yang menyenangkan melainkan syarat: kompiler graph menulis
// `result.surface.<nama pin>` apa adanya, jadi medan yang namanya bergeser
// menjadi galat kompilasi Slang pada material yang tidak diapa-apakan siapa pun.

static const float kPi = 3.14159265358979323846;
static const float kMinRoughness = 0.0045;  // di bawah ini GGX meledak jadi NaN
// Lantai untuk sumbu NDF. Anisotropi penuh membuat sumbu pendek jatuh tepat ke
// nol, dan pembagian dengannya menghasilkan lobe yang lenyap alih-alih tipis.
static const float kMinAlpha = kMinRoughness * kMinRoughness;
// Kosinus sudut "tepi menyerempet" model F82, kira-kira 82 derajat.
static const float kF82CosTheta = 1.0 / 7.0;
// Dua tetapan bentuk Fujii dari Oren-Nayar. Yang kedua adalah albedo rata-rata
// suku roughness-nya, yaitu 2*integral(0..1) Ehat(mu) mu dmu — bentuk tertutup,
// bukan angka hasil pengukuran.
static const float kOrenNayarC1 = 0.5 - 2.0 / (3.0 * kPi);
static const float kOrenNayarC2 = 2.0 / 3.0 - 28.0 / (15.0 * kPi);

struct OpenPBRSurface {
    float  baseWeight;
    float3 baseColor;
    float  baseMetalness;
    float  baseDiffuseRoughness;

    float  specularWeight;
    float3 specularColor;
    float  specularRoughness;
    float  specularRoughnessAnisotropy;
    float  specularIor;

    float  coatWeight;
    float3 coatColor;
    float  coatRoughness;
    float  coatRoughnessAnisotropy;
    float  coatIor;
    float  coatDarkening;

    float  fuzzWeight;
    float3 fuzzColor;
    float  fuzzRoughness;

    // Nilai bawaannya harus sama dengan nilai bawaan pin di katalog node.
    // Keduanya adalah satu keputusan yang tertulis di dua tempat, dan yang di
    // sini yang berlaku saat sebuah pin dibiarkan tidak tersambung.
    static OpenPBRSurface defaults() {
        OpenPBRSurface s;
        s.baseWeight = 1.0;
        s.baseColor = float3(0.8);
        s.baseMetalness = 0.0;
        s.baseDiffuseRoughness = 0.0;

        s.specularWeight = 1.0;
        s.specularColor = float3(1.0);
        s.specularRoughness = 0.3;
        s.specularRoughnessAnisotropy = 0.0;
        s.specularIor = 1.5;

        s.coatWeight = 0.0;
        s.coatColor = float3(1.0);
        s.coatRoughness = 0.0;
        s.coatRoughnessAnisotropy = 0.0;
        s.coatIor = 1.6;
        s.coatDarkening = 1.0;

        s.fuzzWeight = 0.0;
        s.fuzzColor = float3(1.0);
        s.fuzzRoughness = 0.5;
        return s;
    }
};

// Lapisan yang disertakan. Dirakit `AssembleMaterialModule` dari graph yang
// bersangkutan; bawaannya menyalakan semuanya supaya berkas ini tetap sah
// dibaca sendirian.
//
// Nilainya diketahui saat kompilasi, jadi lapisan yang mati bukan cabang yang
// tidak diambil melainkan kode yang tidak pernah ada.
#ifndef OPENPBR_HAS_COAT
#define OPENPBR_HAS_COAT 1
#endif
#ifndef OPENPBR_HAS_FUZZ
#define OPENPBR_HAS_FUZZ 1
#endif
#ifndef OPENPBR_HAS_ANISOTROPY
#define OPENPBR_HAS_ANISOTROPY 1
#endif
#ifndef OPENPBR_HAS_DIFFUSE_ROUGHNESS
#define OPENPBR_HAS_DIFFUSE_ROUGHNESS 1
#endif

/// Bingkai shading di sebuah titik. Tangent dan bitangent hanya dipakai lobe
/// yang anisotropik; yang isotropik boleh mengabaikannya.
struct ShadingFrame {
    float3 normal;
    float3 tangent;
    float3 bitangent;
    float3 view;
};

/// Reflektansi normal dari indeks bias. Inilah yang membuat `specularIor`
/// bermakna alih-alih sekadar sebuah angka: 1.5 menghasilkan 0.04, angka yang
/// biasa dipakai sebagai F0 dielektrik.
float iorToF0(float ior) {
    const float r = (ior - 1.0) / (ior + 1.0);
    return r * r;
}

float3 fresnelSchlick(float3 f0, float cosTheta) {
    const float m = clamp(1.0 - cosTheta, 0.0, 1.0);
    const float m2 = m * m;
    return f0 + (float3(1.0) - f0) * (m2 * m2 * m);
}

/// Fresnel logam menurut model "F82-tint" yang ditetapkan spesifikasi.
///
/// **Inilah yang membuat `specularColor` berarti pada logam.** Schlick polos
/// hanya mengenal warna pada insidensi tegak lurus, sehingga pin warna spekular
/// terbuang habis begitu `baseMetalness` mencapai 1. Logam sungguhan justru
/// menunjukkan penurunan pantulan di dekat siluet, dan F82 memberi pengarang
/// kendali atasnya: `edgeTint` adalah warna di sudut menyerempet 82°,
/// dinyatakan sebagai pecahan dari kurva Schlick di sana.
///
/// **Pada `edgeTint` putih ia tereduksi tepat ke Schlick**, jadi menyalakannya
/// tidak mengubah satu pun material yang tidak menyentuh pin itu.
float3 fresnelF82(float3 f0, float3 edgeTint, float cosTheta) {
    const float3 schlick = fresnelSchlick(f0, cosTheta);

    const float m = clamp(1.0 - cosTheta, 0.0, 1.0);
    const float m2 = m * m;
    const float m6 = m2 * m2 * m2;
    // Bobot koreksi mencapai 1 tepat di mu = 1/7 dan nol di kedua ujungnya —
    // yang memang titik maksimum mu*(1-mu)^6, jadi ia tidak pernah melebihi 1.
    // Konstantanya 1 / (mu * (1-mu)^6) pada mu = 1/7.
    const float weight = cosTheta * m6 * (823543.0 / 46656.0);
    const float3 schlickAtEdge = fresnelSchlick(f0, kF82CosTheta);

    // Tepi tidak boleh lebih terang daripada Schlick — sifat yang membuat model
    // ini tidak bisa menghasilkan logam bertepi mustahil, dan yang dinyatakan
    // spesifikasi sebagai keuntungan parametrisasinya.
    return max(schlick - weight * schlickAtEdge * (float3(1.0) - edgeTint), float3(0.0));
}

/// Rasio IOR dasar dielektrik terhadap medium di atasnya.
///
/// **Coat mengubah apa yang bersentuhan dengan dasar.** Tanpa coat, dasar itu
/// bersinggungan dengan udara; dengan coat, ia bersinggungan dengan medium coat
/// yang jauh lebih rapat, dan pantulannya anjlok — pada `coatIor` 1,6 dan
/// `specularIor` 1,5, F0 turun dari 0,04 ke sekitar 0,001. Mengabaikannya
/// membuat setiap permukaan berpernis memantul puluhan kali lebih kuat daripada
/// seharusnya di lobe dasarnya.
///
/// Coat yang hanya menutupi sebagian dinyatakan sebagai interpolasi, karena
/// itulah artinya secara statistik: sebagian permukaan bertemu udara, sebagian
/// bertemu pernis.
float specularIorRatio(OpenPBRSurface surface) {
#if OPENPBR_HAS_COAT
    const float coatIor = max(surface.coatIor, 1e-4);
    const float specularIor = max(surface.specularIor, 1e-4);
    const float ratio = specularIor / coatIor;
    // **Rasio di bawah satu dibalik, bukan dipakai apa adanya.** F0 sendiri
    // kebal terhadap eta -> 1/eta, jadi pembalikan ini tidak mengubah apa pun
    // pada coat penuh — tetapi interpolasi *menuju* rasio itu berubah, dan
    // tanpa pembalikan coat separuh mendarat di F0 yang salah. Spesifikasi
    // menyebutnya di bagian TIR coat, dan implementasi referensi MaterialX
    // mengerjakannya persis begini.
    const float coated = ratio > 1.0 ? ratio : coatIor / specularIor;
    return lerp(surface.specularIor, coated, saturate(surface.coatWeight));
#else
    return surface.specularIor;
#endif
}

/// Sumbu roughness anisotropik. Anisotropi 0 mengembalikan dua nilai yang sama,
/// jadi lobe isotropik adalah kasus khusus dari yang sama — bukan cabang lain.
void anisotropicAlpha(float roughness, float anisotropy, out float ax, out float ay) {
    // Dijepit di kedua ujung. Spesifikasi membatasi r ke [0,1] dengan alasan
    // yang disebutkannya sendiri: alpha di atas satu tidak menghasilkan tampilan
    // kasar yang masuk akal, hanya lobe yang melebar tanpa arti.
    const float r = clamp(roughness, kMinRoughness, 1.0);
    const float alpha = r * r;
#if !OPENPBR_HAS_ANISOTROPY
    // Material yang tidak menyentuh anisotropi mendapat lobe isotropik apa
    // adanya: satu perkalian, tanpa akar kuadrat dan tanpa cabang.
    ax = alpha;
    ay = alpha;
#else
    // **Rumus OpenPBR, bukan rumus Disney.** Keduanya melebarkan lobe, tetapi
    // mengawetkan hal yang berbeda: yang Disney menjaga hasil kali axay, yang
    // ini menjaga ax^2 + ay^2 = 2*alpha^2. Spesifikasi menyebut alasannya
    // langsung — renderer yang *mematikan* anisotropi harus tetap menghasilkan
    // highlight yang sepadan dengan yang menyalakannya, dan itu hanya berlaku
    // kalau kekasaran rata-ratanya yang diawetkan.
    //
    //     ax = r^2 * sqrt(2 / (1 + (1-a)^2))     ay = (1-a) * ax
    //
    // Anisotropi negatif memutar sumbu panjangnya 90°, bukan menjadi tidak sah.
    // Itu perluasan di luar rentang [0,1] yang ditetapkan spesifikasi; pada
    // rentang yang ditetapkan, hasilnya persis rumus di atas.
    const float a = clamp(abs(anisotropy), 0.0, 1.0);
    const float oneMinusA = 1.0 - a;
    const float alphaLong = alpha * sqrt(2.0 / (1.0 + oneMinusA * oneMinusA));
    const float alphaShort = max(oneMinusA * alphaLong, kMinAlpha);
    if (anisotropy >= 0.0) {
        ax = alphaLong;
        ay = alphaShort;
    } else {
        ax = alphaShort;
        ay = alphaLong;
    }
#endif
}

float ggxDistribution(float ax, float ay, float3 h, ShadingFrame frame) {
    const float hx = dot(h, frame.tangent) / max(ax, 1e-6);
    const float hy = dot(h, frame.bitangent) / max(ay, 1e-6);
    const float hz = dot(h, frame.normal);
    const float d = hx * hx + hy * hy + hz * hz;
    return 1.0 / max(kPi * ax * ay * d * d, 1e-8);
}

/// Smith height-correlated, bentuk anisotropiknya.
float smithVisibility(float ax, float ay, float3 v, float3 l, ShadingFrame frame) {
    const float nv = abs(dot(frame.normal, v)) + 1e-5;
    const float nl = abs(dot(frame.normal, l)) + 1e-5;
    const float lambdaV =
        nl * length(float3(ax * dot(v, frame.tangent), ay * dot(v, frame.bitangent), nv));
    const float lambdaL =
        nv * length(float3(ax * dot(l, frame.tangent), ay * dot(l, frame.bitangent), nl));
    return 0.5 / max(lambdaV + lambdaL, 1e-6);
}

/// Koefisien Oren–Nayar bentuk Fujii, yang dipilih spesifikasi.
///
/// **Bukan bentuk klasiknya.** Spesifikasi menolak yang klasik dengan
/// kata-katanya sendiri — "suffers from artifacts and also does not conserve
/// energy (i.e. is too dark)" — dan menetapkan bentuk ini sebagai gantinya.
/// Perhatikan juga bahwa sigma **adalah** `baseDiffuseRoughness`, tidak
/// dikuadratkan: pengkuadratan adalah kebiasaan dari roughness spekular, dan
/// membawanya ke sini menggelapkan lagi permukaan yang sudah terlalu gelap.
void orenNayarCoefficients(float sigma, out float a, out float b) {
    a = 1.0 / (1.0 + kOrenNayarC1 * sigma);
    b = sigma * a;
}

/// Albedo terarah suku Oren–Nayar, untuk warna dasar putih.
///
/// **Bentuk tertutupnya diturunkan, bukan disalin.** Spesifikasi menunjuk
/// Portsmouth 2024 untuk ini tanpa menuliskannya. Integralnya elementer sekali
/// integral azimutnya dipisah menurut tanda s — separuh lingkaran memakai
/// t = max(mu_i, mu_o), separuhnya t = 1 — dan menghasilkan
///
///     Ehat(mu) = a + b * (2 sin/pi) [ (theta - sin*mu)/2 + (1 - sin^3)/(3 mu) - 1/3 ]
///
/// Dicocokkan terhadap integrasi numerik langsung sampai 1e-6.
float orenNayarAlbedo(float a, float b, float mu) {
    const float m = clamp(mu, 1e-4, 1.0);
    const float sn = sqrt(max(0.0, 1.0 - m * m));
    const float theta = acos(m);
    return a + b * (2.0 * sn / kPi) *
                   ((theta - sn * m) * 0.5 + (1.0 - sn * sn * sn) / (3.0 * m) - 1.0 / 3.0);
}

/// Bobot suku kompensasi energi.
///
/// **Inilah yang membuat Oren–Nayar berhenti terlalu gelap.** Model aslinya
/// hanya menghitung satu pantulan antar-mikrofaset dan membuang sisanya:
/// 5,0% energi pada sigma 0,25, 9,4% pada 0,5, dan **16,7% pada 1,0**. Yang
/// dikembalikan di sini adalah pantulan lanjutan itu, tersebar merata — dan
/// sifat yang membuatnya sah adalah uji tungku putih lulus tepat 1,0 pada
/// seluruh sigma dan seluruh sudut pandang.
float3 orenNayarMultiScatter(float3 rho, float averageAlbedo) {
    const float rest = max(1.0 - averageAlbedo, 1e-4);
    return rho * rho * (averageAlbedo / rest) / max(float3(1.0) - rho * rest, float3(1e-4));
}

/// Difus EON: Oren–Nayar bentuk Fujii ditambah kompensasi energinya.
///
/// `baseDiffuseRoughness` 0 jatuh persis ke Lambert, jadi material yang tidak
/// menyentuhnya tidak membayar satu instruksi pun untuk yang di atas.
float3 diffuseLobe(OpenPBRSurface surface, float nl, float nv, float lv) {
    if (!OPENPBR_HAS_DIFFUSE_ROUGHNESS || surface.baseDiffuseRoughness <= 0.0) {
        return surface.baseColor / kPi;
    }
    float a, b;
    orenNayarCoefficients(saturate(surface.baseDiffuseRoughness), a, b);

    const float s = lv - nl * nv;
    const float t = s <= 0.0 ? 1.0 : max(nl, nv) + 1e-5;
    const float3 orenNayar = surface.baseColor * (a + b * s / t);

    const float3 compensation = orenNayarMultiScatter(surface.baseColor,
                                                      a + b * kOrenNayarC2) *
                                (1.0 - orenNayarAlbedo(a, b, nl)) *
                                (1.0 - orenNayarAlbedo(a, b, nv));
    return (orenNayar + compensation) / kPi;
}

/// Albedo terarah lobe difus, sudah termasuk warna dasarnya.
///
/// **Dipakai jalur lingkungan.** Di bawah iradiansi seragam yang keluar dari
/// lobe difus adalah albedo terarahnya, bukan `baseColor` begitu saja —
/// dan memakai `baseColor` di sana berarti kekasaran difus terlihat di bawah
/// lampu tetapi lenyap di bawah langit. Pada kekasaran nol keduanya sama.
float3 diffuseAlbedo(OpenPBRSurface surface, float mu) {
    if (!OPENPBR_HAS_DIFFUSE_ROUGHNESS || surface.baseDiffuseRoughness <= 0.0) {
        return surface.baseColor;
    }
    float a, b;
    orenNayarCoefficients(saturate(surface.baseDiffuseRoughness), a, b);
    const float averageAlbedo = a + b * kOrenNayarC2;
    const float directional = orenNayarAlbedo(a, b, mu);
    return surface.baseColor * directional +
           orenNayarMultiScatter(surface.baseColor, averageAlbedo) * (1.0 - directional) *
               (1.0 - averageAlbedo);
}

/// Reflektansi terarah lapisan fuzz, `E_fuzz` di spesifikasi.
///
/// **Fit Gaussian terhadap tabel Zeltner 2022**, disalin dari implementasi
/// referensi MaterialX (`mx_zeltner_sheen_dir_albedo`). Inilah yang membuat
/// pelapisan fuzz mungkin tanpa tabel: yang di bawahnya diredam `1 - E_fuzz`,
/// dan tanpa besaran ini fuzz hanya bisa menambah energi.
float zeltnerSheenAlbedo(float nv, float roughness) {
    const float y = roughness;
    const float s = y * (0.0206607 + 1.58491 * y) / (0.0379424 + y * (1.32227 + y));
    const float m =
        y * (-0.193854 + y * (-1.14885 + y * (1.7932 - 0.95943 * y * y))) / (0.046391 + y);
    const float o =
        y * (0.000654023 + (-0.0207818 + 0.119681 * y) * y) / (1.26264 + y * (-1.92021 + y));
    const float g = (nv - m) / s;
    // Dijepit: suku offset fit-nya sesekali turun sedikit di bawah nol, dan
    // reflektansi negatif membuat peredaman lapisan bawah justru menguatkannya.
    return saturate(exp(-0.5 * g * g) / (s * sqrt(2.0 * kPi)) + o);
}

/// Koefisien matriks LTC lobe fuzz, fit rasional Zeltner 2022.
float zeltnerSheenAInv(float nv, float roughness) {
    return (2.58126 * nv + 0.813703 * roughness) * roughness /
           (1.0 + 0.310327 * nv * nv + 2.60994 * nv * roughness);
}

float zeltnerSheenBInv(float nv, float roughness) {
    return sqrt(max(0.0, 1.0 - nv)) * (roughness - 1.0) * roughness * roughness * roughness /
           (0.0000254053 + 1.71228 * nv - 1.71506 * nv * roughness +
            1.34174 * roughness * roughness);
}

/// Lobe fuzz: Linearly Transformed Cosine yang difit ke simulasi microflake
/// SGGX berserat.
///
/// **Ini model yang ditetapkan spesifikasi**, bukan sheen invers-roughness yang
/// biasa dipakai — `open_pbr_surface.mtlx` menyetel `mode = "zeltner"` secara
/// eksplisit. Serat yang sumbunya tegak lurus permukaan menghasilkan sorot di
/// sudut menyerempet, dan `fuzzRoughness` mengatur seberapa berserat sebarannya:
/// rendah berarti serat tipis dan kain berkilau, tinggi berarti microflake bulat
/// dan permukaan berdebu.
///
/// **Mengembalikan lobe yang sudah memuat kosinusnya sendiri.** Spesifikasi
/// menuliskannya sebagai `mu_i * f_fuzz = F * E_fuzz * D`, jadi pemanggil tidak
/// boleh mengalikannya lagi dengan `n . l` — dan itulah sebab lobe ini dipasang
/// setelah kosinusnya, bukan sebelum.
float zeltnerSheenLobe(float3 l, ShadingFrame frame, float nv, float roughness) {
    // Tangent di bidang yang memuat pandangan dan normal. Arah inilah yang
    // mengorientasikan lobe LTC; sembarang tangent lain memutar sorotnya.
    float3 x = frame.view - frame.normal * nv;
    const float lenSqr = dot(x, x);
    // Pandangan yang berimpit normal membuat bidang itu tidak terdefinisi, dan
    // di sana orientasi apa pun sama benarnya.
    x = lenSqr > 1e-8 ? x * rsqrt(lenSqr) : frame.tangent;
    const float3 y = cross(frame.normal, x);

    const float3 w = float3(dot(l, x), dot(l, y), dot(l, frame.normal));
    const float aInv = zeltnerSheenAInv(nv, roughness);
    const float bInv = zeltnerSheenBInv(nv, roughness);

    // Dikembalikan ke konfigurasi asalnya, yaitu kosinus terjepit.
    const float3 wo = float3(aInv * w.x + bInv * w.z, aInv * w.y, w.z);
    const float woSqr = max(dot(wo, wo), 1e-8);
    const float ratio = aInv / woSqr;
    return (max(wo.z, 0.0) / kPi) * ratio * ratio;
}

/// Sumbu anisotropi setelah diputar peta tangent.
///
/// **Inilah yang membuat anisotropi bisa diarahkan.** Tanpanya sumbu panjang
/// selalu mengikuti tangent mesh, dan logam sikat melingkar — dasar panci,
/// pelek roda — tidak bisa dibuat sama sekali: sorotnya akan selalu meregang
/// menurut jahitan UV alih-alih menurut goresannya.
///
/// Arahnya diberikan di ruang tangent, jadi (1,0,0) berarti "seperti tangent
/// mesh". Hasilnya diproyeksikan kembali ke bidang permukaan, karena sumbu yang
/// menyembul keluar bidang memutar highlight ke arah yang tidak ada padanannya
/// di permukaan.
float3 rotatedTangent(ShadingFrame frame, float3 tangentSpaceDirection) {
    if (dot(tangentSpaceDirection, tangentSpaceDirection) < 1e-8) {
        return frame.tangent;
    }
    const float3 t = frame.tangent * tangentSpaceDirection.x +
                     frame.bitangent * tangentSpaceDirection.y +
                     frame.normal * tangentSpaceDirection.z;
    const float3 projected = t - frame.normal * dot(frame.normal, t);
    const float lenSqr = dot(projected, projected);
    // Arah yang jatuh tepat pada normal tidak menunjuk ke mana pun di bidang
    // permukaan, dan di sana tangent mesh tetap yang berlaku.
    return lenSqr > 1e-8 ? projected * rsqrt(lenSqr) : frame.tangent;
}

/// Bingkai shading milik coat.
///
/// **Dibangun dari bingkai mesh, bukan dari bingkai dasar yang sudah dilekukkan
/// peta normalnya.** Spesifikasi menyatakan `geometry_normal` dan
/// `geometry_coat_normal` sebagai dua pelekukan yang berdiri sendiri atas normal
/// yang sama — dan itu memang gambaran fisisnya: coat setebal berapa pun
/// mengisi lekukan dasarnya, sehingga permukaan luarnya tidak mengikutinya.
/// Itulah kulit jeruk pada cat mobil.
ShadingFrame makeCoatFrame(ShadingFrame meshFrame, float3 coatNormal, float3 coatTangent) {
    ShadingFrame coat = meshFrame;
    if (dot(coatNormal, coatNormal) > 1e-8) {
        const float3 n = normalize(coatNormal);
        coat.normal = normalize(meshFrame.tangent * n.x + meshFrame.bitangent * n.y +
                                meshFrame.normal * n.z);
    }
    coat.tangent = rotatedTangent(coat, coatTangent);
    coat.bitangent = cross(coat.normal, coat.tangent);
    return coat;
}

/// Suku DFG lobe GGX secara analitik: x menskala F0, y adalah bias.
///
/// **Ini yang membuat jalur cahaya langsung bisa memakai model energi yang sama
/// dengan jalur lingkungan.** Yang terakhir membacanya dari LUT; yang pertama
/// tidak punya LUT sama sekali, dan sebelumnya karena itu memakai `1 - F`
/// polos — sehingga satu material menjawab dua hal berbeda tergantung dari mana
/// cahayanya datang.
///
/// Fit rasional kuadratik dari implementasi referensi MaterialX
/// (`mx_ggx_dir_albedo_analytic`). Perhatikan parameternya alpha, bukan
/// roughness.
float2 ggxDirectionalAlbedo(float nv, float alpha) {
    const float x = saturate(nv);
    const float y = saturate(alpha);
    const float x2 = x * x;
    const float y2 = y * y;
    const float4 r = float4(0.1003, 0.9345, 1.0, 1.0) +
                     float4(-0.6303, -2.323, -1.765, 0.2281) * x +
                     float4(9.748, 2.229, 8.263, 15.94) * y +
                     float4(-2.038, -3.748, 11.53, -55.83) * x * y +
                     float4(29.34, 1.424, 28.96, 13.08) * x2 +
                     float4(-8.245, -0.7684, -7.507, 41.26) * y2 +
                     float4(-26.44, 1.436, -36.11, 54.9) * x2 * y +
                     float4(19.99, 0.2913, 15.86, 300.2) * x * y2 +
                     float4(-5.448, 0.6286, 33.37, -285.1) * x2 * y2;
    return clamp(float2(r.x / r.z, r.y / r.w), float2(0.0), float2(1.0));
}

/// Reflektansi rata-rata Fresnel Schlick atas seluruh belahan.
///
/// Tetapan 1/21 eksak untuk eksponen Schlick 5. Fresnel pada satu arah akan
/// mengembalikan energi yang salah persis di sudut menyerempet, dan itulah
/// sudut tempat energi yang hilang paling terlihat.
float3 fresnelAverage(float3 f0) { return f0 + (float3(1.0) - f0) / 21.0; }

/// Pengali kompensasi energi untuk lobe GGX pantulan-tunggal.
///
/// **GGX satu pantulan membuang cahaya yang di permukaan nyata memantul lagi di
/// lereng sebelah**, dan buangannya besar: logam putih kehilangan 27,6%
/// energinya pada kekasaran 0,6 dan 63,9% pada 1,0. Itu bukan kesalahan kecil —
/// itu logam kasar yang tampak abu-abu kotor, dan tidak ada penyetelan material
/// yang bisa memperbaikinya karena energinya hilang sebelum sampai ke
/// penyetelan.
float3 ggxEnergyCompensation(float3 f0, float singleScatterAlbedo) {
    const float ess = max(singleScatterAlbedo, 1e-4);
    return float3(1.0) + fresnelAverage(f0) * ((1.0 - ess) / ess);
}

/// Emisi setelah melewati coat.
///
/// **Spesifikasi menaruh emisi di bawah coat, bukan di atasnya**, dan alasannya
/// disebut langsung: supaya cahaya yang dipancarkan ikut diwarnai serapan coat.
/// Itulah yang membuat glow stick dan layar di balik kaca bisa digambar tanpa
/// memodelkan emitornya sebagai benda terpisah.
///
/// Faktor transmisinya komplemen Schlick — penuh menghadap kamera, nol di tepi
/// menyerempet, karena di sana cahayanya terpantul balik ke dalam.
///
/// Fuzz **tidak** ikut meredam, mengikuti implementasi referensi: di sana emisi
/// adalah EDF yang berdiri di samping BSDF berlapis, bukan di bawahnya.
float3 coatedEmission(OpenPBRSurface surface, float3 emissive, float nv) {
#if OPENPBR_HAS_COAT
    const float3 coatF0 = float3(iorToF0(surface.coatIor));
    const float3 transmitted = surface.coatColor * (float3(1.0) - fresnelSchlick(coatF0, nv));
    return emissive * lerp(float3(1.0), transmitted, saturate(surface.coatWeight));
#else
    return emissive;
#endif
}

/// Kekasaran dasar setelah diasarkan coat di atasnya.
///
/// **Coat yang kasar mengaburkan yang terlihat lewatnya.** Tanpa ini, sebuah
/// dasar licin di bawah pernis buram tetap memantul setajam cermin — pantulan
/// yang seharusnya tersapu justru bertahan utuh.
///
/// Dimodelkan sebagai konvolusi dua NDF: variansinya dijumlahkan dalam ruang
/// kemiringan, dan **variansi coat dihitung dua kali** karena pantulannya
/// menembus batas coat sekali masuk dan sekali keluar.
float coatRoughenedSpecular(OpenPBRSurface surface) {
#if OPENPBR_HAS_COAT
    const float base = saturate(surface.specularRoughness);
    const float coat = saturate(surface.coatRoughness);
    const float base4 = base * base * base * base;
    const float coat4 = coat * coat * coat * coat;
    const float roughened = pow(min(1.0, base4 + 2.0 * coat4), 0.25);
    return lerp(base, roughened, saturate(surface.coatWeight));
#else
    return surface.specularRoughness;
#endif
}

/// Rasio IOR dasar setelah dimodulasi `specularWeight`.
///
/// **`specularWeight` bukan pengali lobe.** Mengalikan lobe menurunkan pantulan
/// menyerempet sama besarnya dengan pantulan tegak lurus. Spesifikasi justru
/// memodulasi reflektansi pada insidensi normal *saja*, lewat rasio IOR yang
/// diubah, sehingga tepi menyerempet tetap mendekati satu — dan itulah yang
/// membedakan plastik kusam dari plastik yang salah dimodelkan.
///
/// Yang dijamin rumus ini: pada insidensi normal, F0 yang dihasilkan tepat
/// `specularWeight * F0_asli`. Di sudut lain, kurva Fresnel penuhlah yang
/// berlaku.
///
/// Penjepitan ke 0,99999 sekaligus menegakkan batas `xi <= 1/F` yang disebut
/// spesifikasi: reflektansi terskala tidak boleh mencapai satu, karena rasio
/// IOR-nya akan meledak ke tak hingga di sana.
float modulatedSpecularIor(OpenPBRSurface surface) {
    const float eta = specularIorRatio(surface);
    const float scaled = clamp(surface.specularWeight * iorToF0(eta), 0.0, 0.99999);
    const float epsilon = sign(eta - 1.0) * sqrt(scaled);
    return (1.0 + epsilon) / max(1.0 - epsilon, 1e-6);
}

/// Albedo hemisferis faktor Fresnel dielektrik.
///
/// Hampiran tertutup d'Eon 2021 yang ditunjuk spesifikasi, akurat dalam 0,2%
/// pada eta di [1,3] — jadi penggelapan coat tidak menuntut LUT sendiri.
float fresnelAverageAlbedo(float eta) {
    return log((10893.0 * eta - 1438.2) / (-774.4 * eta * eta + 10212.0 * eta + 1.0));
}

/// Faktor penggelapan coat: berapa bagian yang tersisa setelah cahaya memantul
/// berulang di sisi dalam coat.
///
/// **Ini efek yang dikendalikan `coatDarkening`, dan hanya ini.** Cahaya yang
/// keluar dari dasar sebagian dipantulkan kembali oleh sisi dalam coat, menimpa
/// dasar lagi, dan terserap lagi — sehingga warnanya menggelap sekaligus menjadi
/// lebih jenuh. Dasar yang memantulkan seluruh energi tidak kehilangan apa pun,
/// dan rumusnya memang menjawab 1 di sana.
float3 coatDarkeningFactor(OpenPBRSurface surface, float3 baseAlbedo, float coatReflectance) {
    const float etaC = max(surface.coatIor, 1.0);

    // Koefisien pantulan internal, yaitu bagian energi yang kembali ke dasar.
    // Dasar kasar menyebarkan cahayanya dan sering melewati sudut kritis;
    // dasar mulus memantul spekular dan jarang melewatinya. Karena itu keduanya
    // punya nilai sendiri, dijembatani taksiran kekasaran dasar.
    const float kRough = 1.0 - (1.0 - fresnelAverageAlbedo(etaC)) / (etaC * etaC);
    const float kSmooth = coatReflectance;

    // Dielektrik yang pantulan spekularnya lemah berperilaku seperti permukaan
    // kasar: hampir seluruh cahayanya masuk ke lapis difus dan keluar tersebar.
    const float fS = iorToF0(specularIorRatio(surface));
    const float rDielectric =
        lerp(1.0, surface.specularRoughness, saturate(surface.specularWeight * fS));
    const float rBase = lerp(rDielectric, surface.specularRoughness, surface.baseMetalness);

    const float k = lerp(kSmooth, kRough, saturate(rBase));
    return (1.0 - k) / max(float3(1.0) - baseAlbedo * k, float3(1e-4));
}

/// Apa yang tersisa dari lapisan di bawah coat.
///
/// **Tiga hal yang berbeda, dan hanya yang ketiga milik `coatDarkening`.**
/// Menyatukan ketiganya di bawah satu pengali — seperti sebelumnya — membuat
/// `coatDarkening = 0` ikut menghapus warna pernisnya, sehingga pernis merah
/// menjadi bening. Spesifikasi hanya membatalkan penggelapannya di sana.
///
///   1. serapan medium coat                → `coatColor`
///   2. energi yang dipantulkan permukaan coat → `1 - E_coat`
///   3. pantulan berulang di dalam coat     → `coatDarkeningFactor`
///
/// Cakupan coat masuk ke keduanya secara terpisah, dan untuk penggelapan ia
/// masuk sebagai hasil kali `C * delta` — persis skema yang direkomendasikan
/// spesifikasi: coat yang menutupi separuh permukaan hanya menggelapkan
/// separuhnya.
float3 coatUnderlayer(OpenPBRSurface surface, float coatReflectance) {
    const float3 transmitted = surface.coatColor * (1.0 - coatReflectance);
    // Albedo dasar adalah albedo tiap slab penyusunnya, dicampur menurut bobot
    // campurannya — dan bagi slab logam yang menskalanya `specularWeight`,
    // karena Fresnel logam memang dikalikannya.
    const float3 baseAlbedo =
        surface.baseColor * surface.baseWeight *
        lerp(1.0, saturate(surface.specularWeight), surface.baseMetalness);
    const float3 darkening = coatDarkeningFactor(surface, baseAlbedo, coatReflectance);
    return lerp(float3(1.0), transmitted, surface.coatWeight) *
           lerp(float3(1.0), darkening, surface.coatWeight * surface.coatDarkening);
}

/// Evaluasi permukaan terhadap satu arah cahaya.
///
/// `lightDirection` menunjuk **dari permukaan ke cahaya**. Arah yang terbalik
/// adalah kesalahan yang tidak menghasilkan galat apa pun — hanya permukaan yang
/// gelap di sisi yang salah — jadi arahnya disebutkan di sini alih-alih
/// diandaikan.
float3 evaluateOpenPBR(OpenPBRSurface surface, ShadingFrame frame, ShadingFrame coatFrame,
                       float3 lightDirection, float3 lightRadiance) {
    const float3 n = frame.normal;
    const float3 v = frame.view;
    const float3 l = lightDirection;
    const float nl = dot(n, l);
    if (nl <= 0.0) {
        return float3(0.0);
    }
    const float nv = max(dot(n, v), 1e-5);
    const float3 h = normalize(v + l);
    const float vh = max(dot(v, h), 1e-5);

    // Logam tidak punya lobe difus, dan Fresnel-nya berbentuk lain sama sekali.
    // Keduanya dinyatakan sebagai interpolasi, bukan sebagai cabang: metalness
    // di antara 0 dan 1 adalah nilai yang sah dan sering dipakai di tepi peta.
    //
    // **Dua Fresnel, bukan satu F0 yang di-lerp.** Menggabungkannya di tingkat
    // F0 memaksa keduanya memakai kurva yang sama, dan kurva logam justru bukan
    // Schlick — itulah yang membuat `specularColor` tidak berpengaruh apa pun
    // pada logam sebelum ini.
    // **`specularWeight` masuk lewat dua pintu yang berbeda**, dan itu memang
    // yang ditetapkan spesifikasi: pada dielektrik ia memodulasi rasio IOR,
    // pada logam ia mengalikan Fresnel-nya. Menyeragamkannya jadi satu pengali
    // lobe — seperti sebelumnya — mematikan kilau tepi dielektrik yang justru
    // seharusnya bertahan.
    const float3 dielectricF0 =
        float3(iorToF0(modulatedSpecularIor(surface))) * surface.specularColor;
    const float3 dielectricFresnel = fresnelSchlick(dielectricF0, vh);
    // Reflektansi logam pada insidensi normal adalah baseWeight * baseColor;
    // bobotnya ikut, sama seperti ia ikut pada lobe difus.
    const float3 metalF0 = surface.baseColor * surface.baseWeight;
    const float3 metalFresnel =
        fresnelF82(metalF0, surface.specularColor, vh) * surface.specularWeight;
    const float3 fresnel = lerp(dielectricFresnel, metalFresnel, surface.baseMetalness);

    const float baseRoughness = coatRoughenedSpecular(surface);
    float ax, ay;
    anisotropicAlpha(baseRoughness, surface.specularRoughnessAnisotropy, ax, ay);

    // Pemetaan anisotropi mengawetkan kekasaran rata-rata, jadi alpha isotropik
    // ekuivalennya tetap r^2 — dan itu yang benar untuk ditanyakan ke albedo
    // terarah.
    const float2 dfg = ggxDirectionalAlbedo(nv, max(baseRoughness * baseRoughness, kMinAlpha));
    const float3 compensation =
        ggxEnergyCompensation(lerp(dielectricF0, metalF0, surface.baseMetalness), dfg.x + dfg.y);

    const float3 specular = fresnel * (ggxDistribution(ax, ay, h, frame) *
                                       smithVisibility(ax, ay, v, l, frame)) *
                            compensation;

    // **Yang sampai ke difus adalah sisa dari albedo terarah spekularnya, bukan
    // sisa dari Fresnel pada satu arah.** Keduanya berbeda jauh pada permukaan
    // kasar, dan yang kedua itulah yang membuat jalur ini berselisih dengan
    // jalur lingkungan atas material yang sama. Bagian logamnya dipotong
    // terpisah oleh `1 - baseMetalness`.
    const float3 dielectricAlbedo = (dielectricF0 * dfg.x + float3(dfg.y)) * compensation;
    const float3 diffuseEnergy =
        max(float3(1.0) - dielectricAlbedo, float3(0.0)) * (1.0 - surface.baseMetalness);
    const float3 diffuse =
        diffuseLobe(surface, nl, nv, dot(l, v)) * diffuseEnergy * surface.baseWeight;

    float3 result = diffuse + specular;
    // Sorot coat dikumpulkan terpisah karena kosinusnya bukan kosinus dasar.
    float3 coatLayer = float3(0.0);

    if (OPENPBR_HAS_COAT && surface.coatWeight > 0.0) {
        float cx, cy;
        anisotropicAlpha(surface.coatRoughness, surface.coatRoughnessAnisotropy, cx, cy);
        const float coatF0 = iorToF0(surface.coatIor);
        const float coatRoughness = saturate(surface.coatRoughness);
        // Sudut pandang diukur terhadap normal coat, bukan normal dasar. Pada
        // material yang tidak mengemudikan `coatNormal` keduanya benda yang
        // sama, jadi ini tidak menambah apa pun di sana.
        const float coatNv = max(dot(coatFrame.normal, coatFrame.view), 1e-5);
        const float2 coatDfg =
            ggxDirectionalAlbedo(coatNv, max(coatRoughness * coatRoughness, kMinAlpha));
        const float3 coatCompensation =
            ggxEnergyCompensation(float3(coatF0), coatDfg.x + coatDfg.y);

        const float3 coatSpecular = fresnelSchlick(float3(coatF0), vh) *
                                    (ggxDistribution(cx, cy, h, coatFrame) *
                                     smithVisibility(cx, cy, v, l, coatFrame)) *
                                    coatCompensation;

        // **Penskalaan albedo memakai albedo TERARAH coat, bukan Fresnel pada
        // satu arah.** Yang dikurangi dari lapisan di bawah adalah seluruh
        // energi yang dipantulkan coat ke arah penonton — sebuah integral atas
        // lobe, dan pada coat kasar ia jauh lebih besar daripada Fresnel-nya.
        const float coatAlbedo = saturate(((coatF0 * coatDfg.x + coatDfg.y) *
                                           coatCompensation).x);
        result = result * coatUnderlayer(surface, coatAlbedo);
        // **Sorot coat membawa kosinusnya sendiri**, diukur terhadap normal
        // coat. Memakai kosinus dasar untuk keduanya membuat coat yang
        // dinormal-map — kulit jeruk pada cat mobil — memantul seakan
        // permukaannya rata.
        coatLayer = coatSpecular * (surface.coatWeight * max(dot(coatFrame.normal, l), 0.0));
    }

    // **Kosinusnya dipasang di sini, bukan di akhir.** Lobe fuzz dan sorot coat
    // sudah memuat kosinusnya masing-masing, jadi keduanya ditambahkan setelah
    // suku-suku lain dikalikan `n . l` — bukan sebelum.
    result = result * nl + coatLayer;

    if (OPENPBR_HAS_FUZZ && surface.fuzzWeight > 0.0) {
        // **Fuzz berada di atas coat, jadi ia meredam seluruh yang di bawahnya**
        // — termasuk sorot coat. Kode sebelumnya menambahkan fuzz sebelum coat,
        // sehingga urutan lapisnya justru terbalik: yang paling atas ikut
        // diredam yang di bawahnya.
        //
        // Rentangnya dijepit ke rentang fit aslinya.
        const float fuzzRoughness = clamp(surface.fuzzRoughness, 0.01, 1.0);
        const float fuzzAlbedo = zeltnerSheenAlbedo(nv, fuzzRoughness);
        result *= 1.0 - fuzzAlbedo * surface.fuzzWeight;
        result += surface.fuzzColor *
                  (fuzzAlbedo * zeltnerSheenLobe(l, frame, nv, fuzzRoughness) *
                   surface.fuzzWeight);
    }

    return result * lightRadiance;
}

/// Bentuk ringkas: coat memakai bingkai yang sama dengan dasarnya.
///
/// **Ini yang benar untuk material yang tidak mengemudikan `coatNormal`**, dan
/// itu hampir semuanya. Menyalurkan bingkai yang sama dua kali tidak menambah
/// ongkos apa pun — seluruhnya di-inline, dan kompilator melihat keduanya benda
/// yang sama.
float3 evaluateOpenPBR(OpenPBRSurface surface, ShadingFrame frame, float3 lightDirection,
                       float3 lightRadiance) {
    return evaluateOpenPBR(surface, frame, frame, lightDirection, lightRadiance);
}

// --- Pencahayaan berbasis gambar ---------------------------------------------
//
// **Ketiga masukannya diserahkan sudah jadi, bukan diambil sendiri dari
// tekstur.** `evaluateOpenPBR_IBL` tidak tahu apakah irradiance-nya datang dari
// probe statis, dari harmonik bola, atau nanti dari global illumination — dan
// justru itu titik sambungnya: mengganti sumbernya tidak menyentuh satu baris
// pun model shading. Fungsi yang mengambil sendiri dari sampler tertentu akan
// mengunci sumbernya ke dalam model.

/// Iradiansi dari sembilan koefisien harmonik bola.
///
/// **Cermin dari `EvaluateIrradiance` di sisi C++, termasuk ketiga konstanta
/// konvolusinya.** Keduanya harus bergerak bersama: yang di sini mengembalikan E
/// yang sudah dikonvolusi lobe kosinus, dan melewatkan konvolusinya menghasilkan
/// angka yang tetap masuk akal — halus dan berwarna benar — tapi salah sebesar
/// faktor yang berbeda per pita.
float3 evaluateIrradianceSh(float3 sh[9], float3 normal) {
    const float3 n = normalize(normal);
    const float a0 = 3.141593;  // pi
    const float a1 = 2.094395;  // 2pi/3
    const float a2 = 0.785398;  // pi/4

    float3 result = sh[0] * (0.282095 * a0);
    result += sh[1] * (0.488603 * n.y * a1);
    result += sh[2] * (0.488603 * n.z * a1);
    result += sh[3] * (0.488603 * n.x * a1);
    result += sh[4] * (1.092548 * n.x * n.y * a2);
    result += sh[5] * (1.092548 * n.y * n.z * a2);
    result += sh[6] * (0.315392 * (3.0 * n.z * n.z - 1.0) * a2);
    result += sh[7] * (1.092548 * n.x * n.z * a2);
    result += sh[8] * (0.546274 * (n.x * n.x - n.y * n.y) * a2);
    // Orde dua tidak bisa mewakili tepi tajam tanpa melewati batas, dan
    // iradiansi negatif merambat ke seluruh perhitungan sesudahnya.
    return max(result, float3(0.0));
}

/// Kekasaran → level mip pada peta prefilter.
///
/// Linear terhadap kekasaran, sama dengan `RoughnessForMip` di sisi C++.
/// Keduanya harus bergerak bersama: pemetaan yang berbeda berarti mip yang
/// dibaca bukan mip yang dibakar untuk kekasaran itu, dan yang terlihat adalah
/// pantulan yang buramnya tidak sesuai dengan slider.
float prefilterMipForRoughness(float roughness, float mipCount) {
    return saturate(roughness) * max(mipCount - 1.0, 0.0);
}

/// Arah baca peta prefilter untuk sebuah permukaan.
float3 prefilterDirection(ShadingFrame frame) {
    return reflect(-frame.view, frame.normal);
}

/// Suku DFG yang sudah dibaca dari LUT: x = skala F0, y = bias.
float3 environmentSpecular(float3 f0, float2 dfg, float3 prefiltered) {
    return prefiltered * (f0 * dfg.x + dfg.y);
}

/// Pantulan lingkungan sebuah permukaan.
///
/// `irradiance` adalah E — sudah dikonvolusi lobe kosinus — bukan radiance
/// rata-rata. Pembagian dengan pi ada di dalam sini; melakukannya dua kali
/// menghasilkan difus yang terlalu gelap sebesar faktor pi, dan sekali pun tidak
/// menghasilkan yang terlalu terang sebesar faktor yang sama.
float3 evaluateOpenPBR_IBL(OpenPBRSurface surface, ShadingFrame frame, ShadingFrame coatFrame,
                           float3 irradiance, float3 prefilteredBase, float3 prefilteredCoat,
                           float2 dfg) {
    const float nv = max(dot(frame.normal, frame.view), 1e-5);

    const float3 dielectricF0 =
        float3(iorToF0(modulatedSpecularIor(surface))) * surface.specularColor;
    const float3 metalF0 = surface.baseColor * surface.baseWeight;
    const float3 f0 = lerp(dielectricF0, metalF0, surface.baseMetalness);

    // **Pembagian energi tiga suku, cermin dari `SplitEnergy` di sisi C++.**
    // Split-sum satu-pantulan membuang cahaya yang di permukaan nyata memantul
    // lagi di lereng sebelah, dan buangannya besar: logam putih kehilangan 27,6%
    // energinya pada kekasaran 0,6 dan 63,9% pada 1,0. Itu bukan kesalahan kecil
    // — itu logam kasar yang tampak abu-abu kotor, dan tidak ada penyetelan
    // material yang bisa memperbaikinya karena energinya hilang sebelum sampai
    // ke penyetelan.
    const float3 singleScatter = f0 * dfg.x + dfg.y;
    const float missing = 1.0 - (dfg.x + dfg.y);
    const float3 average = fresnelAverage(f0);
    const float3 multiScatter =
        singleScatter * average / max(float3(1.0) - missing * average, float3(1e-6)) * missing;
    // Sisanya, dan hanya sisanya, boleh dipakai difus. Ditulis sebagai
    // pengurangan supaya ketiganya berjumlah tepat satu menurut konstruksi —
    // dan itu yang membuat uji white furnace lulus di seluruh rentang.
    //
    // Logam putih menyisakan nol di sini dengan sendirinya. Aturan "metal
    // mengambil dari lapis spekular, bukan iradiansi difus" karena itu tidak
    // perlu ditulis sebagai cabang tersendiri — cabang berarti dua tempat yang
    // bisa berselisih.
    const float3 diffuseEnergy =
        max(float3(1.0) - singleScatter - multiScatter, float3(0.0)) *
        (1.0 - surface.baseMetalness);

    // **F82 di jalur lingkungan diterapkan sebagai rasio, bukan dibakar ke LUT.**
    // LUT DFG dibangun untuk Schlick; membakar ulang varian F82 berarti sebuah
    // LUT kedua berdimensi warna tepi, dan itu perubahan yang jauh lebih besar
    // daripada yang dibayarnya. Rasio pada arah pandang adalah hampiran — tetapi
    // hampiran yang membuat logam bertepi gelap tetap bertepi gelap di bawah
    // lingkungan, bukan hanya di bawah lampu. Dua jalur yang berselisih tentang
    // logam yang sama jauh lebih menyesatkan daripada satu hampiran yang
    // disebutkan.
    //
    // Pada `specularColor` putih rasionya tepat 1, jadi jalur ini tidak berubah
    // sama sekali untuk material yang tidak menyentuh pin itu.
    const float3 metalTint = fresnelF82(metalF0, surface.specularColor, nv) /
                             max(fresnelSchlick(metalF0, nv), float3(1e-4));
    // Bobot spekular tidak lagi mengalikan seluruh lobe di sini: bagian
    // dielektriknya sudah termuat di `dielectricF0`, dan justru itu yang membuat
    // energinya tetap kekal — yang tidak dipantulkan spekular jatuh ke difus
    // dengan sendirinya lewat pembagian energi di atas. Yang tersisa hanyalah
    // bagian logamnya.
    const float3 specular =
        (environmentSpecular(f0, dfg, prefilteredBase) + multiScatter * (irradiance / kPi)) *
        lerp(float3(1.0), metalTint * surface.specularWeight, surface.baseMetalness);
    float3 result = diffuseAlbedo(surface, nv) * (irradiance / kPi) * diffuseEnergy *
                        surface.baseWeight +
                    specular;

    if (OPENPBR_HAS_COAT && surface.coatWeight > 0.0) {
        const float coatF0 = iorToF0(surface.coatIor);
        const float3 coatSpecular = environmentSpecular(float3(coatF0), dfg, prefilteredCoat);

        // Suku DFG yang diserahkan pemanggil dibaca pada kekasaran **dasar**,
        // jadi coat menghitung albedo terarahnya sendiri. Memakai yang dasar di
        // sini membuat coat licin di atas dasar kasar meredam terlalu banyak.
        const float coatRoughness = saturate(surface.coatRoughness);
        // Sudut pandang diukur terhadap normal coat, bukan normal dasar. Pada
        // material yang tidak mengemudikan `coatNormal` keduanya benda yang
        // sama, jadi ini tidak menambah apa pun di sana.
        const float coatNv = max(dot(coatFrame.normal, coatFrame.view), 1e-5);
        const float2 coatDfg =
            ggxDirectionalAlbedo(coatNv, max(coatRoughness * coatRoughness, kMinAlpha));
        const float coatAlbedo =
            saturate(((coatF0 * coatDfg.x + coatDfg.y) *
                      ggxEnergyCompensation(float3(coatF0), coatDfg.x + coatDfg.y)).x);

        result = result * coatUnderlayer(surface, coatAlbedo) +
                 coatSpecular * surface.coatWeight;
    }

    // Fuzz paling atas, sama seperti di jalur cahaya langsung. Di bawah
    // iradiansi, yang dipancarkan lapisan fuzz adalah albedo terarahnya —
    // tidak perlu lobe-nya sama sekali.
    if (OPENPBR_HAS_FUZZ && surface.fuzzWeight > 0.0) {
        const float fuzzRoughness = clamp(surface.fuzzRoughness, 0.01, 1.0);
        const float fuzzAlbedo = zeltnerSheenAlbedo(nv, fuzzRoughness);
        result *= 1.0 - fuzzAlbedo * surface.fuzzWeight;
        result += surface.fuzzColor * (irradiance / kPi) * (fuzzAlbedo * surface.fuzzWeight);
    }

    return result;
}

/// Bentuk ringkas jalur lingkungan, dengan alasan yang sama.
float3 evaluateOpenPBR_IBL(OpenPBRSurface surface, ShadingFrame frame, float3 irradiance,
                           float3 prefilteredBase, float3 prefilteredCoat, float2 dfg) {
    return evaluateOpenPBR_IBL(surface, frame, frame, irradiance, prefilteredBase,
                               prefilteredCoat, dfg);
}
