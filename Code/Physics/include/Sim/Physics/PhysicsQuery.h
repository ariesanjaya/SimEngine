#pragma once

#include "Sim/Physics/PhysicsTypes.h"

#include <cstdint>

/// Pertanyaan yang diajukan gameplay kepada simulasi: apa yang kena peluru ini,
/// apakah ada tanah di bawah kaki, siapa saja yang berada di radius ledakan.
///
/// **Batasnya terhadap Embree, ditulis di sini alih-alih hanya dipahami.**
/// Keduanya menembakkan ray, dan itu satu-satunya kemiripannya:
///
/// - Yang di sini menjawab **keadaan simulasi saat ini** — bentuk tabrakan yang
///   sudah bergerak, benda kinematik di posisi frame ini, dan benda yang baru
///   ditambahkan. Ia harus murah cukup untuk dipanggil ratusan kali per frame
///   dan jawabannya kedaluwarsa satu langkah kemudian.
/// - Embree menjawab **geometri yang digambar** — setiap segitiga, termasuk
///   hiasan yang tidak punya collider — untuk bake pencahayaan yang berjalan
///   sekali dan hasilnya disimpan.
///
/// Memakai yang satu untuk pekerjaan yang lain gagal secara diam-diam: raycast
/// fisika terhadap dunia yang penuh hiasan tanpa collider menembusnya seolah
/// tidak ada, sementara bake yang memakai bentuk tabrakan membakar bayangan
/// kotak untuk pohon.
namespace sim::physics {

/// Lapisan tabrakan sebuah benda, dan lapisan yang ditanyakan sebuah query.
///
/// **Bitmask, bukan indeks.** Sebuah peluru menanyakan "dinding atau musuh",
/// bukan "lapisan 3" — dan pertanyaan yang hanya bisa menyebut satu lapisan
/// memaksa pemanggil menembakkan ray berkali-kali untuk satu jawaban.
using LayerMask = uint32_t;

inline constexpr LayerMask kDefaultLayer = 1u;
inline constexpr LayerMask kAllLayers = 0xFFFFFFFFu;
/// Tidak cocok dengan apa pun. Ada supaya "tidak menyaring apa-apa" dan
/// "menyaring semuanya" bisa dibedakan tanpa menebak arti nol.
inline constexpr LayerMask kNoLayers = 0u;

/// Apa yang boleh dijawab sebuah query.
struct QueryFilter {
    /// Benda ikut dipertimbangkan bila `body.layer & layers` bukan nol.
    LayerMask layers = kAllLayers;

    /// **Dipisah dari `layers` dengan sengaja.** "Abaikan semua yang bergerak"
    /// adalah pertanyaan yang berulang — deteksi tanah, penempatan bangunan —
    /// dan menjawabnya lewat lapisan menuntut setiap benda dinamis diberi
    /// lapisan tersendiri, disiplin yang akan gagal pada benda yang lupa diberi.
    bool hitStatic = true;
    bool hitDynamic = true;
};

/// Satu perpotongan.
struct RayHit {
    BodyHandle body = BodyHandle::Invalid;
    /// Titik kena, ruang dunia.
    Vec3 position{0.0f};
    /// Normal permukaan di titik itu, menghadap ke luar benda yang kena.
    Vec3 normal{0.0f};
    /// Jarak dari titik asal sepanjang arah ray. Nol berarti asalnya sudah di
    /// dalam bentuknya.
    float distance = 0.0f;
};

}  // namespace sim::physics
