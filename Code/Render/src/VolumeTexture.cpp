#include "Sim/Render/VolumeTexture.h"

#include <algorithm>
#include <cmath>

namespace sim::render {
namespace {

/// Kuantisasi ke bilangan bulat tanpa tanda, dengan pembulatan — bukan
/// pemotongan. Pemotongan menggeser seluruh volume setengah tingkat ke bawah,
/// dan pada kabut tipis pergeseran itu adalah selisih antara terlihat dan
/// hilang sama sekali.
uint32_t Quantise(float unit, uint32_t maxValue) {
    const float clamped = std::clamp(unit, 0.0f, 1.0f);
    return static_cast<uint32_t>(std::lround(clamped * static_cast<float>(maxValue)));
}

}  // namespace

bool EncodeVolume(const VolumeGrid& grid, VolumeTextureFormat format,
                  std::vector<std::byte>& outBytes, VolumeTextureDesc& outDesc) {
    outBytes.clear();
    outDesc = VolumeTextureDesc{};
    if (grid.Empty() || grid.sizeX == 0 || grid.sizeY == 0 || grid.sizeZ == 0) {
        return false;
    }

    outDesc.format = format;
    outDesc.sizeX = grid.sizeX;
    outDesc.sizeY = grid.sizeY;
    outDesc.sizeZ = grid.sizeZ;
    outDesc.origin = grid.origin;
    outDesc.voxelSize = grid.voxelSize;

    const float low = grid.minValue;
    const float high = grid.maxValue;
    const float range = high - low;
    if (!(range > 0.0f)) {
        // Grid yang seluruhnya bernilai sama. Disandikan nol dengan skala nol,
        // jadi dekodenya menghasilkan `bias` di mana pun — alih-alih membagi
        // dengan rentang nol dan menyebarkan bukan-angka ke seluruh gambar.
        outDesc.scale = 0.0f;
        outDesc.bias = low;
        outBytes.assign(outDesc.ByteCount(), std::byte{0});
        return true;
    }

    const uint32_t maxTexel = format == VolumeTextureFormat::R16Unorm ? 65535u : 255u;
    outDesc.scale = range;
    outDesc.bias = low;
    outBytes.resize(outDesc.ByteCount());

    for (std::size_t i = 0; i < grid.values.size(); ++i) {
        const uint32_t quantised = Quantise((grid.values[i] - low) / range, maxTexel);
        if (format == VolumeTextureFormat::R16Unorm) {
            // Little-endian eksplisit, bukan tata letak memori mesin ini:
            // texel 16-bit dibaca GPU menurut format Vulkan-nya, dan
            // menuliskan endianness host membuatnya hanya benar di arsitektur
            // tempat ia dibuat.
            outBytes[i * 2] = static_cast<std::byte>(quantised & 0xffu);
            outBytes[i * 2 + 1] = static_cast<std::byte>((quantised >> 8) & 0xffu);
        } else {
            outBytes[i] = static_cast<std::byte>(quantised);
        }
    }
    return true;
}

float DecodeTexel(const VolumeTextureDesc& desc, std::span<const std::byte> bytes, uint32_t x,
                  uint32_t y, uint32_t z) {
    if (x >= desc.sizeX || y >= desc.sizeY || z >= desc.sizeZ) {
        return desc.bias;
    }
    const std::size_t index =
        (static_cast<std::size_t>(z) * desc.sizeY + static_cast<std::size_t>(y)) * desc.sizeX +
        static_cast<std::size_t>(x);

    float unit = 0.0f;
    if (desc.format == VolumeTextureFormat::R16Unorm) {
        if ((index * 2 + 1) >= bytes.size()) {
            return desc.bias;
        }
        const auto low = static_cast<uint32_t>(bytes[index * 2]);
        const auto high = static_cast<uint32_t>(bytes[index * 2 + 1]);
        unit = static_cast<float>(low | (high << 8)) / 65535.0f;
    } else {
        if (index >= bytes.size()) {
            return desc.bias;
        }
        unit = static_cast<float>(static_cast<uint32_t>(bytes[index])) / 255.0f;
    }
    return unit * desc.scale + desc.bias;
}

}  // namespace sim::render
