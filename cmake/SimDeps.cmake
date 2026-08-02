# Seluruh dependensi pihak ketiga, versi terkunci.
#
# Penjelasan pilihan versi ada di docs/DEPENDENCIES.md.
# Setelah konfigurasi pertama, build berikutnya tidak menyentuh jaringan.
# Untuk build sepenuhnya offline: -DFETCHCONTENT_FULLY_DISCONNECTED=ON
include_guard(GLOBAL)

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# ---------------------------------------------------------------------------
# SDL3 — jendela, input, monitor, surface Vulkan
# ---------------------------------------------------------------------------
if(SIM_SDL_SHARED)
    set(SDL_SHARED ON  CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
else()
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON  CACHE BOOL "" FORCE)
endif()
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS        OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES     OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL      OFF CACHE BOOL "" FORCE)

# SDL memperlakukan sebagian dependensi opsional sebagai wajib begitu ia
# mendeteksi platformnya, lalu gagal konfigurasi kalau paket dev-nya tidak ada.
# Alih-alih memaksa memasang paket, kita probe sendiri dan matikan yang memang
# tidak tersedia. Fitur yang dimatikan di sini tidak dipakai fase editor;
# backend audio baru relevan di E9.
if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    function(_sim_sdl_probe sdl_option pkg_name)
        set(_available OFF)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(_SIM_PROBE QUIET "${pkg_name}")
            if(_SIM_PROBE_FOUND)
                set(_available ON)
            endif()
        endif()
        set(${sdl_option} ${_available} CACHE BOOL "" FORCE)
        if(NOT _available)
            message(STATUS "SDL: ${sdl_option}=OFF (paket dev '${pkg_name}' tidak ditemukan)")
        endif()
    endfunction()

    _sim_sdl_probe(SDL_X11_XSCRNSAVER xscrnsaver)
    _sim_sdl_probe(SDL_ALSA           alsa)
    _sim_sdl_probe(SDL_PULSEAUDIO     libpulse)
    _sim_sdl_probe(SDL_PIPEWIRE       libpipewire-0.3)
    _sim_sdl_probe(SDL_JACK           jack)
    _sim_sdl_probe(SDL_SNDIO          sndio)
    _sim_sdl_probe(SDL_WAYLAND        wayland-client)
endif()
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.4.12
    GIT_SHALLOW    TRUE)

# ---------------------------------------------------------------------------
# glm — matematika
# ---------------------------------------------------------------------------
set(GLM_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.3
    GIT_SHALLOW    TRUE)

# ---------------------------------------------------------------------------
# spdlog — logging
# ---------------------------------------------------------------------------
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL       OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.17.0
    GIT_SHALLOW    TRUE)

# ---------------------------------------------------------------------------
# nlohmann/json — serialisasi aset & level, JSON-RPC untuk MCP
# ---------------------------------------------------------------------------
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install    OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    GIT_SHALLOW    TRUE)

# ---------------------------------------------------------------------------
# EnTT — storage komponen scene
# ---------------------------------------------------------------------------
set(ENTT_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(EnTT
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG        v3.16.0
    GIT_SHALLOW    TRUE)

# ---------------------------------------------------------------------------
# doctest — unit test
# ---------------------------------------------------------------------------
set(DOCTEST_WITH_TESTS      OFF CACHE BOOL "" FORCE)
set(DOCTEST_NO_INSTALL      ON  CACHE BOOL "" FORCE)
FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.5.3
    GIT_SHALLOW    TRUE)

FetchContent_MakeAvailable(SDL3 glm spdlog nlohmann_json EnTT doctest)

# ---------------------------------------------------------------------------
# Dear ImGui — branch docking (docking + multi-viewport + DPI per monitor)
#
# Repo ImGui tidak menyediakan CMakeLists, jadi targetnya kita susun sendiri.
# Tag WAJIB berakhiran -docking; tag biasa tidak punya fitur docking.
# ---------------------------------------------------------------------------
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9b-docking
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(imgui)

# Sengaja dipecah dua target. Target `imgui` hanya berisi inti ImGui dan tidak
# menyeret header Vulkan maupun SDL. Modul Editor/EditorFramework me-link target
# ini saja, sehingga aturan "Editor tidak boleh melihat Vulkan" di
# docs/ARCHITECTURE.md ditegakkan oleh build system, bukan oleh disiplin.
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    # Pembungkus std::string untuk InputText. Tanpa ini setiap field teks harus
    # disalin bolak-balik ke buffer char, dan Inspector generik di E3 tidak bisa
    # menyunting std::string tanpa kode khusus.
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp)
target_include_directories(imgui SYSTEM PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/misc/cpp)
target_compile_definitions(imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
set_target_properties(imgui PROPERTIES FOLDER "ThirdParty")
add_library(ImGui::ImGui ALIAS imgui)

# ImGuizmo — gizmo translate/rotate/scale di dalam viewport.
#
# Hanya bergantung pada inti ImGui (menggambar lewat ImDrawList), jadi ikut
# target `imgui` dan tidak melanggar aturan modul: Editor tetap tidak melihat
# Vulkan. Dibungkus di Sim::EditorFramework (Gizmo.h) supaya panel tidak
# memanggil ImGuizmo langsung — kalau suatu saat harus diganti, yang berubah
# hanya satu berkas.
FetchContent_Declare(imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    # Dipatok ke commit, bukan tag: rilis bertag terakhir ImGuizmo (1.83) jauh
    # lebih tua daripada ImGui 1.92 yang kita pakai. Commit ini sudah diuji
    # kompilasi bersih terhadapnya.
    GIT_TAG        5ab7676402ace03cdf930b2d972f59c7d03c6fa8
    GIT_SHALLOW    FALSE
    # Trik yang sama seperti VMA: SOURCE_SUBDIR diarahkan ke folder tanpa
    # CMakeLists supaya FetchContent hanya mengunduh. CMakeLists bawaan
    # ImGuizmo ikut membangun contoh berbasis GLFW dan mendefinisikan target
    # bernama `imguizmo` juga, yang akan bentrok dengan target kita.
    SOURCE_SUBDIR  src)
FetchContent_MakeAvailable(imguizmo)

add_library(imguizmo STATIC ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp)
target_include_directories(imguizmo SYSTEM PUBLIC ${imguizmo_SOURCE_DIR}/src)
target_link_libraries(imguizmo PUBLIC imgui)
set_target_properties(imguizmo PROPERTIES FOLDER "ThirdParty")
add_library(ImGuizmo::ImGuizmo ALIAS imguizmo)

add_library(imgui_backend STATIC
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(imgui_backend SYSTEM PUBLIC ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui_backend PUBLIC imgui SDL3::SDL3 Vulkan::Vulkan)
set_target_properties(imgui_backend PROPERTIES FOLDER "ThirdParty")
add_library(ImGui::Backend ALIAS imgui_backend)

# ---------------------------------------------------------------------------
# VulkanMemoryAllocator — alokasi memori GPU
#
# SOURCE_SUBDIR diarahkan ke folder tanpa CMakeLists supaya FetchContent hanya
# mengunduh, tidak menjalankan build system VMA (yang ikut membangun contoh).
# VMA header-only; VMA_IMPLEMENTATION didefinisikan di satu TU di Code/RHI.
# ---------------------------------------------------------------------------
FetchContent_Declare(vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  include)
FetchContent_MakeAvailable(vma)

add_library(vma INTERFACE)
target_include_directories(vma SYSTEM INTERFACE ${vma_SOURCE_DIR}/include)
target_link_libraries(vma INTERFACE Vulkan::Vulkan)
add_library(GPUOpen::VMA ALIAS vma)

# ---------------------------------------------------------------------------
# Lua 5.4 + sol2 — runtime scripting
# ---------------------------------------------------------------------------
if(SIM_WITH_LUA)
    FetchContent_Declare(lua
        GIT_REPOSITORY https://github.com/lua/lua.git
        GIT_TAG        v5.4.8
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(lua)

    # Repo resmi Lua tidak punya CMakeLists. Ambil semua .c kecuali entry point
    # interpreter/compiler standalone dan berkas test internal.
    file(GLOB _lua_sources ${lua_SOURCE_DIR}/*.c)
    foreach(_excluded lua.c luac.c onelua.c ltests.c)
        list(REMOVE_ITEM _lua_sources ${lua_SOURCE_DIR}/${_excluded})
    endforeach()

    add_library(lua STATIC ${_lua_sources})
    target_include_directories(lua SYSTEM PUBLIC ${lua_SOURCE_DIR})
    # LUA_USE_LINUX menyalakan POSIX + dlopen. readline hanya dipakai lua.c,
    # yang sudah kita keluarkan, jadi tidak perlu link readline.
    target_compile_definitions(lua PUBLIC LUA_USE_LINUX)
    target_link_libraries(lua PUBLIC m ${CMAKE_DL_LIBS})
    set_target_properties(lua PROPERTIES FOLDER "ThirdParty" C_STANDARD 11)
    add_library(Lua::Lua ALIAS lua)

    FetchContent_Declare(sol2
        GIT_REPOSITORY https://github.com/ThePhD/sol2.git
        GIT_TAG        v3.5.0
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  include)
    FetchContent_MakeAvailable(sol2)

    add_library(sol2 INTERFACE)
    target_include_directories(sol2 SYSTEM INTERFACE ${sol2_SOURCE_DIR}/include)
    target_link_libraries(sol2 INTERFACE lua)
    # Pemeriksaan penuh saat Debug; dimatikan di Release demi performa.
    target_compile_definitions(sol2 INTERFACE
        $<$<CONFIG:Debug>:SOL_ALL_SAFETIES_ON=1>)
    add_library(Sol2::Sol2 ALIAS sol2)
endif()

# ---------------------------------------------------------------------------
# IconFontCppHeaders — konstanta codepoint ikon
#
# Di-vendor, bukan di-fetch: berkasnya satu header hasil generate, dan
# codepoint-nya harus cocok persis dengan berkas .ttf di Resources/Fonts.
# Menautkan keduanya ke revisi yang bergerak sendiri-sendiri adalah cara paling
# mudah mendapati separuh ikon berubah jadi kotak kosong setelah update.
# ---------------------------------------------------------------------------
add_library(icon_font_headers INTERFACE)
target_include_directories(icon_font_headers SYSTEM INTERFACE
    "${CMAKE_SOURCE_DIR}/Third-Party/IconFontCppHeaders")
add_library(IconFonts::Headers ALIAS icon_font_headers)

# ---------------------------------------------------------------------------
# stb — stb_image untuk dekode tekstur dan pembuatan thumbnail
#
# Header-only dan tanpa build system, jadi SOURCE_SUBDIR diarahkan ke folder
# tanpa CMakeLists seperti VMA. STB_IMAGE_IMPLEMENTATION didefinisikan di satu
# TU saja, di Code/Assets.
# ---------------------------------------------------------------------------
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        f0569113c93ad095470c54bf34a17b36646bbbb5
    GIT_SHALLOW    FALSE
    SOURCE_SUBDIR  deprecated)
FetchContent_MakeAvailable(stb)

add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE ${stb_SOURCE_DIR})
add_library(Stb::Stb ALIAS stb)

# ---------------------------------------------------------------------------
# Ditambahkan pada milestone berikutnya (lihat docs/DEPENDENCIES.md):
#   E7.1 imgui-node-editor        graph material & state machine animasi
#   A0   cpp-httplib              transport HTTP untuk MCP server
#   E8   ufbx, cgltf, meshoptimizer
# ---------------------------------------------------------------------------
