sim_add_library(Render
    SOURCES
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
        ${CMAKE_SOURCE_DIR}/Shaders/line.frag)

add_dependencies(SimRender SimShadersGrid)
