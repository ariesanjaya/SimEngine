sim_add_library(Terrain
    SOURCES
        src/HeightmapPng.cpp
        src/Terrain.cpp
        src/TerrainBrush.cpp
        src/TerrainIo.cpp
    PUBLIC_DEPS
        Sim::Core
    # PRIVATE: header publik Terrain tidak membocorkan nlohmann maupun stb.
    # stb dipakai dua arah — dekodernya untuk membaca PNG 16-bit, dan
    # kompresor deflate-nya untuk menulisnya.
    PRIVATE_DEPS
        nlohmann_json::nlohmann_json
        Stb::Impl)
