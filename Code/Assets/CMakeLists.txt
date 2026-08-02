sim_add_library(Assets
    SOURCES
        src/AssetDatabase.cpp
        src/AssetTypes.cpp
        src/Importer.cpp
        src/Thumbnail.cpp
    PUBLIC_DEPS
        Sim::Core
    # PRIVATE: header publik Assets tidak membocorkan stb maupun nlohmann,
    # sehingga modul yang memakainya tidak ikut menyeret keduanya.
    PRIVATE_DEPS
        Stb::Stb
        nlohmann_json::nlohmann_json)
