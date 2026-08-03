# Menerapkan seluruh patch di sebuah folder ke sumber dependensi, dan aman
# dijalankan ulang.
#
# FetchContent menjalankan PATCH_COMMAND setiap kali langkah patch-nya dianggap
# usang, bukan hanya setelah unduhan pertama. `git apply` yang polos akan gagal
# pada jalankan kedua karena perubahannya sudah ada, dan kegagalan itu
# menghentikan konfigurasi — jadi di sini patch yang sudah terpasang dikenali
# lebih dulu lewat --reverse --check, lalu dilewati.
#
# Diterapkan satu per satu, bukan sebagai satu berkas gabungan: menambah patch
# baru tidak boleh membuat folder build yang sudah ada gagal dikonfigurasi
# hanya karena patch lamanya sudah terpasang.
#
# Dipanggil lewat `cmake -P` dengan GIT_EXECUTABLE dan PATCH_DIR, dari dalam
# folder sumber dependensinya.

if(NOT DEFINED GIT_EXECUTABLE OR NOT DEFINED PATCH_DIR)
    message(FATAL_ERROR "ApplyPatch.cmake butuh -DGIT_EXECUTABLE dan -DPATCH_DIR")
endif()

file(GLOB _patches "${PATCH_DIR}/*.patch")
list(SORT _patches)

foreach(_patch IN LISTS _patches)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${_patch}"
        RESULT_VARIABLE _already_applied
        OUTPUT_QUIET
        ERROR_QUIET)

    if(_already_applied EQUAL 0)
        continue()
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply "${_patch}"
        RESULT_VARIABLE _result
        ERROR_VARIABLE _stderr)

    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "Gagal menerapkan ${_patch}: ${_stderr}")
    endif()
endforeach()
