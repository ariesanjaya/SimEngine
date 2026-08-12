#pragma once

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sim::platform {

class Window;

/// Satu penyaring jenis berkas. `pattern` adalah daftar ekstensi tanpa titik,
/// dipisah titik koma — mis. `"png;jpg"`. `"*"` berarti seluruh berkas.
struct FileDialogFilter {
    std::string name;
    std::string pattern;
};

/// Hasil sebuah dialog berkas.
///
/// **Tiga keadaan, bukan dua.** Dibatalkan dan gagal adalah hal yang berbeda:
/// yang pertama keputusan pengguna dan tidak boleh menghasilkan pesan apa pun,
/// yang kedua adalah editor yang tidak bisa membuka dialognya sama sekali —
/// pada mesin tanpa portal desktop, misalnya — dan itu justru harus disebutkan,
/// karena kalau tidak, tombolnya tampak sekadar tidak berfungsi.
struct FileDialogResult {
    std::vector<std::filesystem::path> paths;
    std::string error;

    bool Cancelled() const { return paths.empty() && error.empty(); }
    /// Pilihan pertama, atau jalur kosong.
    std::filesystem::path First() const { return paths.empty() ? std::filesystem::path{} : paths.front(); }
};

using FileDialogCallback = std::function<void(const FileDialogResult&)>;

/// Menetapkan jendela induk seluruh dialog.
///
/// **Sebuah pendaftaran, bukan parameter di setiap panggilan.** Yang memanggil
/// dialog adalah panel editor, dan panel tidak boleh mengenal `Window` — ia tipe
/// milik lapisan platform yang membawa SDL bersamanya. Yang tahu jendela mana
/// yang utama hanyalah `main()`, dan ia menyebutkannya sekali.
///
/// Boleh null: dialog tetap terbuka, hanya tidak modal terhadap jendela mana pun.
void SetDialogParentWindow(const Window* window);

/// Memilih sebuah folder yang sudah ada.
///
/// **Asinkron.** Dialog sistem berjalan di luar kendali kita; menunggunya berarti
/// menahan seluruh frame editor selama pengguna menelusuri berkasnya. `done`
/// dipanggil belakangan — dan **selalu di main thread**, walaupun SDL boleh
/// memanggil balik dari thread mana pun: pemanggilnya menyentuh state panel, dan
/// state panel tidak pernah dikunci.
///
/// Harus dipanggil dari main thread.
void ShowOpenFolderDialog(std::string_view defaultLocation, FileDialogCallback done);

/// Memilih sebuah berkas yang sudah ada. Ketentuan yang sama dengan di atas.
void ShowOpenFileDialog(std::span<const FileDialogFilter> filters,
                        std::string_view defaultLocation, FileDialogCallback done);

}  // namespace sim::platform
