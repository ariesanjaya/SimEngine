#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Whitebox/HalfEdgeMesh.h"
#include "Sim/Whitebox/Operations.h"
#include "Sim/Whitebox/Picking.h"
#include "Sim/Whitebox/PolygonOutline.h"
#include "Sim/Whitebox/WhiteboxIo.h"
#include "Sim/Whitebox/WhiteboxMesh.h"
#include "Sim/Whitebox/Polygon.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
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

TEST_CASE("kubus tersegitigakan menjadi enam poligon, bukan dua belas face") {
    // **Kriteria terima W1, dan inilah yang membedakan alat rancang dari
    // penyunting segitiga.** Mesh yang datang dari luar selalu tersegitigakan;
    // yang ingin disunting perancang adalah sisinya kembali.
    const HalfEdgeMesh cube = MakeUnitCubeTriangulated();
    REQUIRE(cube.FaceCount() == 12);
    REQUIRE(cube.CheckInvariants().ok);

    PolygonSet polygons;
    polygons.Reset(cube);
    CHECK(polygons.PolygonCount() == 12);  // sebelum digabung, satu per face

    const std::size_t merged = polygons.MergeCoplanar(cube);
    INFO(merged << " rusuk disembunyikan");
    // Enam diagonal, satu per sisi.
    CHECK(merged == 6);
    CHECK(polygons.PolygonCount() == 6);
    CHECK(polygons.HiddenEdgeCount() == 6);

    const MeshCheck check = polygons.CheckInvariants(cube);
    INFO(check.error);
    CHECK(check.ok);

    // Tiap poligon berisi tepat dua segitiga, dan normalnya menghadap keluar.
    for (const PolygonHandle polygon : polygons.Polygons()) {
        CHECK(polygons.PolygonFaces(polygon).size() == 2);

        Vec3 centre(0.0f);
        int samples = 0;
        for (const FaceHandle face : polygons.PolygonFaces(polygon)) {
            for (const VertexHandle vertex : cube.FaceVertices(face)) {
                centre += cube.GetVertex(vertex).position;
                ++samples;
            }
        }
        centre /= static_cast<float>(samples);
        const float outward = glm::dot(polygons.PolygonNormal(cube, polygon),
                                       glm::normalize(centre));
        INFO("poligon " << static_cast<uint32_t>(polygon) << ": normal·keluar = " << outward);
        CHECK(outward > 0.99f);
    }
}

TEST_CASE("memunculkan rusuk mengembalikan pengelompokan persis seperti semula") {
    // Kriteria terima W1. Topologi meshnya memang tidak pernah disentuh —
    // menyembunyikan rusuk hanya mengubah pengelompokan — dan itulah yang
    // membuat pemulihannya persis, bukan mendekati.
    const HalfEdgeMesh cube = MakeUnitCubeTriangulated();

    PolygonSet polygons;
    polygons.Reset(cube);

    // Cuplikan keadaan awal: poligon tiap face.
    std::vector<uint32_t> before;
    for (uint32_t f = 0; f < cube.FaceCount(); ++f) {
        before.push_back(
            static_cast<uint32_t>(polygons.FacePolygon(static_cast<FaceHandle>(f))));
    }

    // Cari sebuah rusuk yang benar-benar menggabungkan sesuatu.
    EdgeHandle diagonal = EdgeHandle::Invalid;
    for (uint32_t e = 0; e < cube.EdgeCount(); ++e) {
        if (polygons.HideEdge(cube, static_cast<EdgeHandle>(e))) {
            diagonal = static_cast<EdgeHandle>(e);
            break;
        }
    }
    REQUIRE(IsValid(diagonal));
    CHECK(polygons.PolygonCount() == 11);
    REQUIRE(polygons.CheckInvariants(cube).ok);

    REQUIRE(polygons.RestoreEdge(cube, diagonal));
    CHECK(polygons.PolygonCount() == 12);
    CHECK(polygons.HiddenEdgeCount() == 0);
    REQUIRE(polygons.CheckInvariants(cube).ok);

    // **Persis seperti semula**, bukan sekadar berjumlah sama: tiap face kembali
    // ke poligon yang sama dengan sebelumnya.
    for (uint32_t f = 0; f < cube.FaceCount(); ++f) {
        INFO("face " << f);
        CHECK(static_cast<uint32_t>(polygons.FacePolygon(static_cast<FaceHandle>(f))) ==
              before[f]);
    }
}

TEST_CASE("menggabungkan seluruh sisi lalu memulihkannya kembali berulang kali") {
    // Gabung-pulihkan bolak-balik adalah persis yang dilakukan undo/redo, dan
    // pengelompokan yang bocor sedikit tiap putaran hanya terlihat sesudah
    // beberapa kali — bukan pada percobaan pertama.
    const HalfEdgeMesh cube = MakeUnitCubeTriangulated();
    PolygonSet polygons;
    polygons.Reset(cube);

    for (int round = 0; round < 5; ++round) {
        const std::size_t merged = polygons.MergeCoplanar(cube);
        INFO("putaran " << round << ": " << merged << " digabung");
        CHECK(polygons.PolygonCount() == 6);
        REQUIRE(polygons.CheckInvariants(cube).ok);

        for (uint32_t e = 0; e < cube.EdgeCount(); ++e) {
            polygons.RestoreEdge(cube, static_cast<EdgeHandle>(e));
        }
        CHECK(polygons.PolygonCount() == 12);
        CHECK(polygons.HiddenEdgeCount() == 0);
        REQUIRE(polygons.CheckInvariants(cube).ok);
    }
}

TEST_CASE("rusuk yang menyudut menolak digabung") {
    // **Sebidang, atau tidak digabung.** Menggabungkan dua bidang yang menyudut
    // menghasilkan "sisi" yang tidak punya satu normal, dan seluruh gunanya
    // poligon adalah bahwa ia punya.
    const HalfEdgeMesh cube = MakeUnitCube();  // enam quad, tanpa diagonal
    PolygonSet polygons;
    polygons.Reset(cube);
    CHECK(polygons.PolygonCount() == 6);

    // Setiap rusuk kubus adalah sudut 90°: tidak satu pun boleh digabung.
    const std::size_t merged = polygons.MergeCoplanar(cube);
    INFO(merged << " rusuk digabung (seharusnya nol)");
    CHECK(merged == 0);
    CHECK(polygons.PolygonCount() == 6);

    // Dan dengan toleransi yang cukup lebar untuk menelan 90°, ia menggabung
    // semuanya — membuktikan bahwa yang menolak tadi memang toleransinya, bukan
    // kode yang kebetulan tidak pernah menggabung apa pun.
    PolygonSet loose;
    loose.Reset(cube);
    CHECK(loose.MergeCoplanar(cube, 91.0f) > 0);
    REQUIRE(loose.CheckInvariants(cube).ok);
}

namespace {

/// Volume mesh tertutup lewat teorema divergensi.
///
/// **Oracle-nya sengaja ditulis di sini, bukan di pustakanya.** Uji yang
/// membandingkan hasil terhadap fungsi milik pustaka yang sama hanya menguji
/// bahwa pustaka itu konsisten dengan dirinya sendiri.
float ClosedVolume(const HalfEdgeMesh& mesh) {
    float total = 0.0f;
    for (uint32_t f = 0; f < mesh.FaceCount(); ++f) {
        const std::vector<VertexHandle> loop = mesh.FaceVertices(static_cast<FaceHandle>(f));
        // Kipas segitiga dari simpul pertama; sah untuk poligon cembung, dan
        // seluruh face di uji ini cembung.
        for (std::size_t i = 1; i + 1 < loop.size(); ++i) {
            const Vec3& a = mesh.GetVertex(loop[0]).position;
            const Vec3& b = mesh.GetVertex(loop[i]).position;
            const Vec3& c = mesh.GetVertex(loop[i + 1]).position;
            total += glm::dot(a, glm::cross(b, c)) / 6.0f;
        }
    }
    return std::abs(total);
}

/// Poligon yang normalnya paling dekat dengan sebuah arah.
PolygonHandle PolygonFacing(const HalfEdgeMesh& mesh, const PolygonSet& polygons,
                            const Vec3& direction) {
    PolygonHandle best = PolygonHandle::Invalid;
    float bestDot = -2.0f;
    for (const PolygonHandle polygon : polygons.Polygons()) {
        const float dot = glm::dot(polygons.PolygonNormal(mesh, polygon), direction);
        if (dot > bestDot) {
            bestDot = dot;
            best = polygon;
        }
    }
    return best;
}

}  // namespace

TEST_CASE("mengekstrusi satu sisi kubus menghasilkan volume yang dihitung") {
    // **Kriteria terima W2.** Kubus satuan bervolume 1; mendorong sisi atasnya
    // sejauh d menghasilkan balok 1x1x(1+d). Angkanya diketahui sebelum
    // operasinya dijalankan — bukan dibaca dari hasilnya.
    HalfEdgeMesh mesh = MakeUnitCube();
    PolygonSet polygons;
    polygons.Reset(mesh);

    INFO("volume awal " << ClosedVolume(mesh));
    CHECK(ClosedVolume(mesh) == doctest::Approx(1.0f).epsilon(0.001));

    const PolygonHandle top = PolygonFacing(mesh, polygons, Vec3(0.0f, 1.0f, 0.0f));
    REQUIRE(IsValid(top));

    const float distance = 0.75f;
    const EditResult extruded = ExtrudePolygon(mesh, polygons, top, distance);
    INFO(extruded.error);
    REQUIRE(extruded.ok);

    // Empat dinding baru menempel pada enam sisi lama.
    CHECK(mesh.FaceCount() == 10);
    CHECK(mesh.VertexCount() == 12);

    INFO("volume sesudah " << ClosedVolume(mesh) << ", diharapkan " << (1.0f + distance));
    CHECK(ClosedVolume(mesh) == doctest::Approx(1.0f + distance).epsilon(0.001));

    // Invarian W0 dan W1 tetap berlaku sesudah operasinya — ini yang menangkap
    // bedah topologi yang menghasilkan bentuk benar tetapi struktur rusak.
    const MeshCheck topology = mesh.CheckInvariants();
    INFO(topology.error);
    CHECK(topology.ok);
    const MeshCheck grouping = polygons.CheckInvariants(mesh);
    INFO(grouping.error);
    CHECK(grouping.ok);
}

TEST_CASE("ekstrusi berulang menumpuk sesuai jumlahnya") {
    // Sekali bisa benar karena kebetulan. Empat kali berturut-turut menguji
    // bahwa hasil tiap langkah benar-benar bisa dipakai langkah berikutnya.
    HalfEdgeMesh mesh = MakeUnitCube();
    PolygonSet polygons;
    polygons.Reset(mesh);

    float expected = 1.0f;
    for (int i = 0; i < 4; ++i) {
        const PolygonHandle top = PolygonFacing(mesh, polygons, Vec3(0.0f, 1.0f, 0.0f));
        REQUIRE(IsValid(top));
        const EditResult extruded = ExtrudePolygon(mesh, polygons, top, 0.5f);
        INFO("putaran " << i << ": " << extruded.error);
        REQUIRE(extruded.ok);
        expected += 0.5f;

        INFO("putaran " << i << " volume " << ClosedVolume(mesh) << " vs " << expected);
        CHECK(ClosedVolume(mesh) == doctest::Approx(expected).epsilon(0.002));
        REQUIRE(mesh.CheckInvariants().ok);
        REQUIRE(polygons.CheckInvariants(mesh).ok);
    }
}

TEST_CASE("ekstrusi nol tidak mengubah apa pun") {
    // Kriteria terima W2. Menumbuhkan dinding berluas nol menghasilkan face yang
    // normalnya tidak tertentu — tidak terlihat sekarang, muncul jauh kemudian
    // sebagai bercak gelap di pencahayaan.
    HalfEdgeMesh mesh = MakeUnitCube();
    PolygonSet polygons;
    polygons.Reset(mesh);

    const std::size_t facesBefore = mesh.FaceCount();
    const std::size_t verticesBefore = mesh.VertexCount();
    const float volumeBefore = ClosedVolume(mesh);

    const PolygonHandle top = PolygonFacing(mesh, polygons, Vec3(0.0f, 1.0f, 0.0f));
    const EditResult extruded = ExtrudePolygon(mesh, polygons, top, 0.0f);
    REQUIRE(extruded.ok);

    CHECK(mesh.FaceCount() == facesBefore);
    CHECK(mesh.VertexCount() == verticesBefore);
    CHECK(ClosedVolume(mesh) == doctest::Approx(volumeBefore));
    CHECK(mesh.CheckInvariants().ok);
}

TEST_CASE("menggeser sisi memindahkannya tanpa menumbuhkan dinding") {
    // Beda dari ekstrusi, dan bedanya harus terlihat: geser memindahkan sisi
    // beserta dinding yang sudah menempel, bukan menumbuhkan yang baru.
    HalfEdgeMesh mesh = MakeUnitCube();
    PolygonSet polygons;
    polygons.Reset(mesh);

    const PolygonHandle top = PolygonFacing(mesh, polygons, Vec3(0.0f, 1.0f, 0.0f));
    REQUIRE(IsValid(top));

    const EditResult moved = TranslatePolygon(mesh, polygons, top, Vec3(0.0f, 0.5f, 0.0f));
    INFO(moved.error);
    REQUIRE(moved.ok);

    // Tidak ada geometri baru.
    CHECK(mesh.FaceCount() == 6);
    CHECK(mesh.VertexCount() == 8);
    // Tetapi bentuknya berubah: balok setinggi 1,5.
    INFO("volume " << ClosedVolume(mesh));
    CHECK(ClosedVolume(mesh) == doctest::Approx(1.5f).epsilon(0.001));

    CHECK(mesh.CheckInvariants().ok);
    CHECK(polygons.CheckInvariants(mesh).ok);
}

TEST_CASE("ekstrusi mempertahankan pengelompokan poligon yang sudah ada") {
    // Nomor face dipertahankan justru supaya ini berlaku: sisi yang sudah
    // digabung dari beberapa segitiga tetap satu sisi sesudah didorong. Kalau
    // tidak, mendorong sisi kubus impor akan memecahnya kembali menjadi
    // segitiga di tangan pengguna.
    HalfEdgeMesh mesh = MakeUnitCubeTriangulated();
    PolygonSet polygons;
    polygons.Reset(mesh);
    REQUIRE(polygons.MergeCoplanar(mesh) == 6);
    REQUIRE(polygons.PolygonCount() == 6);

    const PolygonHandle top = PolygonFacing(mesh, polygons, Vec3(0.0f, 1.0f, 0.0f));
    REQUIRE(IsValid(top));
    REQUIRE(polygons.PolygonFaces(top).size() == 2);

    const EditResult extruded = ExtrudePolygon(mesh, polygons, top, 0.5f);
    INFO(extruded.error);
    REQUIRE(extruded.ok);

    // Sisi atasnya masih satu poligon berisi dua segitiga.
    const std::vector<FaceHandle> faces = polygons.PolygonFaces(extruded.polygon);
    INFO("sisi atas kini berisi " << faces.size() << " face");
    CHECK(faces.size() == 2);

    // Dan volumenya tetap yang dihitung.
    INFO("volume " << ClosedVolume(mesh));
    CHECK(ClosedVolume(mesh) == doctest::Approx(1.5f).epsilon(0.002));
    CHECK(mesh.CheckInvariants().ok);
    CHECK(polygons.CheckInvariants(mesh).ok);
}

TEST_CASE("tiga sisi material A dan tiga material B menghasilkan tepat dua ruas") {
    // **Kriteria terima W3, dan inilah yang diminta sejak awal:** tiap sisi
    // whitebox bisa dipasang material berbeda. Satu ruas per material, bukan
    // satu per sisi — satu panggilan gambar per material adalah yang diminta
    // renderer; satu per sisi adalah enam kali kerja untuk hasil yang sama.
    WhiteboxMesh box = WhiteboxMesh::MakeCube();
    const std::vector<PolygonHandle> sides = box.Polygons().Polygons();
    REQUIRE(sides.size() == 6);

    for (std::size_t i = 0; i < sides.size(); ++i) {
        REQUIRE(box.SetPolygonMaterial(sides[i], i < 3 ? 0 : 1));
    }
    CHECK(box.UsedMaterialCount() == 2);

    const sim::assets::MeshData data = box.BuildMeshData();
    INFO(data.parts.size() << " ruas");
    CHECK(data.parts.size() == 2);
    CHECK(data.parts[0].material == 0);
    CHECK(data.parts[1].material == 1);

    // Enam quad menjadi dua belas segitiga; tidak ada yang hilang maupun
    // terhitung dua kali.
    std::size_t indices = 0;
    for (const sim::assets::SubMesh& part : data.parts) {
        indices += part.indexCount;
    }
    INFO(indices << " indeks");
    CHECK(indices == 12 * 3);
    CHECK(indices == data.indices.size());

    // Ruasnya bersambung tanpa celah maupun tumpang tindih.
    uint32_t expected = 0;
    for (const sim::assets::SubMesh& part : data.parts) {
        CHECK(part.firstIndex == expected);
        expected += part.indexCount;
    }

    // Tiap ruas memuat tiga sisi, jadi keduanya berukuran sama.
    CHECK(data.parts[0].indexCount == data.parts[1].indexCount);

    const MeshCheck check = box.CheckInvariants();
    INFO(check.error);
    CHECK(check.ok);
}

TEST_CASE("sisi tanpa material menghasilkan ruas ber-material -1") {
    // Mengikuti aturan yang sudah dipakai mesh impor: -1 berarti "tidak
    // disebutkan", dan penyunting mengisinya dengan material bawaan. Nol bukan
    // penggantinya — nol adalah slot pertama yang sah.
    WhiteboxMesh box = WhiteboxMesh::MakeCube();
    const std::vector<PolygonHandle> sides = box.Polygons().Polygons();
    REQUIRE(box.SetPolygonMaterial(sides[0], 0));

    const sim::assets::MeshData data = box.BuildMeshData();
    REQUIRE(data.parts.size() == 2);
    // `std::map` mengurutkan, jadi -1 mendahului 0.
    CHECK(data.parts[0].material == kNoMaterial);
    CHECK(data.parts[1].material == 0);
    // Lima sisi tanpa material, satu dengan.
    CHECK(data.parts[0].indexCount == 5 * 2 * 3);
    CHECK(data.parts[1].indexCount == 1 * 2 * 3);
}

TEST_CASE("seluruh sisi bermaterial sama menghasilkan satu ruas") {
    WhiteboxMesh box = WhiteboxMesh::MakeCube();
    for (const PolygonHandle side : box.Polygons().Polygons()) {
        REQUIRE(box.SetPolygonMaterial(side, 3));
    }
    CHECK(box.UsedMaterialCount() == 1);

    const sim::assets::MeshData data = box.BuildMeshData();
    CHECK(data.parts.size() == 1);
    CHECK(data.parts[0].material == 3);
    CHECK(data.parts[0].indexCount == 12 * 3);
}

TEST_CASE("mesh yang dibangun punya normal rata dan batas yang benar") {
    WhiteboxMesh box = WhiteboxMesh::MakeCube();
    const sim::assets::MeshData data = box.BuildMeshData();

    // Simpul tidak dibagi antar sisi: enam quad, empat simpul masing-masing.
    // Simpul yang dibagi hanya bisa membawa satu normal, dan hasilnya rusuk yang
    // membulat — yang justru menghapus tampilan blockout.
    CHECK(data.vertices.size() == 6 * 4);

    // Kubus satuan berpusat di titik asal.
    CHECK(data.boundsMin.x == doctest::Approx(-0.5f));
    CHECK(data.boundsMax.y == doctest::Approx(0.5f));

    // Tiap normal adalah sumbu satuan: kubus tidak punya sisi miring.
    for (const sim::assets::MeshVertex& vertex : data.vertices) {
        const float length = glm::length(vertex.normal);
        CHECK(length == doctest::Approx(1.0f).epsilon(0.001));
        const float axis = std::max({std::abs(vertex.normal.x), std::abs(vertex.normal.y),
                                     std::abs(vertex.normal.z)});
        CHECK(axis == doctest::Approx(1.0f).epsilon(0.001));
    }
}

TEST_CASE("material bertahan melewati ekstrusi") {
    // **Sisi yang kehilangan materialnya sesudah didorong adalah kejutan yang
    // menyalahkan operasi dorongnya.** Poligon dikenali face terkecilnya, jadi
    // nomornya bisa bergeser saat topologinya berubah — materialnya harus ikut
    // dipindahkan, bukan ditinggalkan di nomor lama.
    WhiteboxMesh box = WhiteboxMesh::MakeCube();
    const std::vector<PolygonHandle> sides = box.Polygons().Polygons();
    for (std::size_t i = 0; i < sides.size(); ++i) {
        REQUIRE(box.SetPolygonMaterial(sides[i], static_cast<int>(i)));
    }
    CHECK(box.UsedMaterialCount() == 6);

    const PolygonHandle top =
        PolygonFacing(box.Mesh(), box.Polygons(), Vec3(0.0f, 1.0f, 0.0f));
    REQUIRE(IsValid(top));
    const int topMaterial = box.PolygonMaterial(top);

    const EditResult extruded = box.Extrude(top, 0.5f);
    INFO(extruded.error);
    REQUIRE(extruded.ok);

    // Enam sisi lama mempertahankan materialnya; empat dinding baru belum punya.
    CHECK(box.PolygonMaterial(extruded.polygon) == topMaterial);

    const sim::assets::MeshData data = box.BuildMeshData();
    // Enam material lama ditambah satu ruas -1 untuk dinding baru.
    INFO(data.parts.size() << " ruas");
    CHECK(data.parts.size() == 7);
    CHECK(data.parts[0].material == kNoMaterial);

    CHECK(box.CheckInvariants().ok);
}

TEST_CASE("simpan-muat-simpan menghasilkan byte yang sama") {
    // **Kriteria terima W4**, aturan yang sama dengan E3. Berkas yang isinya
    // bergeser tanpa ada yang menyuntingnya menghasilkan diff palsu di kontrol
    // versi, dan diff palsu membuat yang sungguhan tidak terbaca.
    WhiteboxMesh box = WhiteboxMesh::MakeTriangulatedCube();
    REQUIRE(box.MergeCoplanar() == 6);
    const std::vector<PolygonHandle> sides = box.Polygons().Polygons();
    REQUIRE(sides.size() == 6);
    for (std::size_t i = 0; i < sides.size(); ++i) {
        REQUIRE(box.SetPolygonMaterial(sides[i], static_cast<int>(i % 3)));
    }
    REQUIRE(box.Extrude(sides[0], 0.4f).ok);

    const std::string first = SaveToString(box);

    WhiteboxMesh loaded;
    const WhiteboxIoResult result = LoadFromString(loaded, first);
    INFO(result.error);
    REQUIRE(result.ok);

    const std::string second = SaveToString(loaded);
    CHECK(first == second);

    // Dan yang dimuat memang meshnya, bukan sekadar teks yang sama.
    CHECK(loaded.Mesh().FaceCount() == box.Mesh().FaceCount());
    CHECK(loaded.Mesh().VertexCount() == box.Mesh().VertexCount());
    CHECK(loaded.Polygons().PolygonCount() == box.Polygons().PolygonCount());
    CHECK(loaded.UsedMaterialCount() == box.UsedMaterialCount());
    CHECK(loaded.CheckInvariants().ok);
}

TEST_CASE("yang dimuat bisa disunting lagi persis seperti sebelum disimpan") {
    // **Kriteria terima W4, dan inilah gunanya menyimpan topologi.** Kalau yang
    // tersimpan segitiga, blockout berhenti bisa diubah begitu disimpan — persis
    // kebalikan dari gunanya whitebox.
    WhiteboxMesh original = WhiteboxMesh::MakeCube();
    const PolygonHandle top =
        PolygonFacing(original.Mesh(), original.Polygons(), Vec3(0.0f, 1.0f, 0.0f));
    REQUIRE(original.SetPolygonMaterial(top, 2));

    WhiteboxMesh loaded;
    REQUIRE(LoadFromString(loaded, SaveToString(original)).ok);

    // Ekstrusi yang sama dijalankan pada keduanya.
    const PolygonHandle originalTop =
        PolygonFacing(original.Mesh(), original.Polygons(), Vec3(0.0f, 1.0f, 0.0f));
    const PolygonHandle loadedTop =
        PolygonFacing(loaded.Mesh(), loaded.Polygons(), Vec3(0.0f, 1.0f, 0.0f));
    REQUIRE(original.Extrude(originalTop, 0.6f).ok);
    REQUIRE(loaded.Extrude(loadedTop, 0.6f).ok);

    // Hasilnya harus sama sampai ke berkasnya.
    CHECK(SaveToString(original) == SaveToString(loaded));
    CHECK(ClosedVolume(loaded.Mesh()) == doctest::Approx(ClosedVolume(original.Mesh())));
    CHECK(loaded.CheckInvariants().ok);

    // Termasuk materialnya, yang ikut melewati simpan-muat **dan** ekstrusi.
    CHECK(loaded.PolygonMaterial(loadedTop) == 2);
}

TEST_CASE("berkas whitebox yang rusak ditolak beserta sebabnya") {
    // Yang ditolak harus mengatakan apa yang salah. "Gagal memuat" pada berkas
    // berisi ratusan simpul adalah laporan yang tidak bisa ditindaklanjuti.
    WhiteboxMesh box;

    SUBCASE("bukan JSON") {
        const WhiteboxIoResult result = LoadFromString(box, "{ ini bukan json");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("JSON") != std::string::npos);
    }
    SUBCASE("versi dari masa depan") {
        const WhiteboxIoResult result = LoadFromString(box, R"({"schemaVersion": 99})");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("99") != std::string::npos);
    }
    SUBCASE("face menunjuk simpul yang tidak ada") {
        const WhiteboxIoResult result = LoadFromString(box, R"({
            "schemaVersion": 1,
            "vertices": [[0,0,0],[1,0,0],[0,1,0]],
            "faces": [[0,1,9]]
        })");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("simpul") != std::string::npos);
    }
    SUBCASE("rusuk tersembunyi yang simpulnya tidak bertetangga") {
        const WhiteboxIoResult result = LoadFromString(box, R"({
            "schemaVersion": 1,
            "vertices": [[0,0,0],[1,0,0],[0,1,0],[5,5,5]],
            "faces": [[0,1,2]],
            "hiddenEdges": [[0,3]]
        })");
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("bertetangga") != std::string::npos);
    }
}

TEST_CASE("berkas whitebox bolak-balik lewat disk") {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "sim-whitebox-test";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    const std::filesystem::path path = directory / "blok.simwhitebox";

    WhiteboxMesh box = WhiteboxMesh::MakeCube();
    const std::vector<PolygonHandle> sides = box.Polygons().Polygons();
    REQUIRE(box.SetPolygonMaterial(sides.front(), 7));
    REQUIRE(SaveToFile(box, path).ok);

    WhiteboxMesh loaded;
    const WhiteboxIoResult result = LoadFromFile(loaded, path);
    INFO(result.error);
    REQUIRE(result.ok);
    CHECK(loaded.Mesh().FaceCount() == 6);
    CHECK(loaded.UsedMaterialCount() == 2);  // slot 7 dan sisanya tanpa material

    std::filesystem::remove_all(directory, ec);
}

TEST_CASE("menunjuk sisi memilih poligon, bukan segitiga di bawah kursor") {
    // **Kriteria terima W5.** Pengguna menunjuk sebuah sisi; bahwa sisi itu
    // kebetulan tersusun dari dua segitiga adalah urusan mesin. Mengembalikan
    // segitiga berarti mendorong "sisi" hanya menggerakkan separuhnya.
    WhiteboxMesh box = WhiteboxMesh::MakeTriangulatedCube();
    REQUIRE(box.MergeCoplanar() == 6);
    REQUIRE(box.Polygons().PolygonCount() == 6);

    // Dua sinar yang mengenai dua segitiga **berbeda** pada sisi atas yang sama.
    // Diagonalnya membelah quad dari sudut ke sudut, jadi titik di kedua sisi
    // diagonal itu jatuh di segitiga yang berlainan.
    const PolygonHit a =
        PickPolygon(box, Vec3(-0.3f, 2.0f, -0.2f), Vec3(0.0f, -1.0f, 0.0f));
    const PolygonHit b =
        PickPolygon(box, Vec3(0.3f, 2.0f, 0.2f), Vec3(0.0f, -1.0f, 0.0f));
    REQUIRE(a.hit);
    REQUIRE(b.hit);

    INFO("face " << static_cast<uint32_t>(a.face) << " dan "
                 << static_cast<uint32_t>(b.face));
    CHECK(a.face != b.face);
    // **Tetapi poligonnya sama.** Inilah yang diuji.
    CHECK(a.polygon == b.polygon);

    // Dan poligon itu memang sisi atasnya.
    const Vec3 normal = box.Polygons().PolygonNormal(box.Mesh(), a.polygon);
    CHECK(normal.y == doctest::Approx(1.0f).epsilon(0.01));

    // Jaraknya terukur: dari y = 2 ke permukaan y = 0,5.
    CHECK(a.distance == doctest::Approx(1.5f).epsilon(0.01));
    CHECK(a.position.y == doctest::Approx(0.5f).epsilon(0.01));
}

TEST_CASE("sinar yang meleset dan yang menembus dari dalam") {
    WhiteboxMesh box = WhiteboxMesh::MakeCube();

    // Meleset di samping kubus.
    CHECK_FALSE(PickPolygon(box, Vec3(5.0f, 2.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f)).hit);
    // Arah bernorma nol ditolak alih-alih ditebak.
    CHECK_FALSE(PickPolygon(box, Vec3(0.0f, 2.0f, 0.0f), Vec3(0.0f)).hit);
    // Jangkauan yang lebih pendek daripada sasarannya tidak menjangkaunya.
    CHECK_FALSE(PickPolygon(box, Vec3(0.0f, 2.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), 1.0f).hit);

    // **Dari dalam ruangan, sisi belakang tetap bisa diklik.** Perancang kerap
    // bekerja dari dalam yang baru dibuatnya, dan dinding yang tidak bisa
    // dipilih dari dalam berarti dinding yang tidak bisa dipindahkan tanpa
    // memutar kamera keluar.
    const PolygonHit inside = PickPolygon(box, Vec3(0.0f), Vec3(0.0f, 1.0f, 0.0f));
    REQUIRE(inside.hit);
    CHECK(inside.distance == doctest::Approx(0.5f).epsilon(0.01));

    // Yang terdekat yang dipilih: dari luar, sisi depan mendahului sisi belakang.
    const PolygonHit outside =
        PickPolygon(box, Vec3(0.0f, 0.0f, 3.0f), Vec3(0.0f, 0.0f, -1.0f));
    REQUIRE(outside.hit);
    CHECK(outside.position.z == doctest::Approx(0.5f).epsilon(0.01));
}

// ============================================================================
// W5 — bentuk sisi untuk penyunting
// ============================================================================

TEST_CASE("batas sisi tidak memperlihatkan diagonal yang membelahnya") {
    // **Kriteria terima W5.** Seluruh lapisan poligon ada supaya perancang
    // melihat sisi, bukan segitiga. Sorotan yang menggambar seluruh rusuk face
    // membocorkan diagonal itu kembali ke layar — dan begitu terlihat, orang
    // akan mencoba mengkliknya.
    WhiteboxMesh box = WhiteboxMesh::MakeTriangulatedCube();
    REQUIRE(box.Mesh().FaceCount() == 12);
    REQUIRE(box.MergeCoplanar() == 6);

    const std::vector<PolygonHandle> sides = box.Polygons().Polygons();
    REQUIRE(sides.size() == 6);

    for (const PolygonHandle side : sides) {
        const PolygonOutline outline = BuildPolygonOutline(box, side);
        INFO("sisi " << static_cast<uint32_t>(side));
        // Dua segitiga isi, tetapi hanya empat rusuk batas: diagonalnya di
        // dalam poligon yang sama, jadi ia bukan batas.
        CHECK(outline.triangles.size() == 2);
        CHECK(outline.edges.size() == 4);
        CHECK(outline.area == doctest::Approx(1.0f).epsilon(0.001));

        // Titik beratnya di tengah sisi kubus satuan: satu koordinat ±0.5,
        // dua lainnya nol.
        const Vec3 c = outline.centroid;
        const float away = std::max({std::abs(c.x), std::abs(c.y), std::abs(c.z)});
        CHECK(away == doctest::Approx(0.5f).epsilon(0.001));
        CHECK(std::abs(c.x) + std::abs(c.y) + std::abs(c.z) ==
              doctest::Approx(0.5f).epsilon(0.001));

        // Normalnya sejajar salah satu sumbu dan bernorma satu.
        CHECK(glm::length(outline.normal) == doctest::Approx(1.0f).epsilon(0.001));
        CHECK(std::abs(glm::dot(outline.normal, glm::normalize(c))) ==
              doctest::Approx(1.0f).epsilon(0.001));
    }
}

TEST_CASE("titik berat sisi berbobot luas, bukan rata-rata simpul") {
    // Sebuah persegi panjang 4x1 yang terbelah tidak rata: petak 1x1 di kiri
    // dan petak 3x1 di kanan. Rata-rata keenam simpulnya jatuh di x = 5/3;
    // titik berat sebenarnya di x = 2. Gizmo berdiri di titik ini, dan yang
    // berdiri di 5/3 terlihat seperti ia memegang sisi yang lain.
    WhiteboxData data;
    data.positions = {
        Vec3(0.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), Vec3(4.0f, 0.0f, 0.0f),
        Vec3(4.0f, 0.0f, 1.0f), Vec3(1.0f, 0.0f, 1.0f), Vec3(0.0f, 0.0f, 1.0f),
    };
    data.faces = {{0, 1, 4, 5}, {1, 2, 3, 4}};
    data.faceMaterials = {kNoMaterial, kNoMaterial};

    WhiteboxMesh box;
    std::string error;
    REQUIRE_MESSAGE(WhiteboxMesh::Build(box, data, error), error);
    REQUIRE(box.MergeCoplanar() == 1);

    const std::vector<PolygonHandle> sides = box.Polygons().Polygons();
    REQUIRE(sides.size() == 1);

    const PolygonOutline outline = BuildPolygonOutline(box, sides.front());
    CHECK(outline.area == doctest::Approx(4.0f).epsilon(0.001));
    CHECK(outline.centroid.x == doctest::Approx(2.0f).epsilon(0.001));
    CHECK(outline.centroid.z == doctest::Approx(0.5f).epsilon(0.001));
    // Bukan rata-rata simpul, yang akan menjawab 5/3.
    CHECK(outline.centroid.x != doctest::Approx(5.0f / 3.0f).epsilon(0.001));

    // Enam ruas batas, bukan delapan: rusuk yang dipakai bersama kedua petak
    // tidak ikut. Bukan empat, karena dua sisi panjangnya memang terbagi dua
    // oleh simpul di tengah — dan menggambarnya sebagai satu garis lurus berarti
    // menyembunyikan simpul yang sungguh ada di sana.
    CHECK(outline.edges.size() == 6);

    float perimeter = 0.0f;
    for (const auto& [from, to] : outline.edges) {
        perimeter += glm::length(to - from);
    }
    CHECK(perimeter == doctest::Approx(10.0f).epsilon(0.001));
}

TEST_CASE("sisi yang tidak ada tidak menghasilkan bentuk") {
    // Penyunting menanyakan bentuk sisi tiap frame, termasuk pada frame ketika
    // belum ada yang terpilih. Menjawabnya dengan bentuk kosong jauh lebih baik
    // daripada memaksa setiap pemanggil memeriksa dulu — yang lupa memeriksa
    // akan menggambar sampah alih-alih tidak menggambar apa-apa.
    WhiteboxMesh box = WhiteboxMesh::MakeCube();
    CHECK(BuildPolygonOutline(box, PolygonHandle::Invalid).empty());
    CHECK(BuildPolygonOutline(box, static_cast<PolygonHandle>(999)).empty());
}
