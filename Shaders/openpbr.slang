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
    // Anisotropi negatif memutar sumbu panjangnya 90°, bukan menjadi tidak sah.
    const float aspect = sqrt(1.0 - clamp(abs(anisotropy), 0.0, 0.98) * 0.9);
    if (anisotropy >= 0.0) {
        ax = alpha / aspect;
        ay = alpha * aspect;
    } else {
        ax = alpha * aspect;
        ay = alpha / aspect;
    }
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
    if (surface.baseDiffuseRoughness <= 0.0) {
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

    if (surface.fuzzWeight > 0.0) {
        const float sheen = sheenDistribution(surface.fuzzRoughness, nh);
        result += surface.fuzzColor * (sheen * surface.fuzzWeight);
    }

    if (surface.coatWeight > 0.0) {
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
