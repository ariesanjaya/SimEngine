#pragma once

#include "Sim/Core/Math.h"
#include "Sim/RHI/Device.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

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

    /// `storage` menambahkan `VK_IMAGE_USAGE_STORAGE_BIT`, untuk tekstur yang
    /// diisi compute alih-alih diunggah lewat staging.
    ///
    /// **Diminta, bukan dinyalakan diam-diam.** Usage storage bisa mematikan
    /// kompresi tekstur pada sebagian perangkat, dan yang membayarnya adalah
    /// setiap pembacaan sesudahnya — bukan hanya penulisannya. Pemanggil yang
    /// tidak membutuhkannya tidak boleh ikut membayar.
    bool Create(Device& device, const glm::uvec3& extent, VkFormat format,
                uint32_t bytesPerTexel, bool storage = false);
    void Destroy();

    /// Apakah `format` bisa dipakai sebagai storage image di perangkat ini.
    ///
    /// **Ditanya, bukan diandaikan.** `VK_FORMAT_R8_UNORM` — format kaskade SDF
    /// — bukan format storage yang diwajibkan spesifikasi Vulkan. Ia ada di
    /// hampir setiap perangkat desktop, dan "hampir" adalah alasan pertanyaan
    /// ini ada: yang memakainya harus punya jalur mundur, bukan harapan.
    static bool SupportsStorage(const Device& device, VkFormat format);

    /// Menyalin seluruh isi tekstur ke memori host.
    ///
    /// **Menyubmit sendiri lalu menunggu queue idle**, jadi ia hanya untuk alat
    /// verifikasi — bukan untuk jalur frame. Yang membutuhkannya: membandingkan
    /// isi voxel yang dihasilkan jalur CPU dengan yang dihasilkan jalur compute,
    /// byte demi byte. Perbandingan gambar tidak bisa menjawab pertanyaan itu,
    /// karena satu-satunya pembaca kaskade SDF adalah penelusuran GI — dan GI
    /// tidak deterministik.
    bool Readback(std::vector<uint8_t>& out) const;

    /// Menulis sebuah kotak sub-wilayah. `texels` berisi `extent` texel rapat,
    /// urutan X tercepat.
    ///
    /// **Menyubmit sendiri lalu menunggu queue idle.** Bentuk itu benar untuk
    /// pengisian sekali di waktu setup dan salah untuk apa pun yang berulang
    /// tiap frame — pemanggil per-frame memakai `RecordRegionCopy` dan merekam
    /// ke command buffer frame.
    bool UploadRegion(const glm::uvec3& offset, const glm::uvec3& extent,
                      std::span<const std::byte> texels);

    /// Merekam salinan sebuah sub-wilayah dari buffer staging milik pemanggil.
    ///
    /// Pemanggil bertanggung jawab atas barrier di kedua ujung rangkaian
    /// salinan — lihat `RecordTransition`. Memisahnya begitu disengaja: clipmap
    /// menyalin belasan wilayah ke tiga tekstur tiap frame, dan barrier per
    /// wilayah akan memecah rangkaian salinan itu menjadi belasan titik
    /// sinkronisasi yang tidak satu pun dibutuhkan.
    void RecordRegionCopy(VkCommandBuffer cmd, VkBuffer source, VkDeviceSize sourceOffset,
                          const glm::uvec3& offset, const glm::uvec3& extent) const;

    /// Barrier layout, untuk membingkai rangkaian `RecordRegionCopy`.
    void RecordTransition(VkCommandBuffer cmd, VkImageLayout from, VkImageLayout to);

    VkImage Image() const { return image_; }
    VkImageView View() const { return view_; }
    VkSampler Sampler() const { return sampler_; }
    glm::uvec3 Extent() const { return extent_; }
    bool IsValid() const { return image_ != VK_NULL_HANDLE; }

private:
    Device* device_ = nullptr;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    glm::uvec3 extent_{0};
    uint32_t bytesPerTexel_ = 0;
};

}  // namespace sim::rhi
