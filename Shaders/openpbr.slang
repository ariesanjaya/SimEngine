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

/// Sumbu roughness anisotropik. Anisotropi 0 mengembalikan dua nilai yang sama,
/// jadi lobe isotropik adalah kasus khusus dari yang sama — bukan cabang lain.
void anisotropicAlpha(float roughness, float anisotropy, out float ax, out float ay) {
    const float r = max(roughness, kMinRoughness);
    const float alpha = r * r;
#if !OPENPBR_HAS_ANISOTROPY
    // Material yang tidak menyentuh anisotropi mendapat lobe isotropik apa
    // adanya: satu perkalian, tanpa akar kuadrat dan tanpa cabang.
    ax = alpha;
    ay = alpha;
#else
    // Anisotropi negatif memutar sumbu panjangnya 90°, bukan menjadi tidak sah.
    const float aspect = sqrt(1.0 - clamp(abs(anisotropy), 0.0, 0.98) * 0.9);
    if (anisotropy >= 0.0) {
        ax = alpha / aspect;
        ay = alpha * aspect;
    } else {
        ax = alpha * aspect;
        ay = alpha / aspect;
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

/// Difus Oren–Nayar yang disederhanakan. `baseDiffuseRoughness` 0 jatuh persis
/// ke Lambert, jadi material yang tidak menyentuhnya tidak membayar apa pun.
float3 diffuseLobe(OpenPBRSurface surface, float nl, float nv, float lv) {
    if (!OPENPBR_HAS_DIFFUSE_ROUGHNESS || surface.baseDiffuseRoughness <= 0.0) {
        return surface.baseColor / kPi;
    }
    const float sigma = surface.baseDiffuseRoughness * surface.baseDiffuseRoughness;
    const float a = 1.0 - 0.5 * sigma / (sigma + 0.33);
    const float b = 0.45 * sigma / (sigma + 0.09);
    const float s = lv - nl * nv;
    const float t = s <= 0.0 ? 1.0 : max(nl, nv) + 1e-5;
    return surface.baseColor * (a + b * s / t) / kPi;
}

/// Sheen untuk lobe fuzz. Distribusi invers-roughness, murah dan cukup untuk
/// kain pada preview.
float sheenDistribution(float roughness, float nh) {
    const float alpha = max(roughness, kMinRoughness);
    const float invAlpha = 1.0 / alpha;
    const float sin2 = max(1.0 - nh * nh, 1e-5);
    return (2.0 + invAlpha) * pow(sin2, invAlpha * 0.5) / (2.0 * kPi);
}

/// Peredaman lapisan di bawah coat: cahaya melewati coat dua kali, sekali masuk
/// dan sekali keluar, lalu diwarnai `coatColor`.
float3 coatColorAttenuation(OpenPBRSurface surface, float3 coatFresnel) {
    const float3 transmitted = float3(1.0) - coatFresnel;
    return surface.coatColor * transmitted * transmitted;
}

/// Evaluasi permukaan terhadap satu arah cahaya.
///
/// `lightDirection` menunjuk **dari permukaan ke cahaya**. Arah yang terbalik
/// adalah kesalahan yang tidak menghasilkan galat apa pun — hanya permukaan yang
/// gelap di sisi yang salah — jadi arahnya disebutkan di sini alih-alih
/// diandaikan.
float3 evaluateOpenPBR(OpenPBRSurface surface, ShadingFrame frame, float3 lightDirection,
                       float3 lightRadiance) {
    const float3 n = frame.normal;
    const float3 v = frame.view;
    const float3 l = lightDirection;
    const float nl = dot(n, l);
    if (nl <= 0.0) {
        return float3(0.0);
    }
    const float nv = max(dot(n, v), 1e-5);
    const float3 h = normalize(v + l);
    const float nh = max(dot(n, h), 0.0);
    const float vh = max(dot(v, h), 1e-5);

    // Logam tidak punya lobe difus, dan warna spekularnya adalah baseColor.
    // Keduanya dinyatakan sebagai interpolasi, bukan sebagai cabang: metalness
    // di antara 0 dan 1 adalah nilai yang sah dan sering dipakai di tepi peta.
    const float3 dielectricF0 = float3(iorToF0(surface.specularIor)) * surface.specularColor;
    const float3 f0 = lerp(dielectricF0, surface.baseColor, surface.baseMetalness);

    float ax, ay;
    anisotropicAlpha(surface.specularRoughness, surface.specularRoughnessAnisotropy, ax, ay);
    const float3 fresnel = fresnelSchlick(f0, vh);
    const float3 specular = fresnel * (ggxDistribution(ax, ay, h, frame) *
                                       smithVisibility(ax, ay, v, l, frame) *
                                       surface.specularWeight);

    // Energi yang tidak dipantulkan spekular saja yang sampai ke difus.
    const float3 diffuseEnergy = (float3(1.0) - fresnel) * (1.0 - surface.baseMetalness);
    const float3 diffuse =
        diffuseLobe(surface, nl, nv, dot(l, v)) * diffuseEnergy * surface.baseWeight;

    float3 result = diffuse + specular;

    if (OPENPBR_HAS_FUZZ && surface.fuzzWeight > 0.0) {
        const float sheen = sheenDistribution(surface.fuzzRoughness, nh);
        result += surface.fuzzColor * (sheen * surface.fuzzWeight);
    }

    if (OPENPBR_HAS_COAT && surface.coatWeight > 0.0) {
        float cx, cy;
        anisotropicAlpha(surface.coatRoughness, surface.coatRoughnessAnisotropy, cx, cy);
        const float3 coatF0 = float3(iorToF0(surface.coatIor));
        const float3 coatFresnel = fresnelSchlick(coatF0, vh);
        const float3 coatSpecular = coatFresnel * (ggxDistribution(cx, cy, h, frame) *
                                                   smithVisibility(cx, cy, v, l, frame));

        // Lapisan di bawahnya diredam dua kali — sekali saat cahaya masuk dan
        // sekali saat keluar — lalu diwarnai coatColor. `coatDarkening`
        // mengendalikan seberapa penuh peredaman itu diterapkan.
        const float3 absorb = lerp(float3(1.0), coatColorAttenuation(surface, coatFresnel),
                                   surface.coatDarkening);
        result = result * lerp(float3(1.0), absorb, surface.coatWeight) +
                 coatSpecular * surface.coatWeight;
    }

    return result * lightRadiance * nl;
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
float3 evaluateOpenPBR_IBL(OpenPBRSurface surface, ShadingFrame frame, float3 irradiance,
                           float3 prefilteredBase, float3 prefilteredCoat, float2 dfg) {
    const float nv = max(dot(frame.normal, frame.view), 1e-5);

    const float3 dielectricF0 = float3(iorToF0(surface.specularIor)) * surface.specularColor;
    const float3 f0 = lerp(dielectricF0, surface.baseColor, surface.baseMetalness);

    // **Pembagian energi tiga suku, cermin dari `SplitEnergy` di sisi C++.**
    // Split-sum satu-pantulan membuang cahaya yang di permukaan nyata memantul
    // lagi di lereng sebelah, dan buangannya besar: logam putih kehilangan 27,6%
    // energinya pada kekasaran 0,6 dan 63,9% pada 1,0. Itu bukan kesalahan kecil
    // — itu logam kasar yang tampak abu-abu kotor, dan tidak ada penyetelan
    // material yang bisa memperbaikinya karena energinya hilang sebelum sampai
    // ke penyetelan.
    const float3 singleScatter = f0 * dfg.x + dfg.y;
    const float missing = 1.0 - (dfg.x + dfg.y);
    // Reflektansi rata-rata atas seluruh sudut. Bentuk tertutup 1/21 dari
    // integral Schlick terhadap kosinus; Fresnel pada satu arah akan
    // mengembalikan energi yang salah persis di sudut menyerempet.
    const float3 average = f0 + (float3(1.0) - f0) / 21.0;
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

    const float3 specular =
        (environmentSpecular(f0, dfg, prefilteredBase) + multiScatter * (irradiance / kPi)) *
        surface.specularWeight;
    float3 result =
        surface.baseColor * (irradiance / kPi) * diffuseEnergy * surface.baseWeight + specular;

    if (OPENPBR_HAS_FUZZ && surface.fuzzWeight > 0.0) {
        // Sheen lingkungan tanpa LUT-nya sendiri: sheen menyerempet permukaan,
        // jadi ia didekati sebagai irradiance yang dibobot Fresnel sisi.
        const float grazing = pow(1.0 - nv, 3.0);
        result += surface.fuzzColor * (irradiance / kPi) * (grazing * surface.fuzzWeight);
    }

    if (OPENPBR_HAS_COAT && surface.coatWeight > 0.0) {
        const float3 coatF0 = float3(iorToF0(surface.coatIor));
        const float3 coatSpecular = environmentSpecular(coatF0, dfg, prefilteredCoat);
        const float3 coatFresnel = fresnelSchlick(coatF0, nv);
        const float3 absorb = lerp(float3(1.0), coatColorAttenuation(surface, coatFresnel),
                                   surface.coatDarkening);
        result = result * lerp(float3(1.0), absorb, surface.coatWeight) +
                 coatSpecular * surface.coatWeight;
    }

    return result;
}
