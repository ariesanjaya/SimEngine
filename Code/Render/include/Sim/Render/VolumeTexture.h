#pragma once

#include "Sim/Core/Math.h"
#include "Sim/Core/VolumeGrid.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sim::render {

/// Bagaimana nilai volume disandikan untuk tekstur 3D.
///
/// Keduanya bilangan bulat ternormalisasi, bukan half-float, dan itu pilihan
/// yang disengaja: normalisasinya sama untuk keduanya, jadi shader-nya satu dan
/// beralih kualitas tidak menuntut jalur dekode kedua.
enum class VolumeTextureFormat : uint8_t {
    /// Satu byte per voxel. 256 tingkat — cukup untuk asap tebal, dan mulai
    /// terlihat berpita pada kabut tipis yang menutupi seluruh layar.
    R8Unorm,
    /// Dua byte per voxel. Dipakai ketika pita itu terlihat; harganya dua kali
    /// memori, dan sebuah volume 256³ berarti 32 MB alih-alih 16.
    R16Unorm,
};

/// Nilai yang harus dibawa ke shader supaya texel bisa dikembalikan ke satuan
/// aslinya.
///
/// **Normalisasi memakai rentang grid itu sendiri, bukan 0..1 yang diandaikan.**
/// Grid suhu berkisar ratusan, dan grid asap tipis mungkin tidak pernah melewati
/// 0,05 — menjepit keduanya ke 0..1 membuat yang pertama menjadi putih rata dan
/// yang kedua menghilang. Nilai asli = `texel * scale + bias`.
struct VolumeTextureDesc {
    VolumeTextureFormat format = VolumeTextureFormat::R8Unorm;
    uint32_t sizeX = 0;
    uint32_t sizeY = 0;
    uint32_t sizeZ = 0;
    float scale = 1.0f;
    float bias = 0.0f;
    /// Ukuran voxel dan titik asal grid, diteruskan apa adanya supaya pass
    /// raymarch bisa berpindah antara ruang lokal dan ruang tekstur.
    Vec3 origin{0.0f};
    float voxelSize = 0.0f;

    std::size_t BytesPerTexel() const {
        return format == VolumeTextureFormat::R16Unorm ? 2u : 1u;
    }
    std::size_t ByteCount() const {
        return static_cast<std::size_t>(sizeX) * sizeY * sizeZ * BytesPerTexel();
    }
};

/// Menyandikan sebuah grid menjadi byte yang siap diunggah.
///
/// **Fungsi murni, tanpa Vulkan** — dan itu yang membuatnya bisa diuji tanpa
/// GPU. Yang membuka device hanyalah pemanggilnya.
///
/// Grid yang seluruhnya bernilai sama disandikan sebagai nol dengan `scale` nol
/// dan `bias` bernilai itu; membaginya dengan rentang nol akan menghasilkan
/// bukan-angka yang lalu menyebar ke seluruh gambar.
bool EncodeVolume(const VolumeGrid& grid, VolumeTextureFormat format,
                  std::vector<std::byte>& outBytes, VolumeTextureDesc& outDesc);

/// Mengembalikan nilai asli dari sebuah texel yang sudah disandikan.
///
/// Ada untuk dipakai uji: ia menjawab pertanyaan "berapa yang akan dibaca
/// shader", sehingga round-trip bisa diperiksa tanpa menjalankan shader-nya.
float DecodeTexel(const VolumeTextureDesc& desc, std::span<const std::byte> bytes, uint32_t x,
                  uint32_t y, uint32_t z);

}  // namespace sim::render
