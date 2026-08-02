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
    file(GLOB _sim_vk_candidates "$ENV{HOME}/SDK/vulkan-sdk-*/x86_64")
    if(_sim_vk_candidates)
        list(SORT _sim_vk_candidates)
        list(REVERSE _sim_vk_candidates)   # versi tertinggi lebih dulu
        list(GET _sim_vk_candidates 0 _sim_vk_root)
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
