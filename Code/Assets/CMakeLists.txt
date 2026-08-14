sim_add_library(Assets
    SOURCES
        src/AssetDatabase.cpp
        src/AssetTypes.cpp
        src/Importer.cpp
        src/GltfImport.cpp
        src/MaterialImport.cpp
        src/MeshImport.cpp
        src/UsdImport.cpp
        src/MeshSettings.cpp
        src/TextureSettings.cpp
        src/Thumbnail.cpp
    PUBLIC_DEPS
        Sim::Core
        # Material impor menjadi `.simmatinst`, dan tipe itu milik Sim::Material.
        # PUBLIC karena `MaterialImport.h` menyebutnya di tanda tangannya.
        Sim::Material
    # PRIVATE: header publik Assets tidak membocorkan ImageIO, stb, FBX SDK,
    # maupun nlohmann, sehingga modul yang memakainya tidak ikut menyeretnya.
    # Dekode gambar lewat Sim::ImageIO; yang tersisa dari stb hanya
    # `stb_image_resize2` untuk menskala thumbnail, yang bukan urusan I/O.
    PRIVATE_DEPS
        Sim::ImageIO
        # Kapabilitas volume dibaca dari sini, dengan alasan yang sama seperti
        # kapabilitas gambar: daftar formatnya dibangkitkan, bukan dipatok.
        Sim::Volume
        Stb::Impl
        Fbx::Fbx
        Cgltf::Cgltf
        nlohmann_json::nlohmann_json)

# USD hanya kalau ada; UsdImport.cpp tetap ikut dibangun tanpanya dan menyediakan
# penolakan yang menyebut alasannya.
if(TARGET Usd::Usd)
    target_link_libraries(SimAssets PRIVATE Usd::Usd)
    target_compile_definitions(SimAssets PRIVATE SIM_WITH_USD=1)
endif()
