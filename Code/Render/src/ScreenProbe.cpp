#include "Sim/Render/ScreenProbe.h"

#include <algorithm>
#include <cmath>

namespace sim::render {
namespace {

float SignNotZero(float value) {
    return value >= 0.0f ? 1.0f : -1.0f;
}

/// Titik ke-`index` dari urutan Hammersley 2D berukuran `count`.
///
/// Ditulis ulang di sini alih-alih dipinjam dari `Ibl.h`: yang di sana melayani
/// pemanggangan IBL dan berhak berubah mengikuti kebutuhannya, sementara yang di
/// sini harus cocok dengan shader probe texel demi texel. Dua pemakai yang
/// berbagi satu fungsi lalu menariknya ke arah berbeda adalah cara paling umum
/// sebuah rumus berubah tanpa ada yang memintanya.
Vec2 Hammersley2D(uint32_t index, uint32_t count) {
    uint32_t bits = index;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return {static_cast<float>(index) / static_cast<float>(std::max(count, 1u)),
            static_cast<float>(bits) * 2.3283064365386963e-10f};
}

/// Pergeseran tetap sebuah probe, di [0,1)².
///
/// Rotasi Cranley–Patterson: urutan low-discrepancy yang sama digeser berbeda
/// per probe. Urutannya tetap tersebar merata, tapi dua probe bertetangga tidak
/// lagi mengambil arah yang sama persis — dan kesamaan itulah yang akan terlihat
/// sebagai kisi ubin, bukan sebagai derau.
Vec2 ProbeOffset(uint32_t probeIndex) {
    uint32_t hash = probeIndex * 0x9E3779B9u;
    hash ^= hash >> 15u;
    hash *= 0x85EBCA6Bu;
    hash ^= hash >> 13u;
    const float x = static_cast<float>(hash & 0xFFFFu) / 65536.0f;
    const float y = static_cast<float>((hash >> 16u) & 0xFFFFu) / 65536.0f;
    return {x, y};
}

}  // namespace

Vec2 OctEncode(const Vec3& direction) {
    const float length = std::abs(direction.x) + std::abs(direction.y) + std::abs(direction.z);
    if (length < 1e-20f) {
        return {0.5f, 0.5f};
    }
    const Vec3 n = direction / length;
    Vec2 p(n.x, n.y);
    if (n.z < 0.0f) {
        // Setengah bola bawah dilipat ke luar kotak dalamnya, memenuhi keempat
        // sudutnya. Itu yang membuat seluruh bola muat di satu kotak tanpa
        // menyisakan daerah kosong.
        p = Vec2((1.0f - std::abs(n.y)) * SignNotZero(n.x),
                 (1.0f - std::abs(n.x)) * SignNotZero(n.y));
    }
    return p * 0.5f + Vec2(0.5f);
}

Vec3 OctDecode(const Vec2& uv) {
    const Vec2 f = uv * 2.0f - Vec2(1.0f);
    Vec3 n(f.x, f.y, 1.0f - std::abs(f.x) - std::abs(f.y));
    const float fold = std::max(-n.z, 0.0f);
    n.x += n.x >= 0.0f ? -fold : fold;
    n.y += n.y >= 0.0f ? -fold : fold;
    return glm::normalize(n);
}

void ProbeGrid::Configure(uint32_t viewportWidth, uint32_t viewportHeight,
                          const ProbeGridSettings& settings) {
    settings_ = settings;
    settings_.tileSize = std::max(settings.tileSize, 1u);
    settings_.raysPerAxis = std::max(settings.raysPerAxis, 1u);
    settings_.accumulationFrames = std::max(settings.accumulationFrames, 1u);
    viewport_ = {viewportWidth, viewportHeight};
    if (viewportWidth == 0 || viewportHeight == 0) {
        counts_ = {0, 0};
        return;
    }
    // Dibulatkan ke atas: ubin terakhir yang hanya terisi sebagian tetap butuh
    // probe. Dibulatkan ke bawah, sepotong tepi kanan dan bawah layar tidak
    // punya probe sama sekali — dan tepi layar justru tempat penelusuran
    // screen-space paling sering menyerah ke SDF.
    counts_ = {(viewportWidth + settings_.tileSize - 1) / settings_.tileSize,
               (viewportHeight + settings_.tileSize - 1) / settings_.tileSize};
}

Vec2 ProbeGrid::ProbePixel(uint32_t x, uint32_t y) const {
    const float tile = static_cast<float>(settings_.tileSize);
    const Vec2 centre((static_cast<float>(x) + 0.5f) * tile,
                      (static_cast<float>(y) + 0.5f) * tile);
    return {std::min(centre.x, static_cast<float>(viewport_.x) - 0.5f),
            std::min(centre.y, static_cast<float>(viewport_.y) - 0.5f)};
}

Vec2 ProbeGrid::TileCoordinate(const Vec2& pixel) const {
    const float tile = static_cast<float>(settings_.tileSize);
    // Dikurangi setengah ubin karena probe duduk di pusat ubinnya, bukan di
    // pojoknya — tanpa itu seluruh interpolasi bergeser setengah ubin, dan
    // pergeseran itu terlihat sebagai cahaya yang selalu meleset ke satu arah.
    return {pixel.x / tile - 0.5f, pixel.y / tile - 0.5f};
}

Vec3 ProbeRayDirection(uint32_t ray, uint32_t frame, uint32_t probeIndex,
                       const ProbeGridSettings& settings) {
    const uint32_t axis = std::max(settings.raysPerAxis, 1u);
    const uint32_t count = axis * axis;
    const uint32_t index = ray % count;
    const Vec2 cell(static_cast<float>(index % axis), static_cast<float>(index / axis));

    // Satu ray per sel oktahedral, jadi keenam belasnya menutupi seluruh bola
    // setiap frame. Yang berjitter hanya letaknya di dalam selnya — stratifikasi
    // yang hilang kalau seluruh arah diacak bebas, dan tanpa stratifikasi
    // separuh bola bisa tidak tersampel sama sekali pada sebuah frame.
    const Vec2 jitter = Hammersley2D(frame, settings.accumulationFrames) +
                        ProbeOffset(probeIndex);
    const Vec2 within(jitter.x - std::floor(jitter.x), jitter.y - std::floor(jitter.y));

    const Vec2 uv = (cell + within) / static_cast<float>(axis);
    return OctDecode(uv);
}

Vec3 IntegrateIrradiance(const ProbeRay* rays, uint32_t count, const Vec3& normal) {
    if (rays == nullptr || count == 0) {
        return Vec3(0.0f);
    }
    Vec3 sum(0.0f);
    for (uint32_t i = 0; i < count; ++i) {
        const float cosine = glm::dot(normal, rays[i].direction);
        if (cosine > 0.0f) {
            sum += rays[i].radiance * cosine;
        }
    }
    // 4π luas bola, bukan 2π: yang disampel seluruh bola, dan separuh yang di
    // belakang permukaan menyumbang nol lewat `cosine > 0`.
    constexpr float kSphereArea = 4.0f * 3.14159265358979323846f;
    return sum * (kSphereArea / static_cast<float>(count));
}

namespace {

/// Empat basis SH pertama pada sebuah arah.
Vec4 ShBasis(const Vec3& direction) {
    return {0.282095f, 0.488603f * direction.y, 0.488603f * direction.z,
            0.488603f * direction.x};
}

/// Faktor konvolusi cosinus per orde. Sama dengan `EvaluateIrradiance` di
/// `Ibl.h` — dua rumus iradiansi SH yang berselisih di enginenya sendiri adalah
/// dua jawaban berbeda untuk pertanyaan yang sama.
constexpr float kCosineA0 = 3.14159265358979323846f;
constexpr float kCosineA1 = 2.0943951023931953f;  // 2π/3

}  // namespace

ProbeSh ProjectProbeSh(const ProbeRay* rays, uint32_t count) {
    ProbeSh sh;
    if (rays == nullptr || count == 0) {
        return sh;
    }
    // Bobot sampel bola seragam: 4π/N.
    constexpr float kSphereArea = 4.0f * 3.14159265358979323846f;
    const float weight = kSphereArea / static_cast<float>(count);
    for (uint32_t i = 0; i < count; ++i) {
        const Vec4 basis = ShBasis(rays[i].direction) * weight;
        sh.r += basis * rays[i].radiance.r;
        sh.g += basis * rays[i].radiance.g;
        sh.b += basis * rays[i].radiance.b;
    }
    return sh;
}

Vec3 EvaluateProbeSh(const ProbeSh& sh, const Vec3& normal) {
    const Vec4 basis = ShBasis(normal) * Vec4(kCosineA0, kCosineA1, kCosineA1, kCosineA1);
    return {glm::dot(sh.r, basis), glm::dot(sh.g, basis), glm::dot(sh.b, basis)};
}

ProbeSh BlendProbeSh(const ProbeSh& sh, float weight) {
    return {sh.r * weight, sh.g * weight, sh.b * weight};
}

ProbeSh AddProbeSh(const ProbeSh& a, const ProbeSh& b) {
    return {a.r + b.r, a.g + b.g, a.b + b.b};
}

Vec3 AccumulateProbe(const Vec3& history, const Vec3& current, uint32_t frames,
                     uint32_t maxFrames) {
    const uint32_t weight = std::min(frames + 1, std::max(maxFrames, 1u));
    return history + (current - history) / static_cast<float>(weight);
}

float ProbeWeight(const ProbeSurface& probe, const Vec3& pixelPosition, const Vec3& pixelNormal,
                  const ProbeFilterSettings& settings) {
    if (!probe.valid) {
        return 0.0f;
    }
    const float cosine = glm::dot(probe.normal, pixelNormal);
    if (cosine < settings.normalCosine) {
        return 0.0f;
    }
    // Jarak tegak lurus bidang piksel: dua titik di lantai yang sama boleh
    // berjauhan, dua titik yang terpisah dinding tidak boleh berdekatan.
    const float plane = std::abs(glm::dot(probe.position - pixelPosition, pixelNormal));
    if (plane > settings.planeDistance) {
        return 0.0f;
    }

    // Melandai, bukan memotong tegak. Bobot yang melompat dari satu ke nol
    // membuat batas tempat sebuah probe mulai diabaikan terlihat sebagai garis.
    const float planeWeight = 1.0f - plane / std::max(settings.planeDistance, 1e-6f);
    const float normalWeight = (cosine - settings.normalCosine) /
                               std::max(1.0f - settings.normalCosine, 1e-6f);
    return planeWeight * normalWeight;
}

}  // namespace sim::render
