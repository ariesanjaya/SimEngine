// Implementasi cgltf, dikompilasi tepat sekali di sini.
//
// **Sebagai C, bukan C++**: cgltf menulis
// C99 dan mengandalkan perilaku yang berbeda di kedua bahasa.
//
// Satu TU, dengan alasan yang sama seperti stb_impl.cpp: dua modul yang
// masing-masing men-define makro implementasinya akan membawa definisi ganda.
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
