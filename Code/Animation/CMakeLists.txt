sim_add_library(Animation
    SOURCES
        src/AnimationGraph.cpp
        src/AnimationIo.cpp
        src/Clip.cpp
        src/ClipHistory.cpp
        src/ClipImport.cpp
        src/GltfClipImport.cpp
        src/UsdClipImport.cpp
        src/GraphInstance.cpp
        src/Pose.cpp
        src/PoseTask.cpp
        src/Skeleton.cpp
    PUBLIC_DEPS
        # `Curve` tinggal di Core dan dipakai bersama Particle (E7.2) dan Terrain
        # (E7.3) — track animasi memakai persis kurva yang sama, beserta
        # tangennya, sehingga penyunting kurvanya juga sama.
        Sim::Core
    PRIVATE_DEPS
        nlohmann_json::nlohmann_json
        # Impor klip FBX. Privat: header publik Animation tidak membocorkan satu
        # pun tipe FBX SDK — sama seperti Terrain yang mengurus PNG heightmap-nya
        # sendiri lewat stb tanpa membocorkannya.
        Fbx::Fbx
        Cgltf::Cgltf)

# USD hanya kalau ada; UsdClipImport.cpp tetap ikut dibangun tanpanya dan
# menyediakan penolakan yang menyebut alasannya — pola yang sama dengan
# Code/Assets/CMakeLists.txt.
if(TARGET Usd::Usd)
    target_link_libraries(SimAnimation PRIVATE Usd::Usd)
    target_compile_definitions(SimAnimation PRIVATE SIM_WITH_USD=1)
endif()
