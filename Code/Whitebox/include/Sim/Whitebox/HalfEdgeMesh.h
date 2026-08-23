#pragma once

#include "Sim/Core/Math.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/// Mesh half-edge: sisi yang tahu tetangganya.
///
/// **Array segitiga tidak cukup untuk menyunting topologi.** Menjawab "siapa
/// tetangga sisi ini" dengan menyisir seluruh segitiga membuat setiap operasi
/// kuadratik, dan — yang lebih buruk — membuat setiap kesalahan topologi diam.
/// Half-edge menyimpan jawabannya sebagai struktur, jadi pertanyaannya dijawab
/// dalam waktu tetap dan kesalahannya bisa diperiksa.
///
/// **Sengaja kecil.** Mesh whitebox adalah blockout level: puluhan sampai
/// ratusan sisi, bukan jutaan. Struktur yang tidak perlu cepat pada sejuta
/// segitiga boleh jauh lebih sederhana, dan yang lebih sederhana lebih mudah
/// dibuktikan benar. Rencananya di docs/PLAN-WHITEBOX.md.
namespace sim::whitebox {

/// Rujukan bertipe, supaya indeks simpul tidak pernah tertukar dengan indeks
/// face hanya karena keduanya `uint32_t`.
enum class VertexHandle : uint32_t { Invalid = 0xFFFFFFFFu };
enum class HalfEdgeHandle : uint32_t { Invalid = 0xFFFFFFFFu };
enum class EdgeHandle : uint32_t { Invalid = 0xFFFFFFFFu };
enum class FaceHandle : uint32_t { Invalid = 0xFFFFFFFFu };

inline constexpr bool IsValid(VertexHandle h) { return h != VertexHandle::Invalid; }
inline constexpr bool IsValid(HalfEdgeHandle h) { return h != HalfEdgeHandle::Invalid; }
inline constexpr bool IsValid(EdgeHandle h) { return h != EdgeHandle::Invalid; }
inline constexpr bool IsValid(FaceHandle h) { return h != FaceHandle::Invalid; }

/// Separuh sebuah rusuk, menghadap satu face.
///
/// Rusuk selalu punya dua: satu untuk tiap sisi. Yang di tepi mesh punya
/// pasangan **batas** — half-edge tanpa face — sehingga setiap half-edge selalu
/// punya pasangan, tanpa kekecualian. Kekecualian di struktur data adalah
/// cabang yang harus diingat setiap pemanggil, dan cepat atau lambat ada yang
/// lupa.
struct HalfEdge {
    /// Simpul di pangkalnya. Ujungnya adalah pangkal `next`.
    VertexHandle origin = VertexHandle::Invalid;
    /// Face di sebelah kirinya, atau `Invalid` bila ia half-edge batas.
    FaceHandle face = FaceHandle::Invalid;
    /// Berikutnya mengelilingi face yang sama, berlawanan arah jarum jam.
    HalfEdgeHandle next = HalfEdgeHandle::Invalid;
    /// Separuh yang lain dari rusuk yang sama, menghadap arah berlawanan.
    HalfEdgeHandle twin = HalfEdgeHandle::Invalid;
};

struct Vertex {
    Vec3 position{0.0f};
    /// Salah satu half-edge yang berpangkal di sini. Titik masuk untuk
    /// menelusuri seluruh tetangganya.
    HalfEdgeHandle outgoing = HalfEdgeHandle::Invalid;
};

struct Edge {
    /// Salah satu dari dua half-edge-nya; yang lain adalah `twin`-nya.
    HalfEdgeHandle halfEdge = HalfEdgeHandle::Invalid;
};

struct Face {
    /// Salah satu half-edge yang mengelilinginya.
    HalfEdgeHandle halfEdge = HalfEdgeHandle::Invalid;
};

/// Hasil pemeriksaan invarian.
///
/// **Bukan `bool`.** Struktur yang rusak perlu mengatakan rusaknya di mana —
/// "mesh tidak sah" adalah laporan yang tidak bisa ditindaklanjuti pada mesh
/// berisi ratusan half-edge.
struct MeshCheck {
    bool ok = true;
    std::string error;

    explicit operator bool() const { return ok; }
};

class HalfEdgeMesh {
public:
    void Clear();

    VertexHandle AddVertex(const Vec3& position);

    /// Menambahkan face dari simpul-simpulnya, berlawanan arah jarum jam.
    ///
    /// Mengembalikan `FaceHandle::Invalid` bila permintaannya tidak masuk akal:
    /// kurang dari tiga simpul, simpul berulang, atau rusuk yang arah itu sudah
    /// dipakai face lain — yang terakhir berarti geometri non-manifold, dan
    /// menolaknya di sini jauh lebih murah daripada menemukannya sebagai mesh
    /// yang tidak bisa ditelusuri sepuluh operasi kemudian.
    FaceHandle AddFace(const std::vector<VertexHandle>& vertices);

    /// Menutup mesh: setiap half-edge yang belum berpasangan mendapat pasangan
    /// batas, dan pasangan-pasangan itu dirangkai menjadi lingkaran batas.
    ///
    /// **Dipanggil sekali sesudah seluruh face ditambahkan.** Merangkai batas
    /// setiap kali face ditambah berarti membongkar dan menyusunnya kembali
    /// berulang kali untuk hasil yang sama.
    void FinalizeBoundaries();

    std::size_t VertexCount() const { return vertices_.size(); }
    std::size_t HalfEdgeCount() const { return halfEdges_.size(); }
    std::size_t EdgeCount() const { return edges_.size(); }
    std::size_t FaceCount() const { return faces_.size(); }

    const Vertex& GetVertex(VertexHandle h) const;
    const HalfEdge& GetHalfEdge(HalfEdgeHandle h) const;
    const Edge& GetEdge(EdgeHandle h) const;
    const Face& GetFace(FaceHandle h) const;

    /// Half-edge yang mengelilingi sebuah face, mulai dari `Face::halfEdge`.
    std::vector<HalfEdgeHandle> FaceHalfEdges(FaceHandle face) const;
    /// Simpul yang mengelilingi sebuah face, berlawanan arah jarum jam.
    std::vector<VertexHandle> FaceVertices(FaceHandle face) const;
    /// Half-edge yang berpangkal di sebuah simpul.
    std::vector<HalfEdgeHandle> VertexOutgoing(VertexHandle vertex) const;

    /// Rusuk yang dimiliki sebuah half-edge. Pasangan selalu berbagi rusuk yang
    /// sama — itulah arti "pasangan", dan `CheckInvariants` menegakkannya.
    EdgeHandle HalfEdgeEdge(HalfEdgeHandle halfEdge) const;
    /// Kedua half-edge sebuah rusuk.
    std::pair<HalfEdgeHandle, HalfEdgeHandle> EdgeHalfEdges(EdgeHandle edge) const;
    /// Kedua face yang berbagi sebuah rusuk. Salah satunya `Invalid` bila rusuk
    /// itu di batas mesh.
    std::pair<FaceHandle, FaceHandle> EdgeFaces(EdgeHandle edge) const;

    /// Normal sebuah face, dari Newell — tahan terhadap poligon tak sebidang
    /// sempurna, yang justru keadaan biasa sesudah beberapa kali ekstrusi.
    Vec3 FaceNormal(FaceHandle face) const;

    /// **Menyisir seluruh mesh**, bukan menyampel.
    ///
    /// Ini yang menggantikan kematangan pustaka topologi yang sudah teruji
    /// bertahun-tahun: setiap operasi diakhiri dengan pemeriksaan ini di dalam
    /// ujinya, sehingga kesalahan topologi ditangkap pada operasi yang
    /// menyebabkannya alih-alih pada operasi yang kebetulan tersandung olehnya.
    MeshCheck CheckInvariants() const;

private:
    friend class HalfEdgeMeshTestAccess;

    std::vector<Vertex> vertices_;
    std::vector<HalfEdge> halfEdges_;
    std::vector<Edge> edges_;
    std::vector<Face> faces_;
    /// Sejajar dengan `halfEdges_`: rusuk pemilik tiap half-edge.
    std::vector<EdgeHandle> halfEdgeEdge_;
};

/// Kubus satuan berpusat di titik asal, enam quad menghadap keluar.
///
/// Ada di header karena ia titik awal setiap blockout, dan karena ia mesh
/// tertutup terkecil yang menguji seluruh invarian sekaligus.
HalfEdgeMesh MakeUnitCube();

/// Kubus satuan yang tiap sisinya dua segitiga: 12 face, bukan 6.
///
/// **Bukan versi yang lebih rendah dari yang di atas** melainkan keadaan yang
/// datang dari luar: mesh impor selalu tersegitigakan, dan whitebox harus bisa
/// mengelompokkannya kembali menjadi sisi. Ia karena itu bahan uji utama lapisan
/// poligon — di sinilah "12 face menjadi 6 sisi" punya arti.
HalfEdgeMesh MakeUnitCubeTriangulated();

// --- bentuk awal blockout lain ----------------------------------------------
//
// **Semua sisinya cembung, dan itu bukan selera.** `WhiteboxMesh::BuildMeshData`
// menyegitiga sisi dengan kipas dari simpul pertama — sah hanya untuk sisi
// cembung. Sebuah profil tangga yang ditulis sebagai satu poligon tunggal
// memang lebih ringkas dan menghasilkan segitiga yang menembus dirinya sendiri,
// dan itu tidak terlihat sebagai galat melainkan sebagai bentuk yang salah.
//
// Semuanya berpusat di titik asal dan berukuran 1×1×1, sama dengan kubusnya,
// supaya skala entity berarti hal yang sama untuk bentuk mana pun.

/// Bidang miring: naik sepanjang +Z, tegak di sisi belakangnya.
HalfEdgeMesh MakeUnitRamp();

/// Tangga naik sepanjang +Z. `steps` dijepit ke [1, 64].
///
/// Sisi kiri dan kanannya dibagi menjadi quad per-tingkat, bukan satu poligon
/// bertakik — lihat catatan cembung di atas. Pembagian itu juga yang membuat
/// rusuknya bertemu penuh antar kolom: kolom yang tingginya berbeda dan bertemu
/// di satu rusuk panjang meninggalkan T-junction, dan T-junction adalah retakan
/// setebal satu piksel yang muncul pada sudut pandang tertentu saja.
HalfEdgeMesh MakeUnitStairs(uint32_t steps = 6);

/// Prisma bersegi `sides`, sumbu Y. `sides` dijepit ke [3, 64].
HalfEdgeMesh MakeUnitCylinder(uint32_t sides = 12);

/// Kerucut bersegi `sides` dengan puncak di +Y. `sides` dijepit ke [3, 64].
HalfEdgeMesh MakeUnitCone(uint32_t sides = 12);

}  // namespace sim::whitebox
