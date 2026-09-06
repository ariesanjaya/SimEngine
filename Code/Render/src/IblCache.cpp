#include "IblCache.h"

#include "Sim/Core/AtomicWrite.h"
#include "Sim/Core/Log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace sim::render {
namespace {

constexpr uint64_t kFnvOffset = 1469598103934665603ull;

/// Dinaikkan setiap kali arti isi berkasnya berubah — tata letak texel, urutan
/// mip, atau matematika yang menghasilkannya. Ia bagian dari kunci, jadi
/// menaikkannya membuat seluruh artefak lama tidak pernah terbaca lagi alih-alih
/// terbaca salah.
constexpr uint32_t kCacheVersion = 2;

/// Empat byte pertama berkasnya. Berkas yang bukan milik kita ditolak di sini,
/// sebelum satu pun angka di dalamnya dipercaya.
constexpr char kMagic[4] = {'S', 'I', 'B', 'L'};

struct Header {
    char magic[4];
    uint32_t version;
    uint32_t cubeSize;
    uint32_t mipCount;
    /// Mip pertama yang tidak ikut disaring CPU — nol berarti seluruhnya ikut.
    ///
    /// **Disimpan, bukan disimpulkan dari isinya.** Sebuah artefak yang
    /// mipnya nol karena diserahkan ke GPU dan sebuah artefak yang mipnya nol
    /// karena lingkungannya memang hitam tidak bisa dibedakan dari texelnya —
    /// dan yang salah menebak menghasilkan lingkungan gelap yang tidak pernah
    /// dipanggang ulang, atau satu dispatch sia-sia setiap kali level dibuka.
    uint32_t firstGpuMip;
    uint32_t pad;
    uint64_t texelCount;
};

uint64_t HashInto(uint64_t hash, const void* data, std::size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (std::size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

}  // namespace

uint64_t IblCacheKey(const std::filesystem::path& source, const IblBakeSettings& settings,
                     float intensity) {
    std::error_code code;
    const auto size = std::filesystem::file_size(source, code);
    if (code) {
        return 0;
    }
    const auto written = std::filesystem::last_write_time(source, code);
    if (code) {
        return 0;
    }

    // **Ukuran dan waktu tulisnya, bukan seluruh isinya.** Cache SDF mesh
    // meng-hash byte berkasnya, dan itu benar untuk mesh yang beberapa ratus
    // kilobyte; sebuah HDR 4K adalah 25 MB, dan membacanya seluruhnya hanya
    // untuk memutuskan bahwa kita tidak perlu membacanya membuat penghematannya
    // sendiri ikut hilang. Berkas yang diganti dengan berkas lain berukuran
    // sama persis pada detik yang sama tidak akan terdeteksi — dan itu harga
    // yang diterima, karena yang tersisa cuma menghapus satu folder cache.
    const auto stamp = written.time_since_epoch().count();
    uint64_t hash = HashInto(kFnvOffset, &size, sizeof(size));
    hash = HashInto(hash, &stamp, sizeof(stamp));

    const std::string text = source.generic_string();
    hash = HashInto(hash, text.data(), text.size());

    // Medannya satu per satu, bukan struct-nya utuh: padding tidak
    // diinisialisasi, dan hash atasnya berbeda antar pemanggilan untuk
    // pengaturan yang sama persis.
    hash = HashInto(hash, &settings.cubeSize, sizeof(settings.cubeSize));
    hash = HashInto(hash, &settings.mipCount, sizeof(settings.mipCount));
    hash = HashInto(hash, &settings.prefilterSamples, sizeof(settings.prefilterSamples));
    hash = HashInto(hash, &settings.irradianceSamples, sizeof(settings.irradianceSamples));
    // Ikut kuncinya karena ia mengubah isi berkasnya: yang dipanggang dengan
    // jalur GPU menyimpan mip di atasnya sebagai nol, dan memberikannya kepada
    // pemuat yang tidak akan menjalankan dispatch berarti pantulan hitam.
    hash = HashInto(hash, &settings.firstGpuMip, sizeof(settings.firstGpuMip));
    hash = HashInto(hash, &intensity, sizeof(intensity));
    hash = HashInto(hash, &kCacheVersion, sizeof(kCacheVersion));
    return hash;
}

std::filesystem::path IblCachePath(const std::filesystem::path& cacheDir, uint64_t key) {
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.simibl", static_cast<unsigned long long>(key));
    return cacheDir / name;
}

bool WriteIblCache(const std::filesystem::path& file, const IblBakeCpu& baked,
                   std::string& error) {
    if (!baked.IsValid()) {
        error = "nothing baked to write";
        return false;
    }
    std::error_code code;
    if (!file.parent_path().empty()) {
        std::filesystem::create_directories(file.parent_path(), code);
    }

    // **Ditulis ke berkas sementara lalu dipindahkan**, konvensi yang sama
    // dengan `WriteMeshSdf`. Dua alasannya nyata di sini: sebuah proses yang
    // mati di tengah tulis meninggalkan berkas terpotong yang jalan berikutnya
    // temukan sebagai cache yang sah, dan dua renderer yang memanggang
    // lingkungan yang sama menulis ke nama berkas yang sama — kuncinya memang
    // isi berkas sumbernya, bukan siapa yang memanggangnya. Rename pada
    // filesystem yang sama bersifat atomik, jadi yang terlihat pembaca hanya
    // berkas utuh atau tidak ada berkas sama sekali.
    const std::filesystem::path temporary = UniqueTemporaryPath(file);
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "cannot open " + temporary.string() + " for writing";
        return false;
    }

    Header header{};
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.version = kCacheVersion;
    header.cubeSize = baked.cubeSize;
    header.mipCount = baked.mipCount;
    header.firstGpuMip = baked.firstGpuMip;
    header.pad = 0;
    header.texelCount = baked.cubeTexels.size();

    stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
    stream.write(reinterpret_cast<const char*>(baked.irradiance.coefficients.data()),
                 static_cast<std::streamsize>(sizeof(Vec3) * baked.irradiance.coefficients.size()));
    stream.write(reinterpret_cast<const char*>(baked.cubeTexels.data()),
                 static_cast<std::streamsize>(sizeof(float) * baked.cubeTexels.size()));
    stream.close();
    if (!stream) {
        error = "write failed for " + temporary.string();
        std::filesystem::remove(temporary, code);
        return false;
    }

    std::filesystem::rename(temporary, file, code);
    if (code) {
        error = "cannot move " + temporary.string() + " into place: " + code.message();
        std::filesystem::remove(temporary, code);
        return false;
    }
    return true;
}

namespace {

/// Berapa float yang seharusnya dimiliki cubemap sebesar itu.
///
/// **Dihitung di sini, bukan diambil dari `rhi::TextureCube`.** Berkas ini
/// tinggal di sisi yang tidak menyentuh Vulkan sama sekali, dan rumusnya —
/// enam muka, tiap mip separuh sisinya, empat float per texel — memang milik
/// tata letak artefaknya, bukan milik API grafisnya.
uint64_t ExpectedTexelFloats(uint32_t cubeSize, uint32_t mipCount) {
    uint64_t total = 0;
    for (uint32_t mip = 0; mip < mipCount; ++mip) {
        const uint64_t extent = std::max(cubeSize >> mip, 1u);
        total += 6ull * extent * extent * 4ull;
    }
    return total;
}

}  // namespace

bool ReadIblCache(const std::filesystem::path& file, IblBakeCpu& out, std::string& error) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        error = "no cached environment at " + file.string();
        return false;
    }

    Header header{};
    stream.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!stream || std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0) {
        error = file.string() + " is not a baked environment";
        return false;
    }
    if (header.version != kCacheVersion) {
        error = "cached environment was written by another baker version";
        return false;
    }
    // Batas atas yang masuk akal, dan alasannya bukan kerapian: `texelCount`
    // datang dari berkas, dan mengalokasikan sebanyak yang disebutnya berarti
    // sebuah berkas rusak bisa meminta puluhan gigabyte sebelum satu pun byte
    // isinya dibaca.
    constexpr uint64_t kMaxTexels = 64ull * 1024 * 1024;
    if (header.cubeSize == 0 || header.mipCount == 0 || header.texelCount == 0 ||
        header.texelCount > kMaxTexels) {
        error = "cached environment has an implausible size";
        return false;
    }
    // Mip yang ditangguhkan harus ada di dalam rantainya. Yang menangguhkan
    // seluruh rantai menangguhkan mip 0 juga — dan mip 0 mencuplik lingkungan,
    // yang tidak ada di GPU. Menerimanya berarti mip 0 yang tidak pernah
    // terisi, yaitu pantulan hitam pada permukaan yang paling licin.
    if (header.firstGpuMip != 0 && header.firstGpuMip >= header.mipCount) {
        error = "cached environment defers mip " + std::to_string(header.firstGpuMip) +
                " of only " + std::to_string(header.mipCount);
        return false;
    }
    // **Ukuran yang disebut header harus cocok dengan bentuk yang disebutnya.**
    // `rhi::TextureCube::Create` memang menolak span yang terlalu pendek, jadi
    // yang lolos dari sini tidak akan membaca melewati ujung buffer — tapi
    // ditolak di sini berarti pesannya menyebut artefak yang rusak, bukan
    // "cubemap upload needs N bytes but got M" dari lapisan yang tidak tahu
    // berkas mana yang menyebabkannya.
    if (header.texelCount != ExpectedTexelFloats(header.cubeSize, header.mipCount)) {
        error = "cached environment says " + std::to_string(header.texelCount) +
                " floats but its cube shape needs " +
                std::to_string(ExpectedTexelFloats(header.cubeSize, header.mipCount));
        return false;
    }

    IblBakeCpu baked;
    baked.cubeSize = header.cubeSize;
    baked.mipCount = header.mipCount;
    baked.firstGpuMip = header.firstGpuMip;

    stream.read(reinterpret_cast<char*>(baked.irradiance.coefficients.data()),
                static_cast<std::streamsize>(sizeof(Vec3) * baked.irradiance.coefficients.size()));
    baked.cubeTexels.resize(static_cast<std::size_t>(header.texelCount));
    stream.read(reinterpret_cast<char*>(baked.cubeTexels.data()),
                static_cast<std::streamsize>(sizeof(float) * baked.cubeTexels.size()));
    if (!stream) {
        error = "cached environment is shorter than its header promises";
        return false;
    }

    out = std::move(baked);
    return true;
}

}  // namespace sim::render
