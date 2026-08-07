#pragma once

#include "Sim/RHI/Device.h"

#include <cstddef>
#include <cstdint>

namespace sim::rhi {

/// Tekstur 2D yang isinya ditulis sekali dari CPU lalu hanya dibaca GPU.
///
/// Dipakai thumbnail Asset Browser. Memorinya device-local — bukan host-visible
/// seperti DynamicBuffer — karena setelah diunggah ia dibaca ratusan kali per
/// detik oleh shader UI dan tidak pernah ditulis lagi. Staging buffer sementara
/// dipakai untuk memindahkannya, lalu langsung dibuang.
class Texture2D {
public:
    Texture2D() = default;
    ~Texture2D();

    Texture2D(const Texture2D&) = delete;
    Texture2D& operator=(const Texture2D&) = delete;
    Texture2D(Texture2D&& other) noexcept;
    Texture2D& operator=(Texture2D&& other) noexcept;

    /// Membuat tekstur RGBA8 dan mengunggah `pixels` ke dalamnya.
    ///
    /// `pixels` harus berisi width * height * 4 byte. Unggahannya memblokir
    /// sampai selesai; pemanggil yang membuat banyak tekstur sekaligus wajib
    /// membatasi jumlahnya per frame.
    bool CreateFromRgba(Device& device, uint32_t width, uint32_t height, const void* pixels);

    /// Bentuk umumnya: format apa pun, satu mip, tanpa konversi.
    ///
    /// `bytesPerTexel` diberikan pemanggil, tidak disimpulkan dari `format`.
    /// Menyimpulkannya menuntut tabel format yang harus tumbuh setiap kali ada
    /// format baru, dan tabel yang ketinggalan satu baris menghasilkan unggahan
    /// yang meleset — bukan galat, melainkan gambar yang isinya bergeser.
    bool Create(Device& device, uint32_t width, uint32_t height, VkFormat format,
                uint32_t bytesPerTexel, const void* texels);

    void Destroy();

    VkImageView View() const { return view_; }
    VkSampler Sampler() const { return sampler_; }
    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }
    bool IsValid() const { return image_ != VK_NULL_HANDLE; }

private:
    Device* device_ = nullptr;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

}  // namespace sim::rhi
