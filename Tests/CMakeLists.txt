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
            Sim::Script)
endif()
