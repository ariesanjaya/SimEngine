#include "Sim/Render/LightCluster.h"

#include <algorithm>
#include <cmath>

namespace sim::render {

void ClusterGrid::Build(const ClusterGridSettings& settings, float fovYRadians, float aspect,
                        float nearZ, float farZ) {
    settings_ = settings;
    settings_.tilesX = std::max(settings.tilesX, 1u);
    settings_.tilesY = std::max(settings.tilesY, 1u);
    settings_.slices = std::max(settings.slices, 1u);

    nearZ_ = std::max(nearZ, 0.001f);
    farZ_ = std::max(farZ, nearZ_ * 1.001f);
    tanHalfY_ = std::tan(std::clamp(fovYRadians, 0.001f, 3.10f) * 0.5f);
    tanHalfX_ = tanHalfY_ * std::max(aspect, 0.001f);

    // slice = log(z / near) / log(far / near) * slices
    //       = log(z) * scale - bias
    //
    // Disimpan sebagai skala dan bias supaya shader menghitungnya dengan satu
    // perkalian dan satu pengurangan, dan supaya CPU dan GPU memakai rumus yang
    // sama persis. Dua rumus yang "setara secara matematis" tapi ditulis berbeda
    // akan berselisih satu irisan di tepinya, dan yang terlihat adalah lampu
    // yang hilang tepat pada jarak tertentu.
    const float logRatio = std::log(farZ_ / nearZ_);
    sliceScale_ = static_cast<float>(settings_.slices) / logRatio;
    sliceBias_ = -sliceScale_ * std::log(nearZ_);
}

uint32_t ClusterGrid::SliceOf(float viewDepth) const {
    const float depth = std::max(viewDepth, nearZ_);
    const float slice = std::log(depth) * sliceScale_ + sliceBias_;
    const auto index = static_cast<int>(std::floor(slice));
    return static_cast<uint32_t>(std::clamp(index, 0, static_cast<int>(settings_.slices) - 1));
}

Vec2 ClusterGrid::SliceBounds(uint32_t slice) const {
    const auto count = static_cast<float>(settings_.slices);
    const float from = static_cast<float>(std::min(slice, settings_.slices)) / count;
    const float to = static_cast<float>(std::min(slice + 1, settings_.slices)) / count;
    const float ratio = farZ_ / nearZ_;
    return {nearZ_ * std::pow(ratio, from), nearZ_ * std::pow(ratio, to)};
}

Aabb ClusterGrid::ClusterBounds(uint32_t x, uint32_t y, uint32_t slice) const {
    const Vec2 depth = SliceBounds(slice);
    const auto tilesX = static_cast<float>(settings_.tilesX);
    const auto tilesY = static_cast<float>(settings_.tilesY);

    // Sudut ubin dalam koordinat ternormalisasi [-1, 1].
    //
    // **Baris 0 adalah baris ATAS layar, jadi Y-nya dibalik.** Indeks ubin di
    // sini adalah indeks ubin layar — shader menghitungnya dari `gl_FragCoord`,
    // yang sumbu Y-nya menunjuk ke bawah — sedangkan +Y ruang pandang menunjuk
    // ke atas. Tanpa pembalikan ini, sebuah fragmen mencari lampu di baris ubin
    // yang tercermin: lampu yang ada di bawah layar dicari di daftar milik
    // bagian atas.
    //
    // Kegagalannya tidak terlihat sebagai cahaya yang salah tempat melainkan
    // sebagai **cahaya yang terpotong tepat di batas ubin** — persegi bertepi
    // tegak lurus di ruang layar, yang bentuknya tidak mungkin dihasilkan
    // geometri mana pun. Itu yang menemukannya.
    const float x0 = static_cast<float>(x) / tilesX * 2.0f - 1.0f;
    const float x1 = static_cast<float>(x + 1) / tilesX * 2.0f - 1.0f;
    const float y0 = 1.0f - static_cast<float>(y + 1) / tilesY * 2.0f;
    const float y1 = 1.0f - static_cast<float>(y) / tilesY * 2.0f;

    // Ubin melebar bersama kedalaman, jadi batas melintangnya diambil dari
    // bidang **jauh** irisan — di sanalah ubinnya paling lebar. Memakai bidang
    // dekat menghasilkan kotak yang terlalu kecil, dan lampu yang seharusnya
    // menyala di ujung jauh sebuah cluster akan hilang.
    Aabb box;
    box.min.x = std::min(x0 * tanHalfX_ * depth.x, x0 * tanHalfX_ * depth.y);
    box.max.x = std::max(x1 * tanHalfX_ * depth.x, x1 * tanHalfX_ * depth.y);
    box.min.y = std::min(y0 * tanHalfY_ * depth.x, y0 * tanHalfY_ * depth.y);
    box.max.y = std::max(y1 * tanHalfY_ * depth.x, y1 * tanHalfY_ * depth.y);
    box.min.z = depth.x;
    box.max.z = depth.y;
    return box;
}

bool SphereIntersectsAabb(const Vec3& centre, float radius, const Aabb& box) {
    // Jarak ke titik terdekat pada kotak. Dibandingkan dalam bentuk kuadrat
    // supaya tidak ada akar kuadrat di lingkaran terdalam penyaringan.
    const Vec3 closest = glm::clamp(centre, box.min, box.max);
    const Vec3 delta = centre - closest;
    return glm::dot(delta, delta) <= radius * radius;
}

bool ConeIntersectsSphere(const Vec3& apex, const Vec3& direction, float range,
                          float cosOuterAngle, const Vec3& centre, float radius) {
    const Vec3 toSphere = centre - apex;
    const float distanceSq = glm::dot(toSphere, toSphere);
    const float reach = range + radius;
    if (distanceSq > reach * reach) {
        return false;
    }
    // Bola yang menelan puncak kerucut selalu memotongnya.
    if (distanceSq <= radius * radius) {
        return true;
    }

    const float cosAngle = std::clamp(cosOuterAngle, -1.0f, 1.0f);
    const float sinAngle = std::sqrt(std::max(1.0f - cosAngle * cosAngle, 0.0f));

    // Uji baku: geser puncak mundur sejauh r/sin θ sepanjang sumbu, lalu periksa
    // apakah pusat bola berada di dalam kerucut yang sudah digeser itu.
    // Menggesernya yang membuat bola yang menyerempet sisi kerucut ikut
    // terhitung — tanpa itu hanya pusat bola yang diuji, dan lampu sorot akan
    // memotong benda tepat di tepi berkasnya.
    if (sinAngle > 1e-4f) {
        const Vec3 shiftedApex = apex - direction * (radius / sinAngle);
        const Vec3 toCentre = centre - shiftedApex;
        const float axial = glm::dot(toCentre, direction);
        if (axial <= 0.0f) {
            return false;
        }
        return axial * axial >= glm::dot(toCentre, toCentre) * cosAngle * cosAngle;
    }
    // Kerucut yang sudah menjadi garis: cukup uji jarak bola ke sinar itu.
    const float axial = std::clamp(glm::dot(toSphere, direction), 0.0f, range);
    const Vec3 delta = toSphere - direction * axial;
    return glm::dot(delta, delta) <= radius * radius;
}

const LightInstance* FindSunLight(std::span<const LightInstance> lights) {
    for (const LightInstance& light : lights) {
        if (light.kind == LightKind::Directional) {
            return &light;
        }
    }
    return nullptr;
}

ClusterViewLight ToClusterView(const Mat4& view, const Vec3& position, const Vec3& direction) {
    ClusterViewLight out;
    // Posisi memakai matriks penuh, arah hanya bagian 3x3-nya: translasi tidak
    // boleh ikut pada vektor arah.
    const Vec3 viewPosition = Vec3(view * Vec4(position, 1.0f));
    const Vec3 viewDirection = Mat3(view) * direction;
    out.position = Vec3(viewPosition.x, viewPosition.y, -viewPosition.z);
    out.direction = glm::normalize(Vec3(viewDirection.x, viewDirection.y, -viewDirection.z));
    return out;
}

ClusterAssignment AssignLights(const ClusterGrid& grid, const Mat4& view,
                               std::span<const ClusterLight> lights,
                               const ClusterGridSettings& settings) {
    ClusterAssignment assignment;
    const uint32_t clusters = grid.ClusterCount();
    assignment.ranges.assign(clusters, ClusterAssignment::Range{});
    if (clusters == 0) {
        return assignment;
    }

    // Lampu dipindahkan ke ruang pandang sekali, bukan per cluster. Sebuah
    // adegan dengan 200 lampu dan 3456 cluster berarti selisihnya 200 perkalian
    // matriks melawan 691.200.
    struct ViewLight {
        Vec3 position{0.0f};
        Vec3 direction{0.0f, 0.0f, 1.0f};
        float range = 0.0f;
        float cosOuterAngle = 1.0f;
        bool spot = false;
    };
    std::vector<ViewLight> viewLights;
    viewLights.reserve(lights.size());
    for (const ClusterLight& light : lights) {
        ViewLight entry;
        const ClusterViewLight placed = ToClusterView(view, light.position, light.direction);
        entry.position = placed.position;
        entry.direction = placed.direction;
        entry.range = std::max(light.range, 0.0f);
        entry.cosOuterAngle = light.cosOuterAngle;
        entry.spot = light.type == ClusterLightType::Spot;
        viewLights.push_back(entry);
    }

    const uint32_t limit = std::max(settings.maxLightsPerCluster, 1u);
    std::vector<uint32_t> hits;
    hits.reserve(limit);
    assignment.indices.reserve(static_cast<size_t>(clusters) * 2);

    for (uint32_t slice = 0; slice < grid.Slices(); ++slice) {
        for (uint32_t y = 0; y < grid.TilesY(); ++y) {
            for (uint32_t x = 0; x < grid.TilesX(); ++x) {
                const Aabb box = grid.ClusterBounds(x, y, slice);
                hits.clear();
                bool truncated = false;
                for (uint32_t i = 0; i < viewLights.size(); ++i) {
                    const ViewLight& light = viewLights[i];
                    if (!SphereIntersectsAabb(light.position, light.range, box)) {
                        continue;
                    }
                    if (light.spot) {
                        // Bola pembatas kotak, lalu uji kerucut terhadapnya.
                        // Longgar ke arah yang aman: cluster yang sesekali
                        // menyalakan lampu yang tidak mengenainya jauh lebih
                        // murah daripada lampu yang sesekali padam.
                        const Vec3 centre = box.Centre();
                        const float boxRadius = glm::length(box.Extent());
                        if (!ConeIntersectsSphere(light.position, light.direction, light.range,
                                                  light.cosOuterAngle, centre, boxRadius)) {
                            continue;
                        }
                    }
                    if (hits.size() >= limit) {
                        truncated = true;
                        break;
                    }
                    hits.push_back(i);
                }

                ClusterAssignment::Range& range = assignment.ranges[grid.IndexOf(x, y, slice)];
                range.offset = static_cast<uint32_t>(assignment.indices.size());
                range.count = static_cast<uint32_t>(hits.size());
                assignment.indices.insert(assignment.indices.end(), hits.begin(), hits.end());
                if (truncated) {
                    ++assignment.overflowed;
                }
            }
        }
    }
    return assignment;
}

}  // namespace sim::render
