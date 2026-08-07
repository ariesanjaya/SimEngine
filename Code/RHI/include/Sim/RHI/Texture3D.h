#pragma once

#include "Sim/Core/Math.h"
#include "Sim/RHI/Device.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace sim::rhi {

/// Tekstur volume yang sebagian isinya ditulis ulang tiap frame.
///
/// Dibuat untuk kaskade SDF clipmap. **Yang membedakannya dari `Texture2D`
/// bukan dimensinya melainkan pola tulisnya:** clipmap memperbarui lempeng tepi
/// saja, jadi unggahan sub-wilayah adalah operasi utamanya, bukan kasus khusus.
/// Mengunggah ulang seluruh volume setiap kamera bergerak akan meniadakan
/// seluruh gunanya pengalamatan toroidal.
class Texture3D {
public:
    Texture3D() = default;
    ~Texture3D();

    Texture3D(const Texture3D&) = delete;
    Texture3D& operator=(const Texture3D&) = delete;

    bool Create(Device& device, const glm::uvec3& extent, VkFormat format,
                uint32_t bytesPerTexel);
    void Destroy();

    /// Menulis sebuah kotak sub-wilayah. `texels` berisi `extent` texel rapat,
    /// urutan X tercepat.
    ///
    /// Memblokir sampai selesai. Untuk clipmap itu justru yang diinginkan:
    /// isinya dibaca pass di frame yang sama, dan menunda unggahannya berarti
    /// satu frame membaca voxel milik posisi kamera yang lama.
    bool UploadRegion(const glm::uvec3& offset, const glm::uvec3& extent,
                      std::span<const std::byte> texels);

    VkImageView View() const { return view_; }
    VkSampler Sampler() const { return sampler_; }
    glm::uvec3 Extent() const { return extent_; }
    bool IsValid() const { return image_ != VK_NULL_HANDLE; }

private:
    void Transition(VkCommandBuffer cmd, VkImageLayout from, VkImageLayout to);

    Device* device_ = nullptr;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    glm::uvec3 extent_{0};
    uint32_t bytesPerTexel_ = 0;
};

}  // namespace sim::rhi
