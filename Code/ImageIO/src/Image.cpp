#include "Sim/ImageIO/Image.h"

namespace sim::imageio {

std::size_t BytesPerSample(PixelType type) {
    switch (type) {
        case PixelType::UInt8: return 1;
        case PixelType::UInt16: return 2;
        case PixelType::Float32: return 4;
    }
    return 1;
}

const char* ToString(PixelType type) {
    switch (type) {
        case PixelType::UInt8: return "uint8";
        case PixelType::UInt16: return "uint16";
        case PixelType::Float32: return "float32";
    }
    return "unknown";
}

const char* ToString(ColorSpace space) {
    switch (space) {
        case ColorSpace::Unknown: return "unknown";
        case ColorSpace::Linear: return "linear";
        case ColorSpace::Srgb: return "sRGB";
    }
    return "unknown";
}

const uint8_t* Image::AsU8() const {
    return desc.type == PixelType::UInt8 ? bytes.data() : nullptr;
}

const uint16_t* Image::AsU16() const {
    return desc.type == PixelType::UInt16 ? reinterpret_cast<const uint16_t*>(bytes.data())
                                          : nullptr;
}

const float* Image::AsF32() const {
    return desc.type == PixelType::Float32 ? reinterpret_cast<const float*>(bytes.data()) : nullptr;
}

uint8_t* Image::AsU8() { return desc.type == PixelType::UInt8 ? bytes.data() : nullptr; }

uint16_t* Image::AsU16() {
    return desc.type == PixelType::UInt16 ? reinterpret_cast<uint16_t*>(bytes.data()) : nullptr;
}

float* Image::AsF32() {
    return desc.type == PixelType::Float32 ? reinterpret_cast<float*>(bytes.data()) : nullptr;
}

void Image::Allocate(const ImageDesc& description) {
    desc = description;
    bytes.assign(desc.ByteCount(), 0);
}

}  // namespace sim::imageio
