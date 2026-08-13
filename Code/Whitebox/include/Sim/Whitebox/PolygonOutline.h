#pragma once

#include "Sim/Whitebox/WhiteboxMesh.h"

#include <array>
#include <utility>
#include <vector>

/// Bentuk sebuah sisi, dalam rupa yang bisa langsung digambar penyunting.
///
/// **Batasnya rusuk poligon, bukan rusuk face.** Sisi yang tersusun dari dua
/// segitiga punya satu rusuk diagonal di tengahnya; menggambar seluruh rusuk
/// face berarti menunjukkan diagonal itu kepada perancang — persis pembagian
/// yang seluruh lapisan poligon ada untuk menyembunyikannya.
namespace sim::whitebox {

struct PolygonOutline {
    /// Ruas batas sisi, di ruang lokal mesh.
    std::vector<std::pair<Vec3, Vec3>> edges;
    /// Segitiga isinya, dipakai menyorot sisi yang terpilih.
    std::vector<std::array<Vec3, 3>> triangles;
    /// Titik berat berbobot luas — bukan rata-rata simpul.
    ///
    /// Inilah tempat gizmo berdiri, dan rata-rata simpul menaruhnya di tempat
    /// yang salah begitu sisinya tersegitigakan tidak rata: sepuluh simpul
    /// berdesakan di satu sudut menarik rata-rata ke sudut itu, padahal
    /// bidangnya tidak berubah sama sekali.
    Vec3 centroid{0.0f};
    Vec3 normal{0.0f};
    float area = 0.0f;

    bool empty() const { return triangles.empty(); }
};

PolygonOutline BuildPolygonOutline(const WhiteboxMesh& box, PolygonHandle polygon);

}  // namespace sim::whitebox
