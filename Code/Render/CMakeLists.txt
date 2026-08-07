sim_add_library(Render
    SOURCES
        src/FrameGraph.cpp
        src/FrameGraphExecutor.cpp
        src/VulkanRenderer.cpp
        src/Frustum.cpp
        src/StubRenderer.cpp
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
        ${CMAKE_SOURCE_DIR}/Shaders/grid.vert
        ${CMAKE_SOURCE_DIR}/Shaders/grid.frag
        ${CMAKE_SOURCE_DIR}/Shaders/line.vert
        ${CMAKE_SOURCE_DIR}/Shaders/line.frag
        ${CMAKE_SOURCE_DIR}/Shaders/box.vert
        ${CMAKE_SOURCE_DIR}/Shaders/box.frag)
# openpbr.slang sengaja tidak ada di daftar ini. Ia modul pustaka tanpa entry
# point — yang mengompilasinya adalah `AssembleMaterialModule`, yang menanamnya
# ke dalam modul material lalu menyerahkannya ke ShaderCache. Menambahkannya di
# sini akan menjadi galat slangc pada build yang bersih.

add_dependencies(SimRender SimShadersGrid)
