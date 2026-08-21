#pragma once

#include "Sim/RHI/Device.h"

#include <cstddef>

namespace sim::rhi {

/// Buffer GPU yang isinya ditulis ulang tiap frame dari CPU.
///
/// Dipakai geometri yang berubah terus dan jumlahnya tidak diketahui di muka:
/// garis grid, wireframe seleksi, gizmo bantu. Memorinya host-visible dan
/// dipetakan sekali lalu dibiarkan terpetakan — memetakan ulang tiap frame
/// hanya menambah biaya tanpa manfaat, dan VMA memang menyarankan pemetaan
/// persisten untuk pola tulis-sekali-baca-sekali seperti ini.
///
/// Bukan buffer untuk data statis. Mesh yang tidak berubah akan mendapat
/// buffer device-local dengan staging di E8.
class DynamicBuffer {
public:
    DynamicBuffer() = default;
    ~DynamicBuffer();

    DynamicBuffer(const DynamicBuffer&) = delete;
    DynamicBuffer& operator=(const DynamicBuffer&) = delete;

    bool Create(Device& device, VkBufferUsageFlags usage, VkDeviceSize initialBytes);
    void Destroy();

    /// Memastikan kapasitas cukup. Membuat ulang buffer bila perlu, jadi tidak
    /// boleh dipanggil saat frame yang masih memakainya belum selesai.
    /// Mengembalikan false bila pembuatan gagal.
    bool Reserve(VkDeviceSize bytes);

    /// Menyalin data ke buffer. Mengembalikan false bila tidak muat.
    bool Write(const void* data, VkDeviceSize bytes);

    /// Menyalin data mulai `offset`. Dipakai pemanggil yang mengemas banyak
    /// potongan ke satu buffer supaya bisa disalin dengan satu command buffer
    /// alih-alih satu submit per potongan.
    bool WriteAt(VkDeviceSize offset, const void* data, VkDeviceSize bytes);

    /// Pemetaan host-nya, untuk pemanggil yang **membaca** dari buffer ini.
    /// Nullptr bila belum dibuat. Dipakai readback tangkapan layar; jalur
    /// tulis tetap lewat `Write`/`WriteAt` supaya batasnya diperiksa.
    const void* Mapped() const { return mapped_; }

    VkBuffer Handle() const { return buffer_; }
    VkDeviceSize Capacity() const { return capacity_; }

    /// Naik setiap kali buffernya benar-benar dibuat ulang.
    ///
    /// **Yang membandingkan `Handle()` untuk tahu apakah descriptor perlu
    /// ditulis ulang akan sesekali salah**, dan diamnya total: `VkBuffer` adalah
    /// handle yang boleh dipakai ulang, dan buffer baru yang dibuat tepat
    /// sesudah yang lama dimusnahkan lazimnya mendapat angka yang sama persis.
    /// Descriptor lalu dibiarkan menunjuk memori yang sudah dibebaskan — dan
    /// yang keluar bukan galat melainkan matriks berisi sampah.
    uint64_t Generation() const { return generation_; }
    bool IsValid() const { return buffer_ != VK_NULL_HANDLE; }

private:
    Device* device_ = nullptr;
    VkBufferUsageFlags usage_ = 0;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    VkDeviceSize capacity_ = 0;
    uint64_t generation_ = 0;
};

}  // namespace sim::rhi
