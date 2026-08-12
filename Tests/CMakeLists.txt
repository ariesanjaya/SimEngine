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

sim_add_test(SimEditorTests
    SOURCES
        EditorTests.cpp
    DEPS
        Sim::EditorFramework)

sim_add_test(SimAssetTests
    SOURCES
        AssetTests.cpp
    DEPS
        Sim::Assets)

# Folder mesh sumber, bukan salinannya di build — alasan yang sama dengan
# SIM_SHADER_DIR di bawah: uji yang membaca aset harus membaca berkas yang
# sedang ada di repo, bukan salinan yang tertinggal satu build.
target_compile_definitions(SimAssetTests
    PRIVATE SIM_MESH_DIR="${CMAKE_SOURCE_DIR}/Resources/Meshes")

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
        Sim::Terrain)

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
        Sim::Render)

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
