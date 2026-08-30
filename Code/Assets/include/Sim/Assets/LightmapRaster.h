#pragma once

#include "Sim/Assets/MeshData.h"

#include <cstdint>
#include <vector>

namespace sim::assets {

/// Satu texel lightmap: titik permukaan yang diwakilinya (S5 di
/// docs/PLAN-STATIC-GI.md).
struct LightmapTexel {
    Vec3 position{0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    /// False berarti texel ini tidak ditutupi satu pun segitiga. Ia tetap ada di
    /// dalam larik — atlas adalah petak, bukan daftar — dan yang membacanya
    /// harus tahu bedanya.
    bool covered = false;
};

/// Petak texel sebuah mesh, hasil rasterisasi UV lightmap-nya.
struct LightmapRaster {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<LightmapTexel> texels;
    /// Berapa texel yang benar-benar tertutup permukaan.
    uint32_t coveredCount = 0;

    bool IsValid() const {
        return width > 0 && height > 0 &&
               texels.size() == static_cast<std::size_t>(width) * height;
    }
};

/// Merasterisasi UV lightmap sebuah mesh menjadi petak titik permukaan.
///
/// **Yang dirasterisasi UV lightmap, bukan UV pertama.** Keduanya berbeda sejak
/// S4: yang pertama dirancang untuk tekstur dan boleh berulang, yang kedua unik
/// menurut konstruksi. Merasterisasi yang salah menghasilkan lightmap yang
/// texel-nya dipakai beberapa permukaan sekaligus.
///
/// **Setiap texel yang tersentuh segitiga diambil, bukan hanya yang pusatnya di
/// dalam.** Sebuah segitiga yang lebih kecil daripada satu texel tidak punya
/// satu pun pusat texel di dalamnya, dan yang menampung hanya pusat menghasilkan
/// lubang hitam yang bentuknya mengikuti geometri tipis — pagar, daun, tepi
/// meja. Texel yang pusatnya di luar diproyeksikan ke titik terdekat di dalam
/// segitiganya, supaya posisinya tetap berada di permukaan.
LightmapRaster RasteriseLightmap(const MeshData& mesh, uint32_t width, uint32_t height);

/// Mengisi texel kosong dari tetangga tertutup terdekat.
///
/// **Tanpa ini setiap jahitan chart menjadi garis gelap.** Sampling bilinear di
/// GPU membaca texel di luar chart-nya, dan texel yang tidak pernah diisi adalah
/// nol — jadi tepi setiap chart menggelap satu texel ke dalam, di setiap chart,
/// di seluruh adegan.
///
/// `radius` berapa kali perluasan dilakukan; satu sudah cukup untuk bilinear,
/// lebih dibutuhkan bila lightmap-nya di-mip.
void DilateLightmap(std::vector<Vec3>& values, const LightmapRaster& raster, uint32_t radius = 2);

}  // namespace sim::assets
