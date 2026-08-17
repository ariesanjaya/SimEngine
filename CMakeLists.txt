cmake_minimum_required(VERSION 3.25)

project(SimEngine
    VERSION 0.1.0
    DESCRIPTION "Game engine 3D dengan editor Dear ImGui dan runtime Lua"
    LANGUAGES C CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "Build in-source dilarang. Pakai: cmake --preset linux-clang-debug")
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
endif()
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")

# --- Opsi -------------------------------------------------------------------
option(SIM_BUILD_EDITOR     "Bangun aplikasi editor"                    ON)
option(SIM_BUILD_TESTS      "Bangun unit test (doctest)"                ON)
option(SIM_WITH_LUA         "Bangun runtime scripting Lua"              ON)
# Backend gambar tambahan. Keduanya ON, dan keduanya benar-benar opsional: yang
# mematikannya tetap bisa membangun seluruh mesin, dan yang hilang cuma format
# tambahannya. Ada sebagai opsi supaya jalur tanpa keduanya bisa diuji dengan
# sengaja — jalur yang tidak pernah dijalankan siapa pun adalah jalur yang
# rusaknya baru ketahuan di mesin orang lain.
option(SIM_WITH_EXR         "Impor OpenEXR lewat tinyexr"               ON)
option(SIM_WITH_TIFF        "Impor TIFF lewat libtiff"                  ON)
# OIIO opsional dan didahulukan: bila ada ia menang untuk setiap format yang
# diklaimnya. Yang tidak menyediakannya tidak kehilangan apa pun kecuali DDS.
option(SIM_WITH_OIIO        "Backend OpenImageIO bila tersedia"          ON)
# OpenVDB: bake mesh menjadi SDF untuk clipmap GI, dan nanti impor .vdb.
# **Pengondisi aset, bukan pustaka runtime** — aturan yang sama dengan OIIO.
option(SIM_WITH_OPENVDB     "Bake SDF dan impor volume lewat OpenVDB"   ON)
# PhysX: simulasi E9. **CPU dulu** — tiga fitur PhysX (PBD, soft body FEM,
# deformable surface) menuntut CUDA lewat tanda tangan API-nya dan tidak punya
# jalur CPU sama sekali, jadi jalur GPU adalah lapisan tambahan yang absen di
# perangkat non-NVIDIA. Lihat docs/PLAN-PHYSICS.md.
option(SIM_WITH_PHYSX       "Simulasi fisika lewat PhysX 5"             ON)
option(SIM_WITH_PHYSX_GPU   "Fitur PhysX yang menuntut CUDA"            OFF)
option(SIM_WERROR           "Perlakukan warning sebagai error"          OFF)
option(SIM_STRICT_WARNINGS  "Aktifkan -Wconversion dan kawan-kawannya"  OFF)

# Build sanitizer mematikan ini. Alasannya: clang hanya menautkan runtime
# sanitizer secara statis ke executable, tidak ke shared library, sedangkan SDL
# menautkan dirinya dengan --no-undefined — hasilnya seluruh simbol __asan_*
# menjadi undefined dan link gagal. Menautkan SDL secara statis menghilangkan
# masalahnya tanpa perlu -shared-libasan beserta urusan LD_LIBRARY_PATH-nya.
option(SIM_SDL_SHARED       "Bangun SDL3 sebagai shared library"        ON)

include(SimTargets)
include(SimVulkan)
include(SimDeps)
include(SimShaders)

# --- Modul ------------------------------------------------------------------
add_subdirectory(Code/Core)
# ImageIO: dekode dan enkode gambar untuk pengondisian aset (I0). Didaftarkan
# sebelum Assets, Render, Terrain, dan Vegetation karena keempatnya memakainya —
# alias `Sim::ImageIO` harus sudah ada saat mereka menyebutnya.
add_subdirectory(Code/ImageIO)
# Volume: bake mesh menjadi SDF dan impor .vdb lewat OpenVDB (V1, V2).
# Pengondisi aset — Render dan Assets memakai hasilnya, bukan pustakanya, jadi
# ia didaftarkan sebelum keduanya.
add_subdirectory(Code/Volume)
# Physics: simulasi PhysX 5 (E9). Pengondisi maupun runtime, tapi tidak pernah
# bergantung pada Render — lihat docs/PLAN-PHYSICS.md.
add_subdirectory(Code/Physics)
add_subdirectory(Code/Whitebox)
add_subdirectory(Code/Reflect)
add_subdirectory(Code/Scene)
add_subdirectory(Code/Assets)
add_subdirectory(Code/Platform)
add_subdirectory(Code/RHI)
add_subdirectory(Code/ImGuiIntegration)
add_subdirectory(Code/Render)
# Material: model graph, katalog node, dan validasi untuk aset `.simmat` (E7.1).
# Tidak bergantung pada Lua maupun editor — graph material dikompilasi menjadi
# kode shader, bukan skrip.
add_subdirectory(Code/Material)
# Particle: modul efek, format `.simfx`, dan simulasi CPU deterministik (E7.2).
add_subdirectory(Code/Particle)
# Terrain: heightmap berubin, brush sculpt, undo per goresan, dan I/O
# heightmap PNG 16-bit/RAW (E7.3).
add_subdirectory(Code/Terrain)
# Vegetation: sebaran instance deterministik dari aturan penempatan di atas
# terrain, peta kepadatan yang dicat, dan format `.simveg` (E7.4).
add_subdirectory(Code/Vegetation)
# Animation: rangka, klip berbasis kurva, pencampuran pose, dan graph state
# machine untuk `.simskel`/`.simanim`/`.simanimgraph` (E7.5).
add_subdirectory(Code/Animation)
if(SIM_WITH_LUA)
    add_subdirectory(Code/Script)
endif()
# Sebelum EditorFramework: yang menyalakan server dan mendaftarkan tool-nya
# adalah EditorApp, jadi ia melihat AIBridge — bukan sebaliknya. AIBridge sendiri
# tidak tahu apa-apa tentang editor.
add_subdirectory(Code/AIBridge)
add_subdirectory(Code/EditorFramework)
add_subdirectory(Code/Editor)

if(SIM_BUILD_EDITOR)
    add_subdirectory(Apps/SimEditor)
    # Ikut SIM_BUILD_EDITOR karena ia menyusun inti editor yang sama. Ia tidak
    # menautkan panel maupun SDL — lihat alasannya di CMakeLists-nya.
    add_subdirectory(Apps/SimHeadless)
    # Player (E9). Ikut SIM_BUILD_EDITOR untuk sementara: ia masih menautkan
    # EditorFramework demi SceneView — lihat CMakeLists-nya.
    add_subdirectory(Apps/SimRuntime)
endif()

# Alat berkas: tidak menuntut GPU, jadi ia dibangun juga di build tanpa editor.
add_subdirectory(Apps/SimCook)

if(SIM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Tests)
endif()

# --- Ringkasan konfigurasi --------------------------------------------------
message(STATUS "")
message(STATUS "SimEngine ${PROJECT_VERSION}")
message(STATUS "  compiler     : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  vulkan       : ${Vulkan_VERSION} (${Vulkan_LIBRARY})")
message(STATUS "  editor       : ${SIM_BUILD_EDITOR}")
message(STATUS "  lua          : ${SIM_WITH_LUA}")
message(STATUS "  tests        : ${SIM_BUILD_TESTS}")
message(STATUS "")
