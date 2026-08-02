#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace sim {

/// Memberitahu perubahan berkas di sebuah pohon direktori.
///
/// Ada di `Core`, bukan `Platform`, walau isinya kode khusus sistem operasi.
/// Alasannya lugas: `Platform` mengekspor SDL3 secara publik, sedangkan
/// pemantau ini tidak menyentuh SDL sama sekali — menaruhnya di sana akan
/// menyeret SDL ke modul Assets beserta seluruh binari test-nya.
///
/// **Yang wajib dipahami pemakainya: event bisa hilang.** Antrean kernel Linux
/// bisa meluap (`IN_Q_OVERFLOW`), begitu pula buffer Windows
/// (`ERROR_NOTIFY_ENUM_DIR`). Karena itu `Poll()` mengembalikan bool, dan
/// pemindaian penuh tidak boleh dihapus — ia berubah peran dari mekanisme utama
/// menjadi jalur pemulihan.
///
/// Event adalah **petunjuk, bukan data**: ukuran dan waktu ubah tetap harus
/// di-stat sendiri. Menyimpan satu berkas dari aplikasi lain juga sering
/// menghasilkan beberapa event beruntun, jadi pemakainya sebaiknya menggabung
/// event dalam satu frame alih-alih bereaksi satu per satu.
class FileWatcher {
public:
    struct Event {
        enum class Kind : uint8_t { Added, Removed, Modified };
        Kind kind = Kind::Modified;
        /// Relatif terhadap akar, memakai '/' di semua platform.
        std::string path;
    };

    FileWatcher();
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    /// Mulai memantau. Mengembalikan false bila platform ini tidak didukung
    /// atau sumber daya kernelnya tidak cukup — pemanggil harus kembali ke
    /// pemindaian berkala.
    bool Watch(const std::filesystem::path& root);
    void Stop();
    bool IsWatching() const;

    /// Mengambil event yang tertunda tanpa memblokir.
    ///
    /// Mengembalikan **false** bila ada event yang hilang; saat itu isi `out`
    /// tidak bisa dipercaya lengkap dan pemanggil wajib memindai ulang seluruh
    /// pohon.
    bool Poll(std::vector<Event>& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sim
