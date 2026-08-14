sim_add_test(SimCoreTests
    SOURCES
        CoreTests.cpp
    DEPS
        Sim::Core)

sim_add_test(SimSceneTests
    SOURCES
        SceneTests.cpp
    DEPS
        Sim::Scene)

# ImageIO: seam dekode gambar, kapabilitas per backend, dan penanganan berkas
# rusak. Bebas GPU dan bebas editor dengan sengaja — yang diuji adalah piksel
# yang keluar dari berkas, dan itu bisa diperiksa tanpa satu pun display.
sim_add_test(SimImageIOTests
    SOURCES
        ImageIOTests.cpp
    DEPS
        Sim::ImageIO)

# Berkas gambar uji dibaca dari folder sumber, alasan yang sama dengan
# SIM_MESH_DIR di bawah. SIM_CODE_DIR dipakai uji yang menyisir `Code/` untuk
# memastikan tidak ada `stbi_` yang lolos ke luar backend stb.
target_compile_definitions(SimImageIOTests
    PRIVATE SIM_IMAGE_DIR="${CMAKE_SOURCE_DIR}/Resources/Images"
    PRIVATE SIM_CODE_DIR="${CMAKE_SOURCE_DIR}/Code")

# Volume: bake mesh menjadi SDF. Nilainya diadu dengan jarak analitik, bukan
# sekadar "menghasilkan volume" — grid yang dimensinya benar tapi jaraknya
# meleset menghasilkan sphere tracing yang menembus dinding.
sim_add_test(SimVolumeTests
    SOURCES
        VolumeTests.cpp
    DEPS
        Sim::Volume)

# Fixture .vdb dibaca dari folder sumber, alasan yang sama dengan SIM_MESH_DIR.
target_compile_definitions(SimVolumeTests
    PRIVATE SIM_VOLUME_DIR="${CMAKE_SOURCE_DIR}/Resources/Volumes")

# Physics: lifecycle dunia, rigid body, langkah tetap, dan determinisme.
# Bebas GPU dan bebas editor — yang diuji adalah angka yang keluar dari solver.
sim_add_test(SimPhysicsTests
    SOURCES
        PhysicsTests.cpp
    DEPS
        Sim::Physics
        # **Uji-nya yang butuh keduanya, bukan Sim::Physics.** Ragdoll dibangun
        # dari daftar tulang generik; mengubah `animation::Skeleton` menjadi
        # daftar itu adalah lintasan sepuluh baris yang dimiliki pemanggil, dan
        # di sinilah lintasan itu dijalankan. Kalau `Sim::Physics` sendiri yang
        # menautkan Animation, setiap server dedicated ikut membawa importir FBX
        # dan USD di baliknya.
        Sim::Animation
        Sim::Assets)

# SIM_CODE_DIR dipakai uji yang menyisir `Code/` untuk memastikan tidak ada tipe
# PhysX yang lolos ke luar TU backend-nya.
target_compile_definitions(SimPhysicsTests
    PRIVATE SIM_CODE_DIR="${CMAKE_SOURCE_DIR}/Code")

# Jembatan scene ↔ simulasi: komponen, sinkronisasi transform dua arah, dan
# entity berinduk. Terpisah dari SimPhysicsTests karena yang diuji berbeda —
# yang itu solver, yang ini terjemahannya.
sim_add_test(SimPhysicsSceneTests
    SOURCES
        PhysicsSceneTests.cpp
    DEPS
        Sim::Physics
        Sim::Scene)

# Prefab bawaan ikut diuji dari berkasnya: template fisika yang tidak bisa
# dijatuhkan bukan template fisika.
target_compile_definitions(SimPhysicsSceneTests
    PRIVATE SIM_BUILTIN_DIR="${CMAKE_SOURCE_DIR}/Resources")

# Whitebox: topologi half-edge dan invariannya. Yang diuji di sini adalah
# struktur, bukan tampilan — dan pemeriksa invariannya sendiri ikut diuji bisa
# gagal, karena pemeriksa yang tidak bisa gagal tidak menjaga apa pun.
sim_add_test(SimWhiteboxTests
    SOURCES
        WhiteboxTests.cpp
    DEPS
        Sim::Whitebox
        Sim::Assets)

sim_add_test(SimEditorTests
    SOURCES
        EditorTests.cpp
    DEPS
        Sim::EditorFramework)

# Impor berkas sumber diuji terhadap mesh sungguhan di folder sumber, alasan
# yang sama dengan SIM_MESH_DIR pada SimAssetTests.
target_compile_definitions(SimEditorTests
    PRIVATE SIM_MESH_DIR="${CMAKE_SOURCE_DIR}/Resources/Meshes"
    # Shader renderer dan `openpbr.slang`, dibaca dari folder sumber: uji yang
    # mengompilasi material harus memakai berkas yang sedang ada di repo.
    PRIVATE SIM_SHADER_DIR="${CMAKE_SOURCE_DIR}/Shaders"
    # Material bawaan editor, dipakai uji sebagai material sungguhan yang bisa
    # dikompilasi — bukan graph buatan uji, yang hanya membuktikan uji itu
    # sepakat dengan dirinya sendiri.
    PRIVATE SIM_BUILTIN_DIR="${CMAKE_SOURCE_DIR}/Resources")

sim_add_test(SimAssetTests
    SOURCES
        AssetTests.cpp
    DEPS
        Sim::Assets
        # **Uji-nya yang butuh keduanya, bukan Sim::Assets.** Baker menulis
        # `.ktx2` lewat libktx; yang membacanya di runtime adalah pembaca yang
        # ditulis tangan di `Sim::RHI`. Round-trip di sini karena itu
        # membandingkan dua implementasi yang berbeda — dan itulah satu-satunya
        # bentuk round-trip yang membuktikan sesuatu. Menautkannya dari
        # `Sim::Assets` sendiri akan menyeret Vulkan ke dalam baker, yang harus
        # tetap bisa jalan di mesin build tanpa GPU.
        Sim::RHI
        # Dipakai uji yang **menulis** berkas sumbernya sendiri. Fixture di
        # `Resources/Images` semuanya berukuran delapan piksel — cukup untuk
        # memeriksa tata letak, dan tidak cukup untuk mengukur penghematan VRAM:
        # pada tekstur sekecil itu level terdalam yang tetap menempati satu blok
        # utuh justru mendominasi hasilnya.
        Sim::ImageIO)

# Folder mesh sumber, bukan salinannya di build — alasan yang sama dengan
# SIM_SHADER_DIR di bawah: uji yang membaca aset harus membaca berkas yang
# sedang ada di repo, bukan salinan yang tertinggal satu build.
target_compile_definitions(SimAssetTests
    PRIVATE SIM_MESH_DIR="${CMAKE_SOURCE_DIR}/Resources/Meshes"
    # Gambar uji untuk jalur thumbnail, dibaca dari folder sumber dengan alasan
    # yang sama. Isinya diterangkan di Tests/ImageIOTests.cpp.
    PRIVATE SIM_IMAGE_DIR="${CMAKE_SOURCE_DIR}/Resources/Images"
    # Material induk untuk material hasil impor tinggal di sini, beserta
    # `.meta`-nya — dan GUID di berkas itu yang harus cocok dengan tetapan di
    # `MaterialImport.h`.
    PRIVATE SIM_BUILTIN_DIR="${CMAKE_SOURCE_DIR}/Resources")

# Material: model graph `.simmat`, katalog node (keluarannya mengikuti OpenPBR
# Surface v1.1), validasi, tata letak blok uniform, dan cache SPIR-V. Tidak
# butuh Lua maupun editor.
sim_add_test(SimMaterialTests
    SOURCES
        MaterialTests.cpp
    DEPS
        Sim::Material)

# Direktori shader sumber, bukan salinannya di build. Test yang membaca
# openpbr.slang harus membaca berkas yang sedang disunting orang — salinan yang
# tertinggal satu build akan membuat test lulus atas kode yang sudah berubah.
target_compile_definitions(SimMaterialTests
    PRIVATE SIM_SHADER_DIR="${CMAKE_SOURCE_DIR}/Shaders"
    # Dipakai uji yang membandingkan berkas shader yang **ditanam** modul
    # material dengan berkas yang **disalin** CMake ke sebelah executable.
    PRIVATE SIM_CODE_DIR="${CMAKE_SOURCE_DIR}/Code"
    # Isi bawaan editor, dibaca dari folder sumber dengan alasan yang sama:
    # yang diuji harus berkas yang sedang ada di repo, bukan salinan yang
    # tertinggal satu build.
    PRIVATE SIM_BUILTIN_DIR="${CMAKE_SOURCE_DIR}/Resources")

# Terrain: penyimpanan heightmap berubin, brush, undo per goresan, dan
# round-trip heightmap PNG 16-bit/RAW.
sim_add_test(SimTerrainTests
    SOURCES
        TerrainTests.cpp
    DEPS
        Sim::Terrain
        # Sim::ImageIO adalah dependensi privat Terrain, jadi uji harus
        # memintanya sendiri — sama seperti SimRenderTests.
        Sim::ImageIO)

# Heightmap TIFF uji, dibaca dari folder sumber dengan alasan yang sama dengan
# SIM_MESH_DIR. Isinya diterangkan di Tests/ImageIOTests.cpp.
target_compile_definitions(SimTerrainTests
    PRIVATE SIM_IMAGE_DIR="${CMAKE_SOURCE_DIR}/Resources/Images")

# Vegetation: sebaran deterministik dari aturan penempatan, peta kepadatan yang
# dicat, suntingan tangan yang bertahan, dan format `.simveg`.
sim_add_test(SimVegetationTests
    SOURCES
        VegetationTests.cpp
    DEPS
        Sim::Vegetation)

# Render: frame graph (pembuangan pass, barrier, alias memori) dan frustum
# culling. Keduanya bebas Vulkan dengan sengaja, jadi keduanya bisa diuji tanpa
# GPU — dan justru di keduanyalah kesalahan renderer paling mahal dicari.
sim_add_test(SimRenderTests
    SOURCES
        RenderTests.cpp
    DEPS
        Sim::Render
        # Sim::ImageIO adalah dependensi privat Render, jadi uji harus
        # memintanya sendiri — sama seperti SimAnimationTests dengan Sim::Assets.
        Sim::ImageIO
        # Pembaca KTX2 tinggal di RHI, dan RHI privat milik Render.
        Sim::RHI)

# Unggahan tekstur ke GPU sungguhan. **Satu-satunya uji yang menuntut Vulkan**,
# dan ia melewati dirinya sendiri di mesin yang tidak punya — menggagalkan build
# di sana akan membuat seluruh suite tidak bisa dipercaya di mesin itu.
#
# Terpisah dari SimRenderTests dengan sengaja: yang itu bebas GPU, dan mencampur
# keduanya berarti tidak ada lagi satu pun uji render yang bisa dijalankan tanpa
# driver.
sim_add_test(SimTextureUploadTests
    SOURCES
        TextureUploadTests.cpp
    DEPS
        Sim::RHI
        Sim::Assets)

target_compile_definitions(SimTextureUploadTests
    PRIVATE SIM_IMAGE_DIR="${CMAKE_SOURCE_DIR}/Resources/Images")

# Peta lingkungan uji untuk jalur IBL, dibaca dari folder sumber dengan alasan
# yang sama dengan SIM_MESH_DIR di atas.
target_compile_definitions(SimRenderTests
    PRIVATE SIM_IMAGE_DIR="${CMAKE_SOURCE_DIR}/Resources/Images"
    # Dipakai uji yang menyisir `Code/Render` untuk memastikan jalur tekstur
    # material tidak pernah mendekode gambar sendiri — aturan yang sama bentuknya
    # dengan `stbi_` di I0, dan alasannya juga sama: jalur kedua yang terlanjur
    # ada tidak akan pernah dihapus.
    PRIVATE SIM_CODE_DIR="${CMAKE_SOURCE_DIR}/Code")

# Animation: rangka berurutan topologis, klip berbasis kurva, pencampuran pose,
# penanda fase, root motion, dan format `.simskel`/`.simanim`.
sim_add_test(SimAnimationTests
    SOURCES
        AnimationTests.cpp
    DEPS
        Sim::Animation
        # Kriteria "pose menggerakkan kulit" melintasi dua modul dengan sengaja:
        # yang menghasilkan palet ada di Animation, yang menerapkannya — acuan
        # CPU untuk vertex shader — ada di Assets. Menguji keduanya sendiri-
        # sendiri tidak menangkap sambungannya, dan justru di sambungan itulah
        # kesalahan skinning bersembunyi.
        Sim::Assets)

# Aset klip uji — unitSpin.usda dan unitSpin.gltf — dibaca dari folder sumber,
# alasan yang sama dengan SIM_MESH_DIR pada SimAssetTests.
target_compile_definitions(SimAnimationTests
    PRIVATE SIM_MESH_DIR="${CMAKE_SOURCE_DIR}/Resources/Meshes")

# Particle: model efek `.simfx`, kurva/gradient, dan simulasi CPU deterministik.
sim_add_test(SimParticleTests
    SOURCES
        ParticleTests.cpp
    DEPS
        Sim::Particle)

sim_add_test(SimLevelEditorTests
    SOURCES
        LevelEditorTests.cpp
    DEPS
        Sim::EditorFramework)

# Material bawaan dibaca dari folder sumber, alasan yang sama dengan yang lain.
target_compile_definitions(SimLevelEditorTests
    PRIVATE SIM_BUILTIN_DIR="${CMAKE_SOURCE_DIR}/Resources")

if(SIM_WITH_LUA)
    sim_add_test(SimScriptTests
        SOURCES
            ScriptTests.cpp
        DEPS
            Sim::Script
            # Hot reload diuji lewat jalur yang dipakai editor: AssetDatabase
            # yang melaporkan berkas mana yang berubah. Sim::Assets adalah
            # dependensi privat Script, jadi test harus memintanya sendiri.
            Sim::Assets)

    # Visual scripting: model graph, katalog node, dan compiler graph → Lua.
    # Sim::Assets diminta sendiri dengan alasan yang sama seperti di atas —
    # kriteria terimanya menjalankan Lua hasil kompilasi lewat AssetDatabase.
    sim_add_test(SimGraphTests
        SOURCES
            GraphTests.cpp
        DEPS
            Sim::Script
            Sim::Assets)

    # Terpisah dari SimEditorTests karena hanya ada saat Lua ikut dibangun.
    # Yang diuji di sini adalah jembatannya — menu, panel, dan command yang
    # didaftarkan Lua — bukan `sim.ui`, yang menuntut konteks ImGui hidup.
    sim_add_test(SimEditorScriptingTests
        SOURCES
            EditorScriptingTests.cpp
        DEPS
            Sim::EditorFramework
            Sim::Script
            Sim::Assets)
endif()
