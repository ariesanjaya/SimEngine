#pragma once

#include "Sim/Assets/MeshData.h"
#include "Sim/Whitebox/HalfEdgeMesh.h"
#include "Sim/Whitebox/Operations.h"
#include "Sim/Whitebox/Polygon.h"

#include <cstdint>
#include <vector>

/// Whitebox utuh: topologi, pengelompokan sisi, dan material per sisi.
///
/// **Inilah yang disimpan sebuah aset**, bukan segitiga hasilnya. Yang tersimpan
/// harus yang bisa disunting lagi; segitiga adalah keluaran, dan menyimpannya
/// berarti blockout yang tidak bisa diubah sesudah disimpan.
namespace sim::whitebox {

/// Slot material yang belum ditetapkan.
///
/// Mengikuti aturan yang sudah dipakai mesh impor: ruas ber-`material` −1 berarti
/// "berkasnya tidak menyebut material untuk ruas ini", dan penyunting mengisinya
/// dengan material bawaan. Nol bukan penggantinya — nol adalah slot pertama yang
/// sah.
inline constexpr int kNoMaterial = -1;

class WhiteboxMesh {
public:
    /// Memulai dari kubus satuan, titik awal setiap blockout.
    static WhiteboxMesh MakeCube();
    /// Kubus yang tiap sisinya dua segitiga, seperti yang datang dari importir.
    static WhiteboxMesh MakeTriangulatedCube();

    /// **Hanya bisa dibaca, dan itu disengaja.** Penetapan material sejajar
    /// dengan poligon, dan poligon dikenali face terkecilnya — jadi setiap
    /// perubahan topologi maupun pengelompokan bisa menggeser nomornya. Kalau
    /// pemanggil boleh mengubah keduanya langsung, materialnya tertinggal di
    /// nomor lama tanpa satu pun tanda, dan sisi yang kehilangan materialnya
    /// akan menyalahkan operasi yang kebetulan dijalankan terakhir.
    ///
    /// Setiap yang mengubah lewat kelas ini, supaya materialnya ikut dipindahkan.
    const HalfEdgeMesh& Mesh() const { return mesh_; }
    const PolygonSet& Polygons() const { return polygons_; }

    // --- operasi yang mengubah bentuk -----------------------------------------

    EditResult Extrude(PolygonHandle polygon, float distance);
    EditResult Translate(PolygonHandle polygon, const Vec3& displacement);

    // --- operasi yang mengubah pengelompokan ----------------------------------

    bool HideEdge(EdgeHandle edge, float toleranceDegrees = 1.0f);
    bool RestoreEdge(EdgeHandle edge);
    std::size_t MergeCoplanar(float toleranceDegrees = 1.0f);

    /// Slot material sebuah sisi, atau `kNoMaterial`.
    int PolygonMaterial(PolygonHandle polygon) const;
    /// Menetapkan slot material sebuah sisi. False bila poligonnya tidak ada.
    bool SetPolygonMaterial(PolygonHandle polygon, int material);

    /// Banyaknya slot material yang benar-benar dipakai.
    std::size_t UsedMaterialCount() const;

    /// Menyusun mesh yang bisa digambar.
    ///
    /// **Sisi sematerial digabung menjadi satu ruas**, bukan satu ruas per sisi:
    /// kubus dengan tiga sisi bermaterial A dan tiga bermaterial B menghasilkan
    /// dua ruas, bukan enam. Satu panggilan gambar per material adalah yang
    /// diminta renderer; satu per sisi adalah enam kali kerja untuk hasil yang
    /// sama.
    assets::MeshData BuildMeshData() const;

    /// Memeriksa bahwa penetapan materialnya konsisten dengan pengelompokannya.
    MeshCheck CheckInvariants() const;

private:
    /// Dipanggil sesudah operasi yang mengubah topologi: penetapan material
    /// mengikuti poligon, dan poligon dikenali face terkecilnya — jadi nomor
    /// yang bergeser harus ikut dipindahkan, bukan ditinggalkan.
    void RemapMaterials(const std::vector<uint32_t>& oldFacePolygon);

    HalfEdgeMesh mesh_;
    PolygonSet polygons_;
    /// Diindeks dengan nomor poligon (yaitu face perwakilannya).
    std::vector<int> polygonMaterial_;
};

}  // namespace sim::whitebox
