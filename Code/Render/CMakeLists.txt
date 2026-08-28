sim_add_library(Render
    SOURCES
        src/FrameGraph.cpp
        src/FrameGraphExecutor.cpp
        src/Presenter.cpp
        src/VulkanRenderer.cpp
        src/Frustum.cpp
        src/Ibl.cpp
        src/IblBaker.cpp
        src/IblCache.cpp
        src/ProbeVolume.cpp
        src/LightCluster.cpp
        src/Atmosphere.cpp
        src/Bloom.cpp
        src/CloudNoise.cpp
        src/ClusterAssign.cpp
        src/ComputeGradient.cpp
        src/DepthPyramid.cpp
        src/DrawCull.cpp
        src/Denoise.cpp
        src/PostProcess.cpp
        src/ProbeField.cpp
        src/RadianceCache.cpp
        src/ScreenProbe.cpp
        src/ScreenTrace.cpp
        src/SdfClipmap.cpp
        src/SdfClipmapResource.cpp
        src/SdfVolume.cpp
        src/TieredTrace.cpp
        src/ShadowAtlas.cpp
        src/SkyAtmosphere.cpp
        src/TraceBackend.cpp
        src/ShadowCascades.cpp
        src/TimeOfDay.cpp
        src/ToneMap.cpp
        src/MaterialPreviewMesh.cpp
        src/StubRenderer.cpp
        src/VulkanMaterialPreview.cpp
        src/ThumbnailCache.cpp
        src/VolumeTexture.cpp
        src/VolumeResource.cpp
        src/VolumePass.cpp
    PUBLIC_DEPS
        Sim::Core
    # PRIVATE: header publik Render (IViewportRenderer, Types) sengaja bebas
    # Vulkan, sehingga modul Editor yang me-link Render tidak ikut melihatnya.
    PRIVATE_DEPS
        Sim::Assets
        Sim::RHI
        # Peta lingkungan dibaca lewat Sim::ImageIO, bukan lewat stb langsung.
        # Privat: header publik Render tidak membocorkannya, sama dengan Assets
        # dan Vegetation.
        Sim::ImageIO)

sim_compile_shaders(SimShadersGrid
    SOURCES
        ${CMAKE_SOURCE_DIR}/Shaders/grid.vert.slang
        ${CMAKE_SOURCE_DIR}/Shaders/grid.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/line.vert.slang
        ${CMAKE_SOURCE_DIR}/Shaders/line.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/box.vert.slang
        ${CMAKE_SOURCE_DIR}/Shaders/box.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/box_bindless.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/shadow.vert.slang
        # Tahap fragmennya saja. Rangka kawat memakai `shadow.vert` di atas
        # sebagai tahap vertexnya — lihat catatan di berkasnya.
        ${CMAKE_SOURCE_DIR}/Shaders/wireframe.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/prepass.vert.slang
        ${CMAKE_SOURCE_DIR}/Shaders/prepass.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/hiz_reduce.vert.slang
        ${CMAKE_SOURCE_DIR}/Shaders/hiz_reduce.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/gi_probe.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/gi_filter.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/sdf_debug.vert.slang
        ${CMAKE_SOURCE_DIR}/Shaders/sdf_debug.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/fullscreen.vert.slang
        ${CMAKE_SOURCE_DIR}/Shaders/lum_seed.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/lum_reduce.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/exposure.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/tonemap.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/fullscreen_uv.vert.slang
        ${CMAKE_SOURCE_DIR}/Shaders/present.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/bloom_down.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/bloom_up.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/sky_transmittance.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/sky_multiscatter.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/sky_view.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/sky_draw.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/sky_aerial.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/sky_aerial_apply.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/sky_clouds.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/sky_hdri.frag.slang
        # Raymarch volume .vdb. Acuan CPU-nya di Code/Volume/src/VolumeRaymarch.cpp,
        # dan rumus keduanya harus sama persis.
        ${CMAKE_SOURCE_DIR}/Shaders/volume_raymarch.frag.slang
        # Pass compute pertama di engine ini (G3). Akhiran `.comp` tidak
        # menuntut aturan build sendiri: yang menentukan tahap shader adalah
        # akhirannya, dan itu berlaku sama untuk `.vert`, `.frag`, dan `.comp`.
        ${CMAKE_SOURCE_DIR}/Shaders/debug_gradient.comp.slang
        ${CMAKE_SOURCE_DIR}/Shaders/cluster_assign.comp.slang
        ${CMAKE_SOURCE_DIR}/Shaders/draw_cull.comp.slang
        ${CMAKE_SOURCE_DIR}/Shaders/sdf_fill.comp.slang)
# openpbr.slang sengaja tidak ada di daftar ini. Ia modul pustaka tanpa entry
# point — yang mengompilasinya adalah `AssembleMaterialModule`, yang menanamnya
# ke dalam modul material lalu menyerahkannya ke ShaderCache. Menambahkannya di
# sini akan menjadi galat slangc pada build yang bersih.

add_dependencies(SimRender SimShadersGrid)

# Beberapa shader disalin sebagai SUMBER, bukan dikompilasi. Modul material
# dirakit saat editor berjalan — teksnya ditanam lalu diserahkan ke slangc saat
# itu juga — jadi yang harus ada di sebelah executable adalah teksnya.
#
# **Bukan hanya `openpbr.slang`.** Shader material untuk pass forward menanam
# deklarasi set 0 milik renderer dari berkas yang sama persis yang di-`#include`
# `box.frag`, dan berkas yang tidak ada di sebelah executable menjadi `#error`
# saat modulnya dirakit — yaitu "preprocessor error" dari slangc, jauh dari
# sebabnya. Daftar ini karena itu harus mengikuti apa yang ditanam
# `MaterialPrograms`; ketidakcocokannya muncul sebagai material yang gagal
# dikompilasi hanya di editor yang sudah dipasang, bukan di uji yang membaca
# folder sumber.
set(SIM_SHADER_SOURCES_COPIED
    openpbr.slang
    box_push.slang
    box_varyings.slang
    cluster_common.slang
    shadow_common.slang
    gi_resolve.slang
    gi_common.slang
    gi_depth.slang)

set(_sim_copied_shaders)
foreach(_shader IN LISTS SIM_SHADER_SOURCES_COPIED)
    list(APPEND _sim_copied_shaders
         "${CMAKE_COMMAND}" -E copy_if_different
         "${CMAKE_SOURCE_DIR}/Shaders/${_shader}"
         "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Shaders/${_shader}")
endforeach()

add_custom_target(SimShadersOpenPbr
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Shaders"
    COMMENT "copy shader sources"
    VERBATIM)
foreach(_shader IN LISTS SIM_SHADER_SOURCES_COPIED)
    add_custom_command(TARGET SimShadersOpenPbr POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/Shaders/${_shader}"
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Shaders/${_shader}"
        DEPENDS "${CMAKE_SOURCE_DIR}/Shaders/${_shader}"
        VERBATIM)
endforeach()
add_dependencies(SimRender SimShadersOpenPbr)
