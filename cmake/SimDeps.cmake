# Seluruh dependensi pihak ketiga, versi terkunci.
#
# Penjelasan pilihan versi ada di docs/DEPENDENCIES.md.
# Setelah konfigurasi pertama, build berikutnya tidak menyentuh jaringan.
# Untuk build sepenuhnya offline: -DFETCHCONTENT_FULLY_DISCONNECTED=ON
include_guard(GLOBAL)

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# Dipakai FetchContent untuk mengambil sumber, dan oleh ApplyPatch.cmake untuk
# menambal salah satunya. Diminta eksplisit supaya ketiadaannya dilaporkan di
# sini, bukan sebagai kegagalan yang membingungkan di tengah langkah patch.
find_package(Git REQUIRED)

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

# ---------------------------------------------------------------------------
# cpp-httplib — transport HTTP untuk MCP server (A0, lihat docs/PLAN-AI.md)
#
# **Setiap kemampuan opsionalnya dimatikan secara eksplisit, bukan dibiarkan
# terdeteksi.** Bawaan cpp-httplib adalah `USE_*_IF_AVAILABLE`: ia memindai
# mesin yang sedang membangun dan menyalakan OpenSSL, zlib, atau Brotli kalau
# kebetulan ada. Artinya dua orang yang membangun commit yang sama mendapat
# pustaka dengan kemampuan berbeda — dan yang menemukannya adalah orang ketiga
# yang binernya menuntut .so yang tidak ada di mesinnya.
#
# TLS memang tidak dibutuhkan di sini dan bukan karena disederhanakan: server
# MCP terkunci ke 127.0.0.1 dan tidak punya opsi bind ke alamat lain, jadi tidak
# pernah ada byte yang meninggalkan mesin ini untuk dienkripsi.
#
# SYSTEM: header-nya sepuluh ribu baris milik orang lain, dan modul ini
# dibangun dengan -Werror.
# ---------------------------------------------------------------------------
set(HTTPLIB_REQUIRE_OPENSSL         OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_ZLIB            OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_BROTLI          OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE    OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE  OFF CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL                 OFF CACHE BOOL "" FORCE)
set(HTTPLIB_TEST                    OFF CACHE BOOL "" FORCE)
# **Bawaannya ON, dan mematikannya bukan penyederhanaan.** Jalur ini memakai
# `getaddrinfo_a`, yang dilayani glibc lewat kolam thread yang ia buat sendiri.
# ThreadSanitizer tidak mengenal thread itu — cache alokator per-thread miliknya
# belum ada di sana — jadi yang terjadi bukan laporan race melainkan segfault di
# dalam sanitizer, pada uji apa pun yang benar-benar menyambung. Kriteria terima
# A0 nomor 3 menuntut jalan di bawah TSan, dan ini yang menghalanginya.
#
# Yang hilang tidak ada: jalur non-blocking ada supaya server DNS yang macet
# tidak menahan thread selama timeout resolver, dan satu-satunya alamat yang
# pernah disambungi di sini adalah 127.0.0.1 — yang tidak pernah menyentuh DNS.
set(HTTPLIB_USE_NON_BLOCKING_GETADDRINFO OFF CACHE BOOL "" FORCE)
FetchContent_Declare(cpp-httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.53.1
    GIT_SHALLOW    TRUE
    SYSTEM)

FetchContent_MakeAvailable(SDL3 glm spdlog nlohmann_json EnTT doctest cpp-httplib)

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

# imgui-node-editor — kanvas node untuk visual scripting (E6.5), dipakai lagi
# untuk graph material (E7.1) dan state machine animasi (E7.5).
#
# Seperti ImGuizmo: hanya bergantung pada inti ImGui, jadi ikut target `imgui`
# dan tidak melanggar aturan "Editor tidak melihat Vulkan". Dibungkus di
# Sim::EditorFramework supaya panel tidak memanggilnya langsung.
FetchContent_Declare(imgui_node_editor
    GIT_REPOSITORY https://github.com/thedmd/imgui-node-editor.git
    # Dipatok ke commit di master, bukan tag maupun develop. Rilis bertag
    # terakhir (v0.9.1) dan HEAD develop sama-sama mendahului ImGui 1.92, yang
    # membuang ImRect::Floor() dan ImGui::GetKeyIndex(); commit ini memuat
    # penggantinya. Menaikkannya kembali ke develop berarti node editor tidak
    # bisa dikompilasi sama sekali, bukan sekadar peringatan.
    GIT_TAG        021aa0ea4da13fed864bafb2a92d4c5205076866
    GIT_SHALLOW    FALSE
    # CMakeLists bawaannya ikut membangun contoh berbasis GLFW/DX11 dan mencari
    # dependensinya sendiri. Trik yang sama seperti VMA dan ImGuizmo: SOURCE_SUBDIR
    # diarahkan ke folder tanpa CMakeLists supaya FetchContent hanya mengunduh.
    SOURCE_SUBDIR  misc
    # Dua patch kecil yang belum ada di hulu, di cmake/patches/:
    #
    # - math-operators: imgui_extra_math mendefinisikan `operator*(float, ImVec2)`
    #   tanpa syarat, sedangkan ImGui 1.92 sudah mendefinisikannya sendiri di
    #   dalam blok IMGUI_DEFINE_MATH_OPERATORS. Penjaganya memakai
    #   IMGUI_DEFINE_MATH_OPERATORS_IMPLEMENTED — makro yang ditetapkan ImGui
    #   persis ketika ia menyediakan operator itu — jadi patch ini tidak perlu
    #   menebak nomor versi dan tetap benar setelah ImGui dinaikkan.
    #
    # - context-menu-button: area klik latar kanvas hanya mendaftarkan tombol
    #   yang dipakai drag/select/navigate, dan MELEWATKAN tombol menu konteks.
    #   Selama ketiganya kebetulan memakai tombol yang sama dengan menu konteks
    #   ia bekerja; begitu keduanya dipisah — pan ke tombol tengah, menu tetap
    #   di kanan — menu konteks latar berhenti muncul sama sekali.
    PATCH_COMMAND ${CMAKE_COMMAND}
        -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
        -DPATCH_DIR=${CMAKE_CURRENT_LIST_DIR}/patches
        -P ${CMAKE_CURRENT_LIST_DIR}/ApplyPatch.cmake)
FetchContent_MakeAvailable(imgui_node_editor)

add_library(imgui_node_editor STATIC
    ${imgui_node_editor_SOURCE_DIR}/imgui_node_editor.cpp
    ${imgui_node_editor_SOURCE_DIR}/imgui_node_editor_api.cpp
    ${imgui_node_editor_SOURCE_DIR}/imgui_canvas.cpp
    ${imgui_node_editor_SOURCE_DIR}/crude_json.cpp)
target_include_directories(imgui_node_editor SYSTEM PUBLIC ${imgui_node_editor_SOURCE_DIR})
target_link_libraries(imgui_node_editor PUBLIC imgui)
set_target_properties(imgui_node_editor PROPERTIES FOLDER "ThirdParty")
add_library(ImGuiNodeEditor::ImGuiNodeEditor ALIAS imgui_node_editor)

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

# Implementasinya dikompilasi tepat sekali, di sini. Lihat catatan panjangnya di
# Third-Party/stb/stb_impl.cpp — singkatnya, dua modul yang sama-sama memakai
# gambar akan membawa definisi ganda kalau masing-masing men-define makronya.
add_library(stb_impl STATIC "${CMAKE_SOURCE_DIR}/Third-Party/stb/stb_impl.cpp")
target_link_libraries(stb_impl PUBLIC stb)
target_include_directories(stb_impl SYSTEM PUBLIC "${CMAKE_SOURCE_DIR}/Third-Party/stb")
# Kode pihak ketiga, jadi peringatannya bukan urusan kita — dan -Werror proyek
# ini akan menggagalkan build karenanya.
target_compile_options(stb_impl PRIVATE -w)
add_library(Stb::Impl ALIAS stb_impl)

# ---------------------------------------------------------------------------
# tinyexr — OpenEXR untuk `Sim::ImageIO` (I2)
#
# **Opsional, dan satu header saja.** Yang dibutuhkan mesin ini dari EXR cuma
# membacanya untuk IBL, dan tinyexr menangani ZIP, PIZ, dan RLE — yang benar-benar
# dipakai berkas EXR di jalur kerja ini. Diambil seperti stb dan cgltf.
#
# Pustaka gambar yang lebih besar sempat ditimbang dan ditolak; ukurannya beserta
# apa yang hilang karenanya ada di docs/DEPENDENCIES.md.
#
# SOURCE_SUBDIR diarahkan ke folder tanpa CMakeLists, trik yang sama seperti VMA
# dan stb: CMakeLists bawaan tinyexr ikut membangun contoh dan test-nya.
# ---------------------------------------------------------------------------
if(SIM_WITH_EXR)
    FetchContent_Declare(tinyexr
        GIT_REPOSITORY https://github.com/syoyo/tinyexr.git
        GIT_TAG        v3.2.0
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  doc)
    FetchContent_MakeAvailable(tinyexr)

    # zlib dipakai, bukan miniz yang dibundel tinyexr: zlib sudah ada sebagai
    # kebergantungan libpng dan libtiff, jadi miniz hanya akan menaruh
    # implementasi deflate kedua di binary yang sama.
    find_package(ZLIB QUIET)
endif()
if(SIM_WITH_EXR AND ZLIB_FOUND)
    # Implementasinya dikompilasi tepat sekali, dengan alasan yang sama seperti
    # stb — lihat catatannya di Third-Party/tinyexr/tinyexr_impl.cpp.
    add_library(tinyexr_impl STATIC "${CMAKE_SOURCE_DIR}/Third-Party/tinyexr/tinyexr_impl.cpp")
    target_include_directories(tinyexr_impl SYSTEM PUBLIC ${tinyexr_SOURCE_DIR})
    target_link_libraries(tinyexr_impl PUBLIC ZLIB::ZLIB)
    # Kode pihak ketiga sebelas ribu baris; peringatannya bukan urusan kita, dan
    # -Werror proyek ini akan menggagalkan build karenanya.
    target_compile_options(tinyexr_impl PRIVATE -w)
    set_target_properties(tinyexr_impl PROPERTIES FOLDER "ThirdParty")
    add_library(TinyExr::TinyExr ALIAS tinyexr_impl)
    message(STATUS "tinyexr v3.2.0 dipakai — impor EXR aktif")
elseif(NOT SIM_WITH_EXR)
    message(STATUS "impor EXR dimatikan (-DSIM_WITH_EXR=OFF)")
else()
    message(STATUS "zlib tidak ditemukan — impor EXR dilewati "
                   "(pasang zlib1g-dev untuk mengaktifkannya)")
endif()

# ---------------------------------------------------------------------------
# KTX-Software (libktx) — penulis kontainer KTX2 untuk baker tekstur (T2)
#
# **Menulis saja, dan hanya di sisi editor.** Pembacanya ditulis tangan di
# `Sim::RHI` pada T0 dan tetap di sana: runtime yang dikirim bersama game hanya
# perlu membaca dua ratus baris tata letak berkas, dan menyeret libktx ke dalam
# binary itu untuk pekerjaan sebesar itu adalah harga tanpa imbalan.
#
# Yang dibayar di sini adalah **Data Format Descriptor**. Blok itu wajib ada di
# setiap KTX2 dan tidak dibaca pembaca kita sendiri — jadi menyusunnya salah
# menghasilkan berkas yang dibuka sempurna oleh mesin ini dan ditolak setiap alat
# lain, tanpa satu pun tanda sampai seseorang mencoba membukanya di tempat lain.
# Itu persis bentuk kesalahan yang tidak boleh dipilih sendiri.
#
# Dan karena penulisnya libktx sementara pembacanya bukan, uji round-trip di
# `SimAssetTests` menjadi perbandingan dua implementasi yang berbeda — bukan
# pembuktian bahwa satu implementasi konsisten dengan dirinya sendiri, yang persis
# keberatan yang ditulis di `Sim/RHI/Ktx2.h`.
#
# Supercompression Zstd sengaja tidak dipakai: pembaca di `Sim::RHI` menolaknya,
# dan baker yang menghasilkan berkas yang tidak bisa dibaca runtime-nya sendiri
# bukan penghematan.
# ---------------------------------------------------------------------------
set(KTX_FEATURE_TESTS         OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_TOOLS         OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_LOADTEST_APPS OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_DOC           OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_JNI           OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_PY            OFF CACHE BOOL "" FORCE)
# Unggahan GL dan Vulkan bawaan libktx dimatikan: yang mengunggah adalah
# `Sim::RHI`, dan menyalakannya berarti libktx ikut menautkan loader Vulkan ke
# dalam target yang tidak menyentuh GPU sama sekali.
#
# `KTX_FEATURE_KTX1` sengaja **dibiarkan menyala** meski tidak ada satu pun
# berkas KTX1 di jalur kerja ini: mematikannya pada v4.4.2 meninggalkan
# `ktxTexture1_constructFromStreamAndHeader` yang masih dipanggil `texture.c`,
# dan build-nya gagal saat menaut. Kegagalannya di pustaka orang lain, jadi yang
# bisa dilakukan hanyalah tidak memakainya. Disetel eksplisit, bukan dibiarkan
# memakai bawaannya, supaya cache lama yang sudah terlanjur berisi OFF ikut
# diperbaiki alih-alih gagal menaut dengan pesan yang tidak menyebut sebabnya.
set(KTX_FEATURE_KTX1          ON  CACHE BOOL "" FORCE)
set(KTX_FEATURE_GL_UPLOAD     OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_VK_UPLOAD     OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_ETC_UNPACK    OFF CACHE BOOL "" FORCE)

FetchContent_Declare(ktx
    GIT_REPOSITORY https://github.com/KhronosGroup/KTX-Software.git
    GIT_TAG        v4.4.2
    GIT_SHALLOW    TRUE
    # basisu, astc-encoder, dan dfdutils ikut sebagai submodule; yang terakhir
    # itulah yang menyusun DFD.
    GIT_SUBMODULES_RECURSE TRUE)

# Statis, dan hanya untuk blok ini. `BUILD_SHARED_LIBS` adalah variabel global:
# memaksanya lewat cache akan ikut mengubah setiap dependensi yang dideklarasikan
# sesudah baris ini. Variabel biasa cukup karena CMP0077 membuat `option()` di
# dalam libktx menghormatinya, dan nilainya dikembalikan begitu selesai.
set(_sim_saved_shared_libs ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(ktx)
set(BUILD_SHARED_LIBS ${_sim_saved_shared_libs})

# Peringatan pustaka pihak ketiga bukan urusan kita, dan -Werror proyek ini akan
# menggagalkan build karenanya.
foreach(_ktx_target ktx ktx_read obj_basisu_cbind objUtil)
    if(TARGET ${_ktx_target})
        target_compile_options(${_ktx_target} PRIVATE -w)
        set_target_properties(${_ktx_target} PROPERTIES FOLDER "ThirdParty")

        # libktx menaruh `_DEBUG` dan `DEBUG` sebagai definisi **PUBLIC** pada
        # build Debug, jadi keduanya menular ke setiap target yang menautkannya.
        # `_DEBUG` adalah ejaan CRT-nya MSVC, dan oneTBB — yang ikut lewat
        # OpenUSD — menulis `#define TBB_USE_DEBUG _DEBUG` lalu `#if
        # TBB_USE_ASSERT`. Definisi tanpa nilai membuat baris itu menjadi
        # "expected value in expression" di setiap berkas yang menyentuh USD,
        # jauh dari sebabnya. Yang dibutuhkan libktx untuk dirinya sendiri tetap
        # ada; yang dicabut hanya yang menular.
        # Yang dicabut hanya properti INTERFACE-nya; `COMPILE_DEFINITIONS`
        # target itu sendiri tidak disentuh, jadi libktx tetap dibangun dengan
        # keduanya seperti yang dimaksudkan penulisnya.
        #
        # Disaring dengan regex, bukan REMOVE_ITEM: nilainya sebuah generator
        # expression yang mengandung titik koma, jadi CMake sudah memecahnya
        # menjadi dua item — `$<$<CONFIG:Debug>:_DEBUG` dan `DEBUG>` — dan
        # mencocokkan teks utuhnya tidak akan pernah kena.
        get_target_property(_ktx_defs ${_ktx_target} INTERFACE_COMPILE_DEFINITIONS)
        if(_ktx_defs)
            list(FILTER _ktx_defs EXCLUDE REGEX "DEBUG")
            set_property(TARGET ${_ktx_target} PROPERTY INTERFACE_COMPILE_DEFINITIONS
                         "${_ktx_defs}")
        endif()
    endif()
endforeach()
add_library(Ktx::Ktx ALIAS ktx)
message(STATUS "libktx v4.4.2 dipakai — baker tekstur menulis .ktx2")

# ---------------------------------------------------------------------------
# bc7enc_rdo — encoder BC1/BC3/BC4/BC5/BC7 untuk baker tekstur (T2)
#
# **Dipilih karena tidak menuntut apa pun.** ISPC Texture Compressor lebih cepat
# beberapa kali lipat, dan harganya sebuah compiler baru di setiap mesin build —
# keberatan yang sama persis yang menolak OSPRay di PLAN-RENDER. Yang ini C++
# biasa, MIT, dan dua berkas.
#
# `bc7decomp.cpp` ikut karena sisi urainya dibutuhkan dua hal yang sama-sama
# nyata: uji yang memeriksa apa yang sungguh keluar dari encoder, dan
# perhitungan PSNR di T5. Tanpa sisi urai, satu-satunya cara memeriksa hasil
# kompresi adalah melihatnya.
#
# BC6H tidak ada di sini, dan itu bukan pilihan — repo ini memang tidak
# memuatnya. HDR karena itu tetap tanpa kompresi sampai T4 membawa encoder yang
# punya.
#
# SOURCE_SUBDIR diarahkan ke folder yang tidak ada, trik yang sama seperti stb
# dan tinyexr: CMakeLists bawaannya membangun sebuah alat baris perintah beserta
# lodepng dan miniz-nya.
# ---------------------------------------------------------------------------
FetchContent_Declare(bc7enc
    GIT_REPOSITORY https://github.com/richgel999/bc7enc_rdo.git
    GIT_TAG        b9438627eef73a1157e84201b6fa6eb2ffd6d9f0
    GIT_SHALLOW    FALSE
    SOURCE_SUBDIR  cmake-sengaja-tidak-ada)
FetchContent_MakeAvailable(bc7enc)

add_library(bc7enc STATIC
    "${bc7enc_SOURCE_DIR}/rgbcx.cpp"
    "${bc7enc_SOURCE_DIR}/bc7enc.cpp"
    "${bc7enc_SOURCE_DIR}/bc7decomp.cpp")
target_include_directories(bc7enc SYSTEM PUBLIC "${bc7enc_SOURCE_DIR}")
target_compile_options(bc7enc PRIVATE -w)
set_target_properties(bc7enc PROPERTIES FOLDER "ThirdParty" POSITION_INDEPENDENT_CODE ON)
add_library(Bc7Enc::Bc7Enc ALIAS bc7enc)

# ---------------------------------------------------------------------------
# OpenImageIO — backend gambar opsional yang **didahulukan** (I1)
#
# **Opsional dan didahulukan, bukan wajib.** stb, tinyexr, dan libtiff menangani
# seluruh format yang dipakai jalur kerja di sini, jadi build bersih tidak
# membutuhkan apa pun dari blok ini. Yang ditambahkan OIIO ketika ada: impor
# DDS, PSD yang utuh (bukan composited view saja), metadata colorspace untuk PNG
# dan JPEG, dan `ImageBufAlgo` untuk pembandingan gambar di I5.
#
# Alasan panjangnya — termasuk kenapa ia tidak dijadikan syarat — ada di
# docs/DEPENDENCIES.md.
#
# **Root sendiri, bukan menumpang paket OpenUSD.** Distribusi OpenUSD kebetulan
# membawa header OIIO, dan menaruhnya di sana berarti header itu masuk include
# path setiap target yang menautkan `Usd::Usd` — termasuk target yang tidak
# boleh melihatnya. Root terpisah menutup itu secara struktural: hanya
# `Sim::ImageIO` yang melihat headernya.
#
# **Diperiksa dua sisi: header DAN pustaka.** Paket OpenUSD membawa headernya
# tanpa pustakanya, dan kalau hanya header yang ada `#include
# <OpenImageIO/imageio.h>` kompilasi dengan sukses lalu gagal saat link. Jadi
# keduanya harus ada sebelum backend ini dinyalakan.
# ---------------------------------------------------------------------------
set(SIM_OIIO_DEFAULT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../Third-Party/OpenImageIO")
if(EXISTS "${SIM_OIIO_DEFAULT_ROOT}/include/OpenImageIO/imageio.h")
    set(SIM_OIIO_ROOT "${SIM_OIIO_DEFAULT_ROOT}" CACHE PATH "Folder pemasangan OpenImageIO")
else()
    set(SIM_OIIO_ROOT "" CACHE PATH "Folder pemasangan OpenImageIO")
endif()

set(SIM_OIIO_READY FALSE)
if(SIM_WITH_OIIO AND SIM_OIIO_ROOT AND EXISTS "${SIM_OIIO_ROOT}/include/OpenImageIO/imageio.h")
    find_library(SIM_OIIO_LIB NAMES OpenImageIO PATHS "${SIM_OIIO_ROOT}/lib" NO_DEFAULT_PATH)
    find_library(SIM_OIIO_UTIL_LIB NAMES OpenImageIO_Util PATHS "${SIM_OIIO_ROOT}/lib" NO_DEFAULT_PATH)
    if(SIM_OIIO_LIB AND SIM_OIIO_UTIL_LIB)
        set(SIM_OIIO_READY TRUE)
    else()
        message(STATUS
            "Header OpenImageIO ada di ${SIM_OIIO_ROOT}/include tapi pustakanya tidak — "
            "backend OIIO dilewati. Ini disengaja: header tanpa pustaka kompilasi "
            "dengan sukses lalu gagal saat link. Langkahnya ada di docs/DEPENDENCIES.md.")
    endif()
endif()

if(SIM_OIIO_READY)
    # Daftar pendukung dibaca dari simbol yang belum terselesaikan di arsipnya,
    # bukan ditebak. boost datang dari paket yang sama karena arsipnya dibangun
    # terhadap boost 1.78 dan boost tidak menjanjikan ABI stabil antar-versi;
    # sisanya dari sistem, sonamenya cocok dengan yang dipakai saat ia dibangun.
    set(SIM_OIIO_SUPPORT_LIBS "")
    set(SIM_OIIO_MISSING "")

    foreach(boostLib filesystem thread atomic system)
        find_library(SIM_OIIO_BOOST_${boostLib} NAMES boost_${boostLib}
                     PATHS "${SIM_OIIO_ROOT}/lib" NO_DEFAULT_PATH)
        if(SIM_OIIO_BOOST_${boostLib})
            list(APPEND SIM_OIIO_SUPPORT_LIBS "${SIM_OIIO_BOOST_${boostLib}}")
        else()
            list(APPEND SIM_OIIO_MISSING "boost_${boostLib}")
        endif()
    endforeach()

    # Soname disebut persis di samping nama biasanya: paket `-dev` OpenEXR dan
    # Imath tidak selalu terpasang, dan `find_library` dengan nama biasa tidak
    # akan menemukan `libImath-3_1.so.29` yang tanpa symlink `.so`.
    set(SIM_OIIO_SONAME_OpenEXR-3_1     libOpenEXR-3_1.so.30)
    set(SIM_OIIO_SONAME_OpenEXRCore-3_1 libOpenEXRCore-3_1.so.30)
    set(SIM_OIIO_SONAME_Imath-3_1       libImath-3_1.so.29)
    set(SIM_OIIO_SONAME_Iex-3_1         libIex-3_1.so.30)
    set(SIM_OIIO_SONAME_IlmThread-3_1   libIlmThread-3_1.so.30)
    set(SIM_OIIO_SONAME_pugixml         libpugixml.so.1)
    set(SIM_OIIO_SONAME_tiff            libtiff.so.6)
    set(SIM_OIIO_SONAME_jpeg            libjpeg.so.8)
    set(SIM_OIIO_SONAME_png16           libpng16.so.16)
    set(SIM_OIIO_SONAME_z               libz.so.1)

    foreach(systemLib OpenEXR-3_1 OpenEXRCore-3_1 Imath-3_1 Iex-3_1 IlmThread-3_1
                      pugixml tiff jpeg png16 z)
        find_library(SIM_OIIO_SYS_${systemLib}
                     NAMES ${systemLib} ${SIM_OIIO_SONAME_${systemLib}})
        if(SIM_OIIO_SYS_${systemLib})
            list(APPEND SIM_OIIO_SUPPORT_LIBS "${SIM_OIIO_SYS_${systemLib}}")
        else()
            list(APPEND SIM_OIIO_MISSING "${systemLib}")
        endif()
    endforeach()

    if(SIM_OIIO_MISSING)
        list(JOIN SIM_OIIO_MISSING ", " simOiioMissingText)
        message(STATUS
            "OpenImageIO ditemukan tapi pustaka pendukungnya tidak lengkap "
            "(${simOiioMissingText}) — backend OIIO dilewati. "
            "Langkahnya ada di docs/DEPENDENCIES.md.")
        set(SIM_OIIO_READY FALSE)
    endif()
endif()

if(SIM_OIIO_READY)
    # **fmt diambil sendiri, dan versinya tidak bebas dipilih.** Paket OIIO ini
    # memasang `detail/fmt/format.h` sebagai shim satu baris yang meneruskan ke
    # `<fmt/format.h>`, jadi headernya tidak bisa dipakai tanpa fmt di luar.
    # Dipatok 10.2.1 karena itu yang dipakai OIIO 2.5.x; fmt 12 yang dibawa
    # spdlog membuang hal-hal yang masih dipakai header ini. Keduanya tidak
    # pernah bertemu di satu TU — lihat catatan di Code/ImageIO/src/BackendOiio.cpp.
    FetchContent_Declare(fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG        10.2.1
        GIT_SHALLOW    TRUE)
    FetchContent_Populate(fmt)

    add_library(sim_oiio INTERFACE)
    target_link_libraries(sim_oiio INTERFACE
        "${SIM_OIIO_LIB}" "${SIM_OIIO_UTIL_LIB}" ${SIM_OIIO_SUPPORT_LIBS})
    # SYSTEM: header OIIO memancarkan peringatannya sendiri, dan -Werror proyek
    # ini akan menggagalkan build karenanya.
    target_include_directories(sim_oiio SYSTEM INTERFACE
        "${SIM_OIIO_ROOT}/include" "${fmt_SOURCE_DIR}/include")
    # Header-only: menautkan pustaka fmt kedua akan menaruh dua salinan simbol
    # yang sama di satu binary.
    target_compile_definitions(sim_oiio INTERFACE FMT_HEADER_ONLY=1)
    add_library(Oiio::Oiio ALIAS sim_oiio)

    file(STRINGS "${SIM_OIIO_ROOT}/include/OpenImageIO/oiioversion.h" simOiioVersionLines
         REGEX "^#define OIIO_VERSION_(MAJOR|MINOR|PATCH) ")
    string(REGEX MATCHALL "[0-9]+" simOiioVersionParts "${simOiioVersionLines}")
    list(JOIN simOiioVersionParts "." SIM_OIIO_VERSION)
    set(SIM_OIIO_VERSION "${SIM_OIIO_VERSION}" CACHE INTERNAL "Versi OpenImageIO yang aktif")
    message(STATUS "OpenImageIO ${SIM_OIIO_VERSION} dipakai dari ${SIM_OIIO_ROOT} — DDS, PSD utuh, dan metadata colorspace aktif")
elseif(NOT SIM_WITH_OIIO)
    message(STATUS "backend OpenImageIO dimatikan (-DSIM_WITH_OIIO=OFF)")
elseif(NOT SIM_OIIO_ROOT)
    message(STATUS "OpenImageIO tidak ada — dilewati. Backend bawaan menangani "
                   "seluruh format kecuali DDS; langkahnya ada di docs/DEPENDENCIES.md")
endif()

# ---------------------------------------------------------------------------
# libtiff — TIFF untuk heightmap 16/32-bit (I3)
#
# **Dicari, tidak diunduh, dan sengaja bukan pembaca TIFF mini.** TIFF bukan satu
# format melainkan sebuah wadah: LZW, deflate, dan PackBits; strip dan tile;
# predictor; 8/16/32 bit; bilangan bulat dan IEEE float. Heightmap yang
# benar-benar keluar dari World Machine, Gaea, dan sumber GIS memakai
# kombinasi-kombinasi itu — pembaca yang menangani sebagiannya akan menolak
# justru berkas yang paling sering dipakai.
#
# libtiff ada di setiap distro dan berukuran ratusan kilobyte, jadi tidak ada
# yang dibeli dengan membangunnya sendiri.
# ---------------------------------------------------------------------------
if(SIM_WITH_TIFF)
    find_package(TIFF QUIET)
endif()
if(SIM_WITH_TIFF AND TIFF_FOUND)
    message(STATUS "libtiff ${TIFF_VERSION_STRING} dipakai dari ${TIFF_LIBRARIES} — impor TIFF aktif")
elseif(NOT SIM_WITH_TIFF)
    message(STATUS "impor TIFF dimatikan (-DSIM_WITH_TIFF=OFF)")
else()
    message(STATUS "libtiff tidak ditemukan — impor TIFF dilewati "
                   "(pasang libtiff-dev untuk mengaktifkannya)")
endif()

# ---------------------------------------------------------------------------
# cgltf — pembaca glTF/GLB untuk impor mesh (E8.4)
#
# Header-only tanpa build system, jadi SOURCE_SUBDIR diarahkan ke folder tanpa
# CMakeLists — pola yang sama dengan stb dan VMA.
#
# **Dikompilasi sebagai C, bukan C++**: sumbernya C99 dan mengandalkan perilaku
# yang berbeda di kedua bahasa.
# ---------------------------------------------------------------------------
# Dicari sebelum yang memakainya, bukan sesudah. `find_library` menyimpan
# hasilnya di cache, jadi urutan yang terbalik tetap bekerja pada konfigurasi
# kedua dan seterusnya — dan gagal diam-diam hanya pada build dir yang bersih,
# tempat yang paling jarang dicoba.
find_library(SIM_MATH_LIBRARY m)
FetchContent_Declare(cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG        v1.14
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  fuzz)
FetchContent_MakeAvailable(cgltf)

add_library(cgltf STATIC "${CMAKE_SOURCE_DIR}/Third-Party/cgltf/cgltf_impl.c")
target_include_directories(cgltf SYSTEM PUBLIC ${cgltf_SOURCE_DIR})
set_target_properties(cgltf PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
# Kode pihak ketiga: peringatannya bukan urusan kita, dan -Werror proyek ini
# akan menggagalkan build karenanya.
target_compile_options(cgltf PRIVATE -w)
if(SIM_MATH_LIBRARY)
    target_link_libraries(cgltf PRIVATE ${SIM_MATH_LIBRARY})
endif()
add_library(Cgltf::Cgltf ALIAS cgltf)

# ---------------------------------------------------------------------------
# Autodesk FBX SDK — pembaca FBX untuk impor mesh dan klip (E8.4)
#
# **Dicari, tidak diunduh, dan tidak boleh ikut di repo.** Berbeda dengan
# kebergantungan lain di berkas ini, FBX SDK berlisensi milik Autodesk: ia tidak
# bisa diambil FetchContent, tidak bisa divendor ke dalam pohon ini, dan biner
# yang menautnya tunduk pada perjanjian lisensi Autodesk. Yang memakainya harus
# memasangnya sendiri lebih dulu — lihat docs/DEPENDENCIES.md.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

set(SIM_FBXSDK_DEFAULT_ROOT "$ENV{HOME}/SDK/fbxsdk")
if(EXISTS "${SIM_FBXSDK_DEFAULT_ROOT}/include/fbxsdk.h")
    set(SIM_FBXSDK_ROOT "${SIM_FBXSDK_DEFAULT_ROOT}" CACHE PATH "Folder pemasangan Autodesk FBX SDK")
else()
    set(SIM_FBXSDK_ROOT "" CACHE PATH "Folder pemasangan Autodesk FBX SDK")
endif()

# **Yang statis lebih dulu.** Yang dinamis menuntut libfbxsdk.so ikut ditemukan
# saat dijalankan, dan satu-satunya tempat ia tinggal adalah folder pemasangan
# milik satu mesin — biner yang dipindah lalu berhenti bekerja dengan pesan dari
# loader, bukan dari mesin ini. Yang statis menyeret libxml2 dan zlib, dan
# keduanya memang ada di mana-mana.
find_library(SIM_FBXSDK_LIBRARY
             NAMES fbxsdk libfbxsdk.a
             PATHS "${SIM_FBXSDK_ROOT}/lib/release" "${SIM_FBXSDK_ROOT}/lib"
             NO_DEFAULT_PATH)
find_path(SIM_FBXSDK_INCLUDE fbxsdk.h PATHS "${SIM_FBXSDK_ROOT}/include" NO_DEFAULT_PATH)

if(SIM_FBXSDK_LIBRARY AND SIM_FBXSDK_INCLUDE)
    find_package(LibXml2 QUIET)
    find_package(ZLIB QUIET)

    add_library(sim_fbxsdk INTERFACE)
    target_include_directories(sim_fbxsdk SYSTEM INTERFACE "${SIM_FBXSDK_INCLUDE}")
    target_link_libraries(sim_fbxsdk INTERFACE "${SIM_FBXSDK_LIBRARY}")
    if(LibXml2_FOUND)
        target_link_libraries(sim_fbxsdk INTERFACE LibXml2::LibXml2)
    endif()
    if(ZLIB_FOUND)
        target_link_libraries(sim_fbxsdk INTERFACE ZLIB::ZLIB)
    endif()
    target_link_libraries(sim_fbxsdk INTERFACE ${CMAKE_DL_LIBS} Threads::Threads)
    if(SIM_MATH_LIBRARY)
        target_link_libraries(sim_fbxsdk INTERFACE ${SIM_MATH_LIBRARY})
    endif()
    add_library(Fbx::Fbx ALIAS sim_fbxsdk)
    message(STATUS "Autodesk FBX SDK dipakai dari ${SIM_FBXSDK_ROOT} — impor FBX aktif")
else()
    message(FATAL_ERROR
        "Autodesk FBX SDK tidak ditemukan. Impor FBX memakainya, dan tidak ada "
        "jalur cadangan: pasang SDK-nya lalu setel -DSIM_FBXSDK_ROOT=<folder>. "
        "Langkahnya ada di docs/DEPENDENCIES.md.")
endif()

# ---------------------------------------------------------------------------
# OpenUSD — impor .usd/.usda/.usdc/.usdz (E8.4)
#
# **Dicari, tidak diunduh.** Semua kebergantungan lain di berkas ini diambil
# lewat FetchContent karena masing-masing berbiaya detik sampai satu menit.
# OpenUSD bukan salah satunya: ia menyeret oneTBB, dan membangun keduanya dari
# nol memakan puluhan menit dan ratusan megabyte. Menjadikannya wajib berarti
# setiap orang yang hanya ingin mengubah satu panel editor membayar biaya itu
# pada build bersihnya yang pertama.
#
# Jadi USD dipakai kalau ada, dan dilewati kalau tidak. Yang melewatinya tetap
# bisa membangun seluruh mesin; yang hilang hanya impor USD, dan berkas .usd
# yang dibuka di sana ditolak dengan alasan yang menyebut mesinnya — bukan
# dengan "format tidak dikenal" yang mengirim orang memeriksa berkasnya.
#
# Cara menyediakannya ada di docs/DEPENDENCIES.md.
# **Bawaan menunjuk ke dalam pohon ini.** Salinan yang dipakai sebelumnya tinggal
# di cache packman milik SDK lain; cache adalah tempat yang boleh dibersihkan
# kapan saja oleh yang memilikinya, dan build yang bergantung padanya berhenti
# bekerja tanpa ada yang mengubah apa pun di sini.
set(SIM_USD_DEFAULT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../Third-Party/OpenUSD")
if(EXISTS "${SIM_USD_DEFAULT_ROOT}/include/pxr")
    set(SIM_USD_ROOT "${SIM_USD_DEFAULT_ROOT}" CACHE PATH "Folder pemasangan OpenUSD")
else()
    set(SIM_USD_ROOT "" CACHE PATH "Folder pemasangan OpenUSD")
endif()
# Python-nya ikut tinggal di sebelah USD-nya: yang dibutuhkan hanya header dan
# satu pustaka untuk memenuhi rujukan pxr_boost, dan menaruhnya di tempat lain
# berarti satu jalur lagi yang harus benar di mesin orang lain.
set(SIM_USD_PYTHON_ROOT "" CACHE PATH "Python yang dipakai paket USD siap pakai (header saja)")
if(NOT SIM_USD_PYTHON_ROOT AND SIM_USD_ROOT)
    set(SIM_USD_PYTHON_ROOT "${SIM_USD_ROOT}")
endif()
if(SIM_USD_ROOT)
    # `pxr_DIR` disetel langsung, bukan lewat CMAKE_PREFIX_PATH. find_package
    # menyimpan hasil pencariannya di cache dan memakai yang tersimpan itu pada
    # konfigurasi berikutnya — jadi menunjuk SIM_USD_ROOT ke pemasangan USD yang
    # lain pada build dir yang sudah ada tidak akan berpengaruh apa-apa, dan yang
    # terbangun diam-diam tetap yang lama. pxrConfig.cmake ada di akar
    # pemasangannya, jadi keduanya jalur yang sama.
    set(pxr_DIR "${SIM_USD_ROOT}" CACHE PATH "Lokasi pxrConfig.cmake" FORCE)
endif()
# **Paket USD siap pakai tidak lewat find_package.** `pxrConfig.cmake` dari
# paket biner — yang dari packman, yang dipakai usd-exchange — menuntut seluruh
# kebergantungan yang dipakai saat ia dibangun: Python pada versi patch yang
# persis, TBB, OpenSubdiv, dan seterusnya. Semuanya untuk bagian imaging yang
# tidak pernah disentuh importir ini, dan tidak satu pun dari daftar itu yang
# berhenti tumbuh sebelum semuanya disediakan.
#
# Yang dibutuhkan importir cuma pembaca panggungnya. Jadi paket yang sudah jadi
# ditautkan langsung: satu folder header, dan pustaka yang memang dipanggil.
# Yang dibangun sendiri tetap lewat find_package, karena config-nya menyebut apa
# adanya apa yang benar-benar ada.
# `usd_python` ikut walau importir ini tidak memanggil Python: header paket yang
# dibangun dengan Python menyala memancarkan rujukan ke registry pxr_boost, dan
# rujukan itu hidup di sana.
set(SIM_USD_RUNTIME_LIBS usd_usdSkel usd_usdShade usd_usdGeom usd_usd usd_sdf usd_pcp
                         usd_python
                         usd_ar usd_kind usd_ndr usd_sdr usd_plug usd_work usd_trace
                         usd_vt usd_gf usd_tf usd_js usd_arch)
if(SIM_USD_ROOT AND EXISTS "${SIM_USD_ROOT}/include/pxr" AND NOT EXISTS "${SIM_USD_ROOT}/pxrConfig.cmake")
    set(SIM_USD_PREBUILT TRUE)
elseif(SIM_USD_ROOT AND EXISTS "${SIM_USD_ROOT}/include/tbb")
    # Membawa TBB di dalam dirinya adalah tanda paket biner: yang dibangun dari
    # sumber memakai TBB yang terpasang terpisah.
    set(SIM_USD_PREBUILT TRUE)
endif()

if(SIM_USD_PREBUILT)
    set(SIM_USD_FOUND_LIBS "")
    foreach(usdLib IN LISTS SIM_USD_RUNTIME_LIBS)
        find_library(SIM_USD_LIB_${usdLib} NAMES ${usdLib}
                     PATHS "${SIM_USD_ROOT}/lib" NO_DEFAULT_PATH)
        if(SIM_USD_LIB_${usdLib})
            list(APPEND SIM_USD_FOUND_LIBS "${SIM_USD_LIB_${usdLib}}")
        endif()
    endforeach()
    find_library(SIM_USD_TBB NAMES tbb PATHS "${SIM_USD_ROOT}/lib" NO_DEFAULT_PATH)
    if(SIM_USD_TBB)
        list(APPEND SIM_USD_FOUND_LIBS "${SIM_USD_TBB}")
    endif()
    find_library(SIM_USD_PYTHON_LIBRARY NAMES python3.10 python3.11 python3.12 python3
                 PATHS "${SIM_USD_PYTHON_ROOT}/lib")
    if(SIM_USD_PYTHON_LIBRARY)
        list(APPEND SIM_USD_FOUND_LIBS "${SIM_USD_PYTHON_LIBRARY}")
    endif()
endif()

if(SIM_USD_PREBUILT AND SIM_USD_FOUND_LIBS)
    add_library(sim_usd INTERFACE)
    target_link_libraries(sim_usd INTERFACE ${SIM_USD_FOUND_LIBS})
    target_include_directories(sim_usd SYSTEM INTERFACE "${SIM_USD_ROOT}/include")
    # Paket biner umumnya dibangun dengan dukungan Python menyala, dan header
    # `pxr/pxr.h` yang menyalakannya menarik `wrap_python.hpp` — yang butuh
    # `pyconfig.h` walau importir ini tidak pernah memanggil satu pun API Python.
    # Yang dibutuhkan cuma headernya, jadi Python mana pun dengan versi minor
    # yang sama sudah cukup untuk melewatinya.
    find_path(SIM_USD_PYTHON_INCLUDE pyconfig.h
              PATHS "${SIM_USD_PYTHON_ROOT}/include/python3.10"
                    "${SIM_USD_PYTHON_ROOT}/include"
                    /usr/include/python3.10 /usr/include/python3.11 /usr/include/python3.12)
    if(SIM_USD_PYTHON_INCLUDE)
        target_include_directories(sim_usd SYSTEM INTERFACE "${SIM_USD_PYTHON_INCLUDE}")
    endif()
    add_library(Usd::Usd ALIAS sim_usd)
    message(STATUS "OpenUSD (paket siap pakai) dipakai dari ${SIM_USD_ROOT} — impor USD aktif")
    set(pxr_FOUND TRUE)
else()
    find_package(pxr CONFIG QUIET)
endif()
if(pxr_FOUND AND NOT TARGET sim_usd)
    add_library(sim_usd INTERFACE)
    # Build monolitik menghasilkan satu pustaka; yang bukan monolitik
    # menghasilkan puluhan, dan PXR_LIBRARIES menyebut keduanya dengan benar.
    target_link_libraries(sim_usd INTERFACE ${PXR_LIBRARIES})
    target_include_directories(sim_usd SYSTEM INTERFACE ${PXR_INCLUDE_DIRS})
    add_library(Usd::Usd ALIAS sim_usd)
    message(STATUS "OpenUSD ditemukan: ${pxr_DIR} — impor USD aktif")
elseif(NOT TARGET Usd::Usd)
    message(STATUS "OpenUSD tidak ditemukan — impor USD dilewati "
                   "(setel SIM_USD_ROOT untuk mengaktifkannya)")
endif()

# ---------------------------------------------------------------------------
# Cabang TfHashMap — harus sama di pustakanya dan di yang memanggilnya.
#
# **`TfHashMap` bukan satu tipe.** `pxr/base/tf/hashmap.h` memilih induknya lewat
# `ARCH_HAS_GNU_STL_EXTENSIONS`, dan `pxr/base/arch/defines.h` menyalakan makro
# itu hanya pada Linux **dengan GCC**. Dibangun GCC, `TfHashMap` mewarisi
# `__gnu_cxx::hash_map` dan besarnya 40 bait; header yang sama dibaca Clang
# mewarisi `std::unordered_map` dan besarnya 56. Setiap kelas USD yang menyimpan
# `TfHashMap` karena itu punya offset anggota yang berbeda di kedua sisi —
# `UsdGeomXformCache` menaruh `_time` di 40 menurut pustakanya, di 56 menurut
# yang memanggilnya.
#
# **Yang tidak cocok tidak gagal saat ditautkan.** Konstruktornya berjalan di
# dalam pustaka dan destruktornya inline di pemanggil, jadi yang dibongkar bukan
# yang dibangun: `UsdGeomXformCache` yang keluar dari cakupan membaca peta
# hash-nya pada alamat yang salah, membebaskan yang bukan miliknya, dan merusak
# heap. Yang terlihat cuma segfault di `~UsdGeomXformCache` — jauh dari baris
# mana pun yang keliru, dan tetap muncul pada berkas .usda sekecil satu kubus.
#
# Jadi cabangnya dibaca dari binernya, bukan ditebak dari kompiler yang sedang
# dipakai: nama ter-mangle `__gnu_cxx::hash_map` ada di tabel simbol kalau USD
# memakai cabang itu, dan tetap ada setelah binernya di-strip.
if(TARGET sim_usd)
    set(SIM_USD_ABI_PROBE "")
    foreach(usdDir IN ITEMS "${SIM_USD_ROOT}" "${pxr_DIR}")
        if(NOT "${usdDir}" STREQUAL "")
            # TfHashMap tinggal di usd_tf; build monolitik menaruh semuanya di usd_ms.
            file(GLOB usdProbe "${usdDir}/lib/libusd_tf.so*" "${usdDir}/lib/libusd_ms.so*")
            list(APPEND SIM_USD_ABI_PROBE ${usdProbe})
        endif()
    endforeach()
    list(APPEND SIM_USD_ABI_PROBE ${SIM_USD_FOUND_LIBS})
    list(REMOVE_DUPLICATES SIM_USD_ABI_PROBE)

    set(SIM_USD_GNU_HASH FALSE)
    set(SIM_USD_ABI_KNOWN FALSE)
    foreach(usdLib IN LISTS SIM_USD_ABI_PROBE)
        if(EXISTS "${usdLib}" AND NOT IS_DIRECTORY "${usdLib}")
            set(SIM_USD_ABI_KNOWN TRUE)
            file(STRINGS "${usdLib}" usdHit REGEX "9__gnu_cxx8hash_map" LIMIT_COUNT 1)
            if(usdHit)
                set(SIM_USD_GNU_HASH TRUE)
                break()
            endif()
        endif()
    endforeach()

    if(SIM_USD_GNU_HASH)
        # `<ext/hash_set>` yang ditarik cabang ini memancarkan #warning usang, dan
        # SIM_WERROR mengubahnya jadi kesalahan. Yang dimatikan hanya #warning-nya;
        # `-Wno-deprecated` juga akan mematikan peringatan `[[deprecated]]` pada
        # seluruh target yang menautnya, dan itu sinyal yang masih ingin dibaca.
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(sim_usd INTERFACE "-Wno-#warnings")
        elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(sim_usd INTERFACE -Wno-cpp)
        endif()
        if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_definitions(sim_usd INTERFACE ARCH_HAS_GNU_STL_EXTENSIONS)
            message(STATUS "OpenUSD memakai cabang __gnu_cxx::hash_map — "
                           "ARCH_HAS_GNU_STL_EXTENSIONS disetel supaya "
                           "${CMAKE_CXX_COMPILER_ID} melihat tata letak yang sama")
        endif()
    elseif(NOT SIM_USD_ABI_KNOWN)
        message(WARNING
            "Tidak ada pustaka OpenUSD yang bisa diperiksa untuk cabang TfHashMap. "
            "Kalau USD-nya dibangun dengan GCC dan mesin ini tidak, impor USD akan "
            "merusak heap saat UsdGeomXformCache dibongkar.")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Kebalikannya, dan tidak ada tambalannya: makro itu disetel oleh header
        # USD sendiri untuk GCC di Linux, dan baris perintah tidak bisa
        # membatalkan `#define` yang ditulis header.
        message(FATAL_ERROR
            "OpenUSD ini dibangun tanpa cabang __gnu_cxx::hash_map, tapi SimEngine "
            "sedang dibangun dengan GCC di Linux — yang menyalakan cabang itu lewat "
            "header USD sendiri. Tata letak TfHashMap akan berbeda di kedua sisi. "
            "Bangun SimEngine dengan Clang, atau bangun ulang OpenUSD dengan GCC.")
    endif()
endif()

# ---------------------------------------------------------------------------
# OpenVDB — bake mesh menjadi SDF, dan impor volume (.vdb)
#
# **Opsional dan dicari, tidak dibangun.** Membangunnya dari nol menyeret TBB,
# Blosc, dan boost, dan memakan puluhan menit — biaya yang sama yang membuat
# OpenUSD tidak dijadikan syarat. Yang melewatinya tetap bisa membangun seluruh
# mesin; yang hilang adalah SDF mesh yang tepat, dan clipmap GI mundur ke
# hampiran kotak berorientasi yang memang sudah ada.
#
# **Pengondisi aset, bukan pustaka runtime.** Aturan yang sama dengan
# OpenImageIO: ia boleh dipakai importir, baker, dan editor; tidak boleh oleh
# jalur yang dikirim ke pemain. Yang sampai ke renderer adalah grid float yang
# sudah dibake — tipe biasa, tanpa satu pun tipe OpenVDB di header publik.
#
# Cara menyediakannya ada di docs/DEPENDENCIES.md.
# ---------------------------------------------------------------------------
set(SIM_OPENVDB_DEFAULT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../Third-Party/OpenVDB")
if(EXISTS "${SIM_OPENVDB_DEFAULT_ROOT}/include/openvdb/openvdb.h")
    set(SIM_OPENVDB_ROOT "${SIM_OPENVDB_DEFAULT_ROOT}" CACHE PATH "Folder pemasangan OpenVDB")
else()
    set(SIM_OPENVDB_ROOT "" CACHE PATH "Folder pemasangan OpenVDB")
endif()

set(SIM_OPENVDB_READY FALSE)
if(SIM_WITH_OPENVDB AND SIM_OPENVDB_ROOT
   AND EXISTS "${SIM_OPENVDB_ROOT}/include/openvdb/openvdb.h")
    # Diperiksa dua sisi, dengan alasan yang sama seperti OIIO: header tanpa
    # pustaka kompilasi dengan sukses lalu gagal saat link.
    find_library(SIM_OPENVDB_LIB NAMES openvdb
                 PATHS "${SIM_OPENVDB_ROOT}/lib" NO_DEFAULT_PATH)
    # `version.h` dihasilkan saat OpenVDB dibangun, bukan bagian dari sumbernya,
    # dan `Types.h` meng-include-nya dengan tanda kutip — jadi ia harus duduk di
    # sebelah header lain, bukan di folder build yang terpisah. Pemeriksaan ini
    # menangkap salinan header yang diambil dari pohon sumber saja.
    if(SIM_OPENVDB_LIB AND EXISTS "${SIM_OPENVDB_ROOT}/include/openvdb/version.h")
        set(SIM_OPENVDB_READY TRUE)
    elseif(NOT SIM_OPENVDB_LIB)
        message(STATUS
            "Header OpenVDB ada di ${SIM_OPENVDB_ROOT}/include tapi pustakanya tidak — "
            "bake SDF dilewati. Langkahnya ada di docs/DEPENDENCIES.md.")
    else()
        message(STATUS
            "OpenVDB di ${SIM_OPENVDB_ROOT} tidak punya version.h — ia dihasilkan saat "
            "OpenVDB dibangun, jadi salinan dari pohon sumber saja tidak cukup. "
            "Langkahnya ada di docs/DEPENDENCIES.md.")
    endif()
endif()

if(SIM_OPENVDB_READY)
    # TBB dipakai OpenVDB untuk paralelisasi bake-nya. Ia sudah ada di sistem
    # sebagai kebergantungan pustaka lain; yang dibawa paket OpenUSD sengaja
    # tidak dipakai supaya tidak ada dua TBB di satu binary.
    find_library(SIM_OPENVDB_TBB NAMES tbb libtbb.so.12)
    if(NOT SIM_OPENVDB_TBB)
        message(STATUS "libtbb tidak ditemukan — bake SDF dilewati "
                       "(pasang libtbb-dev untuk mengaktifkannya)")
        set(SIM_OPENVDB_READY FALSE)
    endif()
endif()

if(SIM_OPENVDB_READY)
    add_library(sim_openvdb INTERFACE)
    target_link_libraries(sim_openvdb INTERFACE "${SIM_OPENVDB_LIB}" "${SIM_OPENVDB_TBB}")
    # SYSTEM: header OpenVDB memancarkan peringatannya sendiri, dan -Werror
    # proyek ini akan menggagalkan build karenanya.
    target_include_directories(sim_openvdb SYSTEM INTERFACE "${SIM_OPENVDB_ROOT}/include")
    add_library(OpenVdb::OpenVdb ALIAS sim_openvdb)

    file(STRINGS "${SIM_OPENVDB_ROOT}/include/openvdb/version.h" simVdbVersionLines
         REGEX "^#define OPENVDB_LIBRARY_(MAJOR|MINOR|PATCH)_VERSION_NUMBER ")
    string(REGEX MATCHALL "[0-9]+" simVdbVersionParts "${simVdbVersionLines}")
    list(JOIN simVdbVersionParts "." SIM_OPENVDB_VERSION)
    set(SIM_OPENVDB_VERSION "${SIM_OPENVDB_VERSION}" CACHE INTERNAL "Versi OpenVDB yang aktif")
    message(STATUS "OpenVDB ${SIM_OPENVDB_VERSION} dipakai dari ${SIM_OPENVDB_ROOT} — bake SDF mesh aktif")
elseif(NOT SIM_WITH_OPENVDB)
    message(STATUS "OpenVDB dimatikan (-DSIM_WITH_OPENVDB=OFF)")
elseif(NOT SIM_OPENVDB_ROOT)
    message(STATUS "OpenVDB tidak ada — bake SDF mesh dilewati, clipmap GI memakai "
                   "hampiran kotak (setel SIM_OPENVDB_ROOT untuk mengaktifkannya)")
endif()

# ---------------------------------------------------------------------------
# PhysX 5 — simulasi fisika (E9, lihat docs/PLAN-PHYSICS.md)
#
# **Opsional dan dicari, tidak dibangun**, dengan alasan yang sama seperti
# OpenUSD dan OpenVDB: membangunnya dari nol memakan puluhan menit, dan
# menjadikannya syarat berarti setiap orang yang hanya ingin mengubah satu panel
# editor membayar itu pada build bersih pertamanya. Yang melewatinya tetap bisa
# membangun seluruh mesin; yang hilang adalah simulasi, dan `PhysicsWorld`
# mengatakannya di log alih-alih membiarkan benda diam tanpa penjelasan.
#
# **CPU dulu, dan itu bukan sekadar urutan.** Tiga fitur PhysX — PBD, soft body
# FEM, dan deformable surface — menerima `PxCudaContextManager&` **sebagai
# referensi**, jadi CUDA-nya wajib, bukan opsional. Ketiganya tidak pernah ada
# di perangkat non-NVIDIA. `SIM_WITH_PHYSX_GPU` karena itu memilih pemasangan
# PhysX yang mana yang ditautkan, bukan menyalakan cabang kode kedua.
#
# Cara menyediakannya ada di docs/DEPENDENCIES.md.
# ---------------------------------------------------------------------------
set(SIM_PHYSX_DEFAULT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../Third-Party/PhysX")
if(EXISTS "${SIM_PHYSX_DEFAULT_ROOT}/include/PxPhysicsAPI.h")
    set(SIM_PHYSX_ROOT "${SIM_PHYSX_DEFAULT_ROOT}" CACHE PATH "Folder pemasangan PhysX 5")
else()
    set(SIM_PHYSX_ROOT "" CACHE PATH "Folder pemasangan PhysX 5")
endif()

set(SIM_PHYSX_READY FALSE)
if(SIM_WITH_PHYSX AND SIM_PHYSX_ROOT AND EXISTS "${SIM_PHYSX_ROOT}/include/PxPhysicsAPI.h")
    # Diperiksa dua sisi — header **dan** pustaka — dengan alasan yang sama
    # seperti OIIO dan OpenVDB: header tanpa pustaka kompilasi dengan sukses lalu
    # gagal saat link, dan galatnya tidak menyebut sebabnya.
    set(SIM_PHYSX_LIBS "")
    set(SIM_PHYSX_MISSING "")
    # Urutannya penting: penaut GNU memproses arsip sekali, dari kiri ke kanan,
    # jadi yang bergantung harus mendahului yang dibergantungi. PhysX → Common →
    # Foundation adalah rantainya.
    #
    # **Vehicle2 mendahului Extensions**, dan itu bukan urutan yang tampak jelas:
    # `PxVehiclePhysXActorCreate` memakai `PxDefaultMemoryOutputStream` yang
    # tinggal di Extensions. Urutan sebaliknya menghasilkan simbol tak ditemukan
    # yang menyebut nama berkas PhysX, bukan nama pustaka yang salah tempat —
    # ditemukan saat P6, sesudah tautannya berjalan tanpa keluhan selama lima
    # milestone.
    foreach(physxLib PhysXVehicle2 PhysXExtensions PhysXCharacterKinematic PhysXCooking
                     PhysX PhysXPvdSDK PhysXCommon PhysXFoundation)
        find_library(SIM_PHYSX_LIB_${physxLib} NAMES ${physxLib}_static_64 ${physxLib}
                     PATHS "${SIM_PHYSX_ROOT}/lib" NO_DEFAULT_PATH)
        if(SIM_PHYSX_LIB_${physxLib})
            list(APPEND SIM_PHYSX_LIBS "${SIM_PHYSX_LIB_${physxLib}}")
        else()
            list(APPEND SIM_PHYSX_MISSING "${physxLib}")
        endif()
    endforeach()

    if(SIM_PHYSX_MISSING)
        list(JOIN SIM_PHYSX_MISSING ", " simPhysxMissingText)
        message(STATUS
            "Header PhysX ada di ${SIM_PHYSX_ROOT}/include tapi pustakanya tidak lengkap "
            "(${simPhysxMissingText}) — simulasi dilewati. "
            "Langkahnya ada di docs/DEPENDENCIES.md.")
    else()
        set(SIM_PHYSX_READY TRUE)
    endif()
endif()

if(SIM_PHYSX_READY)
    add_library(sim_physx INTERFACE)
    target_link_libraries(sim_physx INTERFACE ${SIM_PHYSX_LIBS} Threads::Threads ${CMAKE_DL_LIBS})
    # SYSTEM: header PhysX memancarkan peringatannya sendiri, dan -Werror proyek
    # ini akan menggagalkan build karenanya.
    target_include_directories(sim_physx SYSTEM INTERFACE "${SIM_PHYSX_ROOT}/include")
    add_library(PhysX::PhysX ALIAS sim_physx)

    file(STRINGS "${SIM_PHYSX_ROOT}/include/foundation/PxPhysicsVersion.h" simPhysxVersionLines
         REGEX "^#define PX_PHYSICS_VERSION_(MAJOR|MINOR|BUGFIX) ")
    string(REGEX MATCHALL "[0-9]+" simPhysxVersionParts "${simPhysxVersionLines}")
    list(JOIN simPhysxVersionParts "." SIM_PHYSX_VERSION)
    set(SIM_PHYSX_VERSION "${SIM_PHYSX_VERSION}" CACHE INTERNAL "Versi PhysX yang aktif")

    if(SIM_WITH_PHYSX_GPU)
        message(STATUS "PhysX ${SIM_PHYSX_VERSION} dipakai dari ${SIM_PHYSX_ROOT} — simulasi aktif, jalur GPU diminta")
    else()
        message(STATUS "PhysX ${SIM_PHYSX_VERSION} dipakai dari ${SIM_PHYSX_ROOT} — simulasi aktif (CPU)")
    endif()
elseif(NOT SIM_WITH_PHYSX)
    message(STATUS "PhysX dimatikan (-DSIM_WITH_PHYSX=OFF) — tidak ada yang disimulasikan")
elseif(NOT SIM_PHYSX_ROOT)
    message(STATUS "PhysX tidak ada — simulasi dilewati "
                   "(setel SIM_PHYSX_ROOT untuk mengaktifkannya)")
endif()

# ---------------------------------------------------------------------------
# Ditambahkan pada milestone berikutnya (lihat docs/DEPENDENCIES.md):
#   E8   cgltf, meshoptimizer
# ---------------------------------------------------------------------------
