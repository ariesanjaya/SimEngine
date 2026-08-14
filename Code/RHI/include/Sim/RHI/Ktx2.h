#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

/// Pembaca kontainer KTX2, sebatas yang dibutuhkan mengunggah tekstur.
///
/// **Ditulis sendiri, bukan lewat libktx — untuk sekarang.** Yang dibutuhkan T0
/// hanyalah membaca: header, indeks level, lalu `memcpy` tiap level ke
/// `VkImage`. Itu sebuah struktur berkas yang seluruhnya dijelaskan
/// spesifikasinya, dan menariknya lewat pustaka penuh berarti dependensi wajib
/// yang belum membayar apa pun.
///
/// libktx tetap akan masuk — di T2, ketika baker harus **menulis** KTX2 dan
/// memakai supercompression Zstd. Menulis kontainer, memilih skema
/// supercompression, dan menyusun DFD adalah pekerjaan yang tidak layak ditulis
/// tangan; membacanya, untuk berkas tanpa supercompression, adalah dua ratus
/// baris.
///
/// **Yang tidak didukung ditolak beserta sebabnya**, bukan dibaca separuh:
/// supercompression apa pun, larik tekstur, kubus, dan volume. Ketiganya sah
/// menurut spesifikasi dan tidak satu pun dipakai jalur tekstur mesin ini hari
/// ini — dan berkas yang diterima separuh menghasilkan gambar yang salah tanpa
/// satu pun pesan.
namespace sim::rhi {

/// Satu tingkat mip di dalam berkas.
struct Ktx2Level {
    /// Offset dari awal berkas, sudah termasuk header dan indeksnya.
    uint64_t offset = 0;
    /// Byte yang benar-benar tersimpan.
    uint64_t length = 0;
    /// Ukuran sesudah dekompresi. Sama dengan `length` bila tanpa
    /// supercompression — dan hari ini hanya itu yang diterima.
    uint64_t uncompressedLength = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct Ktx2Texture {
    /// Nilai `VkFormat` apa adanya, dibaca dari berkasnya.
    ///
    /// **Angka, bukan enum `VkFormat`.** Header ini dipakai kode yang belum
    /// tentu menyertakan Vulkan, dan nomornya toh sudah ditetapkan
    /// spesifikasi KTX2 sebagai nomor `VkFormat`.
    uint32_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<Ktx2Level> levels;
    /// Isi berkasnya, dipegang supaya `levels` menunjuk ke sesuatu.
    std::vector<uint8_t> bytes;

    std::span<const uint8_t> LevelBytes(std::size_t level) const;
    bool IsValid() const { return !levels.empty() && width > 0 && height > 0; }
};

struct Ktx2Result {
    bool ok = false;
    std::string error;

    explicit operator bool() const { return ok; }
};

/// Membaca sebuah `.ktx2` dari memori.
Ktx2Result ReadKtx2(std::span<const uint8_t> bytes, Ktx2Texture& out);

/// Membaca sebuah `.ktx2` dari disk.
Ktx2Result ReadKtx2(const std::filesystem::path& path, Ktx2Texture& out);

/// Dua belas byte penanda di awal setiap berkas KTX2, menurut spesifikasinya.
///
/// Disebut di header supaya uji bisa menyusun berkas dari tata letak
/// spesifikasinya, bukan dari penulis buatan sendiri — round-trip terhadap
/// penulis sendiri hanya membuktikan konsistensi dengan diri sendiri.
inline constexpr uint8_t kKtx2Identifier[12] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32,
                                                0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

}  // namespace sim::rhi
