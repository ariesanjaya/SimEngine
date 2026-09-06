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
    ///
    /// `storage` menambahkan `VK_IMAGE_USAGE_STORAGE_BIT`, yang dituntut
    /// pemanggang yang menulis sebagian mip lewat compute alih-alih
    /// mengunggahnya — lihat `render::IblPrefilter`. **Mati secara bawaan dan
    /// memang harus begitu**: usage yang diminta tanpa ada yang memakainya
    /// menutup pilihan tata letak driver untuk setiap cubemap di mesin ini,
    /// termasuk yang isinya tidak pernah berubah lagi sesudah diunggah.
    ///
    /// Yang menyalakannya tetap mengunggah seluruh rantai: mip yang akan
    /// ditimpa compute diunggah sebagai nol. Membiarkannya tak terunggah
    /// berarti mip itu berisi apa pun yang kebetulan ada di memori sampai
    /// dispatch-nya berjalan — dan dispatch yang gagal lalu menghasilkan
    /// pantulan berkilat-kilat alih-alih pantulan hitam yang jelas salah.
    bool Create(Device& device, uint32_t size, uint32_t mipCount, VkFormat format,
                uint32_t bytesPerTexel, std::span<const std::byte> texels,
                bool storage = false);

    void Destroy();

    VkImageView View() const { return view_; }
    VkSampler Sampler() const { return sampler_; }
    uint32_t Size() const { return size_; }
    uint32_t MipCount() const { return mipCount_; }
    bool IsValid() const { return image_ != VK_NULL_HANDLE; }

    /// Image-nya sendiri, untuk yang perlu membuat view sendiri di atasnya —
    /// satu view per mip, yang tidak bisa diberikan `View()` karena view itu
    /// mencakup seluruh rantai.
    VkImage Image() const { return image_; }
    /// Format yang dipakai `Create`. Dibutuhkan yang membuat view sendiri:
    /// view dengan format lain atas image yang sama sah menurut Vulkan hanya
    /// dalam keadaan yang tidak berlaku di sini.
    VkFormat Format() const { return format_; }
    /// Dibuat dengan `VK_IMAGE_USAGE_STORAGE_BIT`.
    bool IsStorage() const { return storage_; }

    /// Byte yang dibutuhkan `Create` untuk sebuah bentuk cubemap.
    static std::size_t TexelBytes(uint32_t size, uint32_t mipCount, uint32_t bytesPerTexel);

private:
    Device* device_ = nullptr;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    uint32_t size_ = 0;
    uint32_t mipCount_ = 0;
    bool storage_ = false;
};

}  // namespace sim::rhi
