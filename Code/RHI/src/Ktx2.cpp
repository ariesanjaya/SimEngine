#include "Sim/RHI/Ktx2.h"

#include <cstring>
#include <fstream>

namespace sim::rhi {
namespace {

/// Tata letak header KTX2, menurut spesifikasinya. Seluruh medannya
/// little-endian 32 bit, berurutan tanpa sisipan.
constexpr std::size_t kIdentifierBytes = 12;
constexpr std::size_t kHeaderBytes = 80;
constexpr std::size_t kLevelIndexEntryBytes = 24;

uint32_t ReadU32(std::span<const uint8_t> bytes, std::size_t at) {
    uint32_t value = 0;
    std::memcpy(&value, bytes.data() + at, sizeof(value));
    return value;
}

uint64_t ReadU64(std::span<const uint8_t> bytes, std::size_t at) {
    uint64_t value = 0;
    std::memcpy(&value, bytes.data() + at, sizeof(value));
    return value;
}

/// Apakah `[offset, offset + length)` benar-benar berada di dalam `size`.
///
/// **Dihitung dengan pengurangan, bukan penjumlahan.** `offset` dan `length`
/// dibaca apa adanya dari berkas, dan berkas aset datang dari luar — dari
/// unduhan, dari kiriman artis, dari arsip yang rusak. `offset + length`
/// melimpah diam-diam pada nilai seperti `0xFFFFFFFFFFFFFFFF`, hasilnya
/// mengecil, pemeriksaannya lolos, dan yang berikutnya terjadi adalah membaca
/// memori di luar buffer.
///
/// Bentuk `length > size - offset` tidak bisa melimpah karena `offset <= size`
/// sudah dipastikan lebih dulu.
bool FitsInside(uint64_t offset, uint64_t length, std::size_t size) {
    if (offset > size) {
        return false;
    }
    return length <= static_cast<uint64_t>(size) - offset;
}

Ktx2Result Fail(std::string message) {
    Ktx2Result result;
    result.error = std::move(message);
    return result;
}

}  // namespace

std::span<const uint8_t> Ktx2Texture::LevelBytes(std::size_t level) const {
    if (level >= levels.size()) {
        return {};
    }
    const Ktx2Level& entry = levels[level];
    // Diperiksa lagi di sini, bukan dipercayakan pada pembacanya. `Ktx2Texture`
    // adalah struct terbuka: siapa pun boleh menyusunnya tangan, dan yang
    // menyusunnya salah tidak boleh berakhir sebagai pointer di luar buffer.
    if (!FitsInside(entry.offset, entry.length, bytes.size())) {
        return {};
    }
    // Pointer-nya dibentuk **sesudah** rentangnya terbukti di dalam. Membentuk
    // pointer jauh di luar sebuah larik sudah merupakan perilaku tak
    // terdefinisi, bahkan sebelum ia dibaca.
    return std::span<const uint8_t>(bytes.data() + static_cast<std::size_t>(entry.offset),
                                    static_cast<std::size_t>(entry.length));
}

Ktx2Result ReadKtx2(std::span<const uint8_t> bytes, Ktx2Texture& out) {
    out = Ktx2Texture{};

    if (bytes.size() < kHeaderBytes) {
        return Fail("file is shorter than a KTX2 header");
    }
    if (std::memcmp(bytes.data(), kKtx2Identifier, kIdentifierBytes) != 0) {
        return Fail("not a KTX2 file: the 12-byte identifier does not match");
    }

    const uint32_t format = ReadU32(bytes, 12);
    const uint32_t pixelWidth = ReadU32(bytes, 20);
    const uint32_t pixelHeight = ReadU32(bytes, 24);
    const uint32_t pixelDepth = ReadU32(bytes, 28);
    const uint32_t layerCount = ReadU32(bytes, 32);
    const uint32_t faceCount = ReadU32(bytes, 36);
    const uint32_t levelCount = ReadU32(bytes, 40);
    const uint32_t supercompression = ReadU32(bytes, 44);

    // **Format 0 berarti Basis Universal**, yang menuntut transcode sebelum bisa
    // diunggah. Itu jalur tersendiri dan sengaja tidak ada di sini; menolaknya
    // menyebut sebabnya jauh lebih berguna daripada mengunggah blok yang bukan
    // BCn ke `VkImage` ber-BCn.
    if (format == 0) {
        return Fail("this KTX2 holds Basis Universal data, which needs transcoding first");
    }
    if (supercompression != 0) {
        return Fail("supercompressed KTX2 is not supported yet; write it without Zstd");
    }
    if (pixelWidth == 0 || pixelHeight == 0) {
        return Fail("a 2D texture needs a width and a height");
    }
    if (pixelDepth != 0) {
        return Fail("volume textures are not supported");
    }
    // layerCount 0 berarti "bukan larik" — itu ejaan yang sah menurut
    // spesifikasi, dan bukan hal yang sama dengan larik berisi nol lapis.
    if (layerCount > 1) {
        return Fail("texture arrays are not supported");
    }
    if (faceCount != 1) {
        return Fail("cube maps are not supported here; they load through TextureCube");
    }
    if (levelCount == 0) {
        // Nol berarti "pemuat yang membangkitkan mip-nya sendiri". Mesin ini
        // memang tidak melakukannya, jadi berkas itu tidak punya isi yang bisa
        // dipakai.
        return Fail("levelCount is zero, which asks the loader to generate mips itself");
    }

    // Sebuah rantai mip tidak bisa lebih panjang daripada jumlah kali sebuah
    // ukuran bisa dibagi dua — tiga puluh dua sudah jauh di atas tekstur
    // terbesar yang bisa dibuat perangkat mana pun. Tanpa batas ini,
    // `levelCount` bernilai empat miliar dari berkas rusak menjadi permintaan
    // alokasi puluhan gigabyte sebelum satu byte pun diperiksa.
    constexpr uint32_t kMaxLevels = 32;
    if (levelCount > kMaxLevels) {
        return Fail("levelCount " + std::to_string(levelCount) + " is impossible for a texture");
    }

    const std::size_t indexBytes = static_cast<std::size_t>(levelCount) * kLevelIndexEntryBytes;
    if (bytes.size() < kHeaderBytes + indexBytes) {
        return Fail("file ends inside its level index");
    }

    out.format = format;
    out.width = pixelWidth;
    out.height = pixelHeight;
    out.levels.reserve(levelCount);

    for (uint32_t level = 0; level < levelCount; ++level) {
        const std::size_t at = kHeaderBytes + static_cast<std::size_t>(level) *
                                                  kLevelIndexEntryBytes;
        Ktx2Level entry;
        entry.offset = ReadU64(bytes, at);
        entry.length = ReadU64(bytes, at + 8);
        entry.uncompressedLength = ReadU64(bytes, at + 16);
        // Level 0 adalah yang **terbesar**, dan indeksnya memang menyimpannya
        // lebih dulu — berbeda dengan urutan datanya di dalam berkas, yang
        // menurut spesifikasi justru dari yang terkecil.
        entry.width = pixelWidth >> level;
        entry.height = pixelHeight >> level;
        entry.width = entry.width == 0 ? 1 : entry.width;
        entry.height = entry.height == 0 ? 1 : entry.height;

        if (entry.length == 0) {
            return Fail("level " + std::to_string(level) + " is empty");
        }
        if (!FitsInside(entry.offset, entry.length, bytes.size())) {
            return Fail("level " + std::to_string(level) + " points past the end of the file");
        }
        out.levels.push_back(entry);
    }

    // Isinya disalin, bukan dirujuk. Pemanggil menyerahkan span yang bisa saja
    // buffer sementara, dan `Ktx2Texture` hidup lebih lama daripada pembacaannya.
    out.bytes.assign(bytes.begin(), bytes.end());

    Ktx2Result result;
    result.ok = true;
    return result;
}

Ktx2Result ReadKtx2(const std::filesystem::path& path, Ktx2Texture& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return Fail("cannot open " + path.string());
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
    Ktx2Result result = ReadKtx2(bytes, out);
    if (!result.ok) {
        // Nama berkasnya disebut di sini, sekali. Pesan "not a KTX2 file" tanpa
        // nama mengirim orang memeriksa berkas yang salah.
        result.error = path.filename().string() + ": " + result.error;
    }
    return result;
}

}  // namespace sim::rhi
