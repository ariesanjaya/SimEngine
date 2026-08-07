#include "Sim/Render/Atmosphere.h"

#include <algorithm>
#include <cmath>

namespace sim::render {
namespace {

constexpr float kPi = 3.14159265358979323846f;
float SafeSqrt(float value) {
    return std::sqrt(std::max(value, 0.0f));
}

/// Peralihan antara uv texel dan rentang penuh, supaya texel pertama dan
/// terakhir tepat berada di ujung rentangnya. Tanpa itu, pemetaan di dekat
/// zenit meleset setengah texel dan yang terlihat adalah garis jahitan di puncak
/// langit.
float SubUvToUnit(float u, float resolution) {
    return (u - 0.5f / resolution) * (resolution / (resolution - 1.0f));
}

float UnitToSubUv(float u, float resolution) {
    return (u + 0.5f / resolution) * (resolution / (resolution + 1.0f));
}

}  // namespace

float RaySphereNearest(const Vec3& origin, const Vec3& direction, float radius) {
    const float a = glm::dot(direction, direction);
    const float b = 2.0f * glm::dot(direction, origin);
    const float c = glm::dot(origin, origin) - radius * radius;
    const float discriminant = b * b - 4.0f * a * c;
    if (discriminant < 0.0f || a == 0.0f) {
        return -1.0f;
    }
    const float root = SafeSqrt(discriminant);
    const float near = (-b - root) / (2.0f * a);
    const float far = (-b + root) / (2.0f * a);
    if (near < 0.0f && far < 0.0f) {
        return -1.0f;
    }
    if (near < 0.0f) {
        return std::max(0.0f, far);
    }
    if (far < 0.0f) {
        return std::max(0.0f, near);
    }
    return std::max(0.0f, std::min(near, far));
}

MediumDensity SampleDensity(const AtmosphereParameters& atmosphere, float heightKm) {
    MediumDensity density;
    density.rayleigh = std::exp(-heightKm / atmosphere.rayleighScaleHeight);
    density.mie = std::exp(-heightKm / atmosphere.mieScaleHeight);
    // Profil tenda, bukan eksponensial: ozon berkumpul di sebuah lapisan di
    // ketinggian, bukan meluruh dari permukaan. Memodelkannya eksponensial akan
    // meletakkan seluruh serapannya di tempat yang tidak ada ozonnya.
    if (heightKm <= atmosphere.ozoneBottom || heightKm >= atmosphere.ozoneTop) {
        density.ozone = 0.0f;
    } else if (heightKm < atmosphere.ozonePeak) {
        density.ozone = (heightKm - atmosphere.ozoneBottom) /
                        (atmosphere.ozonePeak - atmosphere.ozoneBottom);
    } else {
        density.ozone =
            (atmosphere.ozoneTop - heightKm) / (atmosphere.ozoneTop - atmosphere.ozonePeak);
    }
    density.ozone = std::clamp(density.ozone, 0.0f, 1.0f);
    return density;
}

Vec3 SampleExtinction(const AtmosphereParameters& atmosphere, float heightKm) {
    const MediumDensity density = SampleDensity(atmosphere, heightKm);
    return atmosphere.rayleighScattering * density.rayleigh +
           Vec3(atmosphere.mieExtinction * density.mie) +
           atmosphere.ozoneAbsorption * density.ozone;
}

Vec3 SampleScattering(const AtmosphereParameters& atmosphere, float heightKm) {
    const MediumDensity density = SampleDensity(atmosphere, heightKm);
    // Ozon tidak ikut: ia menyerap tanpa menghamburkan.
    return atmosphere.rayleighScattering * density.rayleigh +
           Vec3(atmosphere.mieScattering * density.mie);
}

Vec3 IntegrateOpticalDepth(const AtmosphereParameters& atmosphere, const Vec3& origin,
                           const Vec3& direction, uint32_t sampleCount) {
    const float toTop = RaySphereNearest(origin, direction, atmosphere.topRadius);
    if (toTop <= 0.0f) {
        return Vec3(0.0f);
    }
    const float toGround = RaySphereNearest(origin, direction, atmosphere.bottomRadius);
    const float length = toGround > 0.0f ? std::min(toGround, toTop) : toTop;
    const float step = length / static_cast<float>(sampleCount);

    Vec3 depth(0.0f);
    for (uint32_t i = 0; i < sampleCount; ++i) {
        // Titik tengah tiap langkah. Memakai tepinya membiaskan seluruh integral
        // ke satu arah, dan biasnya tidak pernah hilang seberapa pun langkah
        // ditambah.
        const float distance = (static_cast<float>(i) + 0.5f) * step;
        const Vec3 position = origin + direction * distance;
        const float height = glm::length(position) - atmosphere.bottomRadius;
        depth += SampleExtinction(atmosphere, height) * step;
    }
    return depth;
}

Vec3 Transmittance(const AtmosphereParameters& atmosphere, const Vec3& origin,
                   const Vec3& direction, uint32_t sampleCount) {
    const Vec3 depth = IntegrateOpticalDepth(atmosphere, origin, direction, sampleCount);
    return Vec3(std::exp(-depth.x), std::exp(-depth.y), std::exp(-depth.z));
}

Vec3 AnalyticZenithOpticalDepth(const AtmosphereParameters& atmosphere) {
    const float thickness = atmosphere.topRadius - atmosphere.bottomRadius;
    // ∫₀^H exp(-h/s) dh = s(1 - exp(-H/s)).
    const float rayleigh =
        atmosphere.rayleighScaleHeight *
        (1.0f - std::exp(-thickness / atmosphere.rayleighScaleHeight));
    const float mie =
        atmosphere.mieScaleHeight * (1.0f - std::exp(-thickness / atmosphere.mieScaleHeight));
    // Luas tenda: setengah alas kali tinggi.
    const float ozone = 0.5f * (atmosphere.ozoneTop - atmosphere.ozoneBottom);
    return atmosphere.rayleighScattering * rayleigh + Vec3(atmosphere.mieExtinction * mie) +
           atmosphere.ozoneAbsorption * ozone;
}

Vec2 TransmittanceParamsToUv(const AtmosphereParameters& atmosphere,
                             const TransmittanceParams& params) {
    const float H = SafeSqrt(atmosphere.topRadius * atmosphere.topRadius -
                             atmosphere.bottomRadius * atmosphere.bottomRadius);
    const float rho =
        SafeSqrt(params.radius * params.radius - atmosphere.bottomRadius * atmosphere.bottomRadius);
    const float discriminant =
        params.radius * params.radius * (params.cosZenith * params.cosZenith - 1.0f) +
        atmosphere.topRadius * atmosphere.topRadius;
    const float distance =
        std::max(0.0f, -params.radius * params.cosZenith + SafeSqrt(discriminant));
    const float minDistance = atmosphere.topRadius - params.radius;
    const float maxDistance = rho + H;
    const float mu = (distance - minDistance) / std::max(maxDistance - minDistance, 1e-6f);
    return Vec2(mu, rho / std::max(H, 1e-6f));
}

TransmittanceParams UvToTransmittanceParams(const AtmosphereParameters& atmosphere,
                                            const Vec2& uv) {
    const float H = SafeSqrt(atmosphere.topRadius * atmosphere.topRadius -
                             atmosphere.bottomRadius * atmosphere.bottomRadius);
    const float rho = H * uv.y;
    TransmittanceParams params;
    params.radius =
        SafeSqrt(rho * rho + atmosphere.bottomRadius * atmosphere.bottomRadius);
    const float minDistance = atmosphere.topRadius - params.radius;
    const float maxDistance = rho + H;
    const float distance = minDistance + uv.x * (maxDistance - minDistance);
    params.cosZenith = distance == 0.0f
                           ? 1.0f
                           : (H * H - rho * rho - distance * distance) /
                                 (2.0f * params.radius * distance);
    params.cosZenith = std::clamp(params.cosZenith, -1.0f, 1.0f);
    return params;
}

SkyViewParams UvToSkyViewParams(const AtmosphereParameters& atmosphere, const Vec2& uv,
                                float viewHeight, const Vec2& dimensions) {
    const Vec2 unit(SubUvToUnit(uv.x, dimensions.x), SubUvToUnit(uv.y, dimensions.y));
    const float beta = std::asin(std::clamp(atmosphere.bottomRadius / viewHeight, -1.0f, 1.0f));
    const float zenithHorizon = kPi - beta;

    SkyViewParams params;
    // **Pemetaan dipadatkan di dekat horizon.** Perubahan warna paling tajam di
    // seluruh langit terjadi dalam beberapa derajat di atas dan di bawah
    // horizon; pemetaan seragam menghabiskan texel pada langit atas yang hampir
    // rata dan menyisakan beberapa texel untuk pita itu.
    if (unit.y < 0.5f) {
        const float coordinate = 1.0f - (1.0f - 2.0f * unit.y) * (1.0f - 2.0f * unit.y);
        params.viewZenithAngle = zenithHorizon * coordinate;
    } else {
        const float coordinate = (unit.y * 2.0f - 1.0f) * (unit.y * 2.0f - 1.0f);
        params.viewZenithAngle = zenithHorizon + beta * coordinate;
    }
    params.lightViewAngle = unit.x * unit.x * kPi;
    return params;
}

Vec2 SkyViewParamsToUv(const AtmosphereParameters& atmosphere, const SkyViewParams& params,
                       float viewHeight, bool intersectsGround, const Vec2& dimensions) {
    const float beta = std::asin(std::clamp(atmosphere.bottomRadius / viewHeight, -1.0f, 1.0f));
    const float zenithHorizon = kPi - beta;

    Vec2 uv;
    if (!intersectsGround) {
        float coordinate = params.viewZenithAngle / std::max(zenithHorizon, 1e-6f);
        coordinate = (1.0f - SafeSqrt(1.0f - coordinate)) / 2.0f;
        uv.y = coordinate;
    } else {
        float coordinate = (params.viewZenithAngle - zenithHorizon) / std::max(beta, 1e-6f);
        coordinate = (SafeSqrt(coordinate) + 1.0f) / 2.0f;
        uv.y = coordinate;
    }
    uv.x = SafeSqrt(params.lightViewAngle / kPi);
    return Vec2(UnitToSubUv(uv.x, dimensions.x), UnitToSubUv(uv.y, dimensions.y));
}

float RayleighPhase(float cosTheta) {
    return 3.0f / (16.0f * kPi) * (1.0f + cosTheta * cosTheta);
}

float MiePhase(float cosTheta, float g) {
    const float g2 = g * g;
    const float scale = 3.0f / (8.0f * kPi);
    const float numerator = (1.0f - g2) * (1.0f + cosTheta * cosTheta);
    const float denominator =
        (2.0f + g2) * std::pow(std::max(1.0f + g2 - 2.0f * g * cosTheta, 1e-6f), 1.5f);
    return scale * numerator / denominator;
}

}  // namespace sim::render
