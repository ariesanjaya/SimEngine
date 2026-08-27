#pragma once

// Deklarasinya pindah ke `Sim/Render/Ibl.h` bersama panggangan CPU-nya: artefak
// masak ini serialisasi `IblBakeCpu`, dan yang tidak menyentuh Vulkan harus bisa
// diuji tanpa perangkat grafis. Berkas ini tinggal supaya `IblCache.cpp` dan
// pemakainya di dalam modul tidak perlu tahu perpindahan itu.
#include "Sim/Render/Ibl.h"
