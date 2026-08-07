sim_add_library(Material
    SOURCES
        src/MaterialCompiler.cpp
        src/MaterialGraph.cpp
        src/MaterialInstance.cpp
        src/MaterialParameterBlock.cpp
        src/MaterialNodeCatalog.cpp
        src/MaterialValidation.cpp
        src/MaterialShaderModule.cpp
        src/ShaderCache.cpp
    PUBLIC_DEPS
        Sim::Core
    # PRIVATE: header publik Material tidak membocorkan nlohmann, sehingga
    # modul yang memakainya tidak ikut menyeretnya.
    PRIVATE_DEPS
        nlohmann_json::nlohmann_json)
