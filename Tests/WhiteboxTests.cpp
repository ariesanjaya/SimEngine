#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Whitebox/HalfEdgeMesh.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace sim;
using namespace sim::whitebox;

TEST_CASE("kubus satuan punya 8 simpul, 12 rusuk, dan 6 face") {
    // Kriteria terima W0. Angkanya bukan sembarang: rumus Euler untuk benda
    // padat tanpa lubang menuntut V - E + F = 2, dan 8 - 12 + 6 = 2. Kubus yang
    // salah rakit hampir selalu melanggarnya, jadi ia uji yang murah dan tajam.
    const HalfEdgeMesh cube = MakeUnitCube();

    CHECK(cube.VertexCount() == 8);
    CHECK(cube.EdgeCount() == 12);
    CHECK(cube.FaceCount() == 6);

    const int euler = static_cast<int>(cube.VertexCount()) - static_cast<int>(cube.EdgeCount()) +
                      static_cast<int>(cube.FaceCount());
    INFO("V - E + F = " << euler);
    CHECK(euler == 2);

    // Tertutup: tiap rusuk dipakai dua face, jadi tidak ada half-edge batas.
    CHECK(cube.HalfEdgeCount() == 24);

    const MeshCheck check = cube.CheckInvariants();
    INFO(check.error);
    CHECK(check.ok);
}

TEST_CASE("tiap sisi kubus menghadap keluar") {
    // Satu quad yang urutan simpulnya terbalik tidak terlihat sebagai galat
    // melainkan sebagai lubang di dinding — sisi itu menghilang saat digambar
    // dengan backface culling. Diperiksa terhadap arah yang **dihitung**: normal
    // tiap sisi harus searah dengan vektor dari pusat kubus ke pusat sisinya.
    const HalfEdgeMesh cube = MakeUnitCube();

    for (uint32_t f = 0; f < cube.FaceCount(); ++f) {
        const FaceHandle face = static_cast<FaceHandle>(f);
        const std::vector<VertexHandle> loop = cube.FaceVertices(face);
        REQUIRE(loop.size() == 4);

        Vec3 centre(0.0f);
        for (const VertexHandle vertex : loop) {
            centre += cube.GetVertex(vertex).position;
        }
        centre /= static_cast<float>(loop.size());

        const Vec3 normal = cube.FaceNormal(face);
        const float outward = glm::dot(normal, glm::normalize(centre));
        INFO("face " << f << ": normal·keluar = " << outward);
        CHECK(outward > 0.99f);
    }
}

TEST_CASE("pemeriksa invarian menolak mesh yang dirusak") {
    // **Pemeriksa yang tidak bisa gagal tidak menjaga apa pun.** Uji ini yang
    // membuat seluruh W0 berarti: ia membuktikan bahwa pemeriksanya benar-benar
    // menyisir, bukan sekadar mengembalikan true.
    //
    // Kerusakan dibuat lewat API publik — sebuah mesh yang face-nya ditambahkan
    // tanpa `FinalizeBoundaries`, sehingga half-edge di tepinya tidak punya
    // pasangan.
    HalfEdgeMesh mesh;
    const VertexHandle a = mesh.AddVertex(Vec3(0.0f, 0.0f, 0.0f));
    const VertexHandle b = mesh.AddVertex(Vec3(1.0f, 0.0f, 0.0f));
    const VertexHandle c = mesh.AddVertex(Vec3(1.0f, 1.0f, 0.0f));
    REQUIRE(mesh.AddFace({a, b, c}) != FaceHandle::Invalid);

    const MeshCheck broken = mesh.CheckInvariants();
    INFO("pesannya: " << broken.error);
    CHECK_FALSE(broken.ok);
    CHECK_FALSE(broken.error.empty());

    // Dan menutupnya membuatnya sah.
    mesh.FinalizeBoundaries();
    const MeshCheck fixed = mesh.CheckInvariants();
    INFO(fixed.error);
    CHECK(fixed.ok);
}

TEST_CASE("satu quad terbuka tetap sah, dengan batas yang tertutup melingkar") {
    // Blockout dimulai dari satu sisi sebelum ia diekstrusi. Mesh terbuka harus
    // sah, bukan kasus khusus yang menunggu W2 untuk diperbaiki.
    HalfEdgeMesh mesh;
    const VertexHandle a = mesh.AddVertex(Vec3(-0.5f, 0.0f, -0.5f));
    const VertexHandle b = mesh.AddVertex(Vec3(0.5f, 0.0f, -0.5f));
    const VertexHandle c = mesh.AddVertex(Vec3(0.5f, 0.0f, 0.5f));
    const VertexHandle d = mesh.AddVertex(Vec3(-0.5f, 0.0f, 0.5f));
    REQUIRE(mesh.AddFace({a, b, c, d}) != FaceHandle::Invalid);
    mesh.FinalizeBoundaries();

    CHECK(mesh.FaceCount() == 1);
    CHECK(mesh.EdgeCount() == 4);
    // Empat di dalam, empat di batas.
    CHECK(mesh.HalfEdgeCount() == 8);

    const MeshCheck check = mesh.CheckInvariants();
    INFO(check.error);
    CHECK(check.ok);

    // Lingkaran batasnya tertutup: berjalan mengikuti `next` dari sebuah
    // half-edge batas kembali ke tempatnya semula sesudah empat langkah.
    HalfEdgeHandle boundary = HalfEdgeHandle::Invalid;
    for (uint32_t i = 0; i < mesh.HalfEdgeCount(); ++i) {
        const HalfEdgeHandle handle = static_cast<HalfEdgeHandle>(i);
        if (!IsValid(mesh.GetHalfEdge(handle).face)) {
            boundary = handle;
            break;
        }
    }
    REQUIRE(IsValid(boundary));

    HalfEdgeHandle current = boundary;
    int steps = 0;
    do {
        current = mesh.GetHalfEdge(current).next;
        ++steps;
        REQUIRE(steps <= 8);
    } while (current != boundary);
    INFO("lingkaran batas panjangnya " << steps);
    CHECK(steps == 4);
}

TEST_CASE("face yang tidak masuk akal ditolak, dan meshnya tidak tersentuh") {
    HalfEdgeMesh mesh;
    const VertexHandle a = mesh.AddVertex(Vec3(0.0f));
    const VertexHandle b = mesh.AddVertex(Vec3(1.0f, 0.0f, 0.0f));
    const VertexHandle c = mesh.AddVertex(Vec3(0.0f, 1.0f, 0.0f));

    SUBCASE("kurang dari tiga simpul") {
        CHECK(mesh.AddFace({a, b}) == FaceHandle::Invalid);
    }
    SUBCASE("simpul berulang") {
        CHECK(mesh.AddFace({a, b, a}) == FaceHandle::Invalid);
    }
    SUBCASE("simpul yang tidak ada") {
        CHECK(mesh.AddFace({a, b, static_cast<VertexHandle>(99u)}) == FaceHandle::Invalid);
    }
    SUBCASE("dua face menghadap arah yang sama pada rusuk yang sama") {
        REQUIRE(mesh.AddFace({a, b, c}) != FaceHandle::Invalid);
        const VertexHandle d = mesh.AddVertex(Vec3(1.0f, 1.0f, 0.0f));
        // Menelusuri a→b ke arah yang sama: non-manifold.
        CHECK(mesh.AddFace({a, b, d}) == FaceHandle::Invalid);
    }

    // **Yang penting: penolakan tidak meninggalkan puing.** Face yang ditolak
    // di tengah akan meninggalkan half-edge menggantung, dan mesh yang rusak
    // separuh jauh lebih sulit didiagnosis daripada penolakan yang bersih.
    mesh.FinalizeBoundaries();
    const MeshCheck check = mesh.CheckInvariants();
    INFO(check.error);
    CHECK(check.ok);
}

TEST_CASE("berputar mengelilingi simpul mencapai seluruh sisi yang menyentuhnya") {
    const HalfEdgeMesh cube = MakeUnitCube();
    // Tiap sudut kubus disentuh tiga sisi, jadi tiga half-edge berpangkal
    // padanya. Ini yang menangkap simpul "terbelah" — dua kipas terpisah yang
    // kebetulan berbagi satu simpul, dan yang membuat penelusuran tetangga
    // diam-diam melewatkan separuh mesh.
    for (uint32_t v = 0; v < cube.VertexCount(); ++v) {
        const std::vector<HalfEdgeHandle> outgoing =
            cube.VertexOutgoing(static_cast<VertexHandle>(v));
        INFO("simpul " << v);
        CHECK(outgoing.size() == 3);
    }
}

namespace sim::whitebox {

/// Pintu belakang khusus uji.
///
/// **Ada supaya pemeriksa invarian bisa dibuktikan menolak.** Kerusakan yang
/// harus ditangkapnya justru yang tidak bisa dibuat lewat API publik — API-nya
/// memang menjaga invarian itu. Tanpa pintu ini, satu-satunya bukti bahwa
/// pemeriksanya bekerja adalah bahwa ia belum pernah menolak apa pun, dan itu
/// bukan bukti.
class HalfEdgeMeshTestAccess {
public:
    static void ClearTwin(HalfEdgeMesh& mesh, uint32_t index) {
        mesh.halfEdges_[index].twin = HalfEdgeHandle::Invalid;
    }
    static void MisdirectTwin(HalfEdgeMesh& mesh, uint32_t index, uint32_t target) {
        mesh.halfEdges_[index].twin = static_cast<HalfEdgeHandle>(target);
    }
    static void BreakFaceLoop(HalfEdgeMesh& mesh, uint32_t index) {
        mesh.halfEdges_[index].next = static_cast<HalfEdgeHandle>(index);
    }
    static void StealHalfEdge(HalfEdgeMesh& mesh, uint32_t index, uint32_t face) {
        mesh.halfEdges_[index].face = static_cast<FaceHandle>(face);
    }
};

}  // namespace sim::whitebox

TEST_CASE("tiap klausa pemeriksa invarian benar-benar menolak") {
    // Kriteria terima W0: pemeriksanya dibuktikan bisa gagal. Bukan sekali,
    // melainkan **per klausa** — pemeriksa yang satu klausanya diam adalah
    // pemeriksa yang menjaga sebagian, dan yang tidak dijaganya justru bagian
    // yang tak seorang pun tahu tidak dijaga.
    SUBCASE("pasangan yang diputus") {
        HalfEdgeMesh cube = MakeUnitCube();
        REQUIRE(cube.CheckInvariants().ok);
        HalfEdgeMeshTestAccess::ClearTwin(cube, 0);
        const MeshCheck check = cube.CheckInvariants();
        INFO(check.error);
        CHECK_FALSE(check.ok);
        CHECK(check.error.find("pasangan") != std::string::npos);
    }

    SUBCASE("pasangan yang menunjuk ke tempat yang salah") {
        HalfEdgeMesh cube = MakeUnitCube();
        // Diarahkan ke half-edge lain yang pasangannya bukan dia — jadi
        // pasangan-dari-pasangan tidak kembali ke dirinya.
        HalfEdgeMeshTestAccess::MisdirectTwin(cube, 0, 5);
        const MeshCheck check = cube.CheckInvariants();
        INFO(check.error);
        CHECK_FALSE(check.ok);
    }

    SUBCASE("lingkaran face yang putus") {
        HalfEdgeMesh cube = MakeUnitCube();
        HalfEdgeMeshTestAccess::BreakFaceLoop(cube, 0);
        const MeshCheck check = cube.CheckInvariants();
        INFO(check.error);
        CHECK_FALSE(check.ok);
    }

    SUBCASE("half-edge yang mengaku milik face lain") {
        HalfEdgeMesh cube = MakeUnitCube();
        HalfEdgeMeshTestAccess::StealHalfEdge(cube, 0, 3);
        const MeshCheck check = cube.CheckInvariants();
        INFO(check.error);
        CHECK_FALSE(check.ok);
        CHECK(check.error.find("face") != std::string::npos);
    }
}
