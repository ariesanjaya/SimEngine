sim_add_library(Particle
    SOURCES
        src/ParticleEffect.cpp
        src/ParticleSystem.cpp
    PUBLIC_DEPS
        Sim::Core
    # PRIVATE: header publik Particle tidak membocorkan nlohmann.
    PRIVATE_DEPS
        nlohmann_json::nlohmann_json)
