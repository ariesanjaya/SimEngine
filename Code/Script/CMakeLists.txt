sim_add_library(Script
    SOURCES
        src/LuaVM.cpp
    PUBLIC_DEPS
        Sim::Core
        Sol2::Sol2)
