# SimHeadless: editor yang sama tanpa jendela, dikendalikan lewat MCP.
#
# **Tidak menautkan Sim::EditorPanels maupun SDL3.** Itu bukan penghematan
# melainkan definisinya: panel memanggil ImGui, dan SDL membuka jendela. Yang
# tersisa adalah inti editor — dunia, indeks aset, riwayat undo, dan tool MCP —
# yang memang tidak pernah menyentuh keduanya.
set(_sim_headless_deps
    Sim::Core
    Sim::RHI
    Sim::Render
    Sim::EditorFramework
    Sim::AIBridge)

if(SIM_WITH_LUA)
    list(APPEND _sim_headless_deps Sim::Script)
endif()

sim_add_executable(SimHeadless
    SOURCES src/main.cpp
    DEPS ${_sim_headless_deps})

if(SIM_WITH_LUA)
    target_compile_definitions(SimHeadless PRIVATE SIM_WITH_LUA=1)
endif()

add_dependencies(SimHeadless SimShadersGrid)
