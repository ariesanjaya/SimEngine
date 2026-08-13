#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sim::imageio {

/// Kedalaman satu sampel, dan sekaligus cara membacanya.
///
/// Ketiganya persis yang dipakai mesin ini hari ini: `UInt8` untuk tekstur dan
/// mask, `UInt16` untuk heightmap, `Float32` untuk peta lingkungan. Menambah
/// yang keempat (half, misalnya) berarti setiap pemanggil harus menanganinya —
/// jadi EXR half dikonversi ke float saat baca, bukan dibawa apa adanya.
enum class PixelType : uint8_t {
    UInt8,
    UInt16,
    Float32,
};

/// Bagaimana angka di berkas berhubungan dengan intensitas cahaya.
///
/// `Unknown` bukan kelalaian melainkan keadaan yang jujur: PNG tanpa chunk
/// `sRGB` maupun `gAMA` memang tidak menyatakan apa-apa, dan menebaknya di
/// lapisan I/O adalah tebakan yang tidak bisa dikoreksi lagi di atasnya. Yang
/// memutuskan adalah slot material yang memakainya.
enum class ColorSpace : uint8_t {
    Unknown,
    Linear,
    Srgb,
};

std::size_t BytesPerSample(PixelType type);
const char* ToString(PixelType type);
const char* ToString(ColorSpace space);

/// Bentuk gambar tanpa pikselnya — cukup untuk mengisi panel detail dan untuk
/// memutuskan apakah gambarnya perlu dibaca sama sekali.
struct ImageDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    /// 1, 3, atau 4. Nilai lain tidak dihasilkan pembaca mana pun di sini:
    /// yang dua kanal (grey+alpha) dinaikkan ke RGBA oleh backend.
    uint32_t channels = 0;
    PixelType type = PixelType::UInt8;
    ColorSpace colorSpace = ColorSpace::Unknown;

    /// Apakah kanal warna sudah dikalikan alfanya.
    ///
    /// Dibawa terpisah dari `colorSpace` karena kesalahannya berbeda bentuk:
    /// colorspace yang salah menggeser seluruh gambar, alfa yang salah hanya
    /// merusak tepinya — dan tepi yang gelap terlihat seperti masalah lain.
    bool premultipliedAlpha = false;

    /// Nama kanal seperti tercatat di berkas — "R", "G", "B", "A", atau nama
    /// sembarang pada EXR multi-channel ("depth", "velocity.x", "objectId").
    ///
    /// **Kosong berarti berkasnya tidak menyimpannya**, bukan berarti kanalnya
    /// RGB. PNG dan HDR memang tidak punya nama kanal, dan mengarang "R","G","B"
    /// untuk keduanya akan membuat pemanggil tidak bisa lagi membedakan "kanal
    /// pertama memang merah" dari "tidak ada yang tahu kanal pertama itu apa".
    ///
    /// Ada karena inilah satu-satunya cara menolak EXR yang bukan RGB dengan
    /// pesan yang berguna. `.exr` dengan kanal depth dan velocity punya bentuk
    /// yang sama persis dengan HDRI; yang membedakannya cuma namanya.
    std::vector<std::string> channelNames;

    std::size_t SampleCount() const {
        return static_cast<std::size_t>(width) * height * channels;
    }
    std::size_t ByteCount() const { return SampleCount() * BytesPerSample(type); }
};

/// Piksel di memori, baris demi baris dari atas, tanpa padding antar-baris.
///
/// **Byte, bukan union bertipe.** Tiga tipe piksel berarti tiga tata letak yang
/// berbeda, dan tiga `std::vector` bertipe berarti dua di antaranya selalu
/// kosong. Yang disimpan adalah bytenya; yang menafsirkan adalah `desc.type`,
/// dan pengakses di bawah menolak menafsirkannya salah.
struct Image {
    ImageDesc desc;
    std::vector<uint8_t> bytes;

    /// Nullptr bila `desc.type` bukan tipe yang diminta. Pemanggil yang sudah
    /// memaksa tipenya lewat `ReadOptions` tidak perlu memeriksanya lagi —
    /// pembacaan yang tidak menghasilkan tipe itu sudah gagal lebih dulu.
    const uint8_t* AsU8() const;
    const uint16_t* AsU16() const;
    const float* AsF32() const;

    uint8_t* AsU8();
    uint16_t* AsU16();
    float* AsF32();

    /// Mengalokasikan `bytes` sesuai `desc`. Isinya tidak ditentukan.
    void Allocate(const ImageDesc& description);
};

}  // namespace sim::imageio
