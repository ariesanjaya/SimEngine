# Pencarian Vulkan SDK.
#
# Urutan: variabel cache SIM_VULKAN_SDK > $ENV{VULKAN_SDK} > SDK lokal yang dikenal
# > loader sistem. SDK lokal dicantumkan supaya `cmake --preset ...` tetap jalan
# walaupun setup-env.sh belum di-source.
include_guard(GLOBAL)

set(SIM_VULKAN_SDK "" CACHE PATH "Root Vulkan SDK (folder berisi include/ dan lib/)")

if(SIM_VULKAN_SDK)
    set(_sim_vk_root "${SIM_VULKAN_SDK}")
elseif(DEFINED ENV{VULKAN_SDK})
    set(_sim_vk_root "$ENV{VULKAN_SDK}")
else()
    # Tata letak SDK berbeda per platform, dan begitu juga tempat pemasangannya.
    # Installer LunarG di Windows menyetel VULKAN_SDK sendiri, jadi cabang ini
    # praktis hanya terpakai di shell yang dibuka sebelum pemasangan selesai —
    # tapi persis itulah keadaan yang membuat orang mengira SDK-nya belum ada.
    if(WIN32)
        file(GLOB _sim_vk_candidates "$ENV{SystemDrive}/VulkanSDK/*")
    else()
        file(GLOB _sim_vk_candidates "$ENV{HOME}/SDK/vulkan-sdk-*/x86_64")
    endif()
    # Disaring lewat headernya, bukan lewat nama. Folder SDK bernama seperti
    # `1.4.357.0`, jadi menyaring "yang tidak mengandung titik" justru membuang
    # semuanya; sementara berkas nyasar di sebelahnya bisa memenangkan urutan
    # versi dan menjadi root yang tidak berisi apa pun.
    set(_sim_vk_sdks "")
    foreach(_candidate ${_sim_vk_candidates})
        if(EXISTS "${_candidate}/include/vulkan/vulkan.h")
            list(APPEND _sim_vk_sdks "${_candidate}")
        endif()
    endforeach()
    if(_sim_vk_sdks)
        list(SORT _sim_vk_sdks)
        list(REVERSE _sim_vk_sdks)   # versi tertinggi lebih dulu
        list(GET _sim_vk_sdks 0 _sim_vk_root)
        message(STATUS "VULKAN_SDK tidak di-set, memakai SDK lokal: ${_sim_vk_root}")
    endif()
endif()

if(_sim_vk_root)
    set(ENV{VULKAN_SDK} "${_sim_vk_root}")
    list(APPEND CMAKE_PREFIX_PATH "${_sim_vk_root}")
endif()

find_package(Vulkan REQUIRED)

# Alat kompilasi shader — dipakai SimShaders.cmake.
find_program(SIM_GLSLC glslc
    HINTS "${_sim_vk_root}/bin" ${Vulkan_GLSLC_EXECUTABLE}
    DOC "Kompiler GLSL -> SPIR-V")
find_program(SIM_SLANGC slangc
    HINTS "${_sim_vk_root}/bin"
    DOC "Kompiler Slang -> SPIR-V")

if(NOT SIM_GLSLC)
    message(WARNING "glslc tidak ditemukan; target shader akan dilewati")
endif()
