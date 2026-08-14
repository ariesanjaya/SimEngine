#pragma once

#include "Sim/Assets/TextureSettings.h"

#include <cstdint>
#include <span>
#include <vector>

/// Kompresi blok BCn untuk baker tekstur.
///
/// **Blok 4×4, dan itu menentukan segalanya di sini.** Tekstur yang sisinya
/// bukan kelipatan empat tetap menghasilkan blok utuh, dan isi piksel yang
/// menonjol keluar tepi ikut menentukan endpoint blok itu — jadi ikut menentukan
/// piksel yang benar-benar terlihat. Mengisinya nol menarik endpoint ke arah
/// hitam dan menggelapkan tepi setiap tekstur berukuran ganjil, tanpa satu pun
/// peringatan.
namespace sim::assets {

/// Format blok yang bisa dihasilkan encoder di sini.
///
/// BC2 sengaja tidak ada: alfa empat bit tanpa interpolasi selalu kalah dari BC3
/// pada ukuran yang sama persis. BC6H juga belum, dan itu bukan pilihan —
/// `bc7enc_rdo` tidak memuat encoder-nya, jadi HDR tetap tanpa kompresi sampai
/// T4 membawa encoder yang punya.
enum class BlockFormat : uint8_t {
    /// RGB 4 bpp, tanpa alfa. Setengah ukuran BC7 dan terlihat bedanya pada
    /// gradien.
    Bc1,
    /// RGB + alfa terinterpolasi, 8 bpp.
    Bc3,
    /// Satu kanal, 4 bpp. Roughness, metalness, occlusion.
    Bc4,
    /// Dua kanal, 8 bpp. Normal map: z-nya direkonstruksi saat disampel.
    Bc5,
    /// RGBA 8 bpp, kualitas jauh di atas BC1/BC3.
    Bc7,
};

struct CompressOptions {
    BlockFormat format = BlockFormat::Bc7;
    TextureQuality quality = TextureQuality::Balanced;

    /// Metrik galat perseptual (YCbCr), bukan RGB apa adanya.
    ///
    /// **Hanya untuk warna, dan tidak pernah untuk normal map.** Metrik
    /// perseptual membelanjakan bit pada kanal yang paling dilihat mata dan
    /// menghemat pada yang lain — tepat untuk warna, dan merusak untuk vektor,
    /// yang ketiga kanalnya sama-sama berarti arah. Kerusakannya halus: bukan
    /// blok yang terlihat, melainkan pencahayaan yang bergeser sedikit di
    /// seluruh permukaan.
    bool perceptual = true;

    /// Berapa thread yang dipakai. Nol berarti sebanyak inti yang ada.
    ///
    /// **Ada karena mengompresi satu tekstur 4K adalah menit, bukan detik.**
    /// Tiap blok 4×4 berdiri sendiri sepenuhnya — tidak ada satu pun keputusan
    /// encoder yang menyeberang antar-blok — jadi pekerjaannya terbagi rata
    /// tanpa syarat apa pun.
    ///
    /// Setel ke 1 bila pemanggilnya sendiri sudah berjalan di dalam `TaskPool`:
    /// N tekstur yang masing-masing membuka N thread menghasilkan N² thread yang
    /// berebut inti yang sama, dan totalnya lebih lambat daripada satu per satu.
    unsigned threads = 0;
};

/// Byte per blok 4×4.
uint32_t BlockByteSize(BlockFormat format);

/// Jumlah blok sepanjang satu sisi, dibulatkan ke atas.
uint32_t BlockCount(uint32_t pixels);

/// Byte yang dibutuhkan satu level dengan ukuran ini.
std::size_t CompressedSize(BlockFormat format, uint32_t width, uint32_t height);

/// Mengompresi satu gambar menjadi blok.
///
/// `pixels` rapat, `width * height * channels` byte. `channels` boleh 1 hanya
/// untuk `Bc4`; format lain menuntut 4.
///
/// Piksel di luar tepi pada blok terakhir **diulang dari tepinya**, bukan diisi
/// nol.
bool CompressBlocks(const CompressOptions& options, std::span<const uint8_t> pixels,
                    uint32_t width, uint32_t height, uint32_t channels,
                    std::vector<uint8_t>& out);

/// Menguraikan blok kembali menjadi RGBA8 rapat, `width * height * 4` byte.
///
/// Ada karena dua hal yang sama-sama menuntutnya: uji yang memeriksa apa yang
/// sungguh keluar dari encoder, dan perhitungan PSNR di T5. Tanpa sisi urai,
/// satu-satunya cara memeriksa hasil kompresi adalah melihatnya.
///
/// Kanal yang tidak dibawa formatnya diisi: `Bc4` menaruh nilainya di merah dan
/// nol di hijau dan biru, `Bc5` menaruh x dan y lalu **membiarkan biru nol** —
/// rekonstruksi z adalah urusan shader, dan mengarangnya di sini akan menutupi
/// shader yang lupa melakukannya.
bool DecompressBlocks(BlockFormat format, std::span<const uint8_t> blocks, uint32_t width,
                      uint32_t height, std::vector<uint8_t>& out);

}  // namespace sim::assets
