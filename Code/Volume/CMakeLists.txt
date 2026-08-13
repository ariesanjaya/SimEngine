sim_add_library(Volume
    SOURCES
        src/SdfBake.cpp
        src/VdbImport.cpp
        src/VolumeRaymarch.cpp
    PUBLIC_DEPS
        # PUBLIC: `SdfBake.h` menyebut `sim::SdfGrid` dan `sim::Vec3` di tanda
        # tangannya. Keduanya tipe biasa di Sim::Core — dan itu memang seluruh
        # maksudnya: yang keluar dari modul ini tidak membawa satu pun tipe
        # OpenVDB, sehingga renderer bisa memakainya tanpa menautkan pustakanya.
        Sim::Core)

# OpenVDB hanya kalau ada; SdfBake.cpp tetap ikut dibangun tanpanya dan menolak
# dengan pesan yang menyebut sebabnya, mengikuti pola UsdImport.cpp.
if(TARGET OpenVdb::OpenVdb)
    target_link_libraries(SimVolume PRIVATE OpenVdb::OpenVdb)
    target_compile_definitions(SimVolume PRIVATE SIM_WITH_OPENVDB=1)
endif()
