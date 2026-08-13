#pragma once

#include "Sim/Terrain/Terrain.h"

/// Menemukan titik yang ditunjuk sinar di atas permukaan terrain.
///
/// **Ditelusuri terhadap heightmap, bukan terhadap segitiga yang kebetulan
/// tergambar.** Dua alasan, dan keduanya menentukan:
///
/// - Segitiga yang tergambar bergantung pada LOD, dan LOD bergantung pada jarak
///   kamera. Menembak segitiga berarti titik yang dipahat berpindah ketika
///   kamera maju-mundur, tanpa satu pun yang berubah di terrainnya.
/// - Segitiganya juga tidak ada di sisi engine yang bisa ditanya: mereka
///   penghuni memori GPU, dan menyalinnya kembali untuk sebuah klik jauh lebih
///   mahal daripada menelusuri heightmap yang memang sudah ada di tangan.
namespace sim::terrain {

struct TerrainHit {
    bool hit = false;
    /// Titik tembus di **ruang lokal terrain** — titik asalnya sudut peta.
    Vec3 position{0.0f};
    /// Jarak dari `origin` di sepanjang arah yang sudah dinormalkan.
    float distance = 0.0f;

    explicit operator bool() const { return hit; }
};

/// Sinar terhadap permukaan terrain, di ruang lokal terrain.
///
/// `direction` tidak perlu bernorma satu — ia dinormalkan di sini, karena arah
/// yang panjangnya bukan satu membuat `distance` berskala lain tanpa ada yang
/// menyebutkannya.
///
/// **Sinar yang berpangkal di bawah permukaan menjawab "tidak kena."** Yang di
/// bawah tanah tidak bisa melihat apa yang dipahatnya, dan mengembalikan titik
/// tembus dari sisi bawah berarti kursor melompat ke tempat yang tidak
/// ditunjuk siapa pun.
///
/// Ketelitiannya dibatasi setengah jarak sampel: langkahnya sebesar itu, lalu
/// perpotongannya dipersempit dengan bagi dua. Fitur yang lebih sempit daripada
/// itu tidak bisa dinyatakan heightmap-nya sendiri.
TerrainHit RaycastTerrain(const Terrain& terrain, const Vec3& origin, const Vec3& direction,
                          float maxDistance = 1e30f);

}  // namespace sim::terrain
