#include "Sim/Assets/Ktx2Write.h"

#include "Sim/Core/Log.h"

#include <ktx.h>

#include <cstdlib>
#include <cstring>

namespace sim::assets {
namespace {

struct TextureHandle {
    ktxTexture2* texture = nullptr;

    ~TextureHandle() {
        if (texture != nullptr) {
            ktxTexture_Destroy(ktxTexture(texture));
        }
    }
};

Ktx2WriteResult Fail(std::string message) {
    Ktx2WriteResult result;
    result.error = std::move(message);
    return result;
}

/// Menyusun teksturnya di memori dan mengisi tiap levelnya.
///
/// Dipisah karena kedua `WriteKtx2` di bawah hanya berbeda pada langkah
/// terakhirnya, dan menyalin dua puluh baris pemeriksaan untuk perbedaan sebesar
/// itu adalah cara yang paling pasti membuat keduanya lambat laun berbeda.
Ktx2WriteResult Build(const Ktx2WriteDesc& desc, TextureHandle& handle) {
    if (desc.width == 0 || desc.height == 0) {
        return Fail("texture has no size");
    }
    if (desc.levels.empty()) {
        return Fail("texture has no mip levels");
    }
    if (desc.vkFormat == 0) {
        // VK_FORMAT_UNDEFINED di sebuah KTX2 berarti "Basis Universal", bukan
        // "belum diisi" — dan berkas yang menyatakannya tanpa data Basis akan
        // ditolak setiap pembaca, termasuk milik kita sendiri.
        return Fail("vkFormat is undefined");
    }

    ktxTextureCreateInfo info{};
    info.vkFormat = desc.vkFormat;
    info.baseWidth = desc.width;
    info.baseHeight = desc.height;
    info.baseDepth = 1;
    info.numDimensions = 2;
    info.numLevels = static_cast<ktx_uint32_t>(desc.levels.size());
    info.numLayers = 1;
    info.numFaces = 1;
    info.isArray = KTX_FALSE;
    // Dibangkitkan di `Sim::ImageIO`, bukan di sini. Yang dibangkitkan libktx
    // tidak tahu apakah pikselnya warna atau angka, dan rata-rata yang salah
    // ruangnya persis kesalahan yang paling mahal di jalur ini.
    info.generateMipmaps = KTX_FALSE;

    const KTX_error_code created =
        ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &handle.texture);
    if (created != KTX_SUCCESS) {
        return Fail(std::string("cannot create KTX2 texture: ") + ktxErrorString(created));
    }

    for (std::size_t level = 0; level < desc.levels.size(); ++level) {
        const Ktx2WriteLevel& entry = desc.levels[level];
        // Ukuran yang diharapkan libktx dihitung dari format dan dimensinya
        // sendiri. Membandingkannya lebih dulu mengubah kegagalan yang muncul
        // sebagai gambar teracak menjadi pesan yang menyebut level mana.
        const ktx_size_t expected =
            ktxTexture_GetImageSize(ktxTexture(handle.texture), static_cast<ktx_uint32_t>(level));
        if (entry.bytes.size() != expected) {
            return Fail("level " + std::to_string(level) + " has " +
                        std::to_string(entry.bytes.size()) + " bytes but the format needs " +
                        std::to_string(expected));
        }
        const KTX_error_code set = ktxTexture_SetImageFromMemory(
            ktxTexture(handle.texture), static_cast<ktx_uint32_t>(level), 0, 0, entry.bytes.data(),
            entry.bytes.size());
        if (set != KTX_SUCCESS) {
            return Fail("level " + std::to_string(level) + ": " + ktxErrorString(set));
        }
    }

    Ktx2WriteResult result;
    result.ok = true;
    return result;
}

}  // namespace

Ktx2WriteResult WriteKtx2(const Ktx2WriteDesc& desc, const std::filesystem::path& path) {
    TextureHandle handle;
    Ktx2WriteResult built = Build(desc, handle);
    if (!built) {
        return built;
    }

    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);

    const KTX_error_code written =
        ktxTexture_WriteToNamedFile(ktxTexture(handle.texture), path.string().c_str());
    if (written != KTX_SUCCESS) {
        return Fail("cannot write " + path.string() + ": " + ktxErrorString(written));
    }

    Ktx2WriteResult result;
    result.ok = true;
    return result;
}

Ktx2WriteResult WriteKtx2(const Ktx2WriteDesc& desc, std::vector<uint8_t>& out) {
    out.clear();
    TextureHandle handle;
    Ktx2WriteResult built = Build(desc, handle);
    if (!built) {
        return built;
    }

    ktx_uint8_t* bytes = nullptr;
    ktx_size_t size = 0;
    const KTX_error_code written =
        ktxTexture_WriteToMemory(ktxTexture(handle.texture), &bytes, &size);
    if (written != KTX_SUCCESS) {
        return Fail(std::string("cannot write KTX2 to memory: ") + ktxErrorString(written));
    }
    out.assign(bytes, bytes + size);
    // Dialokasikan alokator libktx, jadi dibebaskan di sini — di dalam TU yang
    // satu-satunya tahu pustaka mana yang memilikinya.
    free(bytes);

    Ktx2WriteResult result;
    result.ok = true;
    return result;
}

}  // namespace sim::assets
