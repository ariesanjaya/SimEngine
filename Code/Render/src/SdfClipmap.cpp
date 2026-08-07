#include "Sim/Render/SdfClipmap.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace sim::render {
namespace {

/// Sisa yang selalu tidak negatif.
int32_t FloorMod(int32_t value, int32_t divisor) {
    const int32_t remainder = value % divisor;
    return remainder < 0 ? remainder + divisor : remainder;
}

}  // namespace

void SdfClipmap::Configure(const SdfClipmapSettings& settings) {
    settings_ = settings;
    settings_.resolution = std::max(settings.resolution, 2u);
    settings_.cascadeCount = std::clamp(settings.cascadeCount, 1u, kMaxSdfCascades);
    settings_.finestVoxelSize = std::max(settings.finestVoxelSize, 1e-3f);
    // Dibulatkan ke pangkat dua terdekat ke bawah, minimal dua. Pengali yang
    // bukan pangkat dua membuat kisi kaskade kasar tidak selaras dengan yang
    // halus, dan jahitannya bergerak saat kamera maju.
    uint32_t scale = 2;
    while (scale * 2 <= std::max(settings.voxelScale, 2u)) {
        scale *= 2;
    }
    settings_.voxelScale = scale;
    settings_.bandVoxels = std::max(settings.bandVoxels, 1.0f);
    origins_ = {};
    placed_ = false;
}

float SdfClipmap::VoxelSize(uint32_t cascade) const {
    float size = settings_.finestVoxelSize;
    for (uint32_t i = 0; i < cascade; ++i) {
        size *= static_cast<float>(settings_.voxelScale);
    }
    return size;
}

float SdfClipmap::HalfExtent(uint32_t cascade) const {
    return VoxelSize(cascade) * static_cast<float>(settings_.resolution) * 0.5f;
}

float SdfClipmap::MaxRange() const {
    return HalfExtent(settings_.cascadeCount - 1);
}

Vec3 SdfClipmap::WorldOrigin(uint32_t cascade) const {
    const float size = VoxelSize(cascade);
    const glm::ivec3 origin = origins_[cascade];
    return Vec3(static_cast<float>(origin.x), static_cast<float>(origin.y),
                static_cast<float>(origin.z)) *
           size;
}

int SdfClipmap::CascadeFor(const Vec3& worldPosition) const {
    for (uint32_t cascade = 0; cascade < settings_.cascadeCount; ++cascade) {
        const Vec3 origin = WorldOrigin(cascade);
        const float span = VoxelSize(cascade) * static_cast<float>(settings_.resolution);
        const Vec3 local = worldPosition - origin;
        // Sedikit di dalam tepinya, bukan tepat di tepinya: sampel trilinear
        // membaca voxel tetangga, dan tetangga di tepi kaskade tidak ada.
        const float margin = VoxelSize(cascade);
        if (local.x >= margin && local.y >= margin && local.z >= margin &&
            local.x <= span - margin && local.y <= span - margin && local.z <= span - margin) {
            return static_cast<int>(cascade);
        }
    }
    return -1;
}

uint32_t SdfClipmap::WrapAxis(int32_t coordinate) const {
    return static_cast<uint32_t>(FloorMod(coordinate, static_cast<int32_t>(settings_.resolution)));
}

glm::uvec3 SdfClipmap::TexelOf(uint32_t, const glm::ivec3& worldVoxel) const {
    return {WrapAxis(worldVoxel.x), WrapAxis(worldVoxel.y), WrapAxis(worldVoxel.z)};
}

glm::ivec3 SdfClipmap::VoxelOf(uint32_t cascade, const Vec3& worldPosition) const {
    const float size = VoxelSize(cascade);
    // `std::floor`, bukan pemotongan ke arah nol. Konversi float→int C++
    // memotong ke arah nol, jadi -0,05 m akan jatuh ke voxel 0 — voxel yang sama
    // dengan +0,05 m. Kedua sisi titik nol dunia lalu berbagi satu voxel, dan
    // kesalahannya baru muncul setelah kamera melewati origin.
    return {static_cast<int32_t>(std::floor(worldPosition.x / size)),
            static_cast<int32_t>(std::floor(worldPosition.y / size)),
            static_cast<int32_t>(std::floor(worldPosition.z / size))};
}

SdfScrollResult SdfClipmap::Scroll(const Vec3& cameraPosition) {
    SdfScrollResult result;
    const auto resolution = static_cast<int32_t>(settings_.resolution);

    for (uint32_t cascade = 0; cascade < settings_.cascadeCount; ++cascade) {
        // Titik asal dikancing ke kelipatan ukuran voxel kaskade INI. Mengancing
        // ke voxel terhalus untuk semua kaskade akan membuat kaskade kasar
        // bergeser pecahan voxelnya sendiri — dan seluruh isinya jadi basi
        // setiap frame.
        const glm::ivec3 centre = VoxelOf(cascade, cameraPosition);
        const glm::ivec3 wanted = centre - glm::ivec3(resolution / 2);
        const glm::ivec3 previous = origins_[cascade];
        const glm::ivec3 delta = wanted - previous;

        if (!placed_ || std::abs(delta.x) >= resolution || std::abs(delta.y) >= resolution ||
            std::abs(delta.z) >= resolution) {
            // Bergeser lebih jauh daripada lebarnya sendiri: tidak ada satu pun
            // voxel lama yang masih berlaku. Menuliskan tiga lempeng yang
            // saling tumpang tindih penuh justru lebih mahal daripada menulis
            // volumenya sekali.
            origins_[cascade] = wanted;
            SdfScrollRegion region;
            region.cascade = cascade;
            region.min = wanted;
            region.max = wanted + glm::ivec3(resolution);
            result.regions.push_back(region);
            result.fullRewrite = true;
            continue;
        }
        if (delta == glm::ivec3(0)) {
            continue;
        }

        origins_[cascade] = wanted;

        // Satu lempeng per sumbu yang bergerak. Lempeng kedua dan ketiga
        // dipotong terhadap yang sebelumnya, supaya voxel di sudut tidak
        // ditulis dua kali — bukan demi kebenaran, melainkan supaya jumlah voxel
        // yang dilaporkan benar-benar jumlah pekerjaan.
        glm::ivec3 remainingMin = wanted;
        glm::ivec3 remainingMax = wanted + glm::ivec3(resolution);
        for (int axis = 0; axis < 3; ++axis) {
            const int32_t step = delta[axis];
            if (step == 0) {
                continue;
            }
            SdfScrollRegion region;
            region.cascade = cascade;
            region.min = remainingMin;
            region.max = remainingMax;
            if (step > 0) {
                // Maju: lempeng baru muncul di sisi jauh.
                region.min[axis] = remainingMax[axis] - std::min(step, resolution);
                remainingMax[axis] = region.min[axis];
            } else {
                region.max[axis] = remainingMin[axis] + std::min(-step, resolution);
                remainingMin[axis] = region.max[axis];
            }
            if (region.VoxelCount() > 0) {
                result.regions.push_back(region);
            }
        }
    }
    placed_ = true;
    return result;
}

void SdfClipmap::SplitWrapped(const SdfScrollRegion& region, std::vector<TexelBox>& out) const {
    out.clear();
    const auto resolution = static_cast<int32_t>(settings_.resolution);

    // Tiap sumbu dipecah lebih dulu menjadi potongan yang tidak membungkus,
    // lalu ketiganya disilangkan. Memecah langsung di ruang tiga dimensi
    // menuntut delapan cabang yang ditulis tangan; memecah per sumbu lalu
    // menyilangkannya menghasilkan hal yang sama dengan satu aturan saja.
    std::array<std::vector<std::pair<int32_t, int32_t>>, 3> spans;
    for (int axis = 0; axis < 3; ++axis) {
        int32_t from = region.min[axis];
        // Selebar teksturnya atau lebih dijepit, bukan dicabangkan. Bentuk
        // pertama saya memberikan potongan tunggal sepanjang resolusi penuh —
        // dan potongan itu, begitu dibungkus, berakhir di luar tekstur:
        // texel 32 ditambah 64 adalah 96 pada tekstur selebar 64. Segfault,
        // bukan gambar yang salah. Lingkaran di bawah sudah menangani kasusnya
        // dengan benar tanpa cabang apa pun.
        const int32_t to = std::min(region.max[axis], from + resolution);
        while (from < to) {
            const int32_t offset = FloorMod(from, resolution);
            const int32_t chunk = std::min(to - from, resolution - offset);
            spans[static_cast<size_t>(axis)].emplace_back(from, from + chunk);
            from += chunk;
        }
    }

    for (const auto& x : spans[0]) {
        for (const auto& y : spans[1]) {
            for (const auto& z : spans[2]) {
                TexelBox box;
                box.worldMin = {x.first, y.first, z.first};
                box.min = {WrapAxis(x.first), WrapAxis(y.first), WrapAxis(z.first)};
                box.max = {box.min.x + static_cast<uint32_t>(x.second - x.first),
                           box.min.y + static_cast<uint32_t>(y.second - y.first),
                           box.min.z + static_cast<uint32_t>(z.second - z.first)};
                if (box.VoxelCount() > 0) {
                    out.push_back(box);
                }
            }
        }
    }
}

float SdfClipmap::BandRadius(uint32_t cascade) const {
    return VoxelSize(cascade) * settings_.bandVoxels;
}

float SdfClipmap::EncodeDistance(uint32_t cascade, float distance) const {
    const float band = BandRadius(cascade);
    // Dipetakan ke 0..1 dengan nol jarak di tengah, supaya jarak bertanda —
    // yang di dalam permukaan negatif — ikut terwakili. Menyimpan jarak tak
    // bertanda membuat sphere tracing tidak bisa tahu ia sudah di dalam benda,
    // dan ray yang mulai di dalam dinding tidak pernah keluar.
    return std::clamp(distance / (band * 2.0f) + 0.5f, 0.0f, 1.0f);
}

float SdfClipmap::DecodeDistance(uint32_t cascade, float encoded) const {
    const float band = BandRadius(cascade);
    return (std::clamp(encoded, 0.0f, 1.0f) - 0.5f) * band * 2.0f;
}

}  // namespace sim::render
