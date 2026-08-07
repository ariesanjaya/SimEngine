sim_add_library(Animation
    SOURCES
        src/AnimationGraph.cpp
        src/AnimationIo.cpp
        src/Clip.cpp
        src/ClipHistory.cpp
        src/GraphInstance.cpp
        src/Pose.cpp
        src/Skeleton.cpp
    PUBLIC_DEPS
        # `Curve` tinggal di Core dan dipakai bersama Particle (E7.2) dan Terrain
        # (E7.3) — track animasi memakai persis kurva yang sama, beserta
        # tangennya, sehingga penyunting kurvanya juga sama.
        Sim::Core
    PRIVATE_DEPS
        nlohmann_json::nlohmann_json)
