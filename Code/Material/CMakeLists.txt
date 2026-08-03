sim_add_library(Material
    SOURCES
        src/MaterialCompiler.cpp
        src/MaterialGraph.cpp
        src/MaterialNodeCatalog.cpp
        src/MaterialValidation.cpp
    PUBLIC_DEPS
        Sim::Core
    # PRIVATE: header publik Material tidak membocorkan nlohmann, sehingga
    # modul yang memakainya tidak ikut menyeretnya.
    PRIVATE_DEPS
        nlohmann_json::nlohmann_json)
