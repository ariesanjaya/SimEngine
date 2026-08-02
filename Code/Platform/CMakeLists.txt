sim_add_library(Platform
    SOURCES
        src/Display.cpp
        src/Window.cpp
    PUBLIC_DEPS
        Sim::Core
        SDL3::SDL3)
