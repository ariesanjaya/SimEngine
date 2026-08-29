sim_add_library(Core
    SOURCES
        src/Curve.cpp
        src/FileWatcher.cpp
        src/FbxSdkLock.cpp
        src/FrameLimiter.cpp
        src/AtomicWrite.cpp
        src/Log.cpp
        src/MainThreadQueue.cpp
        src/SdfGrid.cpp
        src/VolumeGrid.cpp
        src/TaskPool.cpp
        src/UserPaths.cpp
        src/Uuid.cpp
    PUBLIC_DEPS
        glm::glm
        spdlog::spdlog)

# shell32: `UserPaths.cpp` menanyakan folder Documents ke Windows lewat
# SHGetKnownFolderPath alih-alih menebaknya dari nama. PRIVATE — yang menautkan
# Sim::Core tidak ikut memanggilnya.
if(WIN32)
    target_link_libraries(SimCore PRIVATE shell32)
endif()

# Harus PUBLIC: setiap TU yang meng-include glm lewat Sim/Core/Math.h wajib
# memakai konvensi yang sama, kalau tidak matriks proyeksinya diam-diam berbeda.
target_compile_definitions(SimCore PUBLIC
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE      # Vulkan: depth clip [0,1], bukan [-1,1]
    GLM_ENABLE_EXPERIMENTAL
    GLM_FORCE_EXPLICIT_CTOR)
