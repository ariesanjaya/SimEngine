#include "Sim/ImageIO/MipChain.h"

#include "Sim/Core/Log.h"

#include <stb_image_resize2.h>

#include <algorithm>
#include <cmath>

namespace sim::imageio {
namespace {

/// Tata letak piksel yang dipakai stb, dari jumlah kanal.
///
/// Empat kanal dianggap RGBA **tanpa premultiply**, karena itulah konvensi
/// tunggal mesin ini dan `ToStraightAlpha` sudah menegakkannya lebih dulu.
/// Bedanya bukan kerapian: `STBIR_RGBA` menyaring warna dengan bobot alfa,
/// sedangkan `STBIR_4CHANNEL` menyaring keempat kanal terpisah. Yang salah
/// menghasilkan tepi berpendar pada tekstur beralfa.
stbir_pixel_layout LayoutFor(uint32_t channels) {
    switch (channels) {
        case 1:
            return STBIR_1CHANNEL;
        case 2:
            return STBIR_2CHANNEL;
        case 3:
            return STBIR_RGB;
        default:
            return STBIR_RGBA;
    }
}

stbir_datatype DatatypeFor(const ImageDesc& desc, TextureUsage usage) {
    switch (desc.type) {
        case PixelType::UInt16:
            return STBIR_TYPE_UINT16;
        case PixelType::Float32:
            return STBIR_TYPE_FLOAT;
        case PixelType::UInt8:
            break;
    }
    // Pertanyaannya sama persis dengan yang dijawab `NeedsSrgbGpuFormat`:
    // apakah byte yang tersimpan itu tersandi sRGB. Di sana ia menentukan
    // `VkFormat`, di sini ia menentukan ruang tempat penyaringan dilakukan —
    // dan dua jawaban yang berbeda untuk satu pertanyaan adalah persis cara
    // tekstur berakhir digelapkan dua kali atau tidak sama sekali.
    return NeedsSrgbGpuFormat(usage, desc) ? STBIR_TYPE_UINT8_SRGB : STBIR_TYPE_UINT8;
}

/// Menormalkan ulang kanal xyz sebuah gambar normal map, di tempat.
///
/// Alfa tidak disentuh: pada normal map ia membawa hal lain — height, gloss —
/// dan menormalkannya bersama xyz akan merusaknya.
void Renormalize(Image& image) {
    const ImageDesc& desc = image.desc;
    if (desc.channels < 3) {
        // Dua kanal berarti z-nya direkonstruksi saat disampel, dan panjang yang
        // benar sudah dijamin rekonstruksi itu. Satu kanal bukan normal map.
        return;
    }
    const std::size_t pixels = static_cast<std::size_t>(desc.width) * desc.height;

    auto normalize = [](float& x, float& y, float& z) {
        const float length = std::sqrt(x * x + y * y + z * z);
        // Vektor nol tidak punya arah, dan membaginya dengan nol menghasilkan
        // NaN yang lalu merambat ke seluruh pencahayaan. Yang tidak punya arah
        // dibiarkan apa adanya.
        if (length <= 1e-8f) {
            return;
        }
        x /= length;
        y /= length;
        z /= length;
    };

    switch (desc.type) {
        case PixelType::UInt8: {
            uint8_t* data = image.AsU8();
            for (std::size_t i = 0; i < pixels; ++i) {
                uint8_t* pixel = data + i * desc.channels;
                float x = pixel[0] / 255.0f * 2.0f - 1.0f;
                float y = pixel[1] / 255.0f * 2.0f - 1.0f;
                float z = pixel[2] / 255.0f * 2.0f - 1.0f;
                normalize(x, y, z);
                pixel[0] = static_cast<uint8_t>(std::lround((x * 0.5f + 0.5f) * 255.0f));
                pixel[1] = static_cast<uint8_t>(std::lround((y * 0.5f + 0.5f) * 255.0f));
                pixel[2] = static_cast<uint8_t>(std::lround((z * 0.5f + 0.5f) * 255.0f));
            }
            break;
        }
        case PixelType::UInt16: {
            uint16_t* data = image.AsU16();
            for (std::size_t i = 0; i < pixels; ++i) {
                uint16_t* pixel = data + i * desc.channels;
                float x = pixel[0] / 65535.0f * 2.0f - 1.0f;
                float y = pixel[1] / 65535.0f * 2.0f - 1.0f;
                float z = pixel[2] / 65535.0f * 2.0f - 1.0f;
                normalize(x, y, z);
                pixel[0] = static_cast<uint16_t>(std::lround((x * 0.5f + 0.5f) * 65535.0f));
                pixel[1] = static_cast<uint16_t>(std::lround((y * 0.5f + 0.5f) * 65535.0f));
                pixel[2] = static_cast<uint16_t>(std::lround((z * 0.5f + 0.5f) * 65535.0f));
            }
            break;
        }
        case PixelType::Float32: {
            // Float menyimpan normal apa adanya di −1..1, bukan dipetakan ke
            // 0..1: itu yang dihasilkan setiap alat yang menulis normal ke EXR.
            float* data = image.AsF32();
            for (std::size_t i = 0; i < pixels; ++i) {
                float* pixel = data + i * desc.channels;
                normalize(pixel[0], pixel[1], pixel[2]);
            }
            break;
        }
    }
}

}  // namespace

uint32_t MipLevelCount(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return 0;
    }
    uint32_t largest = std::max(width, height);
    uint32_t levels = 1;
    while (largest > 1) {
        largest >>= 1;
        ++levels;
    }
    return levels;
}

std::vector<Image> BuildMipChain(const Image& source, const MipOptions& options) {
    std::vector<Image> chain;
    const ImageDesc& desc = source.desc;
    if (desc.width == 0 || desc.height == 0 || desc.channels == 0 ||
        source.bytes.size() < desc.ByteCount()) {
        return chain;
    }

    const uint32_t levels = MipLevelCount(desc.width, desc.height);
    chain.reserve(levels);
    chain.push_back(source);
    if (options.renormalize) {
        // Level 0 ikut dinormalkan. Normal map yang datang dari kompresi
        // sebelumnya — atau dari alat yang membulatkan ke delapan bit — tidak
        // pernah tepat sepanjang satu, dan level 0 yang dibiarkan berbeda
        // aturannya dari level lain membuat perbandingan antar-level tidak bisa
        // dipercaya.
        Renormalize(chain.front());
    }

    const stbir_pixel_layout layout = LayoutFor(desc.channels);
    const stbir_datatype type = DatatypeFor(desc, options.usage);

    for (uint32_t level = 1; level < levels; ++level) {
        const Image& previous = chain.back();
        ImageDesc next = desc;
        next.width = std::max(1u, previous.desc.width / 2);
        next.height = std::max(1u, previous.desc.height / 2);

        Image image;
        image.Allocate(next);

        STBIR_RESIZE resize;
        stbir_resize_init(&resize, previous.bytes.data(), static_cast<int>(previous.desc.width),
                          static_cast<int>(previous.desc.height), 0, image.bytes.data(),
                          static_cast<int>(next.width), static_cast<int>(next.height), 0, layout,
                          type);
        // Box, bukan Mitchell yang dipakai API mudahnya. Pada rasio bilangan
        // bulat box adalah rata-rata luas yang sesungguhnya — persis yang
        // diharapkan dari sebuah mip — sementara Mitchell punya lobe negatif
        // yang membuat tepi tajam berdering, dan dering itu terkunci ke dalam
        // tekstur selamanya.
        stbir_set_filters(&resize, STBIR_FILTER_BOX, STBIR_FILTER_BOX);
        // Clamp, bukan wrap. Tekstur yang memang berulang tidak dirugikan clamp
        // pada rasio tepat setengah — kernelnya tidak pernah melewati tepi —
        // sedangkan tekstur yang tidak berulang akan mengambil warna dari sisi
        // seberang bila wrap dipakai.
        stbir_set_edgemodes(&resize, STBIR_EDGE_CLAMP, STBIR_EDGE_CLAMP);

        if (stbir_resize_extended(&resize) == 0) {
            SIM_WARN("ImageIO", "Mip level {} ({}x{}) failed to resample", level, next.width,
                     next.height);
            return chain;
        }
        if (options.renormalize) {
            Renormalize(image);
        }
        chain.push_back(std::move(image));
    }

    return chain;
}

}  // namespace sim::imageio
