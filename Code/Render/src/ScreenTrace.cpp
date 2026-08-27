#include "Sim/Render/ScreenTrace.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sim::render {
namespace {

/// Sinar yang titik asalnya di belakang bidang dekat tidak punya proyeksi yang
/// berarti. Dipotong, bukan dijepit: koordinat layar sebuah titik dengan w
/// negatif adalah pantulan titik itu di seberang layar, dan penelusur yang
/// memakainya menelusuri tempat yang salah dengan penuh keyakinan.
constexpr float kMinW = 1e-4f;

/// Sepersekian sel, dipakai memastikan langkah benar-benar melewati batas sel.
/// Tanpa itu, pembulatan menaruh titik berikutnya tepat di batas dan sel yang
/// sama diuji lagi — anggaran langkah habis tanpa maju satu sel pun.
constexpr float kCellNudge = 1.0f / 128.0f;

Vec3 Unproject(const Mat4& invViewProj, const Vec2& uv, float depth) {
    const Vec4 ndc(uv.x * 2.0f - 1.0f, uv.y * 2.0f - 1.0f, depth, 1.0f);
    const Vec4 world = invViewProj * ndc;
    return Vec3(world) / world.w;
}

/// Nilai parameter saat sinar meninggalkan sel yang memuat `uv`.
float NextCellBoundary(const Vec2& uv0, const Vec2& duv, const glm::uvec2& size, float s) {
    const Vec2 scale(static_cast<float>(size.x), static_cast<float>(size.y));
    const Vec2 point = (uv0 + duv * s) * scale;
    const Vec2 cell(std::floor(point.x), std::floor(point.y));

    float next = std::numeric_limits<float>::max();
    for (int axis = 0; axis < 2; ++axis) {
        const float step = duv[axis] * scale[axis];
        if (std::abs(step) < 1e-9f) {
            continue;
        }
        // Batas ke arah gerak: tepi jauh sel kalau maju, tepi dekatnya kalau
        // mundur.
        const float edge = step > 0.0f ? cell[axis] + 1.0f : cell[axis];
        const float boundary = (edge - uv0[axis] * scale[axis]) / step;
        next = std::min(next, boundary);
    }
    if (next == std::numeric_limits<float>::max()) {
        return std::numeric_limits<float>::max();
    }
    // Setengah sel dinyatakan dalam satuan parameter, dipakai sebagai dorongan.
    const float perCell = 1.0f / std::max(glm::length(duv * scale), 1e-9f);
    return next + perCell * kCellNudge;
}

/// Memotong segmen `[a, b]` di ruang clip terhadap `w >= kMinW`.
/// Mengembalikan false bila seluruhnya di belakang bidang itu.
bool ClipToPositiveW(Vec4& a, Vec4& b) {
    const bool aIn = a.w >= kMinW;
    const bool bIn = b.w >= kMinW;
    if (!aIn && !bIn) {
        return false;
    }
    if (aIn && bIn) {
        return true;
    }
    const float t = (kMinW - a.w) / (b.w - a.w);
    const Vec4 crossing = a + (b - a) * t;
    if (aIn) {
        b = crossing;
    } else {
        a = crossing;
    }
    return true;
}

/// Memotong `[sMin, sMax]` supaya `uv0 + s * duv` tetap di dalam [0,1]².
/// Mengembalikan false bila segmennya sama sekali tidak menyentuh layar.
bool ClipToScreen(const Vec2& uv0, const Vec2& duv, float& sMin, float& sMax) {
    for (int axis = 0; axis < 2; ++axis) {
        if (std::abs(duv[axis]) < 1e-9f) {
            if (uv0[axis] < 0.0f || uv0[axis] > 1.0f) {
                return false;
            }
            continue;
        }
        float tNear = (0.0f - uv0[axis]) / duv[axis];
        float tFar = (1.0f - uv0[axis]) / duv[axis];
        if (tNear > tFar) {
            std::swap(tNear, tFar);
        }
        sMin = std::max(sMin, tNear);
        sMax = std::min(sMax, tFar);
    }
    return sMin <= sMax;
}

}  // namespace

void HiZPyramid::Clear() {
    levels_.clear();
}

void HiZPyramid::Build(uint32_t width, uint32_t height, std::span<const float> depth) {
    Clear();
    if (width == 0 || height == 0 ||
        depth.size() < static_cast<std::size_t>(width) * height) {
        return;
    }

    Level base;
    base.size = {width, height};
    base.texels.assign(depth.begin(), depth.begin() + static_cast<std::ptrdiff_t>(
                                                          static_cast<std::size_t>(width) * height));
    levels_.push_back(std::move(base));

    while (levels_.back().size.x > 1 || levels_.back().size.y > 1) {
        const Level& source = levels_.back();
        Level next;
        next.size = {std::max(1u, source.size.x / 2), std::max(1u, source.size.y / 2)};
        next.texels.resize(static_cast<std::size_t>(next.size.x) * next.size.y);

        for (uint32_t y = 0; y < next.size.y; ++y) {
            // Texel terakhir merangkum sisa barisnya. Tanpa itu, baris dan kolom
            // terakhir dari ukuran ganjil hilang sama sekali — dan yang hilang
            // itu tepi layar, tempat penelusuran screen-space memang paling
            // sering gagal.
            const uint32_t lastY = y + 1 == next.size.y ? source.size.y - 1 : y * 2 + 1;
            for (uint32_t x = 0; x < next.size.x; ++x) {
                const uint32_t lastX = x + 1 == next.size.x ? source.size.x - 1 : x * 2 + 1;
                float closest = -std::numeric_limits<float>::max();
                for (uint32_t sy = y * 2; sy <= lastY; ++sy) {
                    for (uint32_t sx = x * 2; sx <= lastX; ++sx) {
                        closest = std::max(
                            closest,
                            source.texels[static_cast<std::size_t>(sy) * source.size.x + sx]);
                    }
                }
                next.texels[static_cast<std::size_t>(y) * next.size.x + x] = closest;
            }
        }
        levels_.push_back(std::move(next));
    }
}

uint32_t HiZPyramid::LevelsFor(uint32_t width, uint32_t height) {
    uint32_t levels = 1;
    uint32_t size = std::max(std::max(width, height), 1u);
    while (size > 1) {
        size /= 2;
        ++levels;
    }
    return levels;
}

float HiZPyramid::At(uint32_t level, int32_t x, int32_t y) const {
    const Level& target = levels_[level];
    const int32_t cx = std::clamp(x, 0, static_cast<int32_t>(target.size.x) - 1);
    const int32_t cy = std::clamp(y, 0, static_cast<int32_t>(target.size.y) - 1);
    return target.texels[static_cast<std::size_t>(cy) * target.size.x + cx];
}

float HiZPyramid::Sample(uint32_t level, const Vec2& uv) const {
    const glm::uvec2 size = levels_[level].size;
    return At(level, static_cast<int32_t>(std::floor(uv.x * static_cast<float>(size.x))),
              static_cast<int32_t>(std::floor(uv.y * static_cast<float>(size.y))));
}

ScreenTraceResult TraceScreenSpace(const HiZPyramid& depth, const ScreenTraceView& view,
                                   const Vec3& origin, const Vec3& direction, float tMax,
                                   const ScreenTraceSettings& settings) {
    ScreenTraceResult result;
    const float length = glm::length(direction);
    if (!depth.IsValid() || length < 1e-6f || tMax <= 0.0f) {
        result.leftScreen = true;
        return result;
    }
    const Vec3 ray = direction / length;
    const float bias = std::min(settings.originBias, tMax * 0.5f);
    const Vec3 start = origin + ray * bias;

    Vec4 clipStart = view.viewProj * Vec4(start, 1.0f);
    Vec4 clipEnd = view.viewProj * Vec4(origin + ray * tMax, 1.0f);
    if (!ClipToPositiveW(clipStart, clipEnd)) {
        result.leftScreen = true;
        return result;
    }

    const Vec3 ndcStart = Vec3(clipStart) / clipStart.w;
    const Vec3 ndcEnd = Vec3(clipEnd) / clipEnd.w;
    // Bayangan sebuah ruas garis di layar tetap ruas garis lurus, dan depth NDC
    // ikut linear terhadap parameter yang sama — itu sifat proyeksi perspektif
    // yang juga dipakai rasterizer saat menginterpolasi depth. Jadi seluruh
    // penelusuran ini boleh berjalan lurus di ruang NDC.
    const Vec2 uv0(ndcStart.x * 0.5f + 0.5f, ndcStart.y * 0.5f + 0.5f);
    const Vec2 uv1(ndcEnd.x * 0.5f + 0.5f, ndcEnd.y * 0.5f + 0.5f);
    const Vec2 duv = uv1 - uv0;
    const float z0 = ndcStart.z;
    const float dz = ndcEnd.z - ndcStart.z;

    float sMin = 0.0f;
    float sMax = 1.0f;
    if (!ClipToScreen(uv0, duv, sMin, sMax)) {
        result.leftScreen = true;
        return result;
    }
    const uint32_t topLevel = depth.LevelCount() - 1;
    uint32_t level = 0;
    float s = sMin;

    while (result.steps < settings.maxSteps) {
        ++result.steps;
        const float sNext = std::min(NextCellBoundary(uv0, duv, depth.Size(level), s), sMax);
        const Vec2 cellUv = uv0 + duv * ((s + sNext) * 0.5f);
        const float sceneZ = depth.Sample(level, cellUv);

        const float zEnter = z0 + s * dz;
        const float zExit = z0 + sNext * dz;
        // Yang dibandingkan yang **terkecil** di sepanjang potongan ini, bukan
        // yang di ujungnya. Sinar GI boleh mengarah balik ke kamera, dan pada
        // sinar seperti itu depth justru naik — memakai ujungnya saja membuat
        // separuh arah diuji dengan tanda yang salah.
        const float deepest = std::min(zEnter, zExit);

        if (deepest < sceneZ) {
            if (level > 0) {
                // Mungkin menembus permukaan di suatu tempat di dalam sel ini.
                // Turun satu tingkat dan periksa lebih halus — tanpa memajukan
                // `s`, karena yang diperiksa ulang adalah potongan yang sama.
                --level;
                continue;
            }

            // Tingkat terhalus. **Uji ketebalannya di tempat sinar MASUK
            // piksel ini, bukan di tempat ia keluar.** Sinar yang masuk dalam
            // keadaan sudah di belakang permukaan tidak memotong permukaan itu
            // — ia lewat di belakangnya, dan depth buffer yang hanya menyimpan
            // permukaan terdepan tidak punya cara lain untuk tahu. Sinar yang
            // masuk di depan lalu keluar di belakang memang memotongnya, dan
            // itu tidak perlu syarat apa pun.
            if (zEnter < sceneZ) {
                const Vec2 entryUv = uv0 + duv * s;
                const Vec3 entry = Unproject(view.invViewProj, entryUv, zEnter);
                const Vec3 ahead = Unproject(view.invViewProj, entryUv, sceneZ);
                if (glm::distance(entry, ahead) > settings.thickness) {
                    s = sNext;
                    if (s >= sMax) {
                        result.leftScreen = true;
                        return result;
                    }
                    continue;
                }
            }

            const float sHit =
                std::abs(dz) > 1e-9f ? std::clamp((sceneZ - z0) / dz, s, sNext) : s;
            const Vec2 hitUv = uv0 + duv * sHit;
            result.hit = true;
            result.uv = hitUv;
            result.position = Unproject(view.invViewProj, hitUv, sceneZ);
            result.distance = glm::dot(result.position - origin, ray);
            return result;
        }

        s = sNext;
        if (s >= sMax) {
            result.leftScreen = true;
            return result;
        }
        // Di depan seluruh isi sel: lompati sel itu dan naik satu tingkat.
        level = std::min(level + 1, topLevel);
    }

    return result;
}

}  // namespace sim::render
