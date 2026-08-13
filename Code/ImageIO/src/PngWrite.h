#pragma once

#include "Sim/ImageIO/Image.h"

#include <cstdint>
#include <vector>

namespace sim::imageio {

/// Menulis PNG greyscale 8-bit atau 16-bit ke memori.
///
/// Ada karena tidak satu pun pustaka yang dipakai backend lain menulis PNG
/// 16-bit: `stbi_write_png` hanya 8 bit, tinyexr menulis EXR, dan libtiff
/// menulis TIFF. Dan 8 bit adalah 256 tingkat tinggi — langkah empat meter pada
/// terrain setinggi kilometer.
///
/// Mengembalikan false bila gambarnya bukan satu kanal, atau tipenya float.
bool EncodeGreyscalePng(const Image& image, std::vector<uint8_t>& out);

}  // namespace sim::imageio
