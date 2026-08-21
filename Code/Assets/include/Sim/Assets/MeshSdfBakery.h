#pragma once

#include "Sim/Assets/MeshSdfBake.h"
#include "Sim/Core/SdfGrid.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace sim {
class TaskPool;
}

namespace sim::assets {

enum class MeshSdfState : uint8_t {
    /// Sedang dibake, atau baru diantre. **Bukan galat**: yang memintanya
    /// memakai medan kotak sampai ia berubah.
    Pending,
    Ready,
    /// Sudah dicoba dan gagal. Diingat supaya mesh yang tidak bisa dibake tidak
    /// dicoba ulang enam puluh kali per detik.
    Failed,
};

struct MeshSdfRef {
    MeshSdfState state = MeshSdfState::Pending;
    /// Gridnya. Terisi hanya pada `Ready`.
    ///
    /// **`shared_ptr`, bukan pointer telanjang.** Grid Sponza 69 MB dan dipegang
    /// renderer sepanjang level terbuka; sebuah pointer ke dalam peta di sini
    /// akan menggantung begitu mesh-nya dilupakan, dan menyalinnya per pemakai
    /// berarti membayar 69 MB itu sekali per instance.
    std::shared_ptr<const SdfGrid> grid;
};

/// Penjaga hasil bake SDF mesh: dari berkas mesh ke `SdfGrid` di memori.
///
/// **Bentuknya sengaja cermin `TextureBakery`.** Keduanya menjawab pertanyaan
/// yang sama — "berikan bentuk terkondisi dari aset ini, dan jangan bekukan
/// frame saya untuk itu" — dan dua bentuk berbeda untuk pertanyaan yang sama
/// adalah dua tempat yang harus dipahami terpisah.
///
/// Yang membedakan: hasilnya tinggal di memori, bukan di berkas yang dibuka
/// kembali oleh renderer. Grid dipakai CPU saat mengisi clipmap, jadi ia memang
/// harus ada di RAM — dan berkas cache-nya hanya jalan pintas supaya jalan kedua
/// tidak membayar OpenVDB lagi.
class MeshSdfBakery {
public:
    /// `tasks` boleh null: tanpanya bake dikerjakan di tempat, di thread yang
    /// memanggil. Itu jalur uji.
    MeshSdfBakery(std::filesystem::path cacheDir, TaskPool* tasks,
                  const MeshSdfSettings& settings);

    MeshSdfBakery(const MeshSdfBakery&) = delete;
    MeshSdfBakery& operator=(const MeshSdfBakery&) = delete;
    ~MeshSdfBakery();

    /// Meminta medan jarak sebuah berkas mesh. **Tidak pernah memblokir** ketika
    /// ada `TaskPool`.
    MeshSdfRef Request(const std::filesystem::path& source);

    /// Melupakan hasil sebuah berkas, sehingga permintaan berikutnya membangunnya
    /// ulang. Dipakai ketika pengaturan bake-nya berubah.
    void Invalidate(const std::filesystem::path& source);

    /// Berapa yang belum selesai. Dipakai status bar dan jalur ukur.
    std::size_t PendingCount() const;

    const MeshSdfSettings& Settings() const { return settings_; }
    const std::filesystem::path& CacheDirectory() const { return cacheDir_; }

private:
    void Bake(const std::string& key);

    struct Entry {
        MeshSdfState state = MeshSdfState::Pending;
        std::shared_ptr<const SdfGrid> grid;
    };

    std::filesystem::path cacheDir_;
    TaskPool* tasks_ = nullptr;
    MeshSdfSettings settings_;
    mutable std::mutex mutex_;
    /// Tugas yang diantre menangkap `this`, sama seperti `TextureBakery`. Yang
    /// menjaganya bukan penjaga di dalam kelas ini melainkan urutan penutupan:
    /// `TaskPool::Stop` menghentikan worker selagi pemiliknya masih hidup, dan
    /// `TaskPool::StopGuard` di `main` yang menegakkannya.
    std::unordered_map<std::string, Entry> entries_;
};

}  // namespace sim::assets
