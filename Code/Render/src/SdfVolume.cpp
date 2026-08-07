#include "Sim/Render/SdfVolume.h"

#include <algorithm>
#include <cmath>

namespace sim::render {
namespace {

/// Backend sphere tracing di atas `SdfVolume`.
class SdfTraceBackend final : public ITraceBackend {
public:
    SdfTraceBackend(const SdfVolume& volume, uint32_t maxSteps)
        : volume_(volume), maxSteps_(std::max(maxSteps, 1u)) {}

    TraceBackendKind Kind() const override { return TraceBackendKind::Sdf; }

    TraceResult Trace(const Vec3& origin, const Vec3& direction, float tMax) const override {
        TraceResult result;
        const float length = glm::length(direction);
        if (length < 1e-6f || tMax <= 0.0f) {
            return result;
        }
        const Vec3 ray = direction / length;

        // Ambang "sudah sampai" diikat ke ukuran voxel kaskade tempat titiknya
        // berada, bukan ke satu angka tetap. Ambang tetap yang cukup halus untuk
        // kaskade terdekat membuat sphere tracing di kaskade terkasar berputar
        // ratusan langkah untuk mendekati permukaan yang lebarnya satu voxel.
        float travelled = 0.0f;
        for (uint32_t step = 0; step < maxSteps_; ++step) {
            const Vec3 position = origin + ray * travelled;
            const int cascade = volume_.Clipmap().CascadeFor(position);
            if (cascade < 0) {
                // Keluar dari seluruh kaskade: bukan hit, dan bukan kegagalan.
                // Pemanggil yang melanjutkan ke langit tahu ini artinya "tidak
                // ada apa-apa di jangkauan SDF", bukan "tidak ada apa-apa".
                result.steps = step;
                return result;
            }

            float distance = 0.0f;
            if (!volume_.Sample(position, distance)) {
                result.steps = step;
                return result;
            }

            const float voxel = volume_.Clipmap().VoxelSize(static_cast<uint32_t>(cascade));
            if (distance < voxel * 0.5f) {
                result.hit = true;
                result.distance = travelled;
                result.position = position;
                result.steps = step + 1;
                return result;
            }

            // Melangkah sejauh jarak yang dijamin kosong. Melangkah lebih jauh
            // menembus permukaan tipis; melangkah lebih pendek hanya membuang
            // langkah, dan langkah adalah satu-satunya yang dibatasi anggaran.
            travelled += std::max(distance, voxel * 0.25f);
            if (travelled > tMax) {
                result.steps = step + 1;
                return result;
            }
        }
        result.steps = maxSteps_;
        return result;
    }

private:
    const SdfVolume& volume_;
    uint32_t maxSteps_;
};

}  // namespace

void SdfVolume::Configure(const SdfClipmapSettings& settings) {
    clipmap_.Configure(settings);
    const SdfClipmapSettings& resolved = clipmap_.Settings();
    const std::size_t voxels = static_cast<std::size_t>(resolved.resolution) *
                               resolved.resolution * resolved.resolution;
    for (uint32_t cascade = 0; cascade < kMaxSdfCascades; ++cascade) {
        if (cascade < resolved.cascadeCount) {
            // 127 adalah jarak nol pada penyandian bertanda; volume yang belum
            // diisi karena itu berarti "permukaan ada di mana-mana", bukan
            // "ruang kosong tak terbatas". Yang pertama membuat sphere tracing
            // berhenti di langkah pertama; yang kedua membuatnya menembus
            // seluruh dunia dan melaporkan miss yang salah.
            data_[cascade].assign(voxels, 255);
        } else {
            data_[cascade].clear();
        }
    }
    written_ = 0;
}

void SdfVolume::WriteBox(uint32_t cascade, const SdfClipmap::TexelBox& box,
                         const DistanceField& field) {
    const uint32_t resolution = clipmap_.Settings().resolution;
    const float voxel = clipmap_.VoxelSize(cascade);
    std::vector<uint8_t>& target = data_[cascade];

    for (uint32_t z = box.min.z; z < box.max.z; ++z) {
        for (uint32_t y = box.min.y; y < box.max.y; ++y) {
            for (uint32_t x = box.min.x; x < box.max.x; ++x) {
                const glm::ivec3 worldVoxel =
                    box.worldMin + glm::ivec3(static_cast<int32_t>(x - box.min.x),
                                              static_cast<int32_t>(y - box.min.y),
                                              static_cast<int32_t>(z - box.min.z));
                // Pusat voxel, bukan pojoknya. Sampel di pojok menggeser seluruh
                // medan setengah voxel — cukup untuk membuat permukaan tampak
                // bergeser terhadap geometri yang menghasilkannya.
                const Vec3 world = (Vec3(static_cast<float>(worldVoxel.x),
                                         static_cast<float>(worldVoxel.y),
                                         static_cast<float>(worldVoxel.z)) +
                                    Vec3(0.5f)) *
                                   voxel;
                const float encoded = clipmap_.EncodeDistance(cascade, field(world));
                const std::size_t index =
                    (static_cast<std::size_t>(z) * resolution + y) * resolution + x;
                target[index] = static_cast<uint8_t>(std::lround(encoded * 255.0f));
                ++written_;
            }
        }
    }
}

void SdfVolume::Fill(const SdfScrollResult& scroll, const DistanceField& field) {
    for (const SdfScrollRegion& region : scroll.regions) {
        clipmap_.SplitWrapped(region, boxes_);
        for (const SdfClipmap::TexelBox& box : boxes_) {
            WriteBox(region.cascade, box, field);
        }
    }
}

void SdfVolume::FillAll(const DistanceField& field) {
    const uint32_t resolution = clipmap_.Settings().resolution;
    for (uint32_t cascade = 0; cascade < clipmap_.CascadeCount(); ++cascade) {
        SdfScrollRegion region;
        region.cascade = cascade;
        region.min = clipmap_.VoxelOrigin(cascade);
        region.max = region.min + glm::ivec3(static_cast<int32_t>(resolution));
        clipmap_.SplitWrapped(region, boxes_);
        for (const SdfClipmap::TexelBox& box : boxes_) {
            WriteBox(cascade, box, field);
        }
    }
}

uint8_t SdfVolume::At(uint32_t cascade, const glm::uvec3& texel) const {
    const uint32_t resolution = clipmap_.Settings().resolution;
    const std::size_t index =
        (static_cast<std::size_t>(texel.z) * resolution + texel.y) * resolution + texel.x;
    return data_[cascade][index];
}

bool SdfVolume::SampleCascade(uint32_t cascade, const Vec3& worldPosition,
                              float& outDistance) const {
    if (cascade >= clipmap_.CascadeCount() || data_[cascade].empty()) {
        return false;
    }
    const float voxel = clipmap_.VoxelSize(cascade);
    // Dikurangi setengah voxel karena nilainya disimpan di pusat voxel.
    const Vec3 grid = worldPosition / voxel - Vec3(0.5f);
    const glm::ivec3 base(static_cast<int32_t>(std::floor(grid.x)),
                          static_cast<int32_t>(std::floor(grid.y)),
                          static_cast<int32_t>(std::floor(grid.z)));
    const Vec3 fraction = grid - Vec3(static_cast<float>(base.x), static_cast<float>(base.y),
                                      static_cast<float>(base.z));

    float corners[8];
    for (int i = 0; i < 8; ++i) {
        const glm::ivec3 offset((i & 1), (i >> 1) & 1, (i >> 2) & 1);
        const glm::uvec3 texel = clipmap_.TexelOf(cascade, base + offset);
        corners[i] = clipmap_.DecodeDistance(
            cascade, static_cast<float>(At(cascade, texel)) / 255.0f);
    }
    const float x00 = glm::mix(corners[0], corners[1], fraction.x);
    const float x10 = glm::mix(corners[2], corners[3], fraction.x);
    const float x01 = glm::mix(corners[4], corners[5], fraction.x);
    const float x11 = glm::mix(corners[6], corners[7], fraction.x);
    const float y0 = glm::mix(x00, x10, fraction.y);
    const float y1 = glm::mix(x01, x11, fraction.y);
    outDistance = glm::mix(y0, y1, fraction.z);
    return true;
}

bool SdfVolume::Sample(const Vec3& worldPosition, float& outDistance) const {
    const int cascade = clipmap_.CascadeFor(worldPosition);
    if (cascade < 0) {
        return false;
    }
    return SampleCascade(static_cast<uint32_t>(cascade), worldPosition, outDistance);
}

std::unique_ptr<ITraceBackend> CreateSdfTraceBackend(const SdfVolume& volume, uint32_t maxSteps) {
    return std::make_unique<SdfTraceBackend>(volume, maxSteps);
}

}  // namespace sim::render
