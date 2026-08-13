// Enkoder PNG greyscale, 16 bit untuk heightmap dan 8 bit untuk peta bobot dan
// peta hole.
//
// **Ditulis sendiri, bukan lewat dependensi baru.** `stb_image_write` sudah ada
// di proyek dan sudah memuat deflate lengkap — tapi `stbi_write_png` hanya
// menulis 8 bit, dan 8 bit adalah 256 tingkat tinggi, yaitu langkah empat meter
// pada terrain setinggi kilometer. Yang kurang darinya bukan kompresornya
// melainkan wadahnya: sebuah header, satu CRC per chunk, dan satu filter per
// baris. Itu yang ditulis di sini, di atas `stbi_zlib_compress` yang memang
// sudah diekspor stb.
//
// Peta 8-bit lewat jalur yang sama, bukan lewat `stbi_write_png`. Bukan karena
// jalur itu tidak mampu, melainkan supaya seluruh berkas pendamping sebuah
// terrain dihasilkan satu penulis: dua penulis berarti dua perilaku yang bisa
// berbeda dalam hal-hal yang baru terlihat pada berkas orang lain — pilihan
// filter, ukuran keluaran, penanganan gambar kosong.
//
// **Dulu tinggal di `Sim::Terrain`, dan pindah ke sini pada I3.** Rencananya
// semula menghapusnya begitu ada pustaka yang menulis PNG 16-bit sendiri —
// dan tidak ada: tinyexr menulis EXR, libtiff menulis TIFF, `stbi_write_png`
// hanya 8 bit. Jadi enkoder ini tetap ada, permanen; yang berubah cuma
// tempatnya — sekarang ia di belakang seam bersama seluruh I/O gambar yang
// lain, bukan setengah di modul terrain.
//
// Filternya adaptif per baris. Tanpa filter, heightmap 16-bit nyaris tidak
// terkompresi sama sekali — dan PNG yang seukuran RAW tidak ada gunanya, karena
// RAW sudah tersedia.

#include "PngWrite.h"

#include <stb_impl.h>

#include <cstdlib>
#include <cstring>

namespace sim::imageio {
namespace {

uint32_t Crc32(const unsigned char* data, std::size_t length, uint32_t crc = 0xffffffffU) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) != 0 ? 0xedb88320U ^ (c >> 1) : c >> 1;
            }
            table[n] = c;
        }
        ready = true;
    }
    for (std::size_t i = 0; i < length; ++i) {
        crc = table[(crc ^ data[i]) & 0xffU] ^ (crc >> 8);
    }
    return crc;
}

void PushBigEndian32(std::vector<unsigned char>& out, uint32_t value) {
    out.push_back(static_cast<unsigned char>(value >> 24));
    out.push_back(static_cast<unsigned char>(value >> 16));
    out.push_back(static_cast<unsigned char>(value >> 8));
    out.push_back(static_cast<unsigned char>(value));
}

void PushChunk(std::vector<unsigned char>& out, const char type[4], const unsigned char* payload,
               std::size_t length) {
    PushBigEndian32(out, static_cast<uint32_t>(length));
    const std::size_t crcBegin = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload, payload + length);
    const uint32_t crc = Crc32(out.data() + crcBegin, length + 4) ^ 0xffffffffU;
    PushBigEndian32(out, crc);
}

int PaethPredictor(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a);
    const int pb = std::abs(p - b);
    const int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) {
        return a;
    }
    return pb <= pc ? b : c;
}

/// Jumlah nilai absolut baris terfilter, dibaca sebagai byte bertanda. Ini
/// heuristik pemilihan filter yang dianjurkan spesifikasi PNG: baris yang
/// nilainya rapat di sekitar nol adalah baris yang paling bisa dimampatkan
/// deflate.
std::size_t FilterCost(const std::vector<unsigned char>& line) {
    std::size_t sum = 0;
    for (const unsigned char byte : line) {
        sum += byte < 128 ? byte : 256u - byte;
    }
    return sum;
}

/// Menyusun berkas PNG dari baris piksel mentah yang sudah dalam tata letak
/// berkasnya.
std::vector<unsigned char> EncodePng(const std::vector<unsigned char>& raw, int width, int height,
                                     int bitDepth) {
    std::vector<unsigned char> png;
    const int bpp = bitDepth / 8;  // byte per sampel greyscale
    const std::size_t stride = static_cast<std::size_t>(width) * static_cast<std::size_t>(bpp);

    std::vector<unsigned char> filtered;
    filtered.reserve((stride + 1u) * static_cast<std::size_t>(height));

    std::vector<unsigned char> candidate(stride);
    std::vector<unsigned char> best(stride);
    const std::vector<unsigned char> zeros(stride, 0);

    for (int y = 0; y < height; ++y) {
        const unsigned char* line = raw.data() + static_cast<std::size_t>(y) * stride;
        const unsigned char* prior =
            y > 0 ? raw.data() + static_cast<std::size_t>(y - 1) * stride : zeros.data();

        std::size_t bestCost = 0;
        unsigned char bestType = 0;
        for (unsigned char type = 0; type <= 4; ++type) {
            for (std::size_t i = 0; i < stride; ++i) {
                const int a = i >= static_cast<std::size_t>(bpp) ? line[i - bpp] : 0;
                const int b = prior[i];
                const int c = i >= static_cast<std::size_t>(bpp) ? prior[i - bpp] : 0;
                int value = 0;
                switch (type) {
                    case 0: value = line[i]; break;
                    case 1: value = line[i] - a; break;
                    case 2: value = line[i] - b; break;
                    case 3: value = line[i] - ((a + b) >> 1); break;
                    default: value = line[i] - PaethPredictor(a, b, c); break;
                }
                candidate[i] = static_cast<unsigned char>(value & 0xff);
            }
            const std::size_t cost = FilterCost(candidate);
            if (type == 0 || cost < bestCost) {
                bestCost = cost;
                bestType = type;
                best = candidate;
            }
        }
        filtered.push_back(bestType);
        filtered.insert(filtered.end(), best.begin(), best.end());
    }

    const std::vector<unsigned char> compressed =
        stb_impl::Deflate(filtered.data(), filtered.size(), 8);
    if (compressed.empty()) {
        return png;
    }

    static const unsigned char kSignature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    png.insert(png.end(), kSignature, kSignature + 8);

    unsigned char ihdr[13];
    ihdr[0] = static_cast<unsigned char>(static_cast<uint32_t>(width) >> 24);
    ihdr[1] = static_cast<unsigned char>(static_cast<uint32_t>(width) >> 16);
    ihdr[2] = static_cast<unsigned char>(static_cast<uint32_t>(width) >> 8);
    ihdr[3] = static_cast<unsigned char>(static_cast<uint32_t>(width));
    ihdr[4] = static_cast<unsigned char>(static_cast<uint32_t>(height) >> 24);
    ihdr[5] = static_cast<unsigned char>(static_cast<uint32_t>(height) >> 16);
    ihdr[6] = static_cast<unsigned char>(static_cast<uint32_t>(height) >> 8);
    ihdr[7] = static_cast<unsigned char>(static_cast<uint32_t>(height));
    ihdr[8] = static_cast<unsigned char>(bitDepth);
    ihdr[9] = 0;   // greyscale
    ihdr[10] = 0;  // kompresi deflate
    ihdr[11] = 0;  // metode filter standar
    ihdr[12] = 0;  // tanpa interlace
    PushChunk(png, "IHDR", ihdr, sizeof(ihdr));
    PushChunk(png, "IDAT", compressed.data(), compressed.size());
    const unsigned char empty = 0;
    PushChunk(png, "IEND", &empty, 0);
    return png;
}

}  // namespace

bool EncodeGreyscalePng(const Image& image, std::vector<uint8_t>& out) {
    const ImageDesc& desc = image.desc;
    if (desc.width == 0 || desc.height == 0 || desc.channels != 1) {
        return false;
    }
    const auto width = static_cast<int>(desc.width);
    const auto height = static_cast<int>(desc.height);

    if (desc.type == PixelType::UInt8) {
        const uint8_t* values = image.AsU8();
        const std::vector<unsigned char> raw(
            values, values + static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
        out = EncodePng(raw, width, height, 8);
        return !out.empty();
    }

    if (desc.type != PixelType::UInt16) {
        return false;
    }
    const uint16_t* samples = image.AsU16();
    // PNG menyimpan sampel 16-bit big-endian, apa pun endianness mesinnya.
    const std::size_t stride = static_cast<std::size_t>(width) * 2u;
    std::vector<unsigned char> raw(stride * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint16_t value = samples[static_cast<std::size_t>(y) *
                                               static_cast<std::size_t>(width) +
                                           static_cast<std::size_t>(x)];
            raw[static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 2u] =
                static_cast<unsigned char>(value >> 8);
            raw[static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 2u + 1u] =
                static_cast<unsigned char>(value & 0xffU);
        }
    }
    out = EncodePng(raw, width, height, 16);
    return !out.empty();
}

}  // namespace sim::imageio
