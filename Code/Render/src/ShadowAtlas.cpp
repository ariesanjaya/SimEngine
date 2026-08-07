#include "Sim/Render/ShadowAtlas.h"

#include "Sim/Render/Frustum.h"

#include <algorithm>
#include <cmath>

namespace sim::render {
namespace {

/// Sumbu "atas" tiap muka point light.
///
/// Bukan Y untuk semuanya: muka +Y dan −Y memandang lurus sepanjang Y, dan
/// `LookAt` dengan atas yang sejajar arah pandang menghasilkan matriks yang
/// tidak terdefinisi — dua dari enam muka lalu berisi NaN, dan bayangannya
/// hilang hanya di atas dan di bawah lampu.
Vec3 FaceUp(uint32_t face) {
    switch (face) {
        case 2:
            return Vec3(0.0f, 0.0f, 1.0f);
        case 3:
            return Vec3(0.0f, 0.0f, -1.0f);
        default:
            return Vec3(0.0f, 1.0f, 0.0f);
    }
}

/// Pengalokasi ubin pangkat dua di atas kisi ubin terkecil.
///
/// Bentuk quadtree tanpa pohonnya: sebuah ubin berukuran `size` hanya boleh
/// mulai pada kelipatan `size`, jadi keselarasan itu sendiri yang mencegah dua
/// ubin bertindihan. Pohon sungguhan akan mengemas lebih rapat, dan mengemas
/// lebih rapat bukan yang dibutuhkan di sini — yang dibutuhkan adalah alokasi
/// yang hasilnya sama setiap frame.
class TileAllocator {
public:
    TileAllocator(uint32_t resolution, uint32_t minTile)
        : minTile_(std::max(minTile, 1u)),
          cells_(std::max(resolution / std::max(minTile, 1u), 1u)),
          used_(static_cast<size_t>(cells_) * cells_, false) {}

    /// Mencari tempat untuk ubin `size`. Mengembalikan false bila penuh.
    bool Allocate(uint32_t size, uint32_t& outX, uint32_t& outY) {
        const uint32_t span = std::max(size / minTile_, 1u);
        if (span > cells_) {
            return false;
        }
        for (uint32_t y = 0; y + span <= cells_; y += span) {
            for (uint32_t x = 0; x + span <= cells_; x += span) {
                if (!Free(x, y, span)) {
                    continue;
                }
                Mark(x, y, span);
                outX = x * minTile_;
                outY = y * minTile_;
                return true;
            }
        }
        return false;
    }

private:
    bool Free(uint32_t x, uint32_t y, uint32_t span) const {
        for (uint32_t j = 0; j < span; ++j) {
            for (uint32_t i = 0; i < span; ++i) {
                if (used_[static_cast<size_t>(y + j) * cells_ + x + i]) {
                    return false;
                }
            }
        }
        return true;
    }

    void Mark(uint32_t x, uint32_t y, uint32_t span) {
        for (uint32_t j = 0; j < span; ++j) {
            for (uint32_t i = 0; i < span; ++i) {
                used_[static_cast<size_t>(y + j) * cells_ + x + i] = true;
            }
        }
    }

    uint32_t minTile_;
    uint32_t cells_;
    std::vector<bool> used_;
};

/// Ukuran ubin dari kepentingan, dibulatkan ke pangkat dua.
uint32_t TileSizeFor(float importance, const ShadowAtlasSettings& settings) {
    // Jari-jari layar 0,5 (lampu memenuhi separuh tinggi layar) pantas mendapat
    // ubin terbesar; setiap pembagian dua sesudahnya turun satu tingkat.
    float size = static_cast<float>(settings.maxTile) * std::sqrt(std::max(importance, 0.0f) * 2.0f);
    uint32_t rounded = settings.minTile;
    while (rounded * 2 <= settings.maxTile && static_cast<float>(rounded) * 2.0f <= size) {
        rounded *= 2;
    }
    return std::clamp(rounded, settings.minTile, settings.maxTile);
}

}  // namespace

Vec3 ShadowFaceDirection(uint32_t face) {
    switch (face) {
        case 0:
            return Vec3(1.0f, 0.0f, 0.0f);
        case 1:
            return Vec3(-1.0f, 0.0f, 0.0f);
        case 2:
            return Vec3(0.0f, 1.0f, 0.0f);
        case 3:
            return Vec3(0.0f, -1.0f, 0.0f);
        case 4:
            return Vec3(0.0f, 0.0f, 1.0f);
        default:
            return Vec3(0.0f, 0.0f, -1.0f);
    }
}

float ShadowImportance(const LightInstance& light, const Vec3& eye) {
    if (light.kind == LightKind::Directional) {
        return 0.0f;
    }
    const float distance = glm::length(light.position - eye);
    // Lampu yang menelan kamera adalah lampu yang paling penting: ia mengenai
    // seluruh layar. Tanpa cabang ini pembaginya mengecil ke nol dan
    // kepentingannya menjadi tak hingga — yang lolos sebagai NaN begitu ia
    // dipakai dalam perbandingan.
    if (distance <= light.range) {
        return 1.0f;
    }
    return std::clamp(light.range / distance, 0.0f, 1.0f);
}

Mat4 ShadowFaceMatrix(const LightInstance& light, uint32_t face, float nearZ) {
    const float far = std::max(light.range, nearZ * 2.0f);
    const float near = std::max(nearZ, 1e-3f);

    Vec3 forward;
    float fovY = kHalfPi;
    if (light.kind == LightKind::Spot) {
        forward = glm::normalize(light.direction);
        // Dua kali sudut luar, plus sedikit kelonggaran: tepi berkasnya memudar
        // sampai tepat di sudut luar, dan frustum yang dipas persis di sana
        // memotong bayangan tepat pada piksel yang masih menerima cahaya.
        const float outer = std::acos(std::clamp(light.cosOuter, -0.999f, 0.999f));
        fovY = std::clamp(outer * 2.0f * 1.05f, 0.05f, 3.0f);
    } else {
        forward = ShadowFaceDirection(face);
    }

    const Mat4 view = LookAt(light.position, light.position + forward, FaceUp(face));
    // Perspektif biasa, bukan reversed-Z — lihat catatan di header.
    Mat4 projection = glm::perspective(fovY, 1.0f, near, far);
    projection[1][1] *= -1.0f;
    return projection * view;
}

ShadowAtlasResult AllocateShadowAtlas(std::span<const LightInstance> lights, const Vec3& eye,
                                      const ShadowAtlasSettings& settings) {
    ShadowAtlasResult result;
    result.firstEntry.assign(lights.size(), -1);

    struct Candidate {
        uint32_t index = 0;
        float importance = 0.0f;
        uint32_t faces = 1;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(lights.size());
    for (uint32_t i = 0; i < lights.size(); ++i) {
        const LightInstance& light = lights[i];
        if (light.kind == LightKind::Directional || !light.castShadows || light.range <= 0.0f) {
            continue;
        }
        Candidate candidate;
        candidate.index = i;
        candidate.importance = ShadowImportance(light, eye);
        candidate.faces = light.kind == LightKind::Point ? 6u : 1u;
        candidates.push_back(candidate);
    }

    // Yang penting lebih dulu, dan yang seri diurutkan menurut indeks. Urutan
    // yang bergoyang antar-frame membuat lampu bergantian kehilangan bayangan —
    // kedipan yang jauh lebih mencolok daripada bayangan yang memang tidak ada.
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) {
                         if (a.importance != b.importance) {
                             return a.importance > b.importance;
                         }
                         return a.index < b.index;
                     });

    TileAllocator allocator(settings.resolution, settings.minTile);
    uint32_t facesUsed = 0;

    for (const Candidate& candidate : candidates) {
        const LightInstance& light = lights[candidate.index];
        if (facesUsed + candidate.faces > settings.maxFaces) {
            ++result.dropped;
            continue;
        }
        uint32_t size = TileSizeFor(candidate.importance, settings);
        // Point light membayar enam muka, jadi ubinnya diturunkan satu tingkat:
        // tanpa itu sebuah point light memakan atlas sebanyak enam spot yang
        // sama pentingnya, dan yang terlihat adalah spot yang kehilangan
        // bayangan setiap kali ada satu point light di dekatnya.
        if (candidate.faces > 1) {
            size = std::max(size / 2, settings.minTile);
        }

        const auto first = static_cast<int32_t>(result.entries.size());
        bool placed = true;
        for (uint32_t face = 0; face < candidate.faces; ++face) {
            uint32_t x = 0;
            uint32_t y = 0;
            if (!allocator.Allocate(size, x, y)) {
                placed = false;
                break;
            }
            ShadowAtlasEntry entry;
            entry.light = candidate.index;
            entry.face = face;
            entry.x = x;
            entry.y = y;
            entry.size = size;
            entry.nearZ = std::max(light.sourceRadius, 0.02f);
            entry.farZ = std::max(light.range, entry.nearZ * 2.0f);
            entry.viewProjection = ShadowFaceMatrix(light, face, entry.nearZ);
            result.entries.push_back(entry);
        }

        if (!placed) {
            // Sebagian muka sudah masuk sebelum atlas penuh. Dibatalkan
            // seluruhnya, bukan disisakan: point light dengan empat dari enam
            // muka menghasilkan bayangan yang berhenti di tengah udara, dan itu
            // lebih buruk daripada tidak ada bayangan sama sekali.
            result.entries.resize(static_cast<size_t>(first));
            ++result.dropped;
            continue;
        }
        result.firstEntry[candidate.index] = first;
        facesUsed += candidate.faces;
    }
    return result;
}

}  // namespace sim::render
