#pragma once

#include "Sim/Core/Uuid.h"
#include "Sim/Scene/World.h"

#include <vector>

namespace sim::scene {

/// Entity yang merujuk sebuah aset, lewat field `AssetRef` mana pun.
///
/// **Dituntun reflection, bukan daftar komponen yang ditulis tangan.** Setiap
/// field bertipe `AssetRef` ikut terperiksa — termasuk yang bersarang di dalam
/// struct dan yang berada di dalam vektor — sehingga komponen baru yang merujuk
/// aset otomatis ikut terhitung. Daftar yang ditulis tangan akan diam-diam
/// ketinggalan, dan pemakainya adalah peringatan "aset ini masih dipakai": satu
/// komponen yang terlewat berarti peringatan yang menjanjikan lebih daripada
/// yang bisa ditepatinya.
///
/// Hasilnya diurutkan menurut penelusuran hierarki dari akar, supaya dua
/// panggilan atas dunia yang sama menghasilkan urutan yang sama.
std::vector<Entity> EntitiesUsingAsset(const World& world, const Uuid& asset);

}  // namespace sim::scene
