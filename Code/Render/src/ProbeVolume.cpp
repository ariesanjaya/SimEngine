#include "Sim/Render/ProbeVolume.h"

#include "Sim/Core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace sim::render {
namespace {

constexpr uint64_t kFnvOffset = 1469598103934665603ull;

/// Dinaikkan setiap kali arti isi berkasnya berubah. Ia bagian dari kunci, jadi
/// menaikkannya membuat artefak lama tidak pernah terbaca lagi alih-alih
/// terbaca salah.
constexpr uint32_t kProbeCacheVersion = 1;

constexpr char kMagic[4] = {'S', 'P', 'R', 'B'};

struct Header {
    char magic[4];
    uint32_t version;
    float originX;
    float originY;
    float originZ;
    float spacing;
    uint32_t countX;
    uint32_t countY;
    uint32_t countZ;
    uint32_t brickSize;
    uint64_t brickSlotCount;
    uint64_t probeCount;
};

uint64_t HashInto(uint64_t hash, const void* data, std::size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (std::size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

/// Slot sebuah probe di dalam `probes`, atau `kEmptyBrick` bila bricknya tidak
/// dialokasikan.
uint32_t ProbeSlot(const ProbeVolume& volume, const glm::uvec3& probe) {
    const uint32_t index = ProbeBrickIndex(volume.layout, probe);
    if (index >= volume.brickSlots.size()) {
        return kEmptyBrick;
    }
    const uint32_t slot = volume.brickSlots[index];
    if (slot == kEmptyBrick) {
        return kEmptyBrick;
    }
    return ProbeSlotOffset(slot, probe);
}

}  // namespace

uint32_t ProbeBrickIndex(const ProbeVolumeLayout& layout, const glm::uvec3& probe) {
    constexpr uint32_t kSide = ProbeVolumeLayout::kBrickSize;
    const glm::uvec3 bricks = layout.BrickCounts();
    const glm::uvec3 brick(probe.x / kSide, probe.y / kSide, probe.z / kSide);
    return (brick.z * bricks.y + brick.y) * bricks.x + brick.x;
}

uint32_t ProbeSlotOffset(uint32_t slot, const glm::uvec3& probe) {
    constexpr uint32_t kSide = ProbeVolumeLayout::kBrickSize;
    const glm::uvec3 local(probe.x % kSide, probe.y % kSide, probe.z % kSide);
    const uint32_t within = (local.z * kSide + local.y) * kSide + local.x;
    return slot * kSide * kSide * kSide + within;
}

void ProbeCell(const ProbeVolumeLayout& layout, const Vec3& position, glm::uvec3& outBase,
               Vec3& outFraction) {
    const Vec3 grid = (position - layout.origin) / std::max(layout.spacing, 1e-4f);
    for (int axis = 0; axis < 3; ++axis) {
        const auto last = static_cast<float>(layout.counts[axis] - 1);
        const float clamped = std::clamp(grid[axis], 0.0f, last);
        const float floored = std::floor(clamped);
        outBase[axis] = static_cast<uint32_t>(floored);
        outFraction[axis] = clamped - floored;
    }
}

glm::uvec3 ProbeCorner(const ProbeVolumeLayout& layout, const glm::uvec3& base, uint32_t corner) {
    const glm::uvec3 offset((corner & 1u) != 0u ? 1u : 0u, (corner & 2u) != 0u ? 1u : 0u,
                            (corner & 4u) != 0u ? 1u : 0u);
    glm::uvec3 probe = base + offset;
    for (int axis = 0; axis < 3; ++axis) {
        probe[axis] = std::min(probe[axis], layout.counts[axis] - 1);
    }
    return probe;
}

float ProbeCornerWeight(uint32_t corner, const Vec3& fraction) {
    return ((corner & 1u) != 0u ? fraction.x : 1.0f - fraction.x) *
           ((corner & 2u) != 0u ? fraction.y : 1.0f - fraction.y) *
           ((corner & 4u) != 0u ? fraction.z : 1.0f - fraction.z);
}

uint32_t ProbeVolume::AllocatedBrickCount() const {
    return static_cast<uint32_t>(
        std::count_if(brickSlots.begin(), brickSlots.end(),
                      [](uint32_t slot) { return slot != kEmptyBrick; }));
}

uint64_t ProbeVolume::StoredBytes() const {
    // Persis yang ditulis `WriteProbeVolume`: `Sh9` apa adanya, ditambah tabel
    // slot brick-nya. Menuliskannya sebagai rumus tersendiri di sini adalah
    // rumus kedua yang suatu saat tidak sepakat dengan berkasnya.
    return static_cast<uint64_t>(probes.size()) * sizeof(Sh9) +
           static_cast<uint64_t>(brickSlots.size()) * sizeof(uint32_t);
}

uint64_t ProbeVolume::GpuBytes() const {
    return static_cast<uint64_t>(probes.size()) * 9 * sizeof(Vec4) +
           static_cast<uint64_t>(brickSlots.size()) * sizeof(uint32_t);
}

std::vector<uint32_t> AssignProbeBricks(const ProbeVolumeLayout& layout,
                                        std::span<const ProbeOccupancy> volumes, float margin) {
    std::vector<uint32_t> slots;
    if (!layout.IsValid()) {
        return slots;
    }
    const uint32_t brickCount = layout.BrickCount();
    slots.assign(brickCount, kEmptyBrick);

    if (volumes.empty()) {
        // Pemanggil yang belum tahu di mana geometrinya mendapat kisi penuh.
        // Itu jawaban yang benar dan bukan penambal: kisi yang dikosongkan
        // karena daftar kosong adalah adegan yang gelap seluruhnya.
        for (uint32_t brick = 0; brick < brickCount; ++brick) {
            slots[brick] = brick;
        }
        return slots;
    }

    const glm::uvec3 bricks = layout.BrickCounts();
    constexpr uint32_t kSide = ProbeVolumeLayout::kBrickSize;
    const float brickExtent = static_cast<float>(kSide) * layout.spacing;

    uint32_t next = 0;
    for (const ProbeOccupancy& volume : volumes) {
        // Rentang brick yang disentuh kotak ini, dilebarkan `margin` ke segala
        // arah. Dihitung dari kotaknya, bukan dengan menelusuri seluruh brick
        // untuk setiap kotak: yang kedua adalah O(brick × instance), dan pada
        // adegan besar keduanya besar.
        const Vec3 low = volume.minimum - Vec3(margin) - layout.origin;
        const Vec3 high = volume.maximum + Vec3(margin) - layout.origin;
        glm::uvec3 first(0u);
        glm::uvec3 last(0u);
        bool outside = false;
        for (int axis = 0; axis < 3; ++axis) {
            const float lowBrick = std::floor(low[axis] / brickExtent);
            const float highBrick = std::floor(high[axis] / brickExtent);
            if (highBrick < 0.0f || lowBrick >= static_cast<float>(bricks[axis])) {
                outside = true;
                break;
            }
            first[axis] = static_cast<uint32_t>(std::max(lowBrick, 0.0f));
            last[axis] = static_cast<uint32_t>(
                std::min(highBrick, static_cast<float>(bricks[axis]) - 1.0f));
        }
        if (outside) {
            continue;
        }

        for (uint32_t z = first.z; z <= last.z; ++z) {
            for (uint32_t y = first.y; y <= last.y; ++y) {
                for (uint32_t x = first.x; x <= last.x; ++x) {
                    const uint32_t index = (z * bricks.y + y) * bricks.x + x;
                    if (slots[index] == kEmptyBrick) {
                        slots[index] = next++;
                    }
                }
            }
        }
    }
    return slots;
}

ProbeVolumeLayout MakeProbeLayout(const Vec3& boundsMin, const Vec3& boundsMax, float spacing) {
    ProbeVolumeLayout layout;
    layout.spacing = std::max(spacing, 1e-3f);

    // **Batasnya dilebarkan ke kelipatan jaraknya, bukan dipakai apa adanya.**
    // Sebuah permukaan yang duduk tepat di batas akan mengambil sampel di luar
    // kisi dan mendapat nol — dan nol di tepi adegan terbaca sebagai garis
    // gelap yang mengikuti dindingnya, bukan sebagai kisi yang kurang satu
    // baris.
    const Vec3 extent = glm::max(boundsMax - boundsMin, Vec3(0.0f));
    for (int axis = 0; axis < 3; ++axis) {
        const auto span = static_cast<uint32_t>(std::floor(extent[axis] / layout.spacing)) + 2u;
        layout.counts[axis] = span;
    }
    // Setengah sisa dibagi ke kedua sisi supaya kisinya terpusat pada isinya.
    const Vec3 covered = Vec3(layout.counts - glm::uvec3(1u)) * layout.spacing;
    layout.origin = boundsMin - (covered - extent) * 0.5f;
    return layout;
}

ProbeVolume BakeProbeVolumeFromEnvironment(const ProbeVolumeLayout& layout,
                                           const IEnvironmentSampler& environment,
                                           uint32_t sampleCount) {
    ProbeVolume volume;
    if (!layout.IsValid()) {
        return volume;
    }
    volume.layout = layout;

    // **Dihitung sekali, lalu disalin ke seluruh probe.** Tanpa geometri tidak
    // ada yang bisa membedakan satu titik dari titik lain, jadi memanggilnya
    // per-probe hanya menghasilkan angka yang sama beberapa puluh ribu kali.
    // Yang menggantikannya dengan penelusuran per-probe adalah S2.
    const Sh9 uniform = ProjectIrradiance(environment, sampleCount);

    const uint32_t bricks = layout.BrickCount();
    volume.brickSlots.assign(bricks, kEmptyBrick);
    for (uint32_t brick = 0; brick < bricks; ++brick) {
        volume.brickSlots[brick] = brick;  // S1 mengisi seluruhnya
    }
    volume.probes.assign(layout.FullProbeCount(), uniform);
    return volume;
}

Sh9 SampleProbeVolume(const ProbeVolume& volume, const Vec3& position) {
    Sh9 result;
    if (!volume.IsValid() || volume.probes.empty()) {
        return result;
    }
    const ProbeVolumeLayout& layout = volume.layout;

    // Koordinat kisi, dijepit ke tepinya: yang di luar kisi memakai probe
    // terdekat alih-alih nol, karena nol di sana adalah lubang gelap yang
    // bentuknya mengikuti kotak batas.
    Vec3 grid = (position - layout.origin) / layout.spacing;
    glm::uvec3 base(0u);
    Vec3 fraction(0.0f);
    for (int axis = 0; axis < 3; ++axis) {
        const auto last = static_cast<float>(layout.counts[axis] - 1);
        const float clamped = std::clamp(grid[axis], 0.0f, last);
        const float floored = std::floor(clamped);
        base[axis] = static_cast<uint32_t>(floored);
        if (base[axis] + 1 >= layout.counts[axis] && layout.counts[axis] > 1) {
            base[axis] = layout.counts[axis] - 2;
            fraction[axis] = 1.0f;
        } else {
            fraction[axis] = clamped - floored;
        }
    }

    float totalWeight = 0.0f;
    for (uint32_t corner = 0; corner < 8; ++corner) {
        const glm::uvec3 offset((corner & 1u) != 0u ? 1u : 0u, (corner & 2u) != 0u ? 1u : 0u,
                                (corner & 4u) != 0u ? 1u : 0u);
        glm::uvec3 probe = base + offset;
        for (int axis = 0; axis < 3; ++axis) {
            probe[axis] = std::min(probe[axis], layout.counts[axis] - 1);
        }
        const uint32_t slot = ProbeSlot(volume, probe);
        if (slot == kEmptyBrick || slot >= volume.probes.size()) {
            continue;
        }
        const float weight = (offset.x != 0u ? fraction.x : 1.0f - fraction.x) *
                             (offset.y != 0u ? fraction.y : 1.0f - fraction.y) *
                             (offset.z != 0u ? fraction.z : 1.0f - fraction.z);
        if (weight <= 0.0f) {
            continue;
        }
        const Sh9& probeSh = volume.probes[slot];
        for (std::size_t i = 0; i < result.coefficients.size(); ++i) {
            result.coefficients[i] += probeSh.coefficients[i] * weight;
        }
        totalWeight += weight;
    }
    // **Dinormalkan terhadap bobot yang benar-benar terpakai.** Begitu S2
    // membuang brick, sebagian sudut bisa hilang — dan menjumlahkan tujuh dari
    // delapan sudut tanpa menormalkannya menggelapkan tepat di dekat brick yang
    // kosong, yaitu di dekat dinding.
    if (totalWeight > 1e-6f && std::abs(totalWeight - 1.0f) > 1e-6f) {
        for (Vec3& coefficient : result.coefficients) {
            coefficient /= totalWeight;
        }
    }
    return result;
}

Vec2 ProbeOctEncode(const Vec3& direction) {
    const float norm = std::abs(direction.x) + std::abs(direction.y) + std::abs(direction.z);
    const Vec3 d = direction / std::max(norm, 1e-20f);
    Vec2 uv(d.x, d.z);
    if (d.y < 0.0f) {
        // Nol diperlakukan sebagai positif: `sign` pada nol menjawab nol, dan
        // texel di sumbu lalu jatuh ke tempat yang salah.
        const Vec2 sign(uv.x >= 0.0f ? 1.0f : -1.0f, uv.y >= 0.0f ? 1.0f : -1.0f);
        uv = Vec2(1.0f - std::abs(d.z), 1.0f - std::abs(d.x)) * sign;
    }
    return uv * 0.5f + 0.5f;
}

Vec3 ProbeOctDecode(const Vec2& uv) {
    const Vec2 f = uv * 2.0f - 1.0f;
    Vec3 d(f.x, 1.0f - std::abs(f.x) - std::abs(f.y), f.y);
    const float t = std::max(-d.y, 0.0f);
    d.x += d.x >= 0.0f ? -t : t;
    d.z += d.z >= 0.0f ? -t : t;
    return glm::normalize(d);
}

uint32_t ProbeDepthTexel(const Vec3& direction) {
    constexpr uint32_t kSide = ProbeVolume::kDepthSize;
    const Vec2 uv = ProbeOctEncode(direction);
    const auto x = std::min(static_cast<uint32_t>(uv.x * static_cast<float>(kSide)), kSide - 1);
    const auto y = std::min(static_cast<uint32_t>(uv.y * static_cast<float>(kSide)), kSide - 1);
    return y * kSide + x;
}

float ProbeVisibilityWeight(float distance, float mean, float meanSquare) {
    if (distance <= mean) {
        return 1.0f;
    }
    // Varians dijaga tidak nol; alasannya di `probe_grid.slang`.
    const float variance = std::max(meanSquare - mean * mean, 1e-4f);
    const float delta = distance - mean;
    const float chebyshev = variance / (variance + delta * delta);
    return chebyshev * chebyshev * chebyshev;
}

Sh9 SampleProbeVolumeAt(const ProbeVolume& volume, const Vec3& position) {
    if (!volume.HasVisibility()) {
        // Kisi tanpa visibilitas menjawab persis seperti sebelum S3. Itu bukan
        // jalur mundur yang menutupi sesuatu: artefak yang dipanggang sebelum S3
        // memang tidak memuat jawabannya, dan menebaknya lebih buruk daripada
        // menjawab apa adanya.
        return SampleProbeVolume(volume, position);
    }
    Sh9 result;
    const ProbeVolumeLayout& layout = volume.layout;
    if (!volume.IsValid() || volume.probes.empty()) {
        return result;
    }

    glm::uvec3 base(0u);
    Vec3 fraction(0.0f);
    ProbeCell(layout, position, base, fraction);

    float totalWeight = 0.0f;
    for (uint32_t corner = 0; corner < 8; ++corner) {
        const float trilinear = ProbeCornerWeight(corner, fraction);
        if (trilinear <= 0.0f) {
            continue;
        }
        const glm::uvec3 probe = ProbeCorner(layout, base, corner);
        const uint32_t index = ProbeBrickIndex(layout, probe);
        if (index >= volume.brickSlots.size()) {
            continue;
        }
        const uint32_t brickSlot = volume.brickSlots[index];
        if (brickSlot == kEmptyBrick) {
            continue;
        }
        const uint32_t slot = ProbeSlotOffset(brickSlot, probe);
        if (slot >= volume.probes.size()) {
            continue;
        }

        // **Arah dari probe ke titiknya, dan jaraknya.** Itulah yang dijawab
        // peta kedalaman: seberapa jauh geometri terdekat pada arah itu.
        const Vec3 toPoint = position - layout.ProbePosition(probe);
        const float distance = glm::length(toPoint);
        float visibility = 1.0f;
        if (distance > 1e-5f) {
            const uint32_t texel = ProbeDepthTexel(toPoint / distance);
            const std::size_t at =
                static_cast<std::size_t>(slot) * ProbeVolume::kDepthFloats + texel * 2;
            visibility = ProbeVisibilityWeight(distance, volume.depth[at], volume.depth[at + 1]);
        }

        const float weight = trilinear * visibility;
        if (weight <= 0.0f) {
            continue;
        }
        const Sh9& probeSh = volume.probes[slot];
        for (std::size_t i = 0; i < result.coefficients.size(); ++i) {
            result.coefficients[i] += probeSh.coefficients[i] * weight;
        }
        totalWeight += weight;
    }

    // **Dinormalkan terhadap bobot yang benar-benar terpakai**, alasan yang sama
    // dengan `SampleProbeVolume` — dan di sini ia lebih penting lagi: bobot
    // visibilitas membuang sudut secara rutin, bukan sesekali.
    if (totalWeight > 1e-6f) {
        for (Vec3& coefficient : result.coefficients) {
            coefficient /= totalWeight;
        }
    }
    return result;
}

uint64_t ProbeVolumeCacheKey(const ProbeVolumeLayout& layout, uint64_t environmentKey) {
    uint64_t hash = HashInto(kFnvOffset, &environmentKey, sizeof(environmentKey));
    hash = HashInto(hash, &layout.origin.x, sizeof(float) * 3);
    hash = HashInto(hash, &layout.spacing, sizeof(layout.spacing));
    hash = HashInto(hash, &layout.counts.x, sizeof(uint32_t) * 3);
    const uint32_t brickSize = ProbeVolumeLayout::kBrickSize;
    hash = HashInto(hash, &brickSize, sizeof(brickSize));
    hash = HashInto(hash, &kProbeCacheVersion, sizeof(kProbeCacheVersion));
    return hash;
}

std::filesystem::path ProbeVolumeCachePath(const std::filesystem::path& cacheDir, uint64_t key) {
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.simprobe", static_cast<unsigned long long>(key));
    return cacheDir / name;
}

bool WriteProbeVolume(const std::filesystem::path& file, const ProbeVolume& volume,
                      std::string& error) {
    if (!volume.IsValid()) {
        error = "nothing baked to write";
        return false;
    }
    std::error_code code;
    if (!file.parent_path().empty()) {
        std::filesystem::create_directories(file.parent_path(), code);
    }

    const std::filesystem::path temporary = file.string() + ".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "cannot open " + temporary.string() + " for writing";
        return false;
    }

    Header header{};
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.version = kProbeCacheVersion;
    header.originX = volume.layout.origin.x;
    header.originY = volume.layout.origin.y;
    header.originZ = volume.layout.origin.z;
    header.spacing = volume.layout.spacing;
    header.countX = volume.layout.counts.x;
    header.countY = volume.layout.counts.y;
    header.countZ = volume.layout.counts.z;
    header.brickSize = ProbeVolumeLayout::kBrickSize;
    header.brickSlotCount = volume.brickSlots.size();
    header.probeCount = volume.probes.size();

    stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
    stream.write(reinterpret_cast<const char*>(volume.brickSlots.data()),
                 static_cast<std::streamsize>(sizeof(uint32_t) * volume.brickSlots.size()));
    stream.write(reinterpret_cast<const char*>(volume.probes.data()),
                 static_cast<std::streamsize>(sizeof(Sh9) * volume.probes.size()));
    stream.close();
    if (!stream) {
        error = "write failed for " + temporary.string();
        std::filesystem::remove(temporary, code);
        return false;
    }

    std::filesystem::rename(temporary, file, code);
    if (code) {
        error = "cannot move " + temporary.string() + " into place: " + code.message();
        std::filesystem::remove(temporary, code);
        return false;
    }
    return true;
}

bool ReadProbeVolume(const std::filesystem::path& file, ProbeVolume& out, std::string& error) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        error = "no cached probe volume at " + file.string();
        return false;
    }

    Header header{};
    stream.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!stream || std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0) {
        error = file.string() + " is not a baked probe volume";
        return false;
    }
    if (header.version != kProbeCacheVersion) {
        error = "cached probe volume was written by another baker version";
        return false;
    }
    if (header.brickSize != ProbeVolumeLayout::kBrickSize) {
        error = "cached probe volume uses a different brick size";
        return false;
    }

    ProbeVolume volume;
    volume.layout.origin = Vec3(header.originX, header.originY, header.originZ);
    volume.layout.spacing = header.spacing;
    volume.layout.counts = glm::uvec3(header.countX, header.countY, header.countZ);
    if (!volume.layout.IsValid()) {
        error = "cached probe volume has an implausible shape";
        return false;
    }

    // **Yang disebut header harus cocok dengan bentuk yang disebutnya**, dan itu
    // diperiksa sebelum satu byte pun dialokasikan: `probeCount` datang dari
    // berkas, dan mengalokasikan sebanyak yang disebutnya berarti berkas rusak
    // bisa meminta puluhan gigabyte.
    if (header.brickSlotCount != volume.layout.BrickCount()) {
        error = "cached probe volume says " + std::to_string(header.brickSlotCount) +
                " bricks but its shape needs " + std::to_string(volume.layout.BrickCount());
        return false;
    }
    constexpr uint64_t kMaxProbes = 64ull * 1024 * 1024;
    if (header.probeCount == 0 || header.probeCount > kMaxProbes ||
        header.probeCount > volume.layout.FullProbeCount()) {
        error = "cached probe volume has an implausible probe count";
        return false;
    }

    volume.brickSlots.resize(static_cast<std::size_t>(header.brickSlotCount));
    stream.read(reinterpret_cast<char*>(volume.brickSlots.data()),
                static_cast<std::streamsize>(sizeof(uint32_t) * volume.brickSlots.size()));
    volume.probes.resize(static_cast<std::size_t>(header.probeCount));
    stream.read(reinterpret_cast<char*>(volume.probes.data()),
                static_cast<std::streamsize>(sizeof(Sh9) * volume.probes.size()));
    if (!stream) {
        error = "cached probe volume is shorter than its header promises";
        return false;
    }

    // Slot yang menunjuk ke luar daftar probe adalah berkas yang rusak, dan
    // membacanya nanti berarti membaca melewati ujung buffer.
    const uint64_t probesPerBrick = ProbeVolumeLayout::kBrickSize * ProbeVolumeLayout::kBrickSize *
                                    ProbeVolumeLayout::kBrickSize;
    for (const uint32_t slot : volume.brickSlots) {
        if (slot == kEmptyBrick) {
            continue;
        }
        if ((static_cast<uint64_t>(slot) + 1) * probesPerBrick > header.probeCount) {
            error = "cached probe volume has a brick slot outside its probe list";
            return false;
        }
    }

    out = std::move(volume);
    return true;
}

}  // namespace sim::render
