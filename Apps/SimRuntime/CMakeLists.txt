# SimRuntime: player. Menautkan EditorFramework hanya karena terjemahan
# `World` → `ViewportScene` masih tinggal di `SceneView` — utang lapisan yang
# dicatat di src/main.cpp dan di docs/PLAN-RENDER.md, dan yang harus dibayar
# sebelum E9 dianggap selesai. Sebuah player tidak seharusnya membawa ImGui.
set(_sim_runtime_deps
    Sim::Core
    Sim::Platform
    Sim::RHI
    Sim::Render
    Sim::Scene
    Sim::Assets
    Sim::Physics
    Sim::ImageIO
    Sim::EditorFramework
    SDL3::SDL3)

if(SIM_WITH_LUA)
    list(APPEND _sim_runtime_deps Sim::Script)
endif()

sim_add_executable(SimRuntime
    SOURCES src/main.cpp
    DEPS ${_sim_runtime_deps})

if(SIM_WITH_LUA)
    target_compile_definitions(SimRuntime PRIVATE SIM_WITH_LUA=1)
endif()

add_dependencies(SimRuntime SimShadersGrid)

# Shader dan Resources disalin ke sebelah executable, alasan yang sama dengan
# SimEditor: player harus bisa dijalankan dari folder build apa adanya.
add_custom_command(TARGET SimRuntime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/Resources $<TARGET_FILE_DIR:SimRuntime>/Resources)
