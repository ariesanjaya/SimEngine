#pragma once

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

namespace sim {
class TaskPool;
}

/// Penjaga hasil bake tekstur: dari berkas sumber ke `.ktx2` di cache.
///
/// **Ada supaya renderer tidak pernah menyentuh berkas sumber.** Yang diminta
/// renderer adalah sebuah `.ktx2`; yang memutuskan `.ktx2` mana, dan yang
/// membangunnya kalau belum ada, adalah kelas ini. Batas itu bukan kerapian:
/// baker mendekode PNG, membangkitkan mip, dan menjalankan encoder BC7 — puluhan
/// detik untuk sebuah tekstur 4K — dan pekerjaan sebesar itu di dalam jalur
/// gambar berarti editor yang membeku.
namespace sim::assets {

enum class BakeState : uint8_t {
    /// Sedang dikerjakan, atau baru saja diantre. **Bukan galat**, dan yang
    /// menerimanya menggambar placeholder sampai ia berubah.
    Pending,
    Ready,
    /// Sudah dicoba dan gagal. Diingat supaya berkas rusak tidak diurai ulang
    /// enam puluh kali per detik sambil membanjiri log dengan pesan yang sama.
    Failed,
};

struct BakedTextureRef {
    BakeState state = BakeState::Pending;
    /// Berkas `.ktx2`-nya. Terisi hanya pada `Ready`.
    std::filesystem::path path;
};

class TextureBakery {
public:
    /// `tasks` boleh null: tanpanya bake dikerjakan di tempat, di thread yang
    /// memanggil. Itu yang dipakai uji dan jalur headless — keduanya tidak punya
    /// frame yang bisa terlihat membeku.
    TextureBakery(std::filesystem::path cacheDir, TaskPool* tasks);

    TextureBakery(const TextureBakery&) = delete;
    TextureBakery& operator=(const TextureBakery&) = delete;

    /// Meminta hasil bake sebuah berkas tekstur. **Tidak pernah memblokir**
    /// ketika ada `TaskPool`.
    BakedTextureRef Request(const std::filesystem::path& source);

    /// Melupakan hasil untuk sebuah berkas, sehingga permintaan berikutnya
    /// membangunnya ulang.
    ///
    /// Dipanggil ketika pengaturannya berubah di panel. Isinya sendiri tidak
    /// perlu diawasi: kunci cache dihitung dari isi berkasnya, jadi berkas yang
    /// berubah menghasilkan berkas cache yang lain — tetapi jawaban yang sudah
    /// diingat di sini tidak ikut tahu itu.
    void Invalidate(const std::filesystem::path& source);

    /// Berapa yang belum selesai. Dipakai status bar.
    std::size_t PendingCount() const;

    const std::filesystem::path& CacheDirectory() const { return cacheDir_; }

private:
    void Bake(const std::string& key);

    std::filesystem::path cacheDir_;
    TaskPool* tasks_ = nullptr;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, BakedTextureRef> entries_;
};

}  // namespace sim::assets
