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

# Material: model graph `.simmat`, katalog node (keluarannya mengikuti OpenPBR
# Surface v1.1), dan validasi. Tidak butuh Lua maupun editor.
sim_add_test(SimMaterialTests
    SOURCES
        MaterialTests.cpp
    DEPS
        Sim::Material)

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
