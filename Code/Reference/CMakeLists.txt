# Path tracer acuan — R4 di docs/PLAN-EMBREE.md.
#
# **Modul ini tidak menggambar apa pun untuk pemain.** Ia menjawab satu
# pertanyaan yang tidak bisa dijawab renderer real-time: seperti apa adegan ini
# kalau cahayanya benar-benar ditelusuri, tanpa aproksimasi. Yang keluar darinya
# bukan gambar untuk dilihat melainkan **angka untuk dibandingkan** — itulah
# yang membuat tumpukan SDF/probe/radiance-cache bisa dinilai alih-alih
# dipercaya.
#
# **Sim::RHI dan Sim::Render sengaja tidak ada di sini**, alasan yang sama
# dengan Sim::Raycast: acuan yang butuh GPU tidak bisa dijalankan CI.

# **Seluruh modul bergantung slangc**, karena model shading-nya dibangkitkan
# darinya. Tanpa slangc yang hilang bukan cakupan yang diabaikan melainkan
# modul yang tidak punya alat untuk dibangun sama sekali.
if(NOT SIM_SLANGC)
    message(STATUS "slangc tidak ditemukan; Sim::Reference dilewati")
    return()
endif()

set(SIM_REFERENCE_SHADING "${CMAKE_CURRENT_BINARY_DIR}/openpbr_cpu.cpp")

# **Model shading dipancarkan, bukan disalin.** `openpbr_cpu.slang` hanya
# membungkus `openpbr.slang`; C++ yang keluar dari sini adalah sumber yang sama
# yang dikompilasi ke SPIR-V untuk GPU. Sebuah acuan yang model shading-nya
# ditulis ulang dengan tangan hanya menguji ketelitian penyalinnya.
add_custom_command(
    OUTPUT  "${SIM_REFERENCE_SHADING}"
    COMMAND "${SIM_SLANGC}" "${CMAKE_SOURCE_DIR}/Shaders/openpbr_cpu.slang"
            -target cpp -o "${SIM_REFERENCE_SHADING}"
            -I "${CMAKE_SOURCE_DIR}/Shaders"
    # **`openpbr.slang` ikut sebagai dependensi eksplisit.** Tanpanya, mengubah
    # model shading tidak membangkitkan ulang C++-nya, dan acuan ini akan
    # menilai GPU memakai model yang sudah tidak ada lagi — bug yang sama persis
    # dengan yang dijaga identitas kompilator di ShaderCache, dan di sini
    # akibatnya lebih buruk: selisih yang muncul akan terbaca sebagai galat GI.
    DEPENDS "${CMAKE_SOURCE_DIR}/Shaders/openpbr_cpu.slang"
            "${CMAKE_SOURCE_DIR}/Shaders/openpbr.slang"
    COMMENT "slangc -target cpp openpbr_cpu.slang"
    VERBATIM)

# Disertakan `Shading.cpp`, bukan dikompilasi terpisah: yang dibutuhkannya
# definisi struct yang dipancarkan slangc. Menyertakannya *juga* sebagai source
# berarti setiap simbol punya dua salinan.
set_source_files_properties(src/Shading.cpp PROPERTIES OBJECT_DEPENDS "${SIM_REFERENCE_SHADING}")

sim_add_library(Reference
    SOURCES
        src/Shading.cpp
        src/Lights.cpp
        src/PathTracer.cpp
        src/Scene.cpp
        src/ImageCompare.cpp
    PUBLIC_DEPS
        Sim::Core
        Sim::Raycast
    PRIVATE_DEPS
        # Gambar acuan keluar sebagai EXR, dan hanya itu yang dipakainya dari
        # sini. Privat: yang menautkan Sim::Reference tidak ikut menautkan
        # seluruh rantai pembaca gambar.
        Sim::ImageIO)

target_include_directories(SimReference PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# C++ yang dipancarkan slangc bukan kode kita: ia tidak lolos -Werror milik
# `sim_target_defaults`, dan menambalnya berarti menambal keluaran generator
# setiap kali generatornya diperbarui.
target_compile_options(SimReference PRIVATE -Wno-error)

# Aturan yang sama dengan Sim::Raycast, dan alasan yang sama.
sim_raycast_assert_no_gpu_deps(SimReference)
