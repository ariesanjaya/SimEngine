sim_add_library(Core
    SOURCES
        src/FileWatcher.cpp
        src/FrameLimiter.cpp
        src/Log.cpp
        src/MainThreadQueue.cpp
        src/TaskPool.cpp
        src/Uuid.cpp
    PUBLIC_DEPS
        glm::glm
        spdlog::spdlog)

# Harus PUBLIC: setiap TU yang meng-include glm lewat Sim/Core/Math.h wajib
# memakai konvensi yang sama, kalau tidak matriks proyeksinya diam-diam berbeda.
target_compile_definitions(SimCore PUBLIC
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE      # Vulkan: depth clip [0,1], bukan [-1,1]
    GLM_ENABLE_EXPERIMENTAL
    GLM_FORCE_EXPLICIT_CTOR)
