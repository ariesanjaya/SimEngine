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

# Resources disalin ke sebelah executable, alasan yang sama dengan SimEditor —
# dan di sini bukan sekadar kenyamanan. `config.resourceDir` menunjuk folder ini,
# jadi indeks aset bawaan yang dilihat mode headless adalah isi folder ini apa
# adanya. Tanpa aturan salin sendiri, yang dibacanya adalah salinan yang
# kebetulan ditinggalkan build SimEditor terakhir — aset yang ditambahkan
# sesudahnya lalu tidak terlihat, dan yang mencarinya akan mencari di berkas
# sumber yang isinya memang sudah benar.
add_custom_command(TARGET SimHeadless POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/Resources" "$<TARGET_FILE_DIR:SimHeadless>/Resources"
    COMMENT "Menyalin Resources ke folder keluaran"
    VERBATIM)
