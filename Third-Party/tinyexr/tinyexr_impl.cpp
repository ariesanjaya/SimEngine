// Satu-satunya tempat implementasi tinyexr dikompilasi di seluruh build.
//
// Alasannya sama persis dengan `stb_impl.cpp` di sebelahnya: header tinyexr
// menaruh implementasinya di balik makro, jadi ia harus di-define tepat sekali.
// Menaruhnya di pustaka kecil tersendiri menyelesaikannya sekali untuk
// seterusnya, dan sekaligus memberi tempat menaruh `-w` — sebelas ribu baris
// kode pihak ketiga tidak akan pernah lolos dari -Wall -Wextra -Werror proyek
// ini, dan menyetel pengecualiannya di target modul akan ikut mematikan
// peringatan untuk kode yang memang ditulis di sini.
//
// **zlib dipakai, bukan miniz yang dibundel.** Keduanya sama-sama bisa; yang
// membedakan adalah bahwa zlib sudah ada di mesin ini sebagai dependensi
// libpng dan libtiff, jadi memakai miniz berarti dua implementasi deflate di
// satu binary tanpa membeli apa pun.

#include <zlib.h>

#define TINYEXR_USE_MINIZ 0
#define TINYEXR_USE_STB_ZLIB 0
#define TINYEXR_IMPLEMENTATION
#include <tinyexr.h>
