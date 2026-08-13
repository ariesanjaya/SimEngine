#pragma once

#include "Sim/Whitebox/HalfEdgeMesh.h"

#include <cstdint>
#include <vector>

/// Poligon: kelompok face yang membentuk satu bidang bagi pengguna.
///
/// **Inilah yang membedakan alat rancang dari penyunting segitiga.** Kubus punya
/// dua belas segitiga tetapi enam sisi, dan yang didorong perancang adalah
/// sisinya. Tanpa lapisan ini, "dorong sisi ini" berarti memilih dua segitiga
/// dan berharap keduanya bergerak bersama.
///
/// Poligon juga yang menjadi **satuan material** — bukan segitiga, yang tak
/// seorang pun ingin menyetelnya satu per satu.
namespace sim::whitebox {

/// Rujukan ke sebuah poligon.
///
/// **Nilainya kanonik, bukan dialokasikan:** sebuah poligon selalu dikenali oleh
/// face bernomor terkecil di dalamnya. Itu membuat menggabungkan lalu memulihkan
/// mengembalikan nomor yang sama persis — dan nomor yang berubah saat undo akan
/// memutuskan seleksi yang sedang dipegang penyunting.
enum class PolygonHandle : uint32_t { Invalid = 0xFFFFFFFFu };

inline constexpr bool IsValid(PolygonHandle h) { return h != PolygonHandle::Invalid; }

/// Pengelompokan face menjadi poligon.
///
/// **Topologi meshnya tidak disentuh sama sekali.** Menyembunyikan rusuk hanya
/// mengubah pengelompokan, bukan menghapus rusuknya — dan itulah yang membuat
/// pemulihannya persis, bukan mendekati. Rusuk yang benar-benar dihapus harus
/// dibangun ulang dari ingatan tentang bentuknya dahulu, dan ingatan itu selalu
/// kurang sesuatu.
class PolygonSet {
public:
    /// Menyetel ulang: satu poligon per face, tidak ada rusuk tersembunyi.
    void Reset(const HalfEdgeMesh& mesh);

    /// Menyembunyikan sebuah rusuk, menggabungkan poligon di kedua sisinya.
    ///
    /// Menolak dan mengembalikan false bila: rusuknya di batas mesh, kedua
    /// sisinya sudah satu poligon, atau kedua face-nya tidak sebidang dalam
    /// `toleranceDegrees`. Yang terakhir dijaga karena menggabungkan dua bidang
    /// yang menyudut menghasilkan "sisi" yang tidak punya satu normal — dan
    /// seluruh gunanya poligon adalah bahwa ia punya.
    bool HideEdge(const HalfEdgeMesh& mesh, EdgeHandle edge, float toleranceDegrees = 1.0f);

    /// Memunculkan kembali rusuk yang disembunyikan, memisahkan poligonnya.
    ///
    /// Mengembalikan false bila rusuknya memang tidak tersembunyi.
    bool RestoreEdge(const HalfEdgeMesh& mesh, EdgeHandle edge);

    /// Menyembunyikan setiap rusuk yang kedua sisinya sebidang, sekali jalan.
    /// Mengembalikan banyaknya rusuk yang disembunyikan.
    ///
    /// Dipakai saat mesh tersegitigakan masuk dari luar: importir menghasilkan
    /// segitiga, dan yang ingin disunting perancang adalah sisinya kembali.
    std::size_t MergeCoplanar(const HalfEdgeMesh& mesh, float toleranceDegrees = 1.0f);

    std::size_t PolygonCount() const;
    /// Poligon pemilik sebuah face.
    PolygonHandle FacePolygon(FaceHandle face) const;
    /// Face-face di dalam sebuah poligon.
    std::vector<FaceHandle> PolygonFaces(PolygonHandle polygon) const;
    /// Seluruh poligon yang masih terpakai, berurutan.
    std::vector<PolygonHandle> Polygons() const;

    bool IsEdgeHidden(EdgeHandle edge) const;
    std::size_t HiddenEdgeCount() const;

    /// Normal sebuah poligon: rata-rata normal face-nya, berbobot luas.
    ///
    /// Berbobot luas, bukan rata-rata biasa — sebuah sisi yang dibelah menjadi
    /// satu segitiga besar dan satu sliver tidak boleh normalnya ditarik
    /// setengah oleh sliver itu.
    Vec3 PolygonNormal(const HalfEdgeMesh& mesh, PolygonHandle polygon) const;

    /// Memeriksa bahwa pengelompokannya konsisten dengan meshnya.
    ///
    /// Alasan yang sama dengan `HalfEdgeMesh::CheckInvariants`: pengelompokan
    /// yang rusak menghasilkan sisi yang separuhnya tidak ikut bergerak, dan itu
    /// terlihat sebagai bug ekstrusi alih-alih sebagai bug pengelompokan.
    MeshCheck CheckInvariants(const HalfEdgeMesh& mesh) const;

private:
    /// Menghitung ulang komponen terhubung sebuah poligon sesudah sebuah rusuk
    /// dimunculkan kembali.
    void Resplit(const HalfEdgeMesh& mesh, PolygonHandle polygon);

    /// face → face perwakilan poligonnya (yang bernomor terkecil).
    std::vector<uint32_t> facePolygon_;
    /// Diindeks dengan face perwakilan. Kosong berarti face itu bukan
    /// perwakilan — ia anggota poligon yang diwakili face lain.
    std::vector<std::vector<FaceHandle>> polygonFaces_;
    /// Rusuk yang disembunyikan, sejajar dengan indeks rusuk mesh.
    std::vector<bool> hiddenEdge_;
};

}  // namespace sim::whitebox
