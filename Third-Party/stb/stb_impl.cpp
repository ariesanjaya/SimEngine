// Satu-satunya tempat implementasi stb dikompilasi di seluruh build.
//
// Header stb menaruh implementasinya di balik makro, jadi ia harus di-define
// tepat sekali. Selama hanya satu modul yang memakai gambar, "tepat sekali" bisa
// dijaga dengan menaruhnya di modul itu — tapi begitu modul kedua ikut memakainya
// (Assets untuk tekstur dan thumbnail, Terrain untuk heightmap PNG 16-bit),
// keduanya membawa definisi yang sama dan penautan gagal.
//
// Menaruhnya di satu pustaka kecil menyelesaikannya sekali untuk seterusnya:
// pemakai berikutnya cukup menautkan `Stb::Impl` dan tidak perlu tahu bahwa
// masalah ini pernah ada.

#include "stb_impl.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

namespace stb_impl {

std::vector<unsigned char> Deflate(const unsigned char* data, std::size_t length, int quality) {
    std::vector<unsigned char> out;
    if (data == nullptr || length == 0) {
        return out;
    }
    int compressed = 0;
    unsigned char* buffer = stbi_zlib_compress(const_cast<unsigned char*>(data),
                                               static_cast<int>(length), &compressed, quality);
    if (buffer == nullptr) {
        return out;
    }
    out.assign(buffer, buffer + compressed);
    STBIW_FREE(buffer);
    return out;
}

}  // namespace stb_impl
