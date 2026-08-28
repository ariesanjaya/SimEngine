sim_add_library(Assets
    SOURCES
        src/AssetDatabase.cpp
        src/Cook.cpp
        src/AssetTypes.cpp
        src/BlockCompress.cpp
        src/Importer.cpp
        src/Ktx2Write.cpp
        src/GltfImport.cpp
        src/MaterialImport.cpp
        src/MaterialXImport.cpp
        src/MaxMaterial.cpp
        src/LightmapUv.cpp
        src/LightmapUnwrap.cpp
        src/MeshImport.cpp
        src/MeshSdfBake.cpp
        src/MeshSdfBakery.cpp
        src/MeshGeometryCache.cpp
        src/UsdImport.cpp
        src/MeshSettings.cpp
        src/TextureBake.cpp
        src/TextureBakery.cpp
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
        # Penulis kontainer KTX2 untuk baker tekstur. PRIVATE: tipe libktx tidak
        # pernah muncul di header publik modul ini.
        Ktx::Ktx
        # Encoder BC1/BC3/BC4/BC5/BC7, beserta sisi urainya untuk uji dan PSNR.
        Bc7Enc::Bc7Enc
        Fbx::Fbx
        Cgltf::Cgltf
        nlohmann_json::nlohmann_json)

# xatlas hanya kalau ada, dengan pola yang sama: `LightmapUnwrap.cpp` tetap ikut
# dibangun tanpanya dan menolak dengan alasan yang menyebut sakelarnya.
if(TARGET Xatlas::Xatlas)
    target_link_libraries(SimAssets PRIVATE Xatlas::Xatlas)
    target_compile_definitions(SimAssets PRIVATE SIM_WITH_XATLAS=1)
endif()

# MaterialX hanya kalau ada, dengan pola yang sama seperti USD:
# MaterialXImport.cpp tetap ikut dibangun tanpanya dan menolak dengan alasan yang
# menyebut sakelarnya — bukan dengan diam, yang akan terbaca sebagai "berkasnya
# memang tidak memuat material".
if(TARGET MaterialX::MaterialX)
    target_link_libraries(SimAssets PRIVATE MaterialX::MaterialX)
    target_compile_definitions(SimAssets PRIVATE SIM_WITH_MATERIALX=1)
endif()

# USD hanya kalau ada; UsdImport.cpp tetap ikut dibangun tanpanya dan menyediakan
# penolakan yang menyebut alasannya.
if(TARGET Usd::Usd)
    target_link_libraries(SimAssets PRIVATE Usd::Usd)
    target_compile_definitions(SimAssets PRIVATE SIM_WITH_USD=1)
endif()
