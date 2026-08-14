#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

/// Penulis kontainer KTX2, dipakai baker tekstur.
///
/// **Menulis saja; pembacanya ada di `Sim::RHI` dan ditulis tangan.** Pembagian
/// itu disengaja dan bukan kelalaian: runtime yang dikirim bersama game hanya
/// perlu membaca tata letak berkas, sementara menulisnya menuntut menyusun Data
/// Format Descriptor — blok wajib yang tidak dibaca pembaca kita sendiri, jadi
/// menyusunnya salah menghasilkan berkas yang dibuka sempurna oleh mesin ini dan
/// ditolak setiap alat lain, tanpa satu pun tanda.
///
/// Efek sampingnya adalah yang paling berharga: uji round-trip di sini
/// membandingkan **dua implementasi yang berbeda**, bukan membuktikan satu
/// implementasi konsisten dengan dirinya sendiri.
///
/// Tipe libktx tidak pernah muncul di header ini, mengikuti aturan seam yang
/// sama dengan PhysX di `Sim::Physics` dan cgltf di `Sim::Assets`.
namespace sim::assets {

/// Satu tingkat mip yang akan ditulis. `bytes` dipinjam, bukan dimiliki.
struct Ktx2WriteLevel {
    uint32_t width = 0;
    uint32_t height = 0;
    std::span<const uint8_t> bytes;
};

struct Ktx2WriteDesc {
    /// Nomor `VkFormat`, angka apa adanya — modul ini tidak menyertakan Vulkan.
    uint32_t vkFormat = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    /// Level 0 lebih dulu, yaitu yang terbesar. Urutan penyimpanannya di dalam
    /// berkas kebalikannya, dan itu urusan penulisnya.
    std::vector<Ktx2WriteLevel> levels;
};

struct Ktx2WriteResult {
    bool ok = false;
    std::string error;

    explicit operator bool() const { return ok; }
};

/// Menulis `.ktx2` ke berkas. Folder induknya dibuat bila belum ada.
Ktx2WriteResult WriteKtx2(const Ktx2WriteDesc& desc, const std::filesystem::path& path);

/// Menulis `.ktx2` ke memori. Dipakai uji, supaya berkas yang tidak jadi
/// dipakai tidak perlu menyentuh disk sama sekali.
Ktx2WriteResult WriteKtx2(const Ktx2WriteDesc& desc, std::vector<uint8_t>& out);

}  // namespace sim::assets
