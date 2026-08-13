#include "Sim/ImageIO/TextureColor.h"

#include "Backend.h"
#include "PixelOps.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string>

namespace sim::imageio {
namespace {

std::string ToLower(std::string_view text) {
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

/// Slot yang isinya **warna**. Daftarnya pendek dengan sendirinya: hampir semua
/// tekstur di material PBR adalah angka, bukan warna.
///
/// Nama ditulis huruf kecil dan dibandingkan tanpa memedulikan besar-kecilnya,
/// karena slot yang sama muncul sebagai `baseColor` di katalog node, `basecolor`
/// di sebagian berkas glTF, dan `BaseColor` di sebagian DCC.
/// Daftar lengkapnya — termasuk slot angka, yang tidak perlu didaftarkan di
/// sini karena bawaannya sudah `Data` — ada di docs/TEXTURE-CONVENTIONS.md.
const std::array<const char*, 6> kColorSlots{
    "basecolor",  // OpenPBR / glTF
    "albedo",     // nama lama yang masih dipakai banyak alat
    "diffuse",    // lebih tua lagi, masih keluar dari FBX
    "emissive",
    "emission",
    "specularcolor",
};

/// sRGB → linear, IEC 61966-2-1.
///
/// Bagian linear di bawah 0,04045 bukan hiasan: kurva pangkat murni punya
/// turunan tak hingga di nol, dan potongan lurus itulah yang membuat sandinya
/// bisa dibalik dengan stabil di dekat hitam.
float SrgbToLinear(float s) {
    return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
}

/// Kanal warna saja — alfa dilewati. Alfa adalah cakupan, bukan warna, dan
/// mendekodekannya membuat tepi setiap tekstur beralfa salah.
void DecodeSrgbInPlace(Image& image) {
    float* pixels = image.AsF32();
    const uint32_t channels = image.desc.channels;
    const uint32_t colorChannels = channels >= 3 ? 3 : channels;
    const std::size_t count = static_cast<std::size_t>(image.desc.width) * image.desc.height;
    for (std::size_t i = 0; i < count; ++i) {
        for (uint32_t c = 0; c < colorChannels; ++c) {
            float& value = pixels[i * channels + c];
            value = SrgbToLinear(value);
        }
    }
}

}  // namespace

TextureUsage UsageForSlot(std::string_view slot) {
    const std::string lower = ToLower(slot);
    for (const char* name : kColorSlots) {
        if (lower == name) {
            return TextureUsage::Color;
        }
    }
    return TextureUsage::Data;
}

ColorSpace ExpectedColorSpace(TextureUsage usage) {
    return usage == TextureUsage::Color ? ColorSpace::Srgb : ColorSpace::Linear;
}

bool NeedsSrgbGpuFormat(TextureUsage usage, const ImageDesc& desc) {
    if (usage != TextureUsage::Color) {
        return false;
    }
    // Hanya 8-bit. Format `_SRGB` di Vulkan cuma ada untuk delapan bit per
    // kanal, dan berkas float maupun 16-bit memang tidak butuh sandinya.
    if (desc.type != PixelType::UInt8) {
        return false;
    }
    // Berkas yang menyatakan dirinya linear dipercaya. Yang tidak menyatakan
    // apa-apa dianggap sRGB: PNG dan JPEG 8-bit berisi warna nyaris selalu
    // begitu, dan menganggapnya linear adalah kesalahan yang langsung terlihat
    // sebagai tekstur yang terlalu terang.
    return desc.colorSpace != ColorSpace::Linear;
}

void ToLinear(TextureUsage usage, Image& image) {
    if (image.desc.width == 0 || image.desc.height == 0) {
        return;
    }

    if (usage == TextureUsage::Data) {
        // **Uji yang mengunci ada di sini.** Slot data tidak pernah didekode,
        // apa pun kata berkasnya. Kalau berkasnya menyatakan sRGB, itu konflik
        // yang layak dicatat — biasanya berarti normal map disimpan lewat jalur
        // ekspor yang menandainya sebagai warna — tapi yang menentukan tetap
        // slotnya.
        if (image.desc.colorSpace == ColorSpace::Srgb) {
            LogWarning(
                "texture is tagged sRGB but is used as data (normal, roughness, height, …); "
                "leaving the values untouched — decoding them would change the lighting "
                "everywhere without any error appearing");
        }
        return;
    }

    if (image.desc.colorSpace == ColorSpace::Linear) {
        return;  // berkas float dan EXR sudah linear menurut definisinya
    }

    // Dinaikkan ke float sebelum didekode. Mendekode di tempat pada delapan bit
    // membuang justru bagian yang sandi sRGB ada untuk melindunginya: seluruh
    // nilai 0..15 akan jatuh ke 0 atau 1.
    if (image.desc.type != PixelType::Float32) {
        Image converted;
        ConvertType(image, PixelType::Float32, converted);
        image = std::move(converted);
    }
    DecodeSrgbInPlace(image);
    image.desc.colorSpace = ColorSpace::Linear;
}

void ToStraightAlpha(Image& image) {
    if (!image.desc.premultipliedAlpha || image.desc.channels != 4) {
        return;
    }

    const std::size_t count = static_cast<std::size_t>(image.desc.width) * image.desc.height;
    switch (image.desc.type) {
        case PixelType::UInt8: {
            uint8_t* pixels = image.AsU8();
            for (std::size_t i = 0; i < count; ++i) {
                const uint32_t alpha = pixels[i * 4 + 3];
                for (uint32_t c = 0; c < 3; ++c) {
                    uint8_t& value = pixels[i * 4 + c];
                    value = alpha == 0 ? 0
                                       : static_cast<uint8_t>(std::min<uint32_t>(
                                             255, (static_cast<uint32_t>(value) * 255 + alpha / 2) /
                                                      alpha));
                }
            }
            break;
        }
        case PixelType::UInt16: {
            uint16_t* pixels = image.AsU16();
            for (std::size_t i = 0; i < count; ++i) {
                const uint32_t alpha = pixels[i * 4 + 3];
                for (uint32_t c = 0; c < 3; ++c) {
                    uint16_t& value = pixels[i * 4 + c];
                    value = alpha == 0 ? 0
                                       : static_cast<uint16_t>(std::min<uint32_t>(
                                             65535, (static_cast<uint32_t>(value) * 65535 +
                                                     alpha / 2) / alpha));
                }
            }
            break;
        }
        case PixelType::Float32: {
            float* pixels = image.AsF32();
            for (std::size_t i = 0; i < count; ++i) {
                const float alpha = pixels[i * 4 + 3];
                for (uint32_t c = 0; c < 3; ++c) {
                    float& value = pixels[i * 4 + c];
                    // Nol dibiarkan nol, bukan dibagi. Warna piksel yang
                    // sepenuhnya tembus pandang memang tidak bisa dipulihkan
                    // dari nol dikali apa pun.
                    value = alpha == 0.0f ? 0.0f : value / alpha;
                }
            }
            break;
        }
    }
    image.desc.premultipliedAlpha = false;
}

void PrepareTexture(TextureUsage usage, Image& image) {
    // Alfa lebih dulu: berkas yang premultiplied melakukannya pada nilai yang
    // masih tersandikan, jadi membaginya harus terjadi di ruang yang sama.
    ToStraightAlpha(image);
    ToLinear(usage, image);
}

}  // namespace sim::imageio
