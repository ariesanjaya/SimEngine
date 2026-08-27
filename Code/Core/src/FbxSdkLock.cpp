#include "Sim/Core/FbxSdkLock.h"

namespace sim {

std::mutex& FbxSdkMutex() {
    // Static lokal: dibangun sekali, saat pertama dibutuhkan, dan tanpa urutan
    // inisialisasi global yang bisa salah. Ia sengaja tidak pernah dihancurkan
    // secara eksplisit — sebuah importir yang masih berjalan saat proses ditutup
    // lebih baik menunggu mutex yang hidup daripada mengunci yang sudah mati.
    static std::mutex mutex;
    return mutex;
}

}  // namespace sim
