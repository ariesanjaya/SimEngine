sim_add_library(Render
    SOURCES
        src/FrameGraph.cpp
        src/FrameGraphExecutor.cpp
        src/VulkanRenderer.cpp
        src/Frustum.cpp
        src/Ibl.cpp
        src/IblBaker.cpp
        src/LightCluster.cpp
        src/Bloom.cpp
        src/DepthPyramid.cpp
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
        src/TraceBackend.cpp
        src/ShadowCascades.cpp
        src/TimeOfDay.cpp
        src/ToneMap.cpp
        src/MaterialPreviewMesh.cpp
        src/StubRenderer.cpp
        src/VulkanMaterialPreview.cpp
        src/ThumbnailCache.cpp
    PUBLIC_DEPS
        Sim::Core
    # PRIVATE: header publik Render (IViewportRenderer, Types) sengaja bebas
    # Vulkan, sehingga modul Editor yang me-link Render tidak ikut melihatnya.
    PRIVATE_DEPS
        Sim::Assets
        Sim::RHI)

sim_compile_shaders(SimShadersGrid
    SOURCES
        ${CMAKE_SOURCE_DIR}/Shaders/grid.vert.slang
        ${CMAKE_SOURCE_DIR}/Shaders/grid.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/line.vert.slang
        ${CMAKE_SOURCE_DIR}/Shaders/line.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/box.vert.slang
        ${CMAKE_SOURCE_DIR}/Shaders/box.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/shadow.vert.slang
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
        ${CMAKE_SOURCE_DIR}/Shaders/bloom_down.frag.slang
        ${CMAKE_SOURCE_DIR}/Shaders/bloom_up.frag.slang)
# openpbr.slang sengaja tidak ada di daftar ini. Ia modul pustaka tanpa entry
# point — yang mengompilasinya adalah `AssembleMaterialModule`, yang menanamnya
# ke dalam modul material lalu menyerahkannya ke ShaderCache. Menambahkannya di
# sini akan menjadi galat slangc pada build yang bersih.

add_dependencies(SimRender SimShadersGrid)

# openpbr.slang disalin sebagai SUMBER, bukan dikompilasi. Preview material
# menanamnya ke dalam modul yang dirakit lalu memanggil slangc saat itu juga,
# jadi yang harus ada di sebelah executable adalah teksnya — dan menyalinnya
# lewat target sendiri membuat menyunting model shading memicu penyalinan ulang.
add_custom_target(SimShadersOpenPbr
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Shaders/openpbr.slang"
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Shaders/openpbr.slang"
    DEPENDS "${CMAKE_SOURCE_DIR}/Shaders/openpbr.slang"
    COMMENT "copy openpbr.slang"
    VERBATIM)
add_dependencies(SimRender SimShadersOpenPbr)
