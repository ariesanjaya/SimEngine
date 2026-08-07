#include "Sim/Render/Ibl.h"

#include <algorithm>
#include <cmath>

namespace sim::render {
namespace {

constexpr float kMinRoughness = 0.0045f;

/// Bingkai ortonormal di sekitar sebuah normal.
///
/// Sumbu bantunya dipilih dari komponen normal yang paling kecil, bukan selalu
/// Y. Memakai satu sumbu tetap membuat cross product-nya nol tepat ketika normal
/// sejajar dengannya — dan arah itu bukan kasus langka melainkan arah "atas",
/// yang muncul pada setiap permukaan datar.
void BuildFrame(const Vec3& normal, Vec3& tangent, Vec3& bitangent) {
    const Vec3 helper = std::abs(normal.z) < 0.999f ? Vec3(0.0f, 0.0f, 1.0f)
                                                    : Vec3(1.0f, 0.0f, 0.0f);
    tangent = glm::normalize(glm::cross(helper, normal));
    bitangent = glm::cross(normal, tangent);
}

/// Smith untuk IBL. Konstanta k-nya berbeda dari yang dipakai cahaya langsung:
/// `alpha/2` di sini, `(r+1)²/8` di sana. Memakai yang salah membuat logam kasar
/// terlihat terlalu gelap pada pantulan lingkungan, dan selisihnya cukup halus
/// untuk lolos dari pemeriksaan sepintas.
float SmithVisibilityIbl(float nDotV, float nDotL, float roughness) {
    const float alpha = roughness * roughness;
    const float k = alpha * 0.5f;
    const float gv = nDotV / (nDotV * (1.0f - k) + k);
    const float gl = nDotL / (nDotL * (1.0f - k) + k);
    return gv * gl;
}

/// Bilangan biner yang dibalik urutan bitnya, dibagi 2^32 — deret Van der
/// Corput basis 2.
float RadicalInverse(uint32_t bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

/// Basis SH orde dua untuk sebuah arah.
void ShBasis(const Vec3& d, std::array<float, 9>& out) {
    out[0] = 0.282095f;
    out[1] = 0.488603f * d.y;
    out[2] = 0.488603f * d.z;
    out[3] = 0.488603f * d.x;
    out[4] = 1.092548f * d.x * d.y;
    out[5] = 1.092548f * d.y * d.z;
    out[6] = 0.315392f * (3.0f * d.z * d.z - 1.0f);
    out[7] = 1.092548f * d.x * d.z;
    out[8] = 0.546274f * (d.x * d.x - d.y * d.y);
}

}  // namespace

Vec3 GradientSky::Sample(const Vec3& direction) const {
    const Vec3 d = glm::normalize(direction);
    // Gradien dinaikkan pangkat supaya cakrawalanya tidak selebar setengah
    // langit — linear terhadap ketinggian membuat zenit hanya tercapai tepat di
    // puncak, dan lingkungannya terasa rata.
    const float height = std::clamp(d.y, -1.0f, 1.0f);
    Vec3 radiance;
    if (height >= 0.0f) {
        radiance = glm::mix(horizon, zenith, std::pow(height, 0.45f));
    } else {
        radiance = glm::mix(horizon, ground, std::pow(-height, 0.35f));
    }
    if (glm::dot(d, glm::normalize(sunDirection)) >= sunCos) {
        radiance += sunRadiance;
    }
    return radiance;
}

Vec3 CubeFaceDirection(int face, float u, float v) {
    // Dipetakan ke -1..1, dengan V menghadap ke bawah.
    const float s = u * 2.0f - 1.0f;
    const float t = 1.0f - v * 2.0f;
    switch (face) {
        case 0:
            return glm::normalize(Vec3(1.0f, t, -s));
        case 1:
            return glm::normalize(Vec3(-1.0f, t, s));
        case 2:
            return glm::normalize(Vec3(s, 1.0f, -t));
        case 3:
            return glm::normalize(Vec3(s, -1.0f, t));
        case 4:
            return glm::normalize(Vec3(s, t, 1.0f));
        default:
            return glm::normalize(Vec3(-s, t, -1.0f));
    }
}

Vec2 Hammersley(uint32_t index, uint32_t count) {
    const float denominator = static_cast<float>(std::max(count, 1u));
    return {static_cast<float>(index) / denominator, RadicalInverse(index)};
}

Vec3 ImportanceSampleGgx(const Vec2& xi, const Vec3& normal, float roughness) {
    // `alpha` adalah kekasaran kuadrat, bukan kekasarannya. Dua nama yang
    // sering tertukar, dan tertukarnya tidak menghasilkan galat — hanya sebaran
    // sampel yang salah lebar, yang muncul sebagai pantulan yang buramnya tidak
    // sesuai dengan slider kekasaran.
    const float clamped = std::max(roughness, kMinRoughness);
    const float alpha = clamped * clamped;

    const float phi = kTwoPi * xi.x;
    // Distribusi GGX yang dibalik. Pembaginya tidak pernah nol karena kekasaran
    // dijaga di atas kMinRoughness — dan pada alpha nol distribusinya delta,
    // yang tidak punya bentuk terbalik sama sekali.
    const float cosTheta =
        std::sqrt(std::max((1.0f - xi.y) / (1.0f + (alpha * alpha - 1.0f) * xi.y), 0.0f));
    const float sinTheta = std::sqrt(std::max(1.0f - cosTheta * cosTheta, 0.0f));

    const Vec3 local(sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta);
    Vec3 tangent;
    Vec3 bitangent;
    BuildFrame(normal, tangent, bitangent);
    return glm::normalize(tangent * local.x + bitangent * local.y + normal * local.z);
}

DfgTerms IntegrateDfg(float nDotV, float roughness, uint32_t sampleCount) {
    DfgTerms terms;
    const float cosV = std::clamp(nDotV, 1e-4f, 1.0f);
    const Vec3 view(std::sqrt(std::max(1.0f - cosV * cosV, 0.0f)), 0.0f, cosV);
    const Vec3 normal(0.0f, 0.0f, 1.0f);
    const uint32_t count = std::max(sampleCount, 1u);

    for (uint32_t i = 0; i < count; ++i) {
        const Vec3 half = ImportanceSampleGgx(Hammersley(i, count), normal, roughness);
        const Vec3 light = glm::reflect(-view, half);
        const float nDotL = light.z;
        if (nDotL <= 0.0f) {
            continue;
        }
        const float nDotH = std::max(half.z, 0.0f);
        const float vDotH = std::max(glm::dot(view, half), 0.0f);

        const float visibility = SmithVisibilityIbl(cosV, nDotL, roughness);
        // Faktor pdf GGX yang disederhanakan: yang tersisa setelah D dan
        // penyebutnya saling meniadakan.
        const float weight = visibility * vDotH / std::max(nDotH * cosV, 1e-6f);
        // Fresnel dipisah jadi bagian yang mengalikan F0 dan bagian yang tidak —
        // itulah yang membuat F0 keluar dari integral.
        const float fresnel = std::pow(1.0f - vDotH, 5.0f);
        terms.scale += (1.0f - fresnel) * weight;
        terms.bias += fresnel * weight;
    }

    const float inverse = 1.0f / static_cast<float>(count);
    terms.scale *= inverse;
    terms.bias *= inverse;
    return terms;
}

EnergyTerms SplitEnergy(const DfgTerms& dfg, const Vec3& f0) {
    EnergyTerms terms;
    // Energi pantulan-tunggal untuk permukaan yang memantulkan seluruhnya.
    const float single = dfg.scale + dfg.bias;
    terms.singleScatter = f0 * dfg.scale + Vec3(dfg.bias);

    const float missing = 1.0f - single;
    // Reflektansi rata-rata atas seluruh sudut. Bentuk tertutup 1/21 berasal
    // dari integral Schlick terhadap kosinus; memakai Fresnel pada satu arah di
    // sini akan mengembalikan energi yang salah persis di sudut menyerempet.
    const Vec3 average = f0 + (Vec3(1.0f) - f0) / 21.0f;
    // Deret geometri pantulan lanjutan: setiap pantulan berikutnya membawa
    // sisanya lagi, dan jumlah deretnya punya bentuk tertutup.
    const Vec3 denominator = glm::max(Vec3(1.0f) - missing * average, Vec3(1e-6f));
    terms.multiScatter = terms.singleScatter * average / denominator * missing;

    // Sisanya, dan hanya sisanya, boleh dipakai difus. Ditulis sebagai
    // pengurangan, bukan sebagai rumus tersendiri: itu yang membuat ketiganya
    // berjumlah tepat satu menurut konstruksi, bukan menurut kebetulan.
    terms.diffuse = glm::max(Vec3(1.0f) - terms.singleScatter - terms.multiScatter, Vec3(0.0f));
    return terms;
}

DfgLut BakeDfgLut(uint32_t size, uint32_t sampleCount) {
    DfgLut lut;
    lut.size = std::max(size, 2u);
    lut.data.assign(static_cast<size_t>(lut.size) * lut.size * 2, 0.0f);

    for (uint32_t y = 0; y < lut.size; ++y) {
        // Tengah texel, bukan tepinya. Membakar di tepi berarti separuh texel
        // pertama dan terakhir mewakili nilai di luar rentang, dan yang terlihat
        // adalah cermin sempurna yang tetap sedikit buram.
        const float roughness =
            (static_cast<float>(y) + 0.5f) / static_cast<float>(lut.size);
        for (uint32_t x = 0; x < lut.size; ++x) {
            const float nDotV = (static_cast<float>(x) + 0.5f) / static_cast<float>(lut.size);
            const DfgTerms terms = IntegrateDfg(nDotV, roughness, sampleCount);
            const size_t at = (static_cast<size_t>(y) * lut.size + x) * 2;
            lut.data[at] = terms.scale;
            lut.data[at + 1] = terms.bias;
        }
    }
    return lut;
}

DfgTerms DfgLut::Sample(float nDotV, float roughness) const {
    if (size == 0) {
        return {};
    }
    const auto extent = static_cast<float>(size);
    const float u = std::clamp(nDotV, 0.0f, 1.0f) * extent - 0.5f;
    const float v = std::clamp(roughness, 0.0f, 1.0f) * extent - 0.5f;

    const auto x0 = static_cast<int>(std::floor(u));
    const auto y0 = static_cast<int>(std::floor(v));
    const float fx = u - static_cast<float>(x0);
    const float fy = v - static_cast<float>(y0);

    const auto clampIndex = [this](int value) {
        return static_cast<uint32_t>(std::clamp(value, 0, static_cast<int>(size) - 1));
    };
    const uint32_t xa = clampIndex(x0);
    const uint32_t xb = clampIndex(x0 + 1);
    const uint32_t ya = clampIndex(y0);
    const uint32_t yb = clampIndex(y0 + 1);

    const DfgTerms aa = At(xa, ya);
    const DfgTerms ba = At(xb, ya);
    const DfgTerms ab = At(xa, yb);
    const DfgTerms bb = At(xb, yb);

    DfgTerms out;
    out.scale = glm::mix(glm::mix(aa.scale, ba.scale, fx), glm::mix(ab.scale, bb.scale, fx), fy);
    out.bias = glm::mix(glm::mix(aa.bias, ba.bias, fx), glm::mix(ab.bias, bb.bias, fx), fy);
    return out;
}

Sh9 ProjectIrradiance(const IEnvironmentSampler& environment, uint32_t sampleCount) {
    Sh9 sh;
    const uint32_t count = std::max(sampleCount, 1u);

    std::array<float, 9> basis{};
    for (uint32_t i = 0; i < count; ++i) {
        const Vec2 xi = Hammersley(i, count);
        // Sampel merata di **bola**, bukan merata di sudut. Membagi rata theta
        // dan phi menumpuk sampel di kutub, dan lingkungan dengan langit terang
        // lalu terhitung terlalu terang.
        const float z = 1.0f - 2.0f * xi.x;
        const float r = std::sqrt(std::max(1.0f - z * z, 0.0f));
        const float phi = kTwoPi * xi.y;
        const Vec3 direction(r * std::cos(phi), r * std::sin(phi), z);

        const Vec3 radiance = environment.Sample(direction);
        ShBasis(direction, basis);
        for (int b = 0; b < 9; ++b) {
            sh.coefficients[static_cast<size_t>(b)] += radiance * basis[static_cast<size_t>(b)];
        }
    }

    // Bobot Monte Carlo untuk sampling seragam di bola: 4π / N.
    const float weight = 4.0f * kPi / static_cast<float>(count);
    for (Vec3& coefficient : sh.coefficients) {
        coefficient *= weight;
    }
    return sh;
}

Vec3 EvaluateIrradiance(const Sh9& sh, const Vec3& normal) {
    const Vec3 n = glm::normalize(normal);
    // Konvolusi lobe kosinus: Â₀ = π, Â₁ = 2π/3, Â₂ = π/4.
    //
    // **Ini yang membedakan irradiance dari radiance rata-rata.** Melewatkannya
    // menghasilkan angka yang tetap masuk akal — halus, berwarna benar, dan
    // salah sebesar faktor yang berbeda per pita. Yang terlihat adalah difus
    // yang kontrasnya kurang, dan biasanya "diperbaiki" dengan menaikkan
    // intensitas lampu sampai tidak ada lagi yang cocok.
    constexpr float a0 = 3.141593f;
    constexpr float a1 = 2.094395f;
    constexpr float a2 = 0.785398f;

    std::array<float, 9> basis{};
    ShBasis(n, basis);

    Vec3 irradiance = sh.coefficients[0] * basis[0] * a0;
    for (int b = 1; b <= 3; ++b) {
        irradiance += sh.coefficients[static_cast<size_t>(b)] * basis[static_cast<size_t>(b)] * a1;
    }
    for (int b = 4; b <= 8; ++b) {
        irradiance += sh.coefficients[static_cast<size_t>(b)] * basis[static_cast<size_t>(b)] * a2;
    }
    // Irradiance negatif tidak ada artinya secara fisik, dan ia muncul di sini
    // pada lingkungan berkontras tinggi — orde dua tidak bisa mewakili tepi
    // tajam tanpa melewati batas. Dijepit, bukan dibiarkan: warna negatif
    // merambat ke seluruh perhitungan sesudahnya.
    return glm::max(irradiance, Vec3(0.0f));
}

float RoughnessForMip(uint32_t mip, uint32_t mipCount) {
    if (mipCount <= 1) {
        return 0.0f;
    }
    return static_cast<float>(mip) / static_cast<float>(mipCount - 1);
}

Vec3 PrefilterSpecular(const IEnvironmentSampler& environment, const Vec3& reflection,
                       float roughness, uint32_t sampleCount) {
    const Vec3 normal = glm::normalize(reflection);
    if (roughness <= kMinRoughness) {
        // Cermin sempurna: satu pengambilan, dan tanpa cabang ini integralnya
        // akan menyebar sampel di sekitar arah pantul karena alpha dijepit ke
        // kMinRoughness — pantulan tajam jadi selalu sedikit buram.
        return environment.Sample(normal);
    }

    const uint32_t count = std::max(sampleCount, 1u);
    Vec3 sum(0.0f);
    float weight = 0.0f;
    for (uint32_t i = 0; i < count; ++i) {
        const Vec3 half = ImportanceSampleGgx(Hammersley(i, count), normal, roughness);
        // N = V = R, jadi arah cahaya adalah pantulan normal terhadap half.
        const Vec3 light = glm::normalize(half * (2.0f * glm::dot(normal, half)) - normal);
        const float nDotL = glm::dot(normal, light);
        if (nDotL <= 0.0f) {
            continue;
        }
        // Dibobot n·l, bukan dijumlah rata. Sampel yang menyerempet permukaan
        // menyumbang lebih sedikit ke pantulan, dan menjumlahkannya rata
        // membuat pantulan kasar terlihat terlalu terang di tepi objek.
        sum += environment.Sample(light) * nDotL;
        weight += nDotL;
    }
    return weight > 0.0f ? sum / weight : environment.Sample(normal);
}

}  // namespace sim::render
