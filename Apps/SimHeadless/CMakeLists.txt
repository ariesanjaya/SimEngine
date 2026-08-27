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
    Sim::AIBridge
    # Mode ukur menyandikan frame terakhir menjadi PNG. Disebut di sini walau
    # EditorFramework sudah menariknya: yang memakai sebuah pustaka langsung
    # menyebutnya sendiri, kalau tidak ia patah pada hari ketergantungan tak
    # langsung itu dilepas.
    Sim::ImageIO)

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

# Resources disalin oleh target SimResources di CMakeLists akar, dan di sini itu
# bukan sekadar kenyamanan: `config.resourceDir` menunjuk folder itu, jadi indeks
# aset bawaan yang dilihat mode headless adalah isinya apa adanya. SimResources
# ikut dijalankan pada setiap build target ini, jadi yang dibacanya tidak pernah
# salinan sisa build SimEditor terakhir.
add_dependencies(SimHeadless SimResources)
