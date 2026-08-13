sim_add_library(Terrain
    SOURCES
        src/Terrain.cpp
        src/TerrainBrush.cpp
        src/TerrainIo.cpp
        src/TerrainMesh.cpp
        src/TerrainPicking.cpp
    PUBLIC_DEPS
        # `MeshData` muncul di header `TerrainMesh.h`: yang meminta mesh sebuah
        # ubin menerima tipe itu, jadi ia tidak bisa privat. Arahnya sekali
        # jalan — Terrain menghasilkan MeshData, dan Assets tidak tahu apa itu
        # terrain.
        Sim::Assets
        Sim::Core
    # PRIVATE: header publik Terrain tidak membocorkan nlohmann maupun ImageIO.
    #
    # **stb sudah tidak ada di sini sama sekali.** Enkoder PNG 16-bit yang dulu
    # tinggal di `HeightmapPng.cpp` pindah ke `Sim::ImageIO` pada I3, dan
    # bersamanya satu-satunya alasan modul terrain pernah mengenal stb.
    PRIVATE_DEPS
        Sim::ImageIO
        nlohmann_json::nlohmann_json)
