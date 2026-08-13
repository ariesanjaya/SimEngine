#include "PixelOps.h"

#include "Backend.h"

#include <algorithm>
#include <cmath>

namespace sim::imageio {
namespace {

uint16_t WidenU8(uint8_t value) {
    // 257 = 65535/255, jadi 0 tetap 0 dan 255 tetap 65535. Menggeser 8 bit ke
    // kiri akan membuat putih menjadi 65280 — putih yang bukan putih.
    return static_cast<uint16_t>(value) * 257u;
}

uint8_t NarrowU16(uint16_t value) {
    return static_cast<uint8_t>((static_cast<uint32_t>(value) * 255u + 32767u) / 65535u);
}

float Saturate(float value) { return std::clamp(value, 0.0f, 1.0f); }

/// Bobot luminansi yang dipakai stb (`stbi__compute_y`).
///
/// Disamakan dengan sengaja: peta kepadatan vegetasi dan mask terrain dibaca
/// sebagai satu kanal dari berkas RGB, dan nilai yang berbeda di sini berarti
/// sebaran tanaman yang berpindah ketika backendnya berganti.
template <typename T>
T Luminance(T r, T g, T b) {
    return static_cast<T>((static_cast<int>(r) * 77 + static_cast<int>(g) * 150 +
                           static_cast<int>(b) * 29) >>
                          8);
}

float LuminanceF(float r, float g, float b) {
    return r * (77.0f / 256.0f) + g * (150.0f / 256.0f) + b * (29.0f / 256.0f);
}

template <typename T>
void ConvertChannelsTyped(const T* in, T* out, std::size_t pixels, uint32_t from, uint32_t to,
                          T opaque) {
    for (std::size_t i = 0; i < pixels; ++i) {
        const T* source = in + i * from;
        T* target = out + i * to;

        // Nilai warna: satu kanal direplikasi, tiga atau empat kanal diambil
        // apa adanya, dan penurunan ke satu kanal lewat luminansi.
        if (to == 1) {
            if (from == 1 || from == 2) {
                target[0] = source[0];
            } else if constexpr (std::is_same_v<T, float>) {
                target[0] = LuminanceF(source[0], source[1], source[2]);
            } else {
                target[0] = Luminance(source[0], source[1], source[2]);
            }
            continue;
        }

        if (from == 1 || from == 2) {
            target[0] = source[0];
            target[1] = source[0];
            target[2] = source[0];
        } else {
            target[0] = source[0];
            target[1] = source[1];
            target[2] = source[2];
        }

        if (to == 4) {
            // Alfa yang tidak ada di berkas adalah opak, bukan nol. Alfa nol
            // membuat setiap tekstur tanpa alfa menghilang sama sekali — tanpa
            // satu pun galat.
            if (from == 4) {
                target[3] = source[3];
            } else if (from == 2) {
                target[3] = source[1];
            } else {
                target[3] = opaque;
            }
        }
    }
}

}  // namespace

void ConvertType(const Image& source, PixelType target, Image& out) {
    if (source.desc.type == target) {
        out = source;
        return;
    }

    ImageDesc desc = source.desc;
    desc.type = target;
    out.Allocate(desc);
    const std::size_t count = desc.SampleCount();

    switch (source.desc.type) {
        case PixelType::UInt8: {
            const uint8_t* in = source.AsU8();
            if (target == PixelType::UInt16) {
                uint16_t* to = out.AsU16();
                for (std::size_t i = 0; i < count; ++i) {
                    to[i] = WidenU8(in[i]);
                }
            } else {
                float* to = out.AsF32();
                for (std::size_t i = 0; i < count; ++i) {
                    to[i] = static_cast<float>(in[i]) / 255.0f;
                }
            }
            break;
        }
        case PixelType::UInt16: {
            const uint16_t* in = source.AsU16();
            if (target == PixelType::UInt8) {
                uint8_t* to = out.AsU8();
                for (std::size_t i = 0; i < count; ++i) {
                    to[i] = NarrowU16(in[i]);
                }
            } else {
                float* to = out.AsF32();
                for (std::size_t i = 0; i < count; ++i) {
                    to[i] = static_cast<float>(in[i]) / 65535.0f;
                }
            }
            break;
        }
        case PixelType::Float32: {
            const float* in = source.AsF32();
            if (target == PixelType::UInt8) {
                uint8_t* to = out.AsU8();
                for (std::size_t i = 0; i < count; ++i) {
                    to[i] = static_cast<uint8_t>(std::lround(Saturate(in[i]) * 255.0f));
                }
            } else {
                uint16_t* to = out.AsU16();
                for (std::size_t i = 0; i < count; ++i) {
                    to[i] = static_cast<uint16_t>(std::lround(Saturate(in[i]) * 65535.0f));
                }
            }
            break;
        }
    }
}

void ConvertChannels(const Image& source, uint32_t target, Image& out) {
    if (source.desc.channels == target || target == 0) {
        out = source;
        return;
    }

    ImageDesc desc = source.desc;
    desc.channels = target;
    // Nama kanal tidak ikut diubah bentuknya: begitu jumlah kanal dipaksa,
    // nama yang tersisa tidak lagi menyebut kanal yang sama. Lebih baik kosong
    // daripada salah — pemanggil yang butuh namanya membaca tanpa memaksa.
    desc.channelNames.clear();
    out.Allocate(desc);

    const std::size_t pixels = static_cast<std::size_t>(desc.width) * desc.height;
    const uint32_t from = source.desc.channels;
    switch (desc.type) {
        case PixelType::UInt8:
            ConvertChannelsTyped(source.AsU8(), out.AsU8(), pixels, from, target,
                                 static_cast<uint8_t>(255));
            break;
        case PixelType::UInt16:
            ConvertChannelsTyped(source.AsU16(), out.AsU16(), pixels, from, target,
                                 static_cast<uint16_t>(65535));
            break;
        case PixelType::Float32:
            ConvertChannelsTyped(source.AsF32(), out.AsF32(), pixels, from, target, 1.0f);
            break;
    }
}

void ApplyReadOptions(const ReadOptions& options, const std::string& what, Image& image) {
    if (options.channels != 0 && options.channels != image.desc.channels) {
        Image converted;
        ConvertChannels(image, options.channels, converted);
        image = std::move(converted);
    }

    if (options.type.has_value() && *options.type != image.desc.type) {
        // Kuantisasi dicatat, tidak dilakukan diam-diam. Ini yang membedakan
        // "heightmap 32-bit dimuat" dari "heightmap 32-bit dipotong ke 16-bit
        // dan hasilnya kelihatan masuk akal".
        if (image.desc.type == PixelType::Float32) {
            LogWarning(what + ": float samples quantised to " + ToString(*options.type) +
                       " — values outside 0..1 are clamped");
        }
        Image converted;
        ConvertType(image, *options.type, converted);
        image = std::move(converted);
    }
}

}  // namespace sim::imageio
