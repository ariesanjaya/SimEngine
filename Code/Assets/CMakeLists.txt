sim_add_library(Assets
    SOURCES
        src/AssetDatabase.cpp
        src/AssetTypes.cpp
        src/Importer.cpp
        src/GltfImport.cpp
        src/MeshImport.cpp
        src/MeshSettings.cpp
        src/Thumbnail.cpp
    PUBLIC_DEPS
        Sim::Core
    # PRIVATE: header publik Assets tidak membocorkan stb, ufbx, maupun
    # nlohmann, sehingga modul yang memakainya tidak ikut menyeret ketiganya.
    PRIVATE_DEPS
        Stb::Impl
        Ufbx::Ufbx
        Cgltf::Cgltf
        nlohmann_json::nlohmann_json)
