#include "Sim/Assets/BlockCompress.h"

#include "Sim/Core/Log.h"

#include <bc7decomp.h>
#include <bc7enc.h>
#include <rgbcx.h>

#include <algorithm>
#include <mutex>
#include <thread>
#include <vector>

namespace sim::assets {
namespace {

/// Kedua encoder membangun tabel statisnya sekali sebelum dipakai.
///
/// Dijaga `std::call_once` karena baker berjalan di `TaskPool`: dua tekstur yang
/// mulai bersamaan akan membangun tabel yang sama dari dua thread, dan blok
/// pertama masing-masing keluar dari tabel yang setengah terisi. Kerusakannya
/// hanya pada blok pertama beberapa tekstur pertama — persis bentuk bug yang
/// tidak pernah bisa direproduksi.
void EnsureEncodersReady() {
    static std::once_flag once;
    std::call_once(once, []() {
        rgbcx::init();
        bc7enc_compress_block_init();
    });
}

/// Menyalin ubin 4×4 ke buffer rapat, mengulang tepi untuk piksel di luar.
///
/// **Diulang, bukan dinolkan.** Piksel di luar tepi tetap ikut menentukan
/// endpoint bloknya, jadi ikut menentukan piksel yang benar-benar terlihat.
/// Hitam di sana menarik endpoint ke bawah dan menggelapkan tepi kanan dan bawah
/// setiap tekstur yang sisinya bukan kelipatan empat.
void GatherBlock(std::span<const uint8_t> pixels, uint32_t width, uint32_t height,
                 uint32_t channels, uint32_t blockX, uint32_t blockY, uint8_t* out) {
    for (uint32_t y = 0; y < 4; ++y) {
        const uint32_t sourceY = std::min(blockY * 4 + y, height - 1);
        for (uint32_t x = 0; x < 4; ++x) {
            const uint32_t sourceX = std::min(blockX * 4 + x, width - 1);
            const std::size_t from =
                (static_cast<std::size_t>(sourceY) * width + sourceX) * channels;
            const std::size_t to = (static_cast<std::size_t>(y) * 4 + x) * channels;
            for (uint32_t c = 0; c < channels; ++c) {
                out[to + c] = pixels[from + c];
            }
        }
    }
}

/// Tingkat usaha rgbcx untuk BC1/BC3. Rentangnya 0..18.
uint32_t RgbcxLevel(TextureQuality quality) {
    switch (quality) {
        case TextureQuality::Fast:
            return 0;
        case TextureQuality::Best:
            return rgbcx::MAX_LEVEL;
        case TextureQuality::Balanced:
            break;
    }
    return 10;
}

bc7enc_compress_block_params Bc7Params(const CompressOptions& options) {
    bc7enc_compress_block_params params;
    // `..._params_init` tidak menolkan strukturnya lebih dulu, dan medan yang
    // tidak disentuhnya berisi sampah stack. Salah satunya `m_selectors`, yang
    // dibaca begitu `m_force_selectors` kebetulan bernilai bukan nol.
    params.clear();
    bc7enc_compress_block_params_init(&params);
    if (options.perceptual) {
        bc7enc_compress_block_params_init_perceptual_weights(&params);
    } else {
        bc7enc_compress_block_params_init_linear_weights(&params);
    }
    switch (options.quality) {
        case TextureQuality::Fast:
            params.m_uber_level = 0;
            params.m_max_partitions = 0;
            break;
        case TextureQuality::Balanced:
            // **Diturunkan dari uber 1 / 16 partisi setelah diukur.** Tekstur 4K
            // memakan 8,7 detik di sana dan 5,3 detik di sini, sementara PSNR-nya
            // hanya turun dari 46,5 dB ke 46,2 dB. Angka lengkapnya ada di
            // docs/PLAN-TEXTURE.md.
            //
            // Partisinya **tidak** ikut dinolkan meski itu yang paling cepat:
            // tanpa partisi waktunya 3,4 detik tetapi PSNR-nya jatuh ke 41,4 dB
            // — lima desibel, dan itu terlihat sebagai blok pada tepi yang tajam.
            params.m_uber_level = 0;
            params.m_max_partitions = 8;
            break;
        case TextureQuality::Best:
            params.m_uber_level = BC7ENC_MAX_UBER_LEVEL;
            params.m_max_partitions = BC7ENC_MAX_PARTITIONS;
            break;
    }
    return params;
}

uint32_t RequiredChannels(BlockFormat format) { return format == BlockFormat::Bc4 ? 1 : 4; }

}  // namespace

uint32_t BlockByteSize(BlockFormat format) {
    switch (format) {
        case BlockFormat::Bc1:
        case BlockFormat::Bc4:
            return 8;
        case BlockFormat::Bc3:
        case BlockFormat::Bc5:
        case BlockFormat::Bc7:
            return 16;
    }
    return 16;
}

uint32_t BlockCount(uint32_t pixels) { return (pixels + 3) / 4; }

std::size_t CompressedSize(BlockFormat format, uint32_t width, uint32_t height) {
    return static_cast<std::size_t>(BlockCount(width)) * BlockCount(height) *
           BlockByteSize(format);
}

bool CompressBlocks(const CompressOptions& options, std::span<const uint8_t> pixels,
                    uint32_t width, uint32_t height, uint32_t channels,
                    std::vector<uint8_t>& out) {
    out.clear();
    if (width == 0 || height == 0) {
        return false;
    }
    // `Bc4` boleh satu kanal atau empat; yang lain menuntut empat. Diperiksa di
    // sini karena kesalahannya berupa pembacaan di luar buffer, bukan gambar
    // yang salah.
    if (channels != RequiredChannels(options.format) && channels != 4) {
        SIM_WARN("Assets", "Block compression needs {} channels, got {}",
                 RequiredChannels(options.format), channels);
        return false;
    }
    if (pixels.size() < static_cast<std::size_t>(width) * height * channels) {
        SIM_WARN("Assets", "Block compression got {} bytes for {}x{}x{}", pixels.size(), width,
                 height, channels);
        return false;
    }

    EnsureEncodersReady();

    const uint32_t blocksX = BlockCount(width);
    const uint32_t blocksY = BlockCount(height);
    const uint32_t blockBytes = BlockByteSize(options.format);
    out.resize(static_cast<std::size_t>(blocksX) * blocksY * blockBytes);

    const uint32_t level = RgbcxLevel(options.quality);
    const bc7enc_compress_block_params bc7 = Bc7Params(options);

    // Satu baris blok dikerjakan seluruhnya oleh satu thread, dan barisnya
    // dibagi berselang-seling. Membaginya menjadi potongan berurutan akan
    // membuat thread yang kebagian daerah rata selesai jauh lebih awal daripada
    // yang kebagian daerah berdetail — dan yang menentukan lamanya adalah yang
    // terakhir selesai.
    auto encodeRows = [&](uint32_t first, uint32_t stride) {
        std::vector<uint8_t> tile(static_cast<std::size_t>(16) * channels);
        for (uint32_t blockY = first; blockY < blocksY; blockY += stride) {
            for (uint32_t blockX = 0; blockX < blocksX; ++blockX) {
                GatherBlock(pixels, width, height, channels, blockX, blockY, tile.data());
                uint8_t* destination =
                    out.data() + (static_cast<std::size_t>(blockY) * blocksX + blockX) * blockBytes;
                switch (options.format) {
                    case BlockFormat::Bc1:
                        // Tanpa mode tiga warna: mode itu menukar sebuah warna
                        // dengan hitam transparan, dan pada tekstur tanpa alfa
                        // yang dihasilkannya adalah piksel hitam yang tak
                        // terduga.
                        rgbcx::encode_bc1(level, destination, tile.data(), false, false);
                        break;
                    case BlockFormat::Bc3:
                        rgbcx::encode_bc3(level, destination, tile.data());
                        break;
                    case BlockFormat::Bc4:
                        rgbcx::encode_bc4(destination, tile.data(), channels);
                        break;
                    case BlockFormat::Bc5:
                        rgbcx::encode_bc5(destination, tile.data(), 0, 1, channels);
                        break;
                    case BlockFormat::Bc7:
                        bc7enc_compress_block(destination, tile.data(), &bc7);
                        break;
                }
            }
        }
    };

    unsigned threads = options.threads;
    if (threads == 0) {
        threads = std::max(1u, std::thread::hardware_concurrency());
    }
    threads = std::min<unsigned>(threads, blocksY);

    if (threads <= 1) {
        encodeRows(0, 1);
        return true;
    }

    std::vector<std::thread> workers;
    workers.reserve(threads - 1);
    for (unsigned i = 1; i < threads; ++i) {
        workers.emplace_back([&encodeRows, i, threads]() { encodeRows(i, threads); });
    }
    // Thread pemanggil ikut bekerja, bukan menunggu. Ia sudah ada, dan
    // membiarkannya diam berarti membayar sebuah thread tambahan untuk pekerjaan
    // yang bisa dikerjakannya sendiri.
    encodeRows(0, threads);
    for (std::thread& worker : workers) {
        worker.join();
    }
    return true;
}

bool DecompressBlocks(BlockFormat format, std::span<const uint8_t> blocks, uint32_t width,
                      uint32_t height, std::vector<uint8_t>& out) {
    out.assign(static_cast<std::size_t>(width) * height * 4, 0);
    if (width == 0 || height == 0) {
        return false;
    }
    if (blocks.size() < CompressedSize(format, width, height)) {
        return false;
    }

    const uint32_t blocksX = BlockCount(width);
    const uint32_t blocksY = BlockCount(height);
    const uint32_t blockBytes = BlockByteSize(format);

    std::vector<uint8_t> tile(64, 0);
    for (uint32_t blockY = 0; blockY < blocksY; ++blockY) {
        for (uint32_t blockX = 0; blockX < blocksX; ++blockX) {
            const uint8_t* source =
                blocks.data() + (static_cast<std::size_t>(blockY) * blocksX + blockX) * blockBytes;
            std::fill(tile.begin(), tile.end(), static_cast<uint8_t>(0));
            switch (format) {
                case BlockFormat::Bc1:
                    rgbcx::unpack_bc1(source, tile.data());
                    break;
                case BlockFormat::Bc3:
                    rgbcx::unpack_bc3(source, tile.data());
                    break;
                case BlockFormat::Bc4:
                    // Merah saja; hijau dan biru dibiarkan nol seperti yang
                    // dijanjikan headernya.
                    rgbcx::unpack_bc4(source, tile.data(), 4);
                    break;
                case BlockFormat::Bc5:
                    rgbcx::unpack_bc5(source, tile.data(), 0, 1, 4);
                    break;
                case BlockFormat::Bc7:
                    bc7decomp::unpack_bc7(source, reinterpret_cast<bc7decomp::color_rgba*>(
                                                      tile.data()));
                    break;
            }
            // Alfa diisi penuh untuk format yang tidak membawanya, supaya
            // pembanding gambar tidak menuduh setiap piksel berbeda hanya karena
            // kanal yang memang tidak ada.
            if (format == BlockFormat::Bc1 || format == BlockFormat::Bc4 ||
                format == BlockFormat::Bc5) {
                for (uint32_t i = 0; i < 16; ++i) {
                    tile[i * 4 + 3] = 255;
                }
            }
            for (uint32_t y = 0; y < 4; ++y) {
                const uint32_t targetY = blockY * 4 + y;
                if (targetY >= height) {
                    break;
                }
                for (uint32_t x = 0; x < 4; ++x) {
                    const uint32_t targetX = blockX * 4 + x;
                    if (targetX >= width) {
                        break;
                    }
                    const std::size_t to =
                        (static_cast<std::size_t>(targetY) * width + targetX) * 4;
                    const std::size_t from = (static_cast<std::size_t>(y) * 4 + x) * 4;
                    for (uint32_t c = 0; c < 4; ++c) {
                        out[to + c] = tile[from + c];
                    }
                }
            }
        }
    }
    return true;
}

}  // namespace sim::assets
