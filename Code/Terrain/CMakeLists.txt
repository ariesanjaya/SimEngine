sim_add_library(Terrain
    SOURCES
        src/Terrain.cpp
        src/TerrainBrush.cpp
        src/TerrainIo.cpp
    PUBLIC_DEPS
        Sim::Core
    # PRIVATE: header publik Terrain tidak membocorkan nlohmann maupun ImageIO.
    #
    # **stb sudah tidak ada di sini sama sekali.** Enkoder PNG 16-bit yang dulu
    # tinggal di `HeightmapPng.cpp` pindah ke `Sim::ImageIO` pada I3, dan
    # bersamanya satu-satunya alasan modul terrain pernah mengenal stb.
    PRIVATE_DEPS
        Sim::ImageIO
        nlohmann_json::nlohmann_json)
