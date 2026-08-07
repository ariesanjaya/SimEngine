#include "Sim/Render/SdfVolume.h"

#include <algorithm>
#include <array>
#include <limits>
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
                result.layer = TraceLayer::Sdf;
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

/// Jarak bertanda ke sebuah kotak setengah-lebar 0,5 berpusat di titik asal.
///
/// Selalu kubus satuan: matriks modelnyalah yang membawa ukuran dan orientasi
/// benda, persis seperti yang dilakukan `Gather` untuk menggambar.
inline float UnitBoxDistance(const Vec3& point) {
    const Vec3 q = glm::abs(point) - Vec3(0.5f);
    // Suku pertama benar di luar kotak, yang kedua di dalam. Memakai salah
    // satunya saja menghasilkan jarak yang salah tepat di sisi yang lain — dan
    // yang di dalam kotaklah yang menentukan apakah ray yang mulai di dalam
    // dinding bisa keluar.
    return glm::length(glm::max(q, Vec3(0.0f))) + std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f);
}

/// Jarak antara dua kotak sejajar sumbu. Nol bila bersinggungan atau tumpang
/// tindih.
inline float AabbDistance(const Vec3& aMin, const Vec3& aMax, const Vec3& bMin, const Vec3& bMax) {
    const Vec3 gap = glm::max(glm::max(bMin - aMax, aMin - bMax), Vec3(0.0f));
    return glm::length(gap);
}

}  // namespace

void BoxSceneField::Build(std::span<const MeshInstance> meshes) {
    entries_.clear();
    entries_.reserve(meshes.size());

    for (const MeshInstance& mesh : meshes) {
        // Kotak batas dipetakan ke kubus satuan, sama seperti yang dilakukan
        // `Gather` untuk menggambar — supaya yang di-SDF benar-benar bentuk
        // yang tergambar, bukan bentuk yang mirip.
        const Vec3 centre = (mesh.boundsMin + mesh.boundsMax) * 0.5f;
        const Vec3 size = glm::max(mesh.boundsMax - mesh.boundsMin, Vec3(1e-4f));
        Mat4 model = mesh.transform;
        model = glm::translate(model, centre);
        model = glm::scale(model, size);

        const Vec3 scale(glm::length(Vec3(model[0])), glm::length(Vec3(model[1])),
                         glm::length(Vec3(model[2])));
        const float smallest = std::max(std::min({scale.x, scale.y, scale.z}), 1e-4f);
        const float largest = std::max({scale.x, scale.y, scale.z, 1e-4f});

        Entry entry;
        entry.inverse = glm::inverse(model);
        entry.scale = smallest;
        entry.anisotropy = smallest / largest;

        // Kotak batas di ruang dunia: delapan pojok kubus satuan lewat matriks
        // model. Dipakai membuang mesh yang tidak mungkin menyentuh sebuah
        // baris — dan pembuangan itulah yang membuat adegan besar tidak
        // membayar seluruh isinya untuk tiap voxel.
        entry.boundsMin = Vec3(std::numeric_limits<float>::max());
        entry.boundsMax = Vec3(std::numeric_limits<float>::lowest());
        for (int corner = 0; corner < 8; ++corner) {
            const Vec3 local(corner & 1 ? 0.5f : -0.5f, corner & 2 ? 0.5f : -0.5f,
                             corner & 4 ? 0.5f : -0.5f);
            const Vec3 world = Vec3(model * Vec4(local, 1.0f));
            entry.boundsMin = glm::min(entry.boundsMin, world);
            entry.boundsMax = glm::max(entry.boundsMax, world);
        }
        entries_.push_back(entry);
    }
}

float BoxSceneField::Distance(const Vec3& world) const {
    float nearest = std::numeric_limits<float>::max();
    for (const Entry& entry : entries_) {
        const Vec3 local = Vec3(entry.inverse * Vec4(world, 1.0f));
        nearest = std::min(nearest, UnitBoxDistance(local) * entry.scale);
    }
    return nearest;
}

void BoxSceneField::BeginBox(const Vec3& origin, const Vec3& rowStep, const Vec3& outerStep,
                            const Vec3& planeStep, uint32_t count, float band) {
    boxOrigin_ = origin;
    boxRowStep_ = rowStep;
    boxOuterStep_ = outerStep;
    boxPlaneStep_ = planeStep;
    boxCount_ = count;
    boxBand_ = band;

    // Sebuah kotak texel di dunia tetap kotak di ruang lokal: transformasinya
    // affine. Jadi seluruh isi kotak dijangkau dari satu titik asal dan tiga
    // vektor langkah — dan ketiganya cukup dihitung sekali, di sini.
    locals_.resize(entries_.size());
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const Mat4& inverse = entries_[i].inverse;
        locals_[i].origin = Vec3(inverse * Vec4(origin, 1.0f));
        // w = 0: arah, tanpa translasi.
        locals_[i].rowStep = Vec3(inverse * Vec4(rowStep, 0.0f));
        locals_[i].outerStep = Vec3(inverse * Vec4(outerStep, 0.0f));
        locals_[i].planeStep = Vec3(inverse * Vec4(planeStep, 0.0f));
    }
}

void BoxSceneField::Row(uint32_t outer, uint32_t plane, float* out) const {
    for (uint32_t i = 0; i < boxCount_; ++i) {
        out[i] = std::numeric_limits<float>::max();
    }
    if (boxCount_ == 0) {
        return;
    }
    const Vec3 start = boxOrigin_ + boxOuterStep_ * static_cast<float>(outer) +
                       boxPlaneStep_ * static_cast<float>(plane);
    const Vec3 last = start + boxRowStep_ * static_cast<float>(boxCount_ - 1);
    const Vec3 rowMin = glm::min(start, last);
    const Vec3 rowMax = glm::max(start, last);

    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const Entry& entry = entries_[i];
        // Nilai yang akan disimpan tidak pernah lebih kecil daripada jarak
        // dunia dikali `anisotropy`; kalau batas bawah itu saja sudah di luar
        // pita, seluruh baris akan dijepit ke nilai jenuh — jadi mesh ini tidak
        // punya apa pun untuk disumbangkan.
        if (AabbDistance(rowMin, rowMax, entry.boundsMin, entry.boundsMax) * entry.anisotropy >=
            boxBand_) {
            continue;
        }

        const Local& local = locals_[i];
        const Vec3 base = local.origin + local.outerStep * static_cast<float>(outer) +
                          local.planeStep * static_cast<float>(plane);
        for (uint32_t x = 0; x < boxCount_; ++x) {
            const Vec3 point = base + local.rowStep * static_cast<float>(x);
            out[x] = std::min(out[x], UnitBoxDistance(point) * entry.scale);
        }
    }
}

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

template <typename Field>
void SdfVolume::WriteBoxRows(uint32_t cascade, const SdfClipmap::TexelBox& box, Field& field) {
    const uint32_t resolution = clipmap_.Settings().resolution;
    const float voxel = clipmap_.VoxelSize(cascade);
    const glm::uvec3 extent = box.max - box.min;
    if (extent.x == 0 || extent.y == 0 || extent.z == 0) {
        return;
    }

    // **Baris mengikuti sumbu terpanjang kotak, bukan selalu X.** Biaya per
    // baris — satu uji kotak batas dan satu basis lokal per mesh — hanya
    // terbagi kalau barisnya panjang. Lempeng setebal satu voxel di sumbu X,
    // bentuk yang dihasilkan setiap gerakan kamera, akan memberikan ribuan
    // baris berisi satu voxel kalau arahnya dipatok.
    uint32_t major = 0;
    if (extent.y > extent[major]) {
        major = 1;
    }
    if (extent.z > extent[major]) {
        major = 2;
    }
    const uint32_t outerAxis = (major + 1) % 3;
    const uint32_t planeAxis = (major + 2) % 3;
    const uint32_t count = extent[major];

    // Langkah indeks di dalam `data_`: X tercepat, lalu Y, lalu Z.
    const std::array<std::size_t, 3> strides{
        1, resolution, static_cast<std::size_t>(resolution) * resolution};
    const auto axisStep = [voxel](uint32_t axis) {
        Vec3 step(0.0f);
        step[static_cast<int>(axis)] = voxel;
        return step;
    };

    rowScratch_.resize(count);
    std::vector<uint8_t>& target = data_[cascade];

    // Pusat voxel, bukan pojoknya. Sampel di pojok menggeser seluruh medan
    // setengah voxel — cukup untuk membuat permukaan tampak bergeser terhadap
    // geometri yang menghasilkannya.
    const Vec3 origin = (Vec3(static_cast<float>(box.worldMin.x),
                              static_cast<float>(box.worldMin.y),
                              static_cast<float>(box.worldMin.z)) +
                         Vec3(0.5f)) *
                        voxel;
    field.BeginBox(origin, axisStep(major), axisStep(outerAxis), axisStep(planeAxis), count,
                   clipmap_.BandRadius(cascade));

    // Penyandiannya dijabarkan di sini alih-alih memanggil
    // `SdfClipmap::EncodeDistance`. Fungsi itu ada di unit terjemahan lain, jadi
    // ia tidak bisa di-inline — dan ini jalur per voxel, tempat sebuah panggilan
    // yang tak bisa di-inline berharga lebih mahal daripada rumus di dalamnya.
    const float encodeScale = 255.0f / (clipmap_.BandRadius(cascade) * 2.0f);
    const std::size_t rowStride = strides[major];
    for (uint32_t plane = 0; plane < extent[planeAxis]; ++plane) {
        for (uint32_t outer = 0; outer < extent[outerAxis]; ++outer) {
            field.Row(outer, plane, rowScratch_.data());

            const std::size_t base = static_cast<std::size_t>(box.min.z) * strides[2] +
                                     static_cast<std::size_t>(box.min.y) * strides[1] +
                                     box.min.x + outer * strides[outerAxis] +
                                     plane * strides[planeAxis];
            for (uint32_t i = 0; i < count; ++i) {
                const float encoded =
                    std::clamp(rowScratch_[i] * encodeScale + 127.5f, 0.0f, 255.0f);
                target[base + i * rowStride] = static_cast<uint8_t>(encoded + 0.5f);
            }
            written_ += count;
        }
    }
}

namespace {

/// Membungkus `DistanceField` menjadi bentuk penelusur kotak. Dipakai test yang
/// memberi medan sembarang; jalur yang dipakai renderer memakai `BoxSceneField`
/// yang tidak melewati satu pun pemanggilan tak langsung.
struct PointField {
    const SdfVolume::DistanceField& field;
    Vec3 origin{0.0f};
    Vec3 stepX{0.0f};
    Vec3 stepY{0.0f};
    Vec3 stepZ{0.0f};
    uint32_t count = 0;

    void BeginBox(const Vec3& boxOrigin, const Vec3& x, const Vec3& y, const Vec3& z,
                  uint32_t width, float) {
        origin = boxOrigin;
        stepX = x;
        stepY = y;
        stepZ = z;
        count = width;
    }

    void Row(uint32_t y, uint32_t z, float* out) const {
        const Vec3 base = origin + stepY * static_cast<float>(y) + stepZ * static_cast<float>(z);
        for (uint32_t i = 0; i < count; ++i) {
            out[i] = field(base + stepX * static_cast<float>(i));
        }
    }
};

}  // namespace

void SdfVolume::Fill(const SdfScrollResult& scroll, BoxSceneField& field) {
    for (const SdfScrollRegion& region : scroll.regions) {
        clipmap_.SplitWrapped(region, boxes_);
        for (const SdfClipmap::TexelBox& box : boxes_) {
            WriteBoxRows(region.cascade, box, field);
        }
    }
}

void SdfVolume::Fill(const SdfScrollResult& scroll, const DistanceField& field) {
    PointField walker{field};
    for (const SdfScrollRegion& region : scroll.regions) {
        clipmap_.SplitWrapped(region, boxes_);
        for (const SdfClipmap::TexelBox& box : boxes_) {
            WriteBoxRows(region.cascade, box, walker);
        }
    }
}

void SdfVolume::FillAll(BoxSceneField& field) {
    ForEachCascadeBox([&](uint32_t cascade, const SdfClipmap::TexelBox& box) {
        WriteBoxRows(cascade, box, field);
    });
}

void SdfVolume::FillAll(const DistanceField& field) {
    PointField walker{field};
    ForEachCascadeBox([&](uint32_t cascade, const SdfClipmap::TexelBox& box) {
        WriteBoxRows(cascade, box, walker);
    });
}

template <typename Visit>
void SdfVolume::ForEachCascadeBox(Visit&& visit) {
    const uint32_t resolution = clipmap_.Settings().resolution;
    for (uint32_t cascade = 0; cascade < clipmap_.CascadeCount(); ++cascade) {
        SdfScrollRegion region;
        region.cascade = cascade;
        region.min = clipmap_.VoxelOrigin(cascade);
        region.max = region.min + glm::ivec3(static_cast<int32_t>(resolution));
        clipmap_.SplitWrapped(region, boxes_);
        for (const SdfClipmap::TexelBox& box : boxes_) {
            visit(cascade, box);
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
