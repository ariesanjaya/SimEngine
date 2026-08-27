#include "Sim/Core/UserPaths.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// Sesudah windows.h: keduanya menuntutnya lebih dulu.
#include <knownfolders.h>
#include <shlobj.h>

#include <string>
#else
#include <cstdlib>
#endif

namespace sim {

namespace {

/// Nilai variabel lingkungan sebagai path, atau kosong bila tidak diset.
///
/// Variabel yang ada tapi isinya string kosong diperlakukan sama dengan yang
/// tidak ada: `std::filesystem::path("") / ".simengine"` menghasilkan path
/// relatif, dan yang membacanya tidak akan sadar akarnya hilang.
#if defined(_WIN32)

/// **Bukan `std::getenv`.** Dua alasan, dan yang kedua yang sebenarnya penting:
/// CRT Windows menandainya deprecated sehingga `-Werror` menggagalkan build, dan
/// yang dikembalikannya adalah hasil konversi ke codepage ANSI mesin ini. Nama
/// pengguna yang memuat karakter di luar codepage itu — yang biasa saja di
/// sebagian besar dunia — lalu kembali sebagai tanda tanya, dan folder yang
/// dibentuk darinya menunjuk ke tempat yang tidak ada.
std::filesystem::path EnvPath(const wchar_t* name) {
    // Panggilan pertama meminta panjangnya: nilai variabel lingkungan tidak
    // punya batas atas yang bisa diandalkan, jadi buffer tetap adalah pemotongan
    // yang menunggu terjadi.
    const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) {
        return {};  // tidak diset
    }

    std::wstring value(needed, L'\0');
    const DWORD written = ::GetEnvironmentVariableW(name, value.data(), needed);
    // `written` menghitung tanpa NUL, `needed` dengan NUL. Yang tidak lebih
    // kecil berarti nilainya berubah di antara dua panggilan.
    if (written == 0 || written >= needed) {
        return {};
    }
    value.resize(written);
    return std::filesystem::path(value);
}

#else

std::filesystem::path EnvPath(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return {};
    }
    return std::filesystem::path(value);
}

#endif

}  // namespace

std::filesystem::path HomeDirectory() {
#if defined(_WIN32)
    if (std::filesystem::path profile = EnvPath(L"USERPROFILE"); !profile.empty()) {
        return profile;
    }
    // Cadangan untuk lingkungan yang menyetel pasangan lama ini tanpa
    // USERPROFILE — sebagian besar sesi layanan dan shell warisan.
    const std::filesystem::path drive = EnvPath(L"HOMEDRIVE");
    const std::filesystem::path path = EnvPath(L"HOMEPATH");
    if (!drive.empty() && !path.empty()) {
        return drive / path;
    }
#else
    if (std::filesystem::path home = EnvPath("HOME"); !home.empty()) {
        return home;
    }
#endif
    return std::filesystem::current_path();
}

std::filesystem::path ConfigDirectory() {
    return HomeDirectory() / ".simengine";
}

std::filesystem::path DocumentsDirectory() {
#if defined(_WIN32)
    PWSTR wide = nullptr;
    // KF_FLAG_DEFAULT, bukan KF_FLAG_CREATE: yang membuat folder project adalah
    // yang membuat project, bukan yang menghitung namanya. Memanggil ini saat
    // startup tidak boleh meninggalkan folder kosong di mesin orang yang tidak
    // pernah membuat satu pun project.
    const HRESULT hr = ::SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &wide);
    if (SUCCEEDED(hr) && wide != nullptr) {
        std::filesystem::path documents(wide);
        ::CoTaskMemFree(wide);
        return documents;
    }
    if (wide != nullptr) {
        ::CoTaskMemFree(wide);
    }
#endif
    return HomeDirectory() / "Documents";
}

}  // namespace sim
