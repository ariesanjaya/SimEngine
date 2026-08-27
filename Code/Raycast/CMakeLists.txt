# Ray query CPU — R0 di docs/PLAN-EMBREE.md.
#
# **Yang tidak ada di daftar dependensi ini yang penting:** Sim::RHI dan
# Sim::Render. Modul ini murni CPU dan geometri, dan itulah yang membuatnya bisa
# dipakai SimHeadless tanpa satu pun display — beserta seluruh nilai CI-nya.
#
# Sim::Assets juga tidak ada, dan itu disengaja: modul ini menerima posisi dan
# indeks mentah, bukan `MeshData`. Yang menjembatani keduanya adalah R1, dan ia
# tinggal di sisi pemanggil.
# **Hanya satu backend yang ikut dibangun.** Keduanya mendefinisikan kelas
# bernama sama, dan `src/SceneBackend.h` memilih yang mana — lihat catatannya di
# sana. Yang tidak dipilih tidak dikompilasi sama sekali, jadi tidak ada kode
# mati di dalam binary dan tidak ada dua definisi yang bisa bertemu.
if(SIM_WITH_EMBREE)
    set(_sim_raycast_backend src/EmbreeBackend.cpp)
else()
    set(_sim_raycast_backend src/BvhBackend.cpp)
endif()

sim_add_library(Raycast
    SOURCES
        src/Bvh.cpp
        src/RayScene.cpp
        src/Query.cpp
        ${_sim_raycast_backend}
    PUBLIC_DEPS
        # `Vec3`, `Mat4`, dan Möller–Trumbore bersama di Sim/Core/Intersect.h.
        Sim::Core)

# **Aturannya ditegakkan, bukan dituliskan lalu diharapkan.** Sebuah `#include`
# yang menyeret Sim::RHI ke sini tidak menghasilkan galat apa pun — ia hanya
# membuat SimHeadless berhenti bisa dibangun di mesin tanpa Vulkan, beberapa
# bulan kemudian, jauh dari sebabnya. Yang menemukannya harus konfigurasi, bukan
# mesin build orang lain.
function(sim_raycast_assert_no_gpu_deps target)
    get_target_property(_deps ${target} LINK_LIBRARIES)
    if(NOT _deps)
        return()
    endif()
    foreach(_dep IN LISTS _deps)
        string(REPLACE "Sim::" "Sim" _normalised "${_dep}")
        if(_normalised STREQUAL "SimRHI" OR _normalised STREQUAL "SimRender")
            message(FATAL_ERROR
                "Sim::Raycast bergantung pada ${_dep}. Modul ini harus tetap murni CPU — "
                "lihat 'Yang tidak boleh ditunda' di docs/PLAN-EMBREE.md.")
        endif()
        if(TARGET ${_normalised})
            sim_raycast_assert_no_gpu_deps(${_normalised})
        endif()
    endforeach()
endfunction()

if(SIM_WITH_EMBREE)
    # **Privat, dan definisinya juga.** Yang menautkan Sim::Raycast tidak boleh
    # ikut menautkan Embree, dan tidak ada satu pun header publik yang
    # menyebutnya — lihat catatan di src/EmbreeBackend.h.
    target_link_libraries(SimRaycast PRIVATE embree)
    target_compile_definitions(SimRaycast PRIVATE SIM_WITH_EMBREE=1)
endif()

sim_raycast_assert_no_gpu_deps(SimRaycast)
