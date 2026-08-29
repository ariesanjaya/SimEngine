#pragma once

#include "Sim/Assets/MeshData.h"

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

enum class MeshGeometryState : uint8_t {
    /// Sedang dimuat, atau baru diantre. **Bukan galat**: yang memintanya
    /// memakai kotak batas sampai ia berubah.
    Pending,
    Ready,
    /// Sudah dicoba dan gagal. Diingat supaya berkas rusak tidak diurai ulang
    /// enam puluh kali per detik sambil membanjiri log dengan pesan yang sama.
    Failed,
};

struct MeshGeometryRef {
    MeshGeometryState state = MeshGeometryState::Pending;
    /// Segitiganya. Terisi hanya pada `Ready`.
    ///
    /// **`shared_ptr` ke data yang tidak pernah berubah**, dengan alasan yang
    /// sama seperti `MeshSdfRef::grid`: yang memegangnya adalah BVH `Sim::Raycast`
    /// yang menyimpan pointer telanjang ke dalamnya, dan sebuah entri yang
    /// dilupakan cache di tengah frame akan membuat pointer itu menggantung.
    /// Selama BVH-nya masih hidup, geometrinya ikut hidup.
    std::shared_ptr<const MeshData> data;
};

/// Salinan CPU geometri mesh, dijaga hidup selama ada yang memerlukannya.
///
/// **Ini yang tidak dimiliki mesin ini sebelumnya, dan ketiadaannya yang
/// menentukan bentuk R1.** `IViewportRenderer::AcquireMesh` memuat sebuah berkas,
/// mengunggahnya ke GPU, dan tidak menyimpan satu pun segitiga di RAM — itu
/// pilihan yang benar untuk menggambar. Tetapi picking presisi, query authoring,
/// dan path tracer referensi semuanya bekerja di CPU, dan ketiganya menuntut
/// segitiga yang sama.
///
/// **Bentuknya sengaja cermin `MeshSdfBakery` dan `TextureBakery`.** Ketiganya
/// menjawab pertanyaan yang sama — "berikan bentuk terkondisi dari aset ini, dan
/// jangan bekukan frame saya untuk itu" — dan tiga bentuk berbeda untuk satu
/// pertanyaan adalah tiga tempat yang harus dipahami terpisah.
///
/// **Jawaban pertamanya selalu `Pending`.** Mengurai satu FBX memakan ratusan
/// milidetik; memuatnya di dalam penanganan klik berarti editor membeku setiap
/// kali seseorang mengklik benda yang belum pernah diklik. Yang meminta memakai
/// jalur kotak batas sampai geometrinya siap — persis yang dilakukan clipmap SDF
/// sambil menunggu bake-nya.
class MeshGeometryCache {
public:
    /// `tasks` boleh null: tanpanya pemuatan dikerjakan di tempat, di thread yang
    /// memanggil. Itu jalur uji.
    explicit MeshGeometryCache(TaskPool* tasks = nullptr);

    /// Tempat artefak UV lightmap dibaca dan ditulis (S4). Kosong berarti mesh
    /// dimuat tanpa UV lightmap sama sekali.
    ///
    /// **Harus sama dengan yang dipakai renderer.** Unwrap menyusun ulang daftar
    /// vertex, jadi dua pemuat yang tidak sepakat menghasilkan dua mesh yang
    /// berbeda untuk berkas yang sama — dan yang menemukannya adalah orang yang
    /// bertanya kenapa picking mengenai segitiga yang lain daripada yang
    /// tergambar.
    void SetLightmapCacheDir(std::filesystem::path dir) { lightmapCacheDir_ = std::move(dir); }
    ~MeshGeometryCache();

    MeshGeometryCache(const MeshGeometryCache&) = delete;
    MeshGeometryCache& operator=(const MeshGeometryCache&) = delete;

    /// Meminta geometri sebuah berkas mesh. **Tidak pernah memblokir** ketika
    /// ada `TaskPool`.
    MeshGeometryRef Request(const std::filesystem::path& source);

    /// Menaruh geometri yang sudah ada di RAM, tanpa membaca berkas.
    ///
    /// Dipakai yang bentuknya lahir di dalam editor dan tidak punya berkas untuk
    /// diurai: whitebox dan ubin terrain keduanya sudah memegang `MeshData`-nya,
    /// dan memuatnya kembali dari disk akan membaca bentuk sebelum suntingan
    /// terakhir.
    MeshGeometryRef Adopt(const std::string& key, MeshData data);

    /// Menanyakan sebuah entri **tanpa memuat dan tanpa menyisipkan.**
    ///
    /// Dipakai yang bentuknya hanya bisa datang lewat `Adopt` — whitebox dan
    /// ubin terrain tidak punya berkas untuk diurai, jadi `Request` atas kunci
    /// mereka tidak punya arti. Yang belum diadopsi menjawab `Pending`, dan itu
    /// jawaban yang benar: geometrinya memang belum ada.
    MeshGeometryRef Find(const std::string& key) const;

    /// Melupakan sebuah entri. Yang masih memegang `shared_ptr`-nya tidak
    /// terpengaruh — itulah gunanya `shared_ptr`.
    void Invalidate(const std::string& key);

    /// Berapa yang belum selesai. Dipakai status bar dan jalur ukur.
    std::size_t PendingCount() const;
    std::size_t ReadyCount() const;
    /// Perkiraan memori yang dipegang, byte. Dipakai melaporkan ongkosnya —
    /// dan ongkos itu memang harus terlihat.
    std::size_t BytesHeld() const;

private:
    struct Entry {
        MeshGeometryState state = MeshGeometryState::Pending;
        std::shared_ptr<const MeshData> data;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    TaskPool* tasks_ = nullptr;
    std::filesystem::path lightmapCacheDir_;
};

}  // namespace sim::assets
