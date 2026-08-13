#pragma once

#include "Sim/Physics/PhysicsQuery.h"
#include "Sim/Physics/PhysicsTypes.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace sim::physics {

/// Apakah build ini memuat PhysX sama sekali.
///
/// Tanpanya seluruh mesin tetap terbangun; yang hilang adalah simulasi. Yang
/// memeriksanya harus **mengatakan** apa yang hilang, bukan diam-diam
/// membiarkan benda tidak bergerak — benda diam yang seharusnya jatuh terbaca
/// sebagai bug fisika, bukan sebagai pustaka yang tidak dipasang.
bool Available();

/// Versi SDK yang aktif, atau kosong. Muncul di log startup dan DEPENDENCIES.md.
const char* BackendVersion();

/// Apakah build ini memuat jalur GPU (CUDA).
///
/// **Terpisah dari `Available`, dan sengaja.** PBD, soft body FEM, dan
/// deformable surface menuntut CUDA lewat tanda tangan API-nya — mereka tidak
/// punya jalur CPU sama sekali. Pada perangkat non-NVIDIA ketiganya tidak pernah
/// ada, bukan "mati sementara".
bool GpuAvailable();

/// Satu dunia fisika: sebuah `PxScene` beserta benda-benda di dalamnya.
///
/// **Langkahnya tetap dan terpisah dari frame render.** `Advance` menerima waktu
/// nyata yang berlalu dan menjalankan sebanyak mungkin langkah utuh yang muat di
/// dalamnya, menyisakan sisanya untuk frame berikutnya. Yang menggambar
/// menginterpolasi memakai `Alpha()`.
class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    PhysicsWorld(PhysicsWorld&&) noexcept;
    PhysicsWorld& operator=(PhysicsWorld&&) noexcept;

    /// Membangun scene. Mengembalikan false bila PhysX tidak ada di build ini,
    /// atau bila pengaturannya tidak masuk akal — `Error()` menyebut yang mana.
    bool Create(const WorldDesc& desc);
    void Destroy();
    bool IsValid() const;

    /// Pesan dari kegagalan terakhir. Kosong bila tidak ada.
    const std::string& Error() const;

    /// Menambahkan benda. Mengembalikan `BodyHandle::Invalid` bila gagal.
    BodyHandle AddBody(const BodyDesc& desc);
    void RemoveBody(BodyHandle body);
    bool IsAlive(BodyHandle body) const;
    std::size_t BodyCount() const;

    /// Keadaan sebuah benda. False bila handle-nya tidak dikenal.
    bool ReadState(BodyHandle body, BodyState& out) const;

    /// Memindahkan benda kinematik ke transform tujuan.
    ///
    /// **Bukan teleport.** PhysX menggerakkannya sepanjang langkah berikutnya
    /// sehingga ia mendorong benda dinamis di jalurnya — memindahkannya seketika
    /// membuat benda menembusnya tanpa sempat bersentuhan.
    bool MoveKinematic(BodyHandle body, const Vec3& position, const Quat& rotation);

    /// Menempatkan benda apa pun seketika, mengabaikan kecepatannya.
    ///
    /// Dipakai editor dan respawn, bukan gameplay: benda dinamis yang
    /// dipindahkan begini bisa muncul di dalam benda lain, dan solver akan
    /// mendorongnya keluar dengan kekuatan yang mengejutkan.
    bool Teleport(BodyHandle body, const Vec3& position, const Quat& rotation);

    /// Menjalankan simulasi untuk `deltaSeconds` waktu nyata.
    ///
    /// Mengembalikan banyaknya langkah tetap yang benar-benar dijalankan — nol
    /// adalah keadaan yang sah dan sering terjadi pada layar berfrekuensi tinggi.
    uint32_t Advance(float deltaSeconds);

    /// Menjalankan tepat `steps` langkah tetap, mengabaikan akumulator.
    ///
    /// Ada untuk test dan untuk `SimHeadless`: keduanya butuh menjalankan
    /// simulasi yang panjangnya ditentukan angka, bukan oleh waktu yang berlalu.
    void Step(uint32_t steps = 1);

    /// Berapa jauh di antara dua langkah keadaan sekarang berada, 0..1.
    /// Yang menggambar memakainya untuk menginterpolasi.
    float Alpha() const;

    /// Total langkah sejak `Create`. Dipakai uji determinisme.
    uint64_t StepCount() const;

    // --- scene query --------------------------------------------------------
    //
    // **Semuanya `const`, dan boleh dipanggil dari beberapa thread sekaligus —
    // asalkan tidak ada langkah simulasi yang sedang berjalan.** Batas itu milik
    // PhysX, bukan pilihan kita, dan ia ditegakkan: memanggil query di tengah
    // `Step` akan gagal dengan pesan alih-alih membaca keadaan setengah jadi.
    // Yang menabraknya mendapat `false` dan satu baris log, karena kerusakan
    // yang ditimbulkannya muncul jauh dari sebabnya.

    /// Perpotongan terdekat sepanjang sebuah ray.
    ///
    /// `direction` tidak perlu bernorma satu — ia dinormalkan di sini, karena
    /// arah yang panjangnya bukan satu membuat `distance` yang dikembalikan
    /// berskala lain tanpa ada yang menyebutkannya.
    bool Raycast(const Vec3& origin, const Vec3& direction, float maxDistance, RayHit& out,
                 const QueryFilter& filter = {}) const;

    /// Seluruh perpotongan sepanjang ray, terurut dari yang terdekat.
    ///
    /// Ada terpisah karena "tembus berapa lapis" dan "kena apa duluan" adalah
    /// dua pertanyaan berbeda, dan yang pertama tidak bisa dijawab dengan
    /// memanggil yang kedua berulang kali tanpa memindahkan titik asalnya.
    std::size_t RaycastAll(const Vec3& origin, const Vec3& direction, float maxDistance,
                           std::vector<RayHit>& out, const QueryFilter& filter = {}) const;

    /// Menggeser sebuah bola sepanjang arah dan melaporkan sentuhan pertama.
    ///
    /// Inilah yang dipakai gerakan karakter dan proyektil cepat: ray tak
    /// bertebal menembus celah yang lebih sempit dari bendanya, dan gejalanya
    /// adalah peluru yang sesekali melewati dinding tipis.
    bool SweepSphere(float radius, const Vec3& origin, const Vec3& direction, float maxDistance,
                     RayHit& out, const QueryFilter& filter = {}) const;

    /// Benda yang bersinggungan dengan sebuah bola. Untuk radius ledakan,
    /// pemicu, dan "siapa saja yang di dekat sini".
    std::size_t OverlapSphere(const Vec3& center, float radius, std::vector<BodyHandle>& out,
                              const QueryFilter& filter = {}) const;

private:
    struct Impl;
    /// **Pimpl, dan itu yang menegakkan seluruh aturan di atas.** Tanpa ia,
    /// anggota bertipe PhysX akan muncul di header ini dan setiap pemanggil ikut
    /// membutuhkan include PhysX-nya.
    std::unique_ptr<Impl> impl_;
};

}  // namespace sim::physics
