sim_add_library(ImageIO
    SOURCES
        src/BackendExr.cpp
        src/BackendOiio.cpp
        src/BackendStb.cpp
        src/BackendTiff.cpp
        src/Image.cpp
        src/ImageIO.cpp
        src/MipChain.cpp
        src/PixelOps.cpp
        src/PngWrite.cpp
        src/TextureColor.cpp
    # Semuanya PRIVATE. Header publik modul ini hanya menyebut tipe standar —
    # tidak stb, tidak tinyexr, tidak libtiff, tidak spdlog — sehingga modul
    # yang memakainya tidak ikut menyeret satu pun.
    #
    # Yang tidak ada di daftar ini justru yang penting: **Sim::RHI**.
    # `Sim::ImageIO` menghasilkan piksel di memori dan berhenti di situ; yang
    # mengunggahnya ke GPU adalah pemanggilnya. Aturan yang sama menjaga jalur
    # baker dan headless tetap bisa jalan tanpa display.
    PRIVATE_DEPS
        Sim::Core
        Stb::Impl)

# EXR dan TIFF hanya kalau pustakanya ada. Kedua berkas backend tetap ikut
# dibangun tanpanya dan mengembalikan nullptr, mengikuti pola UsdImport.cpp di
# Sim::Assets: yang hilang adalah kemampuannya, bukan berkasnya — dan berkas
# yang disingkirkan dari daftar SOURCES membuat kesalahan sintaks di dalamnya
# baru ketahuan di mesin orang lain.
if(TARGET Oiio::Oiio)
    target_link_libraries(SimImageIO PRIVATE Oiio::Oiio)
    target_compile_definitions(SimImageIO PRIVATE
        SIM_WITH_OIIO=1
        SIM_OIIO_VERSION_STRING="${SIM_OIIO_VERSION}")
endif()

if(TARGET TinyExr::TinyExr)
    target_link_libraries(SimImageIO PRIVATE TinyExr::TinyExr)
    target_compile_definitions(SimImageIO PRIVATE
        SIM_WITH_TINYEXR=1
        SIM_TINYEXR_VERSION_STRING="3.2.0")
endif()

if(SIM_WITH_TIFF AND TIFF_FOUND)
    target_link_libraries(SimImageIO PRIVATE TIFF::TIFF)
    target_compile_definitions(SimImageIO PRIVATE
        SIM_WITH_LIBTIFF=1
        SIM_LIBTIFF_VERSION_STRING="${TIFF_VERSION_STRING}")
endif()
