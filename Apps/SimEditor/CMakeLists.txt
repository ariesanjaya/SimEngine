set(_sim_editor_deps
    Sim::Core
    Sim::Platform
    Sim::RHI
    Sim::ImGuiIntegration
    Sim::Render
    Sim::EditorFramework
    # Diminta sendiri, walau Render dan Assets sudah menautkannya: keduanya
    # memakainya secara PRIVATE, jadi headernya tidak sampai ke sini. Yang
    # dipakai main.cpp cuma satu baris ringkasan backend untuk log startup.
    Sim::ImageIO
    # Sama alasannya: satu baris di log startup. Ditaut tanpa syarat karena
    # modulnya selalu ada — yang bisa tidak ada adalah PhysX di dalamnya, dan
    # justru keadaan itulah yang harus terbaca di log.
    Sim::Physics
    # WHOLE_ARCHIVE: panel mendaftarkan dirinya lewat inisialisasi statik
    # (SIM_REGISTER_PANEL). Tanpa ini linker membuang berkas objek yang
    # simbolnya tidak pernah dirujuk secara eksplisit, dan panel-panelnya hilang
    # tanpa satu pun pesan error — editor cuma terbuka kosong.
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,Sim::EditorPanels>"
    SDL3::SDL3)

if(SIM_WITH_LUA)
    list(APPEND _sim_editor_deps Sim::Script)
endif()

sim_add_executable(SimEditor
    SOURCES src/main.cpp
    DEPS ${_sim_editor_deps})

if(SIM_WITH_LUA)
    target_compile_definitions(SimEditor PRIVATE SIM_WITH_LUA=1)
endif()

add_dependencies(SimEditor SimShadersGrid)

# Resources (font, ikon, tema) disalin ke sebelah executable supaya editor bisa
# dijalankan langsung dari folder build tanpa perlu tahu letak sumbernya.
add_custom_command(TARGET SimEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/Resources" "$<TARGET_FILE_DIR:SimEditor>/Resources"
    COMMENT "Menyalin Resources ke folder keluaran"
    VERBATIM)
