# Helper pembuatan target supaya semua modul SimEngine seragam.
include_guard(GLOBAL)

set(SIM_COMMON_WARNINGS
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wimplicit-fallthrough
    -Wnull-dereference
    -Wformat=2)

# Sengaja dipisah: -Wconversion sangat berisik di kode yang banyak berurusan
# dengan ImGui/Vulkan. Diaktifkan lewat -DSIM_STRICT_WARNINGS=ON saat pembersihan.
set(SIM_STRICT_WARNING_FLAGS
    -Wconversion
    -Wsign-conversion
    -Wdouble-promotion
    -Wold-style-cast)

function(sim_target_defaults target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_compile_options(${target} PRIVATE ${SIM_COMMON_WARNINGS})
    if(SIM_STRICT_WARNINGS)
        target_compile_options(${target} PRIVATE ${SIM_STRICT_WARNING_FLAGS})
    endif()
    if(SIM_WERROR)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
    target_compile_definitions(${target} PUBLIC
        $<$<CONFIG:Debug>:SIM_DEBUG=1>
        $<$<NOT:$<CONFIG:Debug>>:SIM_DEBUG=0>)
    if(WIN32)
        # CRT Microsoft menandai deprecated sederet fungsi C standar — `getenv`,
        # `fopen`, `strcpy` — dan menawarkan penggantinya yang hanya ada di
        # Windows. Menurutinya berarti setiap pemakaian bercabang dua, di kode
        # yang seluruhnya portabel; membiarkannya berarti -Werror menggagalkan
        # build atas fungsi yang dijamin standar. Definisi ini adalah jalan
        # keluar resmi Microsoft untuk pilihan ketiga: pakai yang standar.
        #
        # Yang **bukan** ditutupinya: teks yang keluar dari fungsi-fungsi itu di
        # Windows sudah dikonversi ke codepage ANSI mesin, jadi nilai yang memuat
        # karakter di luar codepage itu kembali rusak. Path karena itu tetap
        # tidak boleh melewatinya — lihat `Sim/Core/UserPaths.h`, yang memakai
        # API lebar untuk alasan ini.
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    set_target_properties(${target} PROPERTIES FOLDER "SimEngine")
endfunction()

# sim_add_library(Nama
#     SOURCES      src/a.cpp src/b.cpp
#     PUBLIC_DEPS  Sim::Core glm::glm
#     PRIVATE_DEPS SDL3::SDL3)
#
# Menghasilkan target `SimNama` dan alias `Sim::Nama`.
# include/ jadi PUBLIC, src/ jadi PRIVATE — inilah yang menegakkan batas modul:
# modul lain tidak bisa meng-include header internal.
function(sim_add_library name)
    cmake_parse_arguments(ARG "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})

    add_library(Sim${name} STATIC ${ARG_SOURCES})
    add_library(Sim::${name} ALIAS Sim${name})

    target_include_directories(Sim${name}
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    target_link_libraries(Sim${name}
        PUBLIC  ${ARG_PUBLIC_DEPS}
        PRIVATE ${ARG_PRIVATE_DEPS})

    sim_target_defaults(Sim${name})
endfunction()

function(sim_add_executable name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})

    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE ${ARG_DEPS})
    sim_target_defaults(${name})
    set_target_properties(${name} PROPERTIES FOLDER "SimEngine/Apps")
endfunction()

function(sim_add_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})

    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE doctest::doctest ${ARG_DEPS})
    sim_target_defaults(${name})
    set_target_properties(${name} PROPERTIES FOLDER "SimEngine/Tests")
    add_test(NAME ${name} COMMAND ${name})
endfunction()
