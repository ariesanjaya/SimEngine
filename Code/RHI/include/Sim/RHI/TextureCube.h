#pragma once

#include "Sim/RHI/Device.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace sim::rhi {

/// Cubemap dengan rantai mip, isinya ditulis sekali dari CPU.
///
/// Dibuat untuk peta lingkungan prefilter IBL: mip ke-*n* menyimpan pantulan
/// pada kekasaran ke-*n*, sehingga shader memilih ketajaman pantulan dengan
/// memilih mip. Rantai mip di sini karena itu **bukan penyaringan biasa** — ia
/// tidak dihasilkan dengan mengecilkan mip sebelumnya, melainkan dibakar satu
/// per satu dengan kekasaran yang berbeda. Membangkitkannya lewat `vkCmdBlitImage`
/// akan menghasilkan gambar yang mirip tapi salah: buram yang rata, bukan buram
/// yang mengikuti lobe GGX.
class TextureCube {
public:
    TextureCube() = default;
    ~TextureCube();

    TextureCube(const TextureCube&) = delete;
    TextureCube& operator=(const TextureCube&) = delete;

    /// Membuat cubemap dan mengunggah seluruh isinya sekaligus.
    ///
    /// Tata letak `texels` yang diharapkan: untuk setiap mip berurutan, enam
    /// muka berurutan, masing-masing `size >> mip` kuadrat texel. Urutan mukanya
    /// urutan Vulkan: +X, −X, +Y, −Y, +Z, −Z.
    ///
    /// Satu panggilan untuk seluruhnya, bukan per muka: transisi layout dan
    /// staging buffer-nya lalu terjadi sekali, dan tidak ada keadaan setengah
    /// terunggah yang bisa dipakai orang.
    bool Create(Device& device, uint32_t size, uint32_t mipCount, VkFormat format,
                uint32_t bytesPerTexel, std::span<const std::byte> texels);

    void Destroy();

    VkImageView View() const { return view_; }
    VkSampler Sampler() const { return sampler_; }
    uint32_t Size() const { return size_; }
    uint32_t MipCount() const { return mipCount_; }
    bool IsValid() const { return image_ != VK_NULL_HANDLE; }

    /// Byte yang dibutuhkan `Create` untuk sebuah bentuk cubemap.
    static std::size_t TexelBytes(uint32_t size, uint32_t mipCount, uint32_t bytesPerTexel);

private:
    Device* device_ = nullptr;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    uint32_t size_ = 0;
    uint32_t mipCount_ = 0;
};

}  // namespace sim::rhi
