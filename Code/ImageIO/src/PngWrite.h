#pragma once

#include "Sim/ImageIO/Image.h"

#include <cstdint>
#include <vector>

namespace sim::imageio {

/// Menulis PNG 8-bit atau 16-bit ke memori: greyscale, RGB, atau RGBA.
///
/// Ada karena tidak satu pun pustaka yang dipakai backend lain menulis PNG
/// 16-bit: `stbi_write_png` hanya 8 bit, tinyexr menulis EXR, dan libtiff
/// menulis TIFF. Dan 8 bit adalah 256 tingkat tinggi — langkah empat meter pada
/// terrain setinggi kilometer.
///
/// **Berwarna menyusul greyscale, dan alasannya bukan kelengkapan.**
/// `editor.screenshot` mengirim jendela editor ke agen sebagai PNG, dan jendela
/// editor tidak pernah satu kanal. Yang menahannya selama ini bukan enkodernya —
/// tata letak filter PNG memang per-byte-piksel, jadi kanal tambahan hanya
/// mengubah `bpp` — melainkan janji untuk tidak menulis berkas yang tidak
/// pernah dibaca satu pun uji.
///
/// Mengembalikan false bila kanalnya bukan 1, 3, atau 4, atau tipenya float.
bool EncodePng(const Image& image, std::vector<uint8_t>& out);

}  // namespace sim::imageio
