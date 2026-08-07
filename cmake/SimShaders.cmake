# Kompilasi shader GLSL/Slang -> SPIR-V saat build.
#
# sim_compile_shaders(NamaTarget
#     SOURCES Shaders/grid.vert Shaders/grid.frag
#     OUTPUT_DIR ${CMAKE_BINARY_DIR}/bin/Shaders)
#
# Menghasilkan target custom yang ikut dependency tracking, sehingga mengedit
# shader memicu rebuild. -MD membuat #include di dalam shader ikut terlacak.
include_guard(GLOBAL)

function(sim_compile_shaders target)
    cmake_parse_arguments(ARG "" "OUTPUT_DIR" "SOURCES" ${ARGN})

    if(NOT SIM_GLSLC)
        message(STATUS "glslc tidak ada, target shader '${target}' dilewati")
        add_custom_target(${target})
        return()
    endif()

    if(NOT ARG_OUTPUT_DIR)
        set(ARG_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Shaders")
    endif()
    file(MAKE_DIRECTORY "${ARG_OUTPUT_DIR}")

    set(_outputs "")
    foreach(_src ${ARG_SOURCES})
        get_filename_component(_abs "${_src}" ABSOLUTE)
        get_filename_component(_name "${_src}" NAME)
        get_filename_component(_dir "${_abs}" DIRECTORY)

        get_filename_component(_ext "${_src}" LAST_EXT)
        if(_ext STREQUAL ".slang")
            # Ekstensi `.slang` dibuang dari nama keluaran: `grid.vert.slang`
            # menjadi `grid.vert.spv`. Sisi C++ memuat shader lewat namanya, dan
            # nama itu tidak boleh ikut berubah hanya karena bahasa sumbernya
            # berganti.
            get_filename_component(_name "${_src}" NAME_WLE)
        endif()
        set(_out "${ARG_OUTPUT_DIR}/${_name}.spv")
        set(_dep "${_out}.d")  # nama bawaan yang ditulis glslc -MD

        if(_ext STREQUAL ".slang")
            if(NOT SIM_SLANGC)
                message(FATAL_ERROR "Shader Slang '${_src}' butuh slangc, tapi tidak ditemukan")
            endif()
            add_custom_command(
                OUTPUT  "${_out}"
                # -matrix-layout-column-major **wajib**, dan harus sama dengan
                # argumen yang dipakai pipeline material di `ShaderCache`.
                # Tanpanya Slang memakai tata letak baris, dan `mul(M, v)`
                # menghasilkan transpose dari yang dimaksud — setiap matriks di
                # seluruh engine lalu salah, tanpa satu pun galat kompilasi.
                COMMAND "${SIM_SLANGC}" "${_abs}" -target spirv -o "${_out}"
                        -profile spirv_1_5 -emit-spirv-directly
                        -matrix-layout-column-major
                        -I "${_dir}" -depfile "${_dep}"
                        $<IF:$<CONFIG:Debug>,-g,-O2>
                DEPENDS "${_abs}"
                DEPFILE "${_dep}"
                COMMENT "slangc ${_name}"
                VERBATIM)
        else()
            add_custom_command(
                OUTPUT  "${_out}"
                # -c wajib supaya glslc meng-compile, bukan mencoba menautkan.
                # -MD dipakai tanpa -MF: glslc menolak kombinasi -o + -MF, dan
                # secara bawaan ia menulis dependensi ke "<output>.d" — yang
                # justru persis nama yang kita berikan ke DEPFILE.
                # $<IF:...> dipakai, bukan dua genex terpisah: genex yang tidak
                # aktif menghasilkan argumen kosong, dan glslc menganggap
                # argumen kosong sebagai berkas masukan kedua.
                COMMAND "${SIM_GLSLC}" -c "${_abs}" -o "${_out}"
                        --target-env=vulkan1.3 -MD
                        $<IF:$<CONFIG:Debug>,-g,-O>
                DEPENDS "${_abs}"
                DEPFILE "${_dep}"
                COMMENT "glslc ${_name}"
                VERBATIM)
        endif()
        list(APPEND _outputs "${_out}")
    endforeach()

    add_custom_target(${target} DEPENDS ${_outputs})
    set_target_properties(${target} PROPERTIES FOLDER "SimEngine/Shaders")
endfunction()
