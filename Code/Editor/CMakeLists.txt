# Target bernama EditorPanels, bukan Editor: `sim_add_library(Editor)` akan
# menghasilkan target "SimEditor" yang bentrok dengan nama executable editor.

# Panel ditemukan lewat pola nama, bukan didaftar satu per satu.
#
# Globbing biasanya dihindari karena membuat build tidak deterministik, tapi di
# sini justru itu yang diminta kriteria terima E2 nomor 1: menambah panel harus
# cukup satu berkas, tanpa menyentuh berkas lain. CONFIGURE_DEPENDS membuat
# generator menjalankan ulang glob setiap build, jadi berkas baru langsung
# terbaca tanpa konfigurasi ulang manual.
#
# Ruang lingkupnya sengaja sempit — hanya berkas berakhiran "Panels.cpp" atau
# "Panel.cpp" di satu folder ini. Sumber lain tetap didaftarkan eksplisit.
file(GLOB _sim_panel_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*Panel*.cpp")
if(NOT _sim_panel_sources)
    message(FATAL_ERROR "Tidak ada berkas *Panel*.cpp di Code/Editor/src")
endif()

# Panel mendaftarkan dirinya lewat inisialisasi statik (SIM_REGISTER_PANEL),
# jadi Apps/SimEditor menautkan pustaka ini dengan WHOLE_ARCHIVE — kalau tidak,
# linker membuang berkas objek yang simbolnya tidak pernah dirujuk, dan
# panelnya hilang tanpa pesan error apa pun.
sim_add_library(EditorPanels
    SOURCES
        ${_sim_panel_sources}
    PUBLIC_DEPS
        Sim::EditorFramework)

list(LENGTH _sim_panel_sources _sim_panel_count)
message(STATUS "  editor panels: ${_sim_panel_count} berkas")
