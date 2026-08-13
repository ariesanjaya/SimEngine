sim_add_library(Vegetation
    SOURCES
        src/Vegetation.cpp
        src/VegetationBrush.cpp
        src/VegetationIo.cpp
    PUBLIC_DEPS
        Sim::Core
        # Publik: aturan penempatan membaca terrain, dan `Terrain` muncul di
        # header. Vegetasi tanpa permukaan tidak punya tempat berdiri — jadi
        # ketergantungan ini bukan kemudahan melainkan definisinya.
        Sim::Terrain
    # PRIVATE: header publik Vegetation tidak membocorkan nlohmann maupun
    # ImageIO. Yang dibutuhkan di sini hanya satu arah — membaca peta kepadatan;
    # yang menulisnya adalah enkoder PNG milik modul Terrain, dipakai ulang
    # supaya peta kepadatan dan peta bobot berformat sama persis.
    PRIVATE_DEPS
        Sim::ImageIO
        nlohmann_json::nlohmann_json)
