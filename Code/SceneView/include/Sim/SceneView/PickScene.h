#pragma once

#include "Sim/Assets/MeshGeometryCache.h"
#include "Sim/Raycast/Query.h"
#include "Sim/Raycast/RayScene.h"
#include "Sim/Scene/World.h"

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/// Geometri adegan yang bisa ditanyai sinar, untuk picking presisi dan query
/// authoring.
///
/// **Dibangun saat ditanya, bukan tiap frame** — dan itu keputusan yang paling
/// menentukan di sini. Sebuah adegan menggambar enam puluh kali per detik dan
/// diklik beberapa kali per menit; menyusun BVH tingkat atas di dalam `Build`
/// berarti membayar seluruh biayanya enam puluh kali untuk setiap kali ia
/// benar-benar dipakai. Yang tersisa di jalur frame hanyalah menyalin daftar
/// ringkas — entity, matriks, dan kunci geometrinya — yang memang sudah disusun
/// `SceneView`.
///
/// **Geometrinya datang asinkron.** Mengurai satu FBX memakan ratusan
/// milidetik; memuatnya di dalam penanganan klik berarti editor membeku setiap
/// kali seseorang mengklik benda yang belum pernah diklik. Mesh yang belum siap
/// tidak masuk scene sama sekali, dan pemanggil jatuh ke jalur kotak batas —
/// jalur yang memang direncanakan bertahan sebagai nilai mundur.
namespace sim::view {

/// Satu benda yang bisa ditembak, dalam bentuk yang tidak menyeret `SceneView`.
struct PickItem {
    scene::Entity entity = scene::kNullEntity;
    Mat4 worldMatrix{1.0f};
    /// Kunci di `assets::MeshGeometryCache`. Kosong berarti dilewati.
    std::string_view meshKey;
    /// Jalur berkas yang dimuat bila kuncinya belum ada di cache. Kosong berarti
    /// geometrinya hanya bisa datang lewat `Adopt` — whitebox dan ubin terrain.
    std::string_view sourcePath;

    /// Kotak dunia benda ini. **Diabaikan picking**, dan ada di sini untuk
    /// panggangan cahaya statis: ia memutuskan brick mana yang perlu dipanggang,
    /// dan menaksirnya dari matriksnya saja salah bentuk — sebuah lantai
    /// 80×0,5×80 menjadi kubus bersisi 160.
    ///
    /// Kedua ujungnya sama berarti belum diisi; yang memakainya lalu harus
    /// memilih jalur konservatif alih-alih membuang brick yang dipakai.
    Vec3 worldMinimum{0.0f};
    Vec3 worldMaximum{0.0f};
};

class PickScene {
public:
    PickScene() = default;
    explicit PickScene(assets::MeshGeometryCache* cache) : cache_(cache) {}

    /// Null berarti tidak ada geometri CPU sama sekali, dan setiap `Sync`
    /// menghasilkan scene kosong — keadaan yang benar untuk build tanpa cache,
    /// dan yang membuat pemanggil tetap jatuh ke jalur kotak batas.
    void SetCache(assets::MeshGeometryCache* cache) { cache_ = cache; }

    /// Menyusun ulang scene dari daftar terbaru. **Dipanggil saat menembak,
    /// bukan tiap frame.**
    ///
    /// Geometri yang sudah punya BVH dipakai kembali apa adanya; yang berubah
    /// hanya instance dan tingkat atasnya. Itulah guna
    /// `RayScene::ClearInstances`.
    void Sync(std::span<const PickItem> items);

    /// Menembakkan sinar dunia. Kosong bila belum ada satu pun geometri siap.
    raycast::RayHit Raycast(const Vec3& origin, const Vec3& direction,
                            float maxDistance = raycast::kUnbounded) const;

    /// Menembak sambil melewatkan entity tertentu.
    ///
    /// **Dibutuhkan setiap kali sebuah benda ditanyai tentang dunia di
    /// sekitarnya.** Menjatuhkan benda ke permukaan di bawahnya menembakkan
    /// sinar dari benda itu sendiri; tanpa pengecualian, yang pertama dikenainya
    /// adalah dirinya sendiri, dan ia mendarat di tempatnya berdiri.
    ///
    /// Dikerjakan dengan menembak ulang dari titik kena, bukan dengan menyaring
    /// di dalam penelusuran: backend menjawab yang terdekat, dan menyaring di
    /// sana berarti setiap query membayar penyaringan yang hampir tidak pernah
    /// dipakai. Percobaannya dibatasi — benda yang bersarang dalam-dalam di
    /// dalam dirinya sendiri berhenti dicari alih-alih menggantung.
    raycast::RayHit RaycastExcluding(const Vec3& origin, const Vec3& direction,
                                     float maxDistance,
                                     std::span<const scene::Entity> ignore) const;

    /// Entity yang kena, atau `kNullEntity`.
    scene::Entity RaycastEntity(const Vec3& origin, const Vec3& direction,
                                float maxDistance = raycast::kUnbounded) const;

    /// Apakah entity ini benar-benar terwakili segitiga di scene ini.
    ///
    /// **Dipakai memutuskan siapa yang masih butuh jalur kotak batas.** Yang
    /// belum terwakili bukan hanya yang tidak punya mesh: mesh yang geometrinya
    /// masih dimuat juga belum ada di sini, dan melewatkannya berarti benda itu
    /// tidak bisa diklik sama sekali sampai pemuatannya selesai.
    bool Covers(scene::Entity entity) const;

    /// Berapa benda yang benar-benar punya segitiga di scene ini. Sisanya masih
    /// menunggu geometrinya, dan pemanggil harus memakai jalur kotak untuk
    /// mereka.
    std::size_t ReadyCount() const { return readyCount_; }
    std::size_t PendingCount() const { return pendingCount_; }
    /// Berapa yang **masih dimuat** — bagian dari `PendingCount` yang akan
    /// berubah kalau ditunggu. Sisanya gagal diurai dan tidak akan pernah siap.
    std::size_t LoadingCount() const { return loadingCount_; }
    const raycast::RayScene& Scene() const { return scene_; }

    /// Melupakan BVH sebuah geometri, sehingga `Sync` berikutnya membangunnya
    /// ulang. Dipakai bentuk yang disunting di editor — whitebox yang baru
    /// diekstrusi bukan lagi bentuk yang BVH-nya sudah dibangun.
    void Invalidate(const std::string& key);

private:
    struct Geometry {
        raycast::GeometryId id = raycast::GeometryId::Invalid;
        /// Dipegang supaya buffernya tidak dibebaskan sementara BVH menyimpan
        /// pointer telanjang ke dalamnya.
        std::shared_ptr<const assets::MeshData> data;
    };

    /// Sidik jari isi daftar terakhir yang disusun.
    ///
    /// **Yang paling mahal di sini adalah membangun BVH tingkat atas**, dan
    /// adegan yang tidak berubah tidak menuntutnya sama sekali — kamera yang
    /// berputar tidak menggeser satu pun benda. Memindai daftarnya jauh lebih
    /// murah daripada membangun ulang pohonnya, jadi sidik jari ini yang
    /// memutuskan.
    static uint64_t Fingerprint(std::span<const PickItem> items);

    assets::MeshGeometryCache* cache_ = nullptr;
    raycast::RayScene scene_;
    std::unordered_map<std::string, Geometry> geometries_;
    std::unordered_set<uint64_t> covered_;
    uint64_t fingerprint_ = 0;
    bool synced_ = false;
    std::size_t readyCount_ = 0;
    std::size_t pendingCount_ = 0;
    std::size_t loadingCount_ = 0;
};

}  // namespace sim::view
