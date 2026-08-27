#include "Sim/Render/Ibl.h"

#include "Sim/Core/Log.h"
#include "Sim/ImageIO/ImageIO.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

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

void AtmosphereSky::Prepare() {
    transmittance_ = BuildTransmittanceLut(atmosphere);
}

Vec3 AtmosphereSky::Sample(const Vec3& direction) const {
    const Vec3 view = glm::normalize(direction);
    const Vec3 sun = glm::normalize(sunDirection);

    // Titik asal berpusat di planet dan dalam kilometer, konvensi yang sama
    // dengan seluruh `Atmosphere.h`. Mencampurnya dengan meter tidak
    // menghasilkan galat apa pun — hanya langit yang seluruhnya hitam atau
    // seluruhnya putih.
    const Vec3 origin(0.0f, atmosphere.bottomRadius + std::max(cameraHeightKm, 0.0f), 0.0f);

    // Sinar berhenti di tanah bila ia menembusnya, dan di puncak atmosfer bila
    // tidak. Yang menembus tanah lalu tetap diintegrasikan sampai puncak
    // atmosfer akan mengintegrasikan udara di dalam tanah: kerapatannya
    // eksponensial yang meledak, dan yang terlihat kabut putih menyilaukan di
    // bawah horizon alih-alih sebuah galat.
    const float toGround = RaySphereNearest(origin, view, atmosphere.bottomRadius);
    const float toTop = RaySphereNearest(origin, view, atmosphere.topRadius);
    const float rayLength = toGround > 0.0f ? toGround : toTop;
    if (rayLength <= 0.0f || stepCount == 0) {
        return Vec3(0.0f);
    }

    const float cosTheta = glm::dot(view, sun);
    const float rayleighPhaseValue = RayleighPhase(cosTheta);
    const float miePhaseValue = MiePhase(cosTheta, atmosphere.miePhaseG);

    Vec3 accumulated(0.0f);
    Vec3 transmittance(1.0f);
    const float step = rayLength / static_cast<float>(stepCount);

    for (uint32_t i = 0; i < stepCount; ++i) {
        const Vec3 position = origin + view * ((static_cast<float>(i) + 0.5f) * step);
        const float radius = glm::length(position);
        const float height = radius - atmosphere.bottomRadius;
        const Vec3 up = position / radius;

        const MediumDensity density = SampleDensity(atmosphere, height);
        const Vec3 rayleigh = atmosphere.rayleighScattering * density.rayleigh;
        const Vec3 mie(atmosphere.mieScattering * density.mie);
        const Vec3 extinction = SampleExtinction(atmosphere, height);

        // Bayangan planet. Titik yang mataharinya sudah terbenam tidak
        // menyumbang hamburan tunggal apa pun; tanpa uji ini, separuh malam
        // tetap disinari matahari yang berada di balik bumi.
        const float sunBlocked =
            RaySphereNearest(position + up * 0.01f, sun, atmosphere.bottomRadius);
        Vec3 toSun(0.0f);
        if (sunBlocked < 0.0f) {
            toSun = transmittance_.IsValid()
                        ? transmittance_.Sample(atmosphere, radius, glm::dot(up, sun))
                        : Transmittance(atmosphere, position, sun, sunStepCount);
        }

        const Vec3 inscatter = toSun * (rayleigh * rayleighPhaseValue + mie * miePhaseValue);
        const Vec3 stepTransmittance(std::exp(-extinction.x * step),
                                     std::exp(-extinction.y * step),
                                     std::exp(-extinction.z * step));
        // Integrasi analitik di dalam langkahnya, sama dengan pass langit dan
        // dengan aerial perspective: perkalian sederhana meninggalkan pita-pita
        // yang terlihat pada jumlah langkah sesedikit ini.
        const Vec3 integrated =
            (inscatter - inscatter * stepTransmittance) / glm::max(extinction, Vec3(1e-9f));

        accumulated += transmittance * integrated;
        transmittance *= stepTransmittance;
    }

    // `solarIrradiance` di sini, bukan di dalam gelungnya: ia tetap sepanjang
    // sinar, dan mengalikannya sekali di akhir menghemat tiga perkalian per
    // langkah tanpa mengubah satu bit pun hasilnya.
    return accumulated * atmosphere.solarIrradiance * intensity;
}

Vec2 DirectionToEquirectUv(const Vec3& direction) {
    const Vec3 d = glm::normalize(direction);
    // atan2(x, −z): pada u = 0 arahnya −Z, yaitu arah pandang bawaan kamera.
    // Konvensi mana pun sah asalkan kedua arah pemetaannya sepakat; yang tidak
    // sah adalah dua konvensi yang berbeda di dua tempat, dan itu tidak muncul
    // sebagai galat melainkan sebagai langit yang berputar terhadap mataharinya.
    const float longitude = std::atan2(d.x, -d.z);
    const float latitude = std::asin(std::clamp(d.y, -1.0f, 1.0f));
    return Vec2(longitude / (2.0f * kPi) + 0.5f, 0.5f - latitude / kPi);
}

Vec3 EquirectUvToDirection(const Vec2& uv) {
    const float longitude = (uv.x - 0.5f) * 2.0f * kPi;
    const float latitude = (0.5f - uv.y) * kPi;
    const float cosLatitude = std::cos(latitude);
    return Vec3(cosLatitude * std::sin(longitude), std::sin(latitude),
                -cosLatitude * std::cos(longitude));
}

Vec3 EquirectEnvironment::SampleUv(const Vec2& uv) const {
    if (!IsValid()) {
        return Vec3(0.0f);
    }
    // Membungkus di U, menjepit di V. U adalah lingkaran penuh — menjepitnya
    // meninggalkan jahitan tegak selebar satu texel yang membelah langit — dan V
    // berakhir di kutub, tempat membungkus akan mengambil warna dari kutub
    // seberang.
    const float x = uv.x * static_cast<float>(width) - 0.5f;
    const float y = std::clamp(uv.y, 0.0f, 1.0f) * static_cast<float>(height) - 0.5f;
    const auto x0 = static_cast<int32_t>(std::floor(x));
    const auto y0 = static_cast<int32_t>(std::floor(y));
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);

    const auto wrapX = [&](int32_t value) {
        const auto w = static_cast<int32_t>(width);
        const int32_t result = value % w;
        return result < 0 ? result + w : result;
    };
    const auto clampY = [&](int32_t value) {
        return std::clamp(value, 0, static_cast<int32_t>(height) - 1);
    };
    const auto texel = [&](int32_t px, int32_t py) {
        const std::size_t at =
            (static_cast<std::size_t>(clampY(py)) * width + static_cast<uint32_t>(wrapX(px))) * 3;
        return Vec3(pixels[at], pixels[at + 1], pixels[at + 2]);
    };

    const Vec3 top = glm::mix(texel(x0, y0), texel(x0 + 1, y0), fx);
    const Vec3 bottom = glm::mix(texel(x0, y0 + 1), texel(x0 + 1, y0 + 1), fx);
    return glm::mix(top, bottom, fy);
}

Vec3 EquirectEnvironment::Sample(const Vec3& direction) const {
    return SampleUv(DirectionToEquirectUv(direction)) * intensity;
}

/// Apakah tiga kanal pertama benar-benar merah, hijau, biru.
///
/// **`.exr` dengan kanal depth, velocity, dan objectId punya bentuk yang sama
/// persis dengan HDRI** — dimensi yang sama, jumlah kanal yang sama, tipe yang
/// sama. Yang membedakannya cuma nama kanalnya, dan tanpa pemeriksaan ini
/// berkas semacam itu dimuat sebagai lingkungan yang menyala dengan warna yang
/// tidak berarti apa-apa. Tidak ada galat di mana pun; hanya adegan yang
/// pencahayaannya aneh.
///
/// Nama kanal yang kosong berarti formatnya memang tidak menyimpannya — PNG dan
/// Radiance tidak punya — dan di situ tidak ada yang bisa diperiksa.
bool IsRgbEquirect(const imageio::ImageDesc& desc, std::string& found) {
    if (desc.channelNames.empty()) {
        return true;
    }
    static constexpr std::array<const char*, 3> kExpected{"R", "G", "B"};
    bool matches = desc.channelNames.size() >= kExpected.size();
    for (std::size_t i = 0; i < kExpected.size() && matches; ++i) {
        matches = desc.channelNames[i] == kExpected[i];
    }
    if (matches) {
        return true;
    }
    for (const std::string& name : desc.channelNames) {
        found += found.empty() ? name : ", " + name;
    }
    return false;
}

EquirectEnvironment LoadHdrEquirect(const std::filesystem::path& path) {
    EquirectEnvironment environment;
    if (path.empty() || !std::filesystem::exists(path)) {
        return environment;
    }
    // Format yang tidak didukung dijawab di sini, sebelum dekode, supaya
    // pesannya menyebut apa yang kurang. Tanpa backend EXR, `.exr` memang tidak
    // muncul di daftar format sama sekali — Asset Browser tidak menawarkannya,
    // dan yang sampai ke sini cuma berkas yang jalurnya diketik tangan.
    if (!imageio::CanRead(path.extension().string())) {
        SIM_WARN("Render", "cannot use {} as environment: no image backend reads {} ({})",
                 path.string(), path.extension().string(), imageio::BackendSummary());
        return environment;
    }
    // Float linear, **tiga kanal secara eksplisit**: peta lingkungan tidak
    // punya alfa, dan meminta empat membuat backend mengarang kanal keempat
    // yang lalu ikut termakan memori di setiap texel tanpa membawa satu bit
    // informasi pun.
    imageio::ReadOptions options;
    options.channels = 3;
    options.type = imageio::PixelType::Float32;

    imageio::Image image;
    const imageio::ImageIoResult decoded = imageio::Read(path, options, image);
    if (!decoded) {
        SIM_WARN("Render", "cannot decode HDR environment: {}", decoded.error);
        return environment;
    }
    if (image.desc.width == 0 || image.desc.height == 0) {
        SIM_WARN("Render", "HDR environment is empty: {}", path.string());
        return environment;
    }

    std::string found;
    if (!IsRgbEquirect(image.desc, found)) {
        SIM_WARN("Render",
                 "{} is not an RGB environment map: channels found are [{}]. "
                 "Multi-channel renders (depth, velocity, cryptomatte) are not "
                 "environment maps.",
                 path.string(), found);
        return environment;
    }

    environment.width = image.desc.width;
    environment.height = image.desc.height;
    const float* pixels = image.AsF32();
    environment.pixels.assign(pixels, pixels + image.desc.SampleCount());
    return environment;
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

/// Arah dunia → muka cubemap beserta uv-nya. **Kebalikan tepat
/// `CubeFaceDirection`**, dan keduanya diuji saling membalik: pemetaan yang
/// meleset tidak menghasilkan galat apa pun, hanya lingkungan yang isinya benar
/// di muka yang salah.
void DirectionToCubeFace(const Vec3& direction, int& face, float& u, float& v) {
    const Vec3 d = glm::normalize(direction);
    const Vec3 a(std::abs(d.x), std::abs(d.y), std::abs(d.z));

    float s = 0.0f;
    float t = 0.0f;
    float major = 1.0f;
    if (a.x >= a.y && a.x >= a.z) {
        major = a.x;
        if (d.x > 0.0f) {
            face = 0;
            s = -d.z;
            t = d.y;
        } else {
            face = 1;
            s = d.z;
            t = d.y;
        }
    } else if (a.y >= a.z) {
        major = a.y;
        if (d.y > 0.0f) {
            face = 2;
            s = d.x;
            t = -d.z;
        } else {
            face = 3;
            s = d.x;
            t = d.z;
        }
    } else {
        major = a.z;
        if (d.z > 0.0f) {
            face = 4;
            s = d.x;
            t = d.y;
        } else {
            face = 5;
            s = -d.x;
            t = d.y;
        }
    }
    major = std::max(major, 1e-9f);
    u = (s / major + 1.0f) * 0.5f;
    v = (1.0f - t / major) * 0.5f;
}

Vec3 CubemapEnvironment::Sample(const Vec3& direction) const {
    if (!IsValid()) {
        return Vec3(0.0f);
    }
    int face = 0;
    float u = 0.0f;
    float v = 0.0f;
    DirectionToCubeFace(direction, face, u, v);

    const auto extent = static_cast<float>(size);
    const float fx = std::clamp(u * extent - 0.5f, 0.0f, extent - 1.0f);
    const float fy = std::clamp(v * extent - 0.5f, 0.0f, extent - 1.0f);
    const auto x0 = static_cast<uint32_t>(fx);
    const auto y0 = static_cast<uint32_t>(fy);
    const uint32_t x1 = std::min(x0 + 1, size - 1);
    const uint32_t y1 = std::min(y0 + 1, size - 1);
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);

    const std::size_t faceBase =
        static_cast<std::size_t>(face) * size * size * 4;
    const auto at = [&](uint32_t x, uint32_t y) {
        const std::size_t index = faceBase + (static_cast<std::size_t>(y) * size + x) * 4;
        return Vec3(texels[index], texels[index + 1], texels[index + 2]);
    };
    const Vec3 top = glm::mix(at(x0, y0), at(x1, y0), tx);
    const Vec3 bottom = glm::mix(at(x0, y1), at(x1, y1), tx);
    return glm::mix(top, bottom, ty);
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
