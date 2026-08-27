#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "Sim/Core/Intersect.h"
#include "Sim/Raycast/Backend.h"
#include "Sim/Raycast/Query.h"
#include "Sim/Raycast/RayScene.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace sim;
using namespace sim::raycast;

namespace {

/// Kubus satuan berpusat di titik asal, dua belas segitiga.
struct Cube {
    std::vector<Vec3> positions;
    std::vector<uint32_t> indices;
};

Cube MakeCube(float halfSize = 0.5f) {
    Cube cube;
    const float h = halfSize;
    cube.positions = {
        Vec3(-h, -h, -h), Vec3(h, -h, -h), Vec3(h, h, -h), Vec3(-h, h, -h),
        Vec3(-h, -h, h),  Vec3(h, -h, h),  Vec3(h, h, h),  Vec3(-h, h, h),
    };
    cube.indices = {
        0, 2, 1, 0, 3, 2,  // −Z
        4, 5, 6, 4, 6, 7,  // +Z
        0, 1, 5, 0, 5, 4,  // −Y
        3, 7, 6, 3, 6, 2,  // +Y
        0, 4, 7, 0, 7, 3,  // −X
        1, 2, 6, 1, 6, 5,  // +X
    };
    return cube;
}

/// Segitiga tunggal di bidang z = 0.
struct OneTriangle {
    std::vector<Vec3> positions{Vec3(0.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f),
                                Vec3(0.0f, 1.0f, 0.0f)};
    std::vector<uint32_t> indices{0, 1, 2};
};

}  // namespace

TEST_CASE("Möller–Trumbore menjawab tiga kasus dasar") {
    const Vec3 a(0.0f, 0.0f, 0.0f);
    const Vec3 b(1.0f, 0.0f, 0.0f);
    const Vec3 c(0.0f, 1.0f, 0.0f);

    SUBCASE("kena, dan barycentric-nya menunjuk titik yang benar") {
        const TriangleHit hit =
            RayTriangle(Vec3(0.25f, 0.25f, -1.0f), Vec3(0.0f, 0.0f, 1.0f), a, b, c);
        REQUIRE(hit);
        CHECK(hit.distance == doctest::Approx(1.0f));
        CHECK(hit.barycentric.x == doctest::Approx(0.25f));
        CHECK(hit.barycentric.y == doctest::Approx(0.25f));

        // Titik yang direkonstruksi dari barycentric harus sama dengan titik
        // yang direkonstruksi dari jarak. Kalau tidak, salah satunya bohong.
        const Vec3 fromBary = a + hit.barycentric.x * (b - a) + hit.barycentric.y * (c - a);
        const Vec3 fromDistance = Vec3(0.25f, 0.25f, -1.0f) + Vec3(0.0f, 0.0f, 1.0f) * hit.distance;
        CHECK(glm::length(fromBary - fromDistance) == doctest::Approx(0.0f).epsilon(1e-5));
    }

    SUBCASE("meleset di luar rusuk") {
        CHECK_FALSE(RayTriangle(Vec3(0.8f, 0.8f, -1.0f), Vec3(0.0f, 0.0f, 1.0f), a, b, c));
    }

    SUBCASE("sejajar bidang segitiga") {
        CHECK_FALSE(RayTriangle(Vec3(0.25f, 0.25f, -1.0f), Vec3(1.0f, 0.0f, 0.0f), a, b, c));
    }

    SUBCASE("di belakang titik asal sinar") {
        CHECK_FALSE(RayTriangle(Vec3(0.25f, 0.25f, 1.0f), Vec3(0.0f, 0.0f, 1.0f), a, b, c));
    }

    SUBCASE("sisi belakang tetap diterima") {
        // Disengaja: lihat catatannya di `Sim/Core/Intersect.h`. Perancang yang
        // berdiri di dalam ruangan harus tetap bisa mengklik dindingnya.
        CHECK(RayTriangle(Vec3(0.25f, 0.25f, 1.0f), Vec3(0.0f, 0.0f, -1.0f), a, b, c));
    }

    SUBCASE("maxDistance menolak yang lebih jauh") {
        CHECK_FALSE(RayTriangle(Vec3(0.25f, 0.25f, -10.0f), Vec3(0.0f, 0.0f, 1.0f), a, b, c,
                                /*maxDistance=*/5.0f));
    }
}

TEST_CASE("Scene kosong dan yang belum di-commit tidak menjawab kena") {
    const OneTriangle triangle;
    RayScene scene;

    CHECK_FALSE(Raycast(scene, Vec3(0.25f, 0.25f, -1.0f), Vec3(0.0f, 0.0f, 1.0f)));

    const GeometryId geometry = scene.AddMesh(triangle.positions, triangle.indices);
    REQUIRE(geometry != GeometryId::Invalid);
    scene.AddInstance(geometry, Mat4(1.0f));

    // **Belum di-commit**, jadi tingkat atasnya belum ada. Menjawab "kena" di
    // sini akan menyembunyikan commit yang lupa dipanggil sampai adegan tumbuh.
    CHECK_FALSE(scene.IsCommitted());
    CHECK_FALSE(Raycast(scene, Vec3(0.25f, 0.25f, -1.0f), Vec3(0.0f, 0.0f, 1.0f)));

    scene.Commit();
    CHECK(scene.IsCommitted());
    CHECK(Raycast(scene, Vec3(0.25f, 0.25f, -1.0f), Vec3(0.0f, 0.0f, 1.0f)));
}

TEST_CASE("Geometri ditolak kalau indeksnya tidak masuk akal") {
    const OneTriangle triangle;
    RayScene scene;

    const std::vector<uint32_t> outOfRange{0, 1, 99};
    CHECK(scene.AddMesh(triangle.positions, outOfRange) == GeometryId::Invalid);

    const std::vector<uint32_t> notMultipleOfThree{0, 1};
    CHECK(scene.AddMesh(triangle.positions, notMultipleOfThree) == GeometryId::Invalid);

    CHECK(scene.AddMesh(triangle.positions, {}) == GeometryId::Invalid);
    CHECK(scene.GeometryCount() == 0);
}

TEST_CASE("Posisi dibaca dari buffer interleaved tanpa repack") {
    // Meniru `assets::MeshVertex`: posisi di offset nol, stride 32.
    struct Vertex {
        Vec3 position;
        Vec3 normal;
        Vec2 uv;
    };
    static_assert(sizeof(Vertex) == 32, "uji ini menguji stride 32");

    std::vector<Vertex> vertices{
        {Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, -1.0f), Vec2(0.0f)},
        {Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, -1.0f), Vec2(1.0f, 0.0f)},
        {Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 0.0f, -1.0f), Vec2(0.0f, 1.0f)},
    };
    const std::vector<uint32_t> indices{0, 1, 2};

    RayScene scene;
    const GeometryId geometry =
        scene.AddMesh(&vertices[0].position, sizeof(Vertex), vertices.size(), indices);
    REQUIRE(geometry != GeometryId::Invalid);
    scene.AddInstance(geometry, Mat4(1.0f));
    scene.Commit();

    CHECK(Raycast(scene, Vec3(0.25f, 0.25f, -1.0f), Vec3(0.0f, 0.0f, 1.0f)));

    // **Buffer dibagi, bukan disalin.** Menggeser simpulnya di tempat harus
    // menggeser geometrinya juga — kalau tidak, ada salinan kedua di suatu
    // tempat, dan adegan sebesar Sponza akan membayarnya dua kali.
    for (Vertex& vertex : vertices) {
        vertex.position.z += 10.0f;
    }
    CHECK_FALSE(Raycast(scene, Vec3(0.25f, 0.25f, -1.0f), Vec3(0.0f, 0.0f, 1.0f), 5.0f));
    CHECK(Raycast(scene, Vec3(0.25f, 0.25f, -1.0f), Vec3(0.0f, 0.0f, 1.0f), 20.0f));
}

TEST_CASE("Klik menembus lubang tidak memilih bendanya") {
    // Bingkai persegi: empat sisi, lubang di tengah. Inilah bentuk yang uji AABB
    // salah jawab — dan alasan R2 ada.
    std::vector<Vec3> positions;
    std::vector<uint32_t> indices;
    const auto quad = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
        const auto base = static_cast<uint32_t>(positions.size());
        positions.insert(positions.end(), {a, b, c, d});
        indices.insert(indices.end(), {base, base + 1u, base + 2u, base, base + 2u, base + 3u});
    };
    // Empat batang mengelilingi lubang [-0.5, 0.5]², bingkai luar [-2, 2]².
    quad(Vec3(-2, 1, 0), Vec3(2, 1, 0), Vec3(2, 2, 0), Vec3(-2, 2, 0));
    quad(Vec3(-2, -2, 0), Vec3(2, -2, 0), Vec3(2, -1, 0), Vec3(-2, -1, 0));
    quad(Vec3(-2, -1, 0), Vec3(-1, -1, 0), Vec3(-1, 1, 0), Vec3(-2, 1, 0));
    quad(Vec3(1, -1, 0), Vec3(2, -1, 0), Vec3(2, 1, 0), Vec3(1, 1, 0));

    RayScene scene;
    const GeometryId geometry = scene.AddMesh(positions, indices);
    REQUIRE(geometry != GeometryId::Invalid);
    scene.AddInstance(geometry, Mat4(1.0f), /*userData=*/7);
    scene.Commit();

    // Lewat lubangnya: AABB akan menjawab "kena", segitiga menjawab "tidak".
    CHECK_FALSE(Raycast(scene, Vec3(0.0f, 0.0f, -1.0f), Vec3(0.0f, 0.0f, 1.0f)));
    // Lewat batangnya: kena.
    const RayHit hit = Raycast(scene, Vec3(0.0f, 1.5f, -1.0f), Vec3(0.0f, 0.0f, 1.0f));
    REQUIRE(hit);
    CHECK(hit.userData == 7);
    CHECK(hit.distance == doctest::Approx(1.0f));
}

TEST_CASE("Tiga mesh dikali seratus instance memetakan ke instance yang benar") {
    const Cube cube = MakeCube();
    const OneTriangle triangle;
    const Cube big = MakeCube(0.25f);

    RayScene scene;
    const std::array<GeometryId, 3> geometries{
        scene.AddMesh(cube.positions, cube.indices),
        scene.AddMesh(triangle.positions, triangle.indices),
        scene.AddMesh(big.positions, big.indices),
    };
    for (const GeometryId geometry : geometries) {
        REQUIRE(geometry != GeometryId::Invalid);
    }

    // 3 × 100 instance berjajar di sumbu X, jarak 4 m.
    for (int i = 0; i < 300; ++i) {
        const Mat4 transform =
            glm::translate(Mat4(1.0f), Vec3(static_cast<float>(i) * 4.0f, 0.0f, 0.0f));
        scene.AddInstance(geometries[static_cast<std::size_t>(i % 3)], transform,
                          static_cast<uint64_t>(1000 + i));
    }
    scene.Commit();
    CHECK(scene.InstanceCount() == 300);
    CHECK(scene.GeometryCount() == 3);

    // Instance 150 memakai geometri 0 (kubus) karena 150 % 3 == 0.
    const RayHit hit = Raycast(scene, Vec3(150.0f * 4.0f, 0.0f, -10.0f), Vec3(0.0f, 0.0f, 1.0f));
    REQUIRE(hit);
    CHECK(hit.userData == 1150);
    CHECK(static_cast<uint32_t>(hit.instance) == 150);
    CHECK(hit.distance == doctest::Approx(9.5f));
    // Normal menghadap ke arah datangnya sinar, bukan menjauh.
    CHECK(hit.normal.z == doctest::Approx(-1.0f));

    // Yang terdekat menang, bukan yang pertama ditemukan penelusuran.
    const RayHit fromLeft = Raycast(scene, Vec3(-10.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f));
    REQUIRE(fromLeft);
    CHECK(fromLeft.userData == 1000);
}

TEST_CASE("Memindahkan instance tidak membangun ulang BVH geometrinya") {
    const Cube cube = MakeCube();
    RayScene scene;
    const GeometryId geometry = scene.AddMesh(cube.positions, cube.indices);
    const InstanceId instance = scene.AddInstance(geometry, Mat4(1.0f), 42);
    scene.Commit();

    CHECK(Raycast(scene, Vec3(0.0f, 0.0f, -5.0f), Vec3(0.0f, 0.0f, 1.0f)));

    scene.SetInstanceTransform(instance, glm::translate(Mat4(1.0f), Vec3(100.0f, 0.0f, 0.0f)));
    CHECK_FALSE(scene.IsCommitted());
    scene.Commit();

    CHECK_FALSE(Raycast(scene, Vec3(0.0f, 0.0f, -5.0f), Vec3(0.0f, 0.0f, 1.0f)));
    const RayHit moved = Raycast(scene, Vec3(100.0f, 0.0f, -5.0f), Vec3(0.0f, 0.0f, 1.0f));
    REQUIRE(moved);
    CHECK(moved.userData == 42);
}

TEST_CASE("Skala instance tidak merusak perbandingan jarak") {
    const Cube cube = MakeCube();
    RayScene scene;
    const GeometryId geometry = scene.AddMesh(cube.positions, cube.indices);

    // Kubus kecil di depan, kubus besar di belakang. Yang di depan harus menang
    // walaupun `t` di ruang lokalnya jauh lebih besar karena skalanya kecil.
    scene.AddInstance(geometry, glm::scale(glm::translate(Mat4(1.0f), Vec3(0.0f, 0.0f, 0.0f)),
                                           Vec3(0.1f)),
                      /*userData=*/1);
    scene.AddInstance(geometry, glm::scale(glm::translate(Mat4(1.0f), Vec3(0.0f, 0.0f, 5.0f)),
                                           Vec3(3.0f)),
                      /*userData=*/2);
    scene.Commit();

    const RayHit hit = Raycast(scene, Vec3(0.0f, 0.0f, -10.0f), Vec3(0.0f, 0.0f, 1.0f));
    REQUIRE(hit);
    CHECK(hit.userData == 1);
    CHECK(hit.distance == doctest::Approx(9.95f));
}

TEST_CASE("Occluded berhenti pada yang pertama, dan menghormati batasnya") {
    const Cube cube = MakeCube();
    RayScene scene;
    const GeometryId geometry = scene.AddMesh(cube.positions, cube.indices);
    scene.AddInstance(geometry, glm::translate(Mat4(1.0f), Vec3(0.0f, 0.0f, 5.0f)));
    scene.Commit();

    CHECK(Occluded(scene, Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f), 10.0f));
    // Berhenti sebelum kubusnya: tidak terhalang.
    CHECK_FALSE(Occluded(scene, Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f), 4.0f));
    // Arah sebaliknya: kosong.
    CHECK_FALSE(Occluded(scene, Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, -1.0f), 10.0f));
}

TEST_CASE("Titik terdekat menemukan permukaan, bukan pusat bendanya") {
    const Cube cube = MakeCube();
    RayScene scene;
    const GeometryId geometry = scene.AddMesh(cube.positions, cube.indices);
    scene.AddInstance(geometry, Mat4(1.0f), /*userData=*/9);
    scene.AddInstance(geometry, glm::translate(Mat4(1.0f), Vec3(20.0f, 0.0f, 0.0f)),
                      /*userData=*/10);
    scene.Commit();

    const ClosestPoint nearest = FindClosestPoint(scene, Vec3(3.0f, 0.0f, 0.0f), 10.0f);
    REQUIRE(nearest);
    CHECK(nearest.userData == 9);
    CHECK(nearest.distance == doctest::Approx(2.5f));
    CHECK(nearest.position.x == doctest::Approx(0.5f));

    // Di luar jangkauan: tidak ditemukan, bukan dikembalikan yang terjauh.
    CHECK_FALSE(FindClosestPoint(scene, Vec3(1000.0f, 0.0f, 0.0f), 1.0f));

    // Yang di dekat instance kedua memilih instance kedua.
    const ClosestPoint other = FindClosestPoint(scene, Vec3(19.0f, 0.0f, 0.0f), 10.0f);
    REQUIRE(other);
    CHECK(other.userData == 10);
}

TEST_CASE("Backend yang dipakai build ini bisa disebut namanya") {
    CHECK(SelectedBackend() == BackendKind::Bvh);
    CHECK(std::string(ToString(BackendKind::Bvh)) == "BVH");
    CHECK(std::string(ToString(BackendKind::Embree)) == "Embree");
}

TEST_CASE("Möller–Trumbore hanya punya satu salinan di Code/") {
    // **Kriteria terima R0, ditegakkan alih-alih diingat.** Rumus perpotongan
    // yang punya dua salinan adalah dua salinan yang suatu saat berselisih satu
    // epsilon — dan selisih itu muncul sebagai klik yang memilih benda berbeda
    // dari yang ditembak baker, bukan sebagai galat kompilasi.
    //
    // Yang dicari tanda tangan rumusnya, bukan namanya: sebuah salinan yang
    // diberi nama lain tetap salinan.
    std::vector<std::string> offenders;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(SIM_CODE_DIR)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::filesystem::path& path = entry.path();
        if (path.extension() != ".cpp" && path.extension() != ".h") {
            continue;
        }
        std::ifstream file(path);
        const std::string text((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
        // Inti Möller–Trumbore: silang arah dengan rusuk kedua, lalu titik
        // rusuk pertama dengan hasilnya sebagai determinan.
        if (text.find("glm::cross(direction, edge2)") == std::string::npos) {
            continue;
        }
        if (path.filename() == "Intersect.h") {
            continue;  // rumahnya
        }
        offenders.push_back(path.filename().string());
    }
    CHECK(offenders.empty());
}

TEST_CASE("ClearInstances mempertahankan geometri beserta BVH-nya") {
    // **Kriteria R1.** `SceneView` menyusun ulang daftar isinya tiap frame;
    // kalau itu berarti membangun ulang BVH tiap mesh, Sponza membayar
    // pembangunan BVH enam puluh kali per detik untuk geometri yang tidak
    // berubah sama sekali.
    const Cube cube = MakeCube();
    RayScene scene;
    const GeometryId geometry = scene.AddMesh(cube.positions, cube.indices);
    REQUIRE(geometry != GeometryId::Invalid);

    for (int i = 0; i < 3; ++i) {
        scene.AddInstance(geometry, glm::translate(Mat4(1.0f), Vec3(i * 4.0f, 0.0f, 0.0f)),
                          static_cast<uint64_t>(i));
    }
    scene.Commit();
    REQUIRE(scene.InstanceCount() == 3);
    REQUIRE(scene.GeometryCount() == 1);
    const std::size_t triangles = scene.TriangleCount();

    scene.ClearInstances();
    CHECK(scene.InstanceCount() == 0);
    // Geometrinya tetap — itulah seluruh gunanya.
    CHECK(scene.GeometryCount() == 1);
    CHECK(scene.TriangleCount() == triangles);
    // Dan belum di-commit, jadi query tidak menjawab kena.
    CHECK_FALSE(scene.IsCommitted());

    // Handle geometrinya masih sah: instance baru boleh memakainya kembali.
    scene.AddInstance(geometry, glm::translate(Mat4(1.0f), Vec3(0.0f, 0.0f, 0.0f)),
                      /*userData=*/99);
    scene.Commit();
    const RayHit hit = Raycast(scene, Vec3(0.0f, 0.0f, -5.0f), Vec3(0.0f, 0.0f, 1.0f));
    REQUIRE(hit);
    CHECK(hit.userData == 99);
}

TEST_CASE("Clear membuang geometrinya juga") {
    const Cube cube = MakeCube();
    RayScene scene;
    scene.AddInstance(scene.AddMesh(cube.positions, cube.indices), Mat4(1.0f));
    scene.Commit();
    REQUIRE(scene.GeometryCount() == 1);

    scene.Clear();
    CHECK(scene.GeometryCount() == 0);
    CHECK(scene.InstanceCount() == 0);
    CHECK(scene.TriangleCount() == 0);
}
