#pragma once

#include "Sim/Assets/AssetTypes.h"
#include "Sim/Core/FileWatcher.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sim {
class TaskPool;
}

namespace sim::assets {

/// Indeks seluruh berkas di folder aset, beserta identitas tetapnya.
///
/// Identitas itu — GUID di berkas `.meta` di sebelah asetnya — adalah inti
/// seluruh modul ini. Karena level menyimpan GUID dan bukan path, mengganti
/// nama atau memindahkan aset tidak menyentuh satu pun berkas level.
///
/// **Model threading.** Pemindaian berjalan di TaskPool dan tidak pernah
/// menyentuh state yang dibaca UI. Ia hanya menghasilkan daftar utuh yang
/// ditukar masuk oleh `Update()` di main thread. Karena itu seluruh pembaca —
/// panel, Inspector, PropertyGrid — bisa memakai referensi tanpa kunci sama
/// sekali, selama mereka tidak menyimpannya melewati satu frame.
class AssetDatabase {
public:
    struct Config {
        std::filesystem::path root;
        /// Kolam untuk pemindaian dan impor. Boleh null: tanpa kolam, semua
        /// berjalan sinkron di dalam Update() — dipakai test.
        TaskPool* tasks = nullptr;
        /// Jeda pemindaian penuh ketika pemantau berkas TIDAK tersedia.
        float pollIntervalSeconds = 1.0f;
        /// Jeda pemindaian penuh ketika pemantau tersedia — murni jaring
        /// pengaman untuk perubahan yang lolos, jadi boleh jarang.
        float safetyNetSeconds = 30.0f;
    };

    bool Initialize(Config config);
    void Shutdown();

    /// Menerapkan hasil pemindaian yang sudah selesai dan menjadwalkan yang
    /// berikutnya. Dipanggil sekali per frame dari main thread.
    void Update(float deltaSeconds);

    /// Memaksa pemindaian selesai sebelum kembali. Dipakai test dan saat
    /// membuka proyek, ketika daftar aset harus sudah lengkap.
    void ScanNow();

    const AssetRecord* Find(const Uuid& guid) const;
    const AssetRecord* FindByRelativePath(std::string_view relativePath) const;
    const std::vector<AssetRecord>& All() const { return records_; }

    /// Isi sebuah folder. `folder` relatif terhadap akar, kosong berarti akar.
    std::vector<const AssetRecord*> InFolder(std::string_view folder, bool recursive) const;

    /// Seluruh folder yang ada, relatif terhadap akar, terurut.
    const std::vector<std::string>& Folders() const { return folders_; }

    /// Aset yang merujuk `guid`.
    std::vector<Uuid> UsersOf(const Uuid& guid) const;

    const std::filesystem::path& Root() const { return root_; }
    std::filesystem::path AbsolutePath(const AssetRecord& record) const;

    /// Naik setiap kali isi indeks berubah. Panel memakainya untuk tahu kapan
    /// harus menyusun ulang tampilannya alih-alih membandingkan daftar.
    uint64_t Version() const { return version_; }

    // --- operasi berkas yang menjaga referensi tetap utuh ---

    /// Mengganti nama berkas beserta `.meta`-nya. GUID tidak berubah, sehingga
    /// tidak ada level yang perlu disunting.
    bool Rename(const Uuid& guid, std::string_view newName, std::string& error);

    /// Memindahkan aset ke folder lain di dalam akar.
    bool Move(const Uuid& guid, std::string_view newFolder, std::string& error);

    /// Menghapus berkas beserta `.meta`-nya. Pemanggil yang wajib memperingatkan
    /// pemakainya lebih dulu — lihat UsersOf().
    bool Delete(const Uuid& guid, std::string& error);

private:
    /// Hasil satu putaran pemindaian, dibangun sepenuhnya di thread latar.
    struct ScanResult {
        std::vector<AssetRecord> records;
        std::vector<std::string> folders;
    };

    /// Cuplikan hasil pemindaian sebelumnya, hanya-baca.
    ///
    /// Diserahkan ke thread pemindai supaya berkas yang ukuran dan waktu
    /// ubahnya tidak berubah bisa dipakai ulang apa adanya — tanpa membuka
    /// `.meta` dan tanpa menjalankan importer. Tanpa ini, folder berisi 10.000
    /// aset membakar satu inti CPU penuh setiap detik hanya untuk menyimpulkan
    /// bahwa tidak ada yang berubah.
    ///
    /// shared_ptr ke objek immutable: pemindai memegang cuplikan lama dengan
    /// aman walau main thread sudah menukar yang baru.
    using Snapshot = std::unordered_map<std::string, AssetRecord>;
    using SnapshotPtr = std::shared_ptr<const Snapshot>;

    static ScanResult Scan(const std::filesystem::path& root, const SnapshotPtr& previous);
    void Apply(ScanResult&& result);
    void Reindex();

    std::filesystem::path root_;
    TaskPool* tasks_ = nullptr;
    float pollInterval_ = 1.0f;
    float safetyNet_ = 30.0f;
    float sinceLastScan_ = 0.0f;

    /// Pemantau perubahan berkas. Bila aktif, pemindaian penuh hanya berjalan
    /// ketika ia melaporkan sesuatu — atau ketika ia melaporkan bahwa ada event
    /// yang hilang, yang memang bisa terjadi di kedua platform.
    FileWatcher watcher_;
    std::vector<FileWatcher::Event> watcherEvents_;

    std::vector<AssetRecord> records_;
    std::vector<std::string> folders_;
    std::unordered_map<Uuid, std::size_t> byGuid_;
    std::unordered_map<std::string, std::size_t> byPath_;
    uint64_t version_ = 0;
    SnapshotPtr snapshot_ = std::make_shared<Snapshot>();

    // Kotak serah-terima antara thread pemindai dan main thread. Mutexnya hanya
    // melindungi kotak ini, bukan indeksnya — indeks tidak pernah disentuh
    // thread lain.
    mutable std::mutex handoffMutex_;
    ScanResult pendingResult_;
    bool hasPendingResult_ = false;
    std::atomic<bool> scanInFlight_{false};
};

}  // namespace sim::assets
