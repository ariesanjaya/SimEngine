# Tambalan TBB untuk paket OpenUSD yang tidak membawa config CMake-nya.
#
# Paket USD siap pakai — yang dari packman/Omniverse, dan yang dipakai
# usd-exchange — membawa TBB lengkap di dalamnya: header di `include/tbb`,
# pustaka di `lib/libtbb.so`. Yang tidak ikut hanya `TBBConfig.cmake`, padahal
# `pxrConfig.cmake` memanggil `find_dependency(TBB)` dan berhenti tanpanya.
#
# **Menunjuk balik ke TBB milik paket USD itu, bukan ke TBB mana pun di mesin
# ini.** Keduanya bukan versi yang berbeda dari barang yang sama melainkan dua
# generasi: TBB lama bersoname `libtbb.so.2`, oneTBB `libtbb.so.12`, dengan tata
# letak objek yang berbeda. USD yang dibangun terhadap yang satu lalu dijalankan
# terhadap yang lain tidak gagal saat ditautkan — ia gagal belakangan, di dalam
# penjadwal tugasnya, pada berkas yang kebetulan cukup besar untuk diproses
# secara paralel.
if(NOT SIM_USD_ROOT)
    set(TBB_FOUND FALSE)
    return()
endif()

find_library(SIM_USD_TBB_LIBRARY NAMES tbb PATHS "${SIM_USD_ROOT}/lib" NO_DEFAULT_PATH)
if(NOT SIM_USD_TBB_LIBRARY OR NOT EXISTS "${SIM_USD_ROOT}/include/tbb")
    set(TBB_FOUND FALSE)
    return()
endif()

if(NOT TARGET TBB::tbb)
    add_library(TBB::tbb SHARED IMPORTED)
    set_target_properties(TBB::tbb PROPERTIES
        IMPORTED_LOCATION "${SIM_USD_TBB_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${SIM_USD_ROOT}/include")
endif()

set(TBB_FOUND TRUE)
