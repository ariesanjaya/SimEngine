sim_add_test(SimCoreTests
    SOURCES
        CoreTests.cpp
    DEPS
        Sim::Core)

# Raycast: BVH dua tingkat dan ketiga query-nya. Bebas GPU dan bebas editor —
# itu bagian dari yang diuji, bukan kebetulan: modul ini harus bisa dipakai
# SimHeadless tanpa satu pun display.
sim_add_test(SimRaycastTests
    SOURCES
        RaycastTests.cpp
    DEPS
        Sim::Raycast)

# Dipakai uji yang menyisir `Code/` untuk memastikan Möller–Trumbore tidak punya
# salinan kedua. Pola yang sama dengan SIM_CODE_DIR di SimImageIOTests.
target_compile_definitions(SimRaycastTests
    PRIVATE SIM_CODE_DIR="${CMAKE_SOURCE_DIR}/Code")

if(SIM_WITH_EMBREE)
    # **Satu-satunya tempat pilihan backend boleh terlihat dari luar modul.**
    # `SimRaycast` menyimpan definisi ini PRIVATE justru supaya tidak ada
    # pemanggil yang bercabang menurutnya; ujinya adalah pengecualian, karena
    # yang diperiksanya memang bahwa `SelectedBackend()` menyebut yang benar.
    target_compile_definitions(SimRaycastTests PRIVATE SIM_WITH_EMBREE=1)
endif()

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

# **Uji yang menautkan editor ikut gerbang SIM_BUILD_EDITOR.** Yang dilewati di
# sini bukan cakupan yang hilang melainkan cakupan yang tidak ada targetnya:
# build tanpa editor tidak punya `Sim::EditorFramework` untuk diuji.
if(SIM_BUILD_EDITOR)
    sim_add_test(SimEditorTests
        SOURCES
            EditorTests.cpp
        DEPS
            Sim::EditorFramework)

    # Impor berkas sumber diuji terhadap mesh sungguhan di folder sumber, alasan
    # yang sama dengan SIM_MESH_DIR pada SimAssetTests.
    target_compile_definitions(SimEditorTests
        PRIVATE SIM_MESH_DIR="${CMAKE_SOURCE_DIR}/Resources/Meshes"
        # Panel-nya sendiri ada di `Sim::Editor`, yang sengaja tidak ditautkan ke
        # uji mana pun. Aturan "panel yang tidak di-dock harus mulai tertutup"
        # karena itu ditegakkan dengan menyisir sumbernya, pola yang sama dengan
        # SIM_CODE_DIR pada SimImageIOTests.
        PRIVATE SIM_CODE_DIR="${CMAKE_SOURCE_DIR}/Code"
        # Shader renderer dan `openpbr.slang`, dibaca dari folder sumber: uji yang
        # mengompilasi material harus memakai berkas yang sedang ada di repo.
        PRIVATE SIM_SHADER_DIR="${CMAKE_SOURCE_DIR}/Shaders"
        # Material bawaan editor, dipakai uji sebagai material sungguhan yang bisa
        # dikompilasi — bukan graph buatan uji, yang hanya membuktikan uji itu
        # sepakat dengan dirinya sendiri.
        PRIVATE SIM_BUILTIN_DIR="${CMAKE_SOURCE_DIR}/Resources")
endif()

sim_add_test(SimAssetTests
    SOURCES
        AssetTests.cpp
    DEPS
        Sim::Assets
        # Bake SDF mesh ada di `Sim::Assets`, pustakanya di `Sim::Volume`, dan
        # tautannya di sana PRIVATE — sengaja: OpenVDB adalah pengondisi aset,
        # bukan sesuatu yang ikut ke jalur yang dikirim. Uji-nya perlu bertanya
        # "build ini punya OpenVDB atau tidak" untuk melewatkan dirinya sendiri
        # bila tidak, jadi ia menautkannya sendiri.
        Sim::Volume
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
        Sim::ImageIO
        # S4 memeriksa UV ubin terrain dengan pemeriksa yang sama yang dipakai
        # importir mesh. Uji yang menulis ulang aturannya hanya menguji
        # salinannya.
        Sim::Assets)

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

# **Uji yang menautkan editor ikut gerbang SIM_BUILD_EDITOR.** Yang dilewati di
# sini bukan cakupan yang hilang melainkan cakupan yang tidak ada targetnya:
# build tanpa editor tidak punya `Sim::EditorFramework` untuk diuji.
if(SIM_BUILD_EDITOR)
    sim_add_test(SimLevelEditorTests
        SOURCES
            LevelEditorTests.cpp
        DEPS
            Sim::EditorFramework)

    # Material bawaan dibaca dari folder sumber, alasan yang sama dengan yang lain.
    target_compile_definitions(SimLevelEditorTests
        PRIVATE SIM_BUILTIN_DIR="${CMAKE_SOURCE_DIR}/Resources")
endif()

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
    if(SIM_BUILD_EDITOR)
        sim_add_test(SimEditorScriptingTests
            SOURCES
                EditorScriptingTests.cpp
            DEPS
                Sim::EditorFramework
                Sim::Script
                Sim::Assets)
    endif()
endif()

# AIBridge diuji lewat HTTP sungguhan, bukan dengan memanggil dispatcher-nya
# langsung — yang dilakukan agen adalah mengirim byte ke soket, jadi itu yang
# diuji. httplib dan nlohmann karena itu ikut ditautkan di sini walaupun
# keduanya PRIVATE di modulnya: yang butuh keduanya adalah kliennya, bukan
# header AIBridge.
sim_add_test(SimAIBridgeTests
    SOURCES
        AIBridgeTests.cpp
    DEPS
        Sim::AIBridge
        httplib::httplib
        nlohmann_json::nlohmann_json)

# Tool MCP diuji di tingkat semantiknya, bukan di tingkat kawat — yang terakhir
# sudah ditutup SimAIBridgeTests. Yang dikunci di sini adalah apa yang dilakukan
# sebuah tool call terhadap World, Selection, dan CommandHistory, dengan
# EditorApp yang berjalan tanpa GPU maupun jendela.
# **Uji yang menautkan editor ikut gerbang SIM_BUILD_EDITOR.** Yang dilewati di
# sini bukan cakupan yang hilang melainkan cakupan yang tidak ada targetnya:
# build tanpa editor tidak punya `Sim::EditorFramework` untuk diuji.
if(SIM_BUILD_EDITOR)
    sim_add_test(SimAiToolsTests
        SOURCES
            AiToolsTests.cpp
        DEPS
            Sim::EditorFramework
            Sim::AIBridge
            Sim::ImageIO
            # Tool A3 menjalankan Lua sungguhan; harness-nya membuat ScriptRuntime
            # sendiri karena EditorApp menerimanya dari pemanggil, bukan membuatnya.
            Sim::Script
            # Uji terrain menyemai terrain lewat TerrainStore::Adopt — jalur yang
            # sama dengan menu "Terrain baru", bukan berkas yang dikarang uji.
            Sim::Terrain
            nlohmann_json::nlohmann_json)
endif()

# Path tracer acuan — R4. Ikut gerbang slangc karena Sim::Reference tidak ada
# tanpanya: model shading-nya dibangkitkan slangc, bukan ditulis tangan.
if(TARGET Sim::Reference)
    sim_add_test(SimReferenceTests
        SOURCES
            ReferenceTests.cpp
        DEPS
            Sim::Reference
            # Uji EXR membaca kembali gambar yang ditulisnya.
            Sim::ImageIO
            # B5 membandingkan tingkat panggang terhadap jalur acuan, jadi ia
            # butuh pemanggangnya sendiri — `AtmosphereSky`, `ProjectIrradiance`,
            # `EvaluateIrradiance`. Acuan yang membandingkan dirinya dengan
            # tiruan pemanggang tidak membandingkan apa pun.
            Sim::Render)
endif()

# --- Model shading OpenPBR dijalankan di CPU ---------------------------------
#
# **Yang diuji berkas ini bukan tiruan `openpbr.slang`, melainkan berkas itu
# sendiri.** C++-nya dipancarkan `slangc -target cpp` dari sumber yang sama yang
# dikompilasi ke SPIR-V untuk GPU, jadi drift bukan sesuatu yang diuji melainkan
# sesuatu yang tidak bisa terjadi.
#
# Pembangkitannya milik `Sim::Reference`, bukan milik direktori ini: dua langkah
# codegen atas berkas yang sama berarti dua salinan yang bisa berselisih, dan
# yang berselisih di sini justru model shading yang seluruh gunanya tunggal.
if(TARGET Sim::Reference)
    sim_add_test(SimOpenPbrCpuTests
        SOURCES
            OpenPbrCpuTests.cpp
        DEPS
            Sim::Reference
            # Uji tungku menuntut LUT DFG yang **sungguhan**, bukan angka yang
            # ditulis di ujinya: yang diperiksa adalah bahwa integral DFG mesin
            # ini dan model shading mesin ini bersama-sama mengawetkan energi.
            # Dua nilai karangan hanya menguji aritmatika penjumlahan.
            Sim::Render)
endif()

# --- Pengalamatan kisi probe dijalankan di CPU (S2) ---------------------------
#
# **Yang diuji berkas ini bukan tiruan `probe_grid.slang`, melainkan berkas itu
# sendiri** — pola dan alasan yang sama dengan `SimOpenPbrCpuTests` di atas.
#
# Yang dijaganya satu hal yang salahnya paling sunyi: shader membaca kisi probe,
# C++ menulisnya, dan indeks yang meleset satu brick tetap mengembalikan angka
# yang masuk akal. Yang terlihat cuma cahaya tak-langsung yang bergeser beberapa
# meter — dan di S2 itu akan terbaca sebagai transport yang salah, bukan sebagai
# pengalamatan yang salah.
#
# Codegen-nya di sini, bukan di `Sim::Render`: yang membutuhkan kembaran CPU-nya
# hanya uji ini, dan menaruh ketergantungan slangc di dalam modul runtime berarti
# renderer tidak bisa dibangun tanpa generator yang tidak dipakainya saat jalan.
if(SIM_SLANGC)
    set(SIM_PROBE_GRID_CPU "${CMAKE_CURRENT_BINARY_DIR}/probe_grid_cpu.cpp")
    add_custom_command(
        OUTPUT  "${SIM_PROBE_GRID_CPU}"
        COMMAND "${SIM_SLANGC}" "${CMAKE_SOURCE_DIR}/Shaders/probe_grid_cpu.slang"
                -target cpp -o "${SIM_PROBE_GRID_CPU}"
                -I "${CMAKE_SOURCE_DIR}/Shaders"
        # `probe_grid.slang` ikut sebagai dependensi eksplisit, alasan yang sama
        # dengan `openpbr.slang`: tanpanya, mengubah pengalamatan tidak
        # membangkitkan ulang C++-nya, dan ujinya akan mengadu sisi C++ dengan
        # shader yang sudah tidak ada lagi.
        DEPENDS "${CMAKE_SOURCE_DIR}/Shaders/probe_grid_cpu.slang"
                "${CMAKE_SOURCE_DIR}/Shaders/probe_grid.slang"
        COMMENT "slangc -target cpp probe_grid_cpu.slang"
        VERBATIM)
    set_source_files_properties(ProbeGridCpuTests.cpp
        PROPERTIES OBJECT_DEPENDS "${SIM_PROBE_GRID_CPU}")

    sim_add_test(SimProbeGridCpuTests
        SOURCES
            ProbeGridCpuTests.cpp
        DEPS
            Sim::Render)
    target_include_directories(SimProbeGridCpuTests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    # C++ yang dipancarkan slangc bukan kode kita.
    target_compile_options(SimProbeGridCpuTests PRIVATE -Wno-error)
endif()
