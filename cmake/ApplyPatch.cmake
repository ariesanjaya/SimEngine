# Menerapkan sebuah patch ke sumber dependensi, dan aman dijalankan ulang.
#
# FetchContent menjalankan PATCH_COMMAND setiap kali langkah patch-nya dianggap
# usang, bukan hanya setelah unduhan pertama. `git apply` yang polos akan gagal
# pada jalankan kedua karena perubahannya sudah ada, dan kegagalan itu
# menghentikan konfigurasi — jadi di sini patch yang sudah terpasang dikenali
# lebih dulu lewat --reverse --check, lalu dilewati.
#
# Dipanggil lewat `cmake -P` dengan GIT_EXECUTABLE dan PATCH_FILE, dari dalam
# folder sumber dependensinya.

if(NOT DEFINED GIT_EXECUTABLE OR NOT DEFINED PATCH_FILE)
    message(FATAL_ERROR "ApplyPatch.cmake butuh -DGIT_EXECUTABLE dan -DPATCH_FILE")
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${PATCH_FILE}"
    RESULT_VARIABLE _already_applied
    OUTPUT_QUIET
    ERROR_QUIET)

if(_already_applied EQUAL 0)
    return()
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply "${PATCH_FILE}"
    RESULT_VARIABLE _result
    ERROR_VARIABLE _stderr)

if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Gagal menerapkan ${PATCH_FILE}: ${_stderr}")
endif()
