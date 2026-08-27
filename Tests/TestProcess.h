#pragma once

#include <cstdint>

// PID proses ini, untuk menyusun nama folder sementara yang unik.
//
// **Ada sebagai header bersama, bukan disalin ke tiap berkas test.** Tujuh
// berkas test memakainya, dan sebelumnya ketujuhnya menulis `#include
// <unistd.h>` beserta `::getpid()` — dua hal yang sama-sama tidak ada di
// Windows. Menambalnya per berkas berarti tujuh salinan cabang platform yang
// harus tetap sepakat; yang berikutnya cukup meng-include berkas ini.
//
// Sejalan dengan `ProcessId()` di `Code/AIBridge/src/McpServer.cpp`. Tidak
// dibagi dengan yang di sana karena yang itu detail internal sebuah modul, dan
// membukanya ke header publik demi test adalah pelebaran API yang tidak dibayar
// siapa pun.

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace sim::tests {

inline int64_t ProcessId() {
#if defined(_WIN32)
    return static_cast<int64_t>(::_getpid());
#else
    return static_cast<int64_t>(::getpid());
#endif
}

}  // namespace sim::tests
