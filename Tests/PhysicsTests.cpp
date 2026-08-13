#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Core/TaskPool.h"
#include "Sim/Physics/PhysicsWorld.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace sim;
using namespace sim::physics;

namespace {

/// Dunia dengan lantai bidang tak hingga, siap menjatuhkan sesuatu ke atasnya.
WorldDesc DefaultWorld() {
    WorldDesc desc;
    // Nol worker: jumlah thread ikut menentukan hasil simulasi, jadi uji
    // determinisme harus menyebutnya alih-alih menerima bawaan mesin.
    desc.workerThreads = 0;
    return desc;
}

BodyHandle AddGroundPlane(PhysicsWorld& world) {
    BodyDesc ground;
    ground.kind = BodyKind::Static;
    ground.shape.kind = ShapeKind::Plane;
    // Bidang PhysX menghadap +X pada rotasi identitas; diputar +90° terhadap Z
    // supaya normalnya menunjuk +Y. Tandanya penting dan tidak kentara: dengan
    // -90° normalnya menunjuk ke bawah, dan yang terlihat bukan galat melainkan
    // benda yang menembus lantai — gejala yang mudah dikira bug solver.
    const float halfAngle = 0.25f * 3.14159265f;
    ground.rotation = Quat(std::cos(halfAngle), 0.0f, 0.0f, std::sin(halfAngle));
    return world.AddBody(ground);
}

}  // namespace

TEST_CASE("tanpa PhysX, dunia menolak dengan pesan alih-alih diam") {
    if (Available()) {
        CHECK(std::string(BackendVersion()).rfind("5.", 0) == 0);
        return;
    }
    // **Benda diam yang seharusnya jatuh terbaca sebagai bug fisika**, bukan
    // sebagai pustaka yang tidak dipasang. Jadi build tanpa PhysX harus
    // mengatakannya.
    PhysicsWorld world;
    CHECK_FALSE(world.Create(DefaultWorld()));
    CHECK_FALSE(world.IsValid());
    CHECK(world.Error().find("PhysX") != std::string::npos);
    CHECK(world.AddBody(BodyDesc{}) == BodyHandle::Invalid);
    CHECK(world.Advance(1.0f / 60.0f) == 0);
    CHECK(GpuAvailable() == false);
}

TEST_CASE("dunia dibuat dan dihancurkan tanpa menggantung") {
    if (!Available()) {
        return;
    }
    PhysicsWorld world;
    REQUIRE(world.Create(DefaultWorld()));
    CHECK(world.IsValid());
    CHECK(world.BodyCount() == 0);
    CHECK(world.StepCount() == 0);

    world.Destroy();
    CHECK_FALSE(world.IsValid());

    // Dibuat ulang di objek yang sama: lifecycle yang hanya benar sekali adalah
    // lifecycle yang gagal saat level kedua dimuat.
    REQUIRE(world.Create(DefaultWorld()));
    CHECK(world.IsValid());
}

TEST_CASE("pengaturan yang tidak masuk akal ditolak dengan sebabnya") {
    if (!Available()) {
        return;
    }
    PhysicsWorld world;

    SUBCASE("langkah nol") {
        WorldDesc desc = DefaultWorld();
        desc.fixedTimeStep = 0.0f;
        CHECK_FALSE(world.Create(desc));
        CHECK(world.Error().find("time step") != std::string::npos);
    }

    SUBCASE("skala nol") {
        WorldDesc desc = DefaultWorld();
        desc.typicalLength = 0.0f;
        CHECK_FALSE(world.Create(desc));
        // Skala yang salah tidak muncul sebagai galat melainkan sebagai benda
        // yang bergetar; ditolak di depan supaya tidak perlu ditebak nanti.
        CHECK(world.Error().find("typical") != std::string::npos);
    }
}

TEST_CASE("bola jatuh dan berhenti di ketinggian yang dihitung") {
    if (!Available()) {
        return;
    }
    // **Kriteria terima P1 dalam bentuk paling kecilnya.** Bukan "benda
    // bergerak ke bawah" — bola berjari-jari r yang diam di atas lantai y = 0
    // berhenti dengan pusatnya di y = r, dan angka itu diketahui sebelum
    // simulasinya dijalankan.
    PhysicsWorld world;
    REQUIRE(world.Create(DefaultWorld()));
    REQUIRE(AddGroundPlane(world) != BodyHandle::Invalid);

    const float radius = 0.5f;
    BodyDesc ball;
    ball.kind = BodyKind::Dynamic;
    ball.shape.kind = ShapeKind::Sphere;
    ball.shape.radius = radius;
    ball.position = Vec3(0.0f, 5.0f, 0.0f);
    const BodyHandle handle = world.AddBody(ball);
    REQUIRE(handle != BodyHandle::Invalid);
    CHECK(world.BodyCount() == 2);

    // Tiga detik pada 60 Hz: cukup untuk jatuh 5 m dan berhenti memantul.
    world.Step(180);
    CHECK(world.StepCount() == 180);

    BodyState state;
    REQUIRE(world.ReadState(handle, state));
    INFO("berhenti di y = " << state.position.y);
    CHECK(state.position.y == doctest::Approx(radius).epsilon(0.05));
    // Dan benar-benar diam, bukan sedang melintas.
    CHECK(std::abs(state.linearVelocity.y) < 0.1f);
}

TEST_CASE("simulasi yang sama menghasilkan hasil yang sama") {
    if (!Available()) {
        return;
    }
    // Determinisme yang dijanjikan dibatasi: build, platform, dan jumlah thread
    // yang sama. Uji ini menegakkan batas itu — dua kali jalan di proses yang
    // sama dengan `workerThreads = 0`.
    const auto run = [] {
        PhysicsWorld world;
        world.Create(DefaultWorld());
        AddGroundPlane(world);
        std::vector<BodyHandle> handles;
        for (int i = 0; i < 8; ++i) {
            BodyDesc box;
            box.shape.kind = ShapeKind::Box;
            box.shape.halfExtents = Vec3(0.25f);
            // Sedikit tidak sejajar supaya tumpukannya benar-benar berinteraksi
            // alih-alih jatuh lurus tanpa menyentuh satu sama lain.
            box.position = Vec3(static_cast<float>(i) * 0.01f, 0.6f + static_cast<float>(i) * 0.6f,
                                static_cast<float>(i) * 0.01f);
            handles.push_back(world.AddBody(box));
        }
        world.Step(240);
        std::vector<Vec3> result;
        for (const BodyHandle handle : handles) {
            BodyState state;
            world.ReadState(handle, state);
            result.push_back(state.position);
        }
        return result;
    };

    const std::vector<Vec3> first = run();
    const std::vector<Vec3> second = run();
    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        INFO("balok " << i);
        CHECK(first[i].x == doctest::Approx(second[i].x));
        CHECK(first[i].y == doctest::Approx(second[i].y));
        CHECK(first[i].z == doctest::Approx(second[i].z));
    }
}

TEST_CASE("langkah tetap tidak bergantung pada waktu frame") {
    if (!Available()) {
        return;
    }
    // **Inilah sifat yang membuat "menara balok saya runtuh di laptop tapi
    // tidak di desktop" tidak terjadi** — tetapi bentuk persisnya perlu
    // dinyatakan dengan hati-hati, karena versi yang berlebihan dari klaim ini
    // tidak benar.
    //
    // Yang dijamin: banyaknya langkah untuk rentang waktu yang sama berbeda
    // **paling banyak satu**, dan keadaan yang sudah mengendap sama persis.
    // Yang tidak dijamin: jumlah langkah yang identik. Satu detik sebagai
    // 20 frame @50 ms memang hanya 59,999998 langkah panjangnya sementara
    // sebagai 60 frame @16,67 ms tepat 60 — jadi 59 lawan 60 adalah jawaban
    // yang benar, bukan penyimpangan yang perlu ditoleransi.
    const auto simulate = [](const std::vector<float>& frames) {
        PhysicsWorld world;
        world.Create(DefaultWorld());
        AddGroundPlane(world);
        BodyDesc ball;
        ball.shape.kind = ShapeKind::Sphere;
        ball.shape.radius = 0.5f;
        ball.position = Vec3(0.0f, 3.0f, 0.0f);
        const BodyHandle handle = world.AddBody(ball);
        for (const float frame : frames) {
            world.Advance(frame);
        }
        BodyState state;
        world.ReadState(handle, state);
        return std::pair<Vec3, uint64_t>{state.position, world.StepCount()};
    };

    // 60 frame mulus vs 20 frame tersendat — total waktu sama persis.
    const std::vector<float> smooth(60, 1.0f / 60.0f);
    std::vector<float> uneven;
    for (int i = 0; i < 20; ++i) {
        uneven.push_back(3.0f / 60.0f);
    }

    const auto [smoothPosition, smoothSteps] = simulate(smooth);
    const auto [unevenPosition, unevenSteps] = simulate(uneven);

    INFO("mulus " << smoothSteps << " langkah, tersendat " << unevenSteps);
    const int64_t difference = static_cast<int64_t>(smoothSteps) - static_cast<int64_t>(unevenSteps);
    CHECK(std::abs(difference) <= 1);

    // Satu detik cukup untuk bola itu mendarat dan diam. Keadaan yang sudah
    // mengendap tidak menyimpan sisa selisih satu langkah tadi — dan justru
    // itulah yang ditanyakan pemain: apakah tumpukannya berakhir sama.
    CHECK(smoothPosition.y == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(unevenPosition.y == doctest::Approx(smoothPosition.y).epsilon(0.001));
}

TEST_CASE("selisih satu langkah tidak menumpuk sepanjang simulasi") {
    if (!Available()) {
        return;
    }
    // Batas "paling banyak satu langkah" hanya berguna kalau ia bertahan. Kalau
    // selisihnya bertambah setiap detik, ia bukan batas melainkan penundaan —
    // maka yang diuji di sini adalah sepuluh detik, bukan satu.
    const auto stepsFor = [](float frameTime, int frameCount) {
        PhysicsWorld world;
        world.Create(DefaultWorld());
        for (int i = 0; i < frameCount; ++i) {
            world.Advance(frameTime);
        }
        return world.StepCount();
    };

    const uint64_t smooth = stepsFor(1.0f / 60.0f, 600);
    const uint64_t uneven = stepsFor(3.0f / 60.0f, 200);
    const uint64_t coarse = stepsFor(1.0f / 30.0f, 300);

    INFO("10 detik: mulus " << smooth << ", tersendat " << uneven << ", kasar " << coarse);
    CHECK(std::abs(static_cast<int64_t>(smooth) - static_cast<int64_t>(uneven)) <= 1);
    CHECK(std::abs(static_cast<int64_t>(smooth) - static_cast<int64_t>(coarse)) <= 1);
}

TEST_CASE("benda kinematik digerakkan transform, bukan gaya") {
    if (!Available()) {
        return;
    }
    PhysicsWorld world;
    REQUIRE(world.Create(DefaultWorld()));

    BodyDesc platform;
    platform.kind = BodyKind::Kinematic;
    platform.shape.halfExtents = Vec3(1.0f, 0.1f, 1.0f);
    platform.position = Vec3(0.0f, 1.0f, 0.0f);
    const BodyHandle handle = world.AddBody(platform);
    REQUIRE(handle != BodyHandle::Invalid);

    // Gravitasi tidak boleh menyentuhnya sama sekali.
    world.Step(60);
    BodyState state;
    REQUIRE(world.ReadState(handle, state));
    CHECK(state.position.y == doctest::Approx(1.0f).epsilon(0.001));

    REQUIRE(world.MoveKinematic(handle, Vec3(0.0f, 2.0f, 0.0f), Quat(1.0f, 0.0f, 0.0f, 0.0f)));
    world.Step(1);
    REQUIRE(world.ReadState(handle, state));
    CHECK(state.position.y == doctest::Approx(2.0f).epsilon(0.001));

    // Dan benda yang bukan kinematik menolak perintah itu, bukan diam-diam
    // mengabaikannya.
    BodyDesc dynamic;
    dynamic.shape.kind = ShapeKind::Sphere;
    const BodyHandle other = world.AddBody(dynamic);
    CHECK_FALSE(world.MoveKinematic(other, Vec3(0.0f), Quat(1.0f, 0.0f, 0.0f, 0.0f)));
}

TEST_CASE("handle yang sudah dihapus ditolak, bukan dibaca") {
    if (!Available()) {
        return;
    }
    PhysicsWorld world;
    REQUIRE(world.Create(DefaultWorld()));
    BodyDesc box;
    const BodyHandle handle = world.AddBody(box);
    REQUIRE(world.IsAlive(handle));

    world.RemoveBody(handle);
    CHECK_FALSE(world.IsAlive(handle));
    CHECK(world.BodyCount() == 0);

    // **Handle, bukan pointer**, dan inilah gunanya: yang basi dijawab
    // penolakan alih-alih alamat yang kebetulan masih terbaca.
    BodyState state;
    CHECK_FALSE(world.ReadState(handle, state));
    CHECK_FALSE(world.Teleport(handle, Vec3(0.0f), Quat(1.0f, 0.0f, 0.0f, 0.0f)));
    // Menghapus dua kali tidak merusak apa pun.
    world.RemoveBody(handle);
}

TEST_CASE("tipe PhysX tidak bocor ke luar TU backend") {
    // Kriteria terima P0, diuji dengan menyisir berkas — bukan dengan disiplin.
    // Bentuk yang sama dengan uji `stbi_` di SimImageIOTests, dan alasannya
    // sama: tipe pustaka yang bocor ke header publik akan menyebar ke setiap
    // pemanggil, dan menariknya kembali sesudah lima modul memakainya jauh
    // lebih mahal daripada menjaganya sejak awal.
    const std::filesystem::path codeDir = std::filesystem::path(SIM_CODE_DIR);
    REQUIRE(std::filesystem::is_directory(codeDir));

    const std::string allowed = "PhysicsWorld.cpp";
    std::vector<std::string> offenders;

    for (const std::filesystem::directory_entry& entry :
         std::filesystem::recursive_directory_iterator(codeDir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::filesystem::path& path = entry.path();
        const std::string extension = path.extension().string();
        if (extension != ".cpp" && extension != ".h") {
            continue;
        }
        if (path.filename().string() == allowed) {
            continue;
        }

        std::ifstream stream(path);
        std::string line;
        while (std::getline(stream, line)) {
            // Komentar dibuang lebih dulu: `PhysicsTypes.h` menyebut
            // `PxRigidActor*` di komentarnya untuk menerangkan apa yang **tidak**
            // boleh menyeberang batas modul — penjelasan yang justru harus ada.
            const std::size_t comment = line.find("//");
            const std::string code = comment == std::string::npos ? line : line.substr(0, comment);
            if (code.find("PxPhysicsAPI.h") != std::string::npos ||
                code.find("physx::") != std::string::npos) {
                offenders.push_back(path.string());
                break;
            }
        }
    }

    INFO("berkas yang melihat PhysX di luar backend: ",
         offenders.empty() ? std::string("(tidak ada)") : offenders.front());
    CHECK(offenders.empty());
}

namespace {

/// Bola statik berjari-jari 1 di titik asal, sasaran raycast yang jawabannya
/// bisa dihitung tangan.
BodyHandle AddSphereAt(PhysicsWorld& world, const Vec3& position, float radius,
                       BodyKind kind = BodyKind::Static, uint32_t layer = kDefaultLayer) {
    BodyDesc desc;
    desc.kind = kind;
    desc.shape.kind = ShapeKind::Sphere;
    desc.shape.radius = radius;
    desc.position = position;
    desc.layer = layer;
    return world.AddBody(desc);
}

}  // namespace

TEST_CASE("raycast menjawab jarak dan normal yang dihitung, bukan yang mendekati") {
    if (!Available()) {
        return;
    }
    PhysicsWorld world;
    REQUIRE(world.Create(DefaultWorld()));
    const BodyHandle sphere = AddSphereAt(world, Vec3(0.0f), 1.0f);
    REQUIRE(sphere != BodyHandle::Invalid);

    // Dari (0, 5, 0) lurus ke bawah menuju bola berjari-jari 1 di titik asal:
    // kena di y = 1, jaraknya 4, normalnya +Y. Ketiganya diketahui sebelum
    // query-nya dijalankan.
    RayHit hit;
    REQUIRE(world.Raycast(Vec3(0.0f, 5.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), 10.0f, hit));
    CHECK(hit.body == sphere);
    CHECK(hit.distance == doctest::Approx(4.0f).epsilon(0.001));
    CHECK(hit.position.y == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(hit.normal.y == doctest::Approx(1.0f).epsilon(0.001));

    // Menyerempet di luar jari-jari tidak kena sama sekali.
    RayHit miss;
    CHECK_FALSE(world.Raycast(Vec3(1.5f, 5.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), 10.0f, miss));

    // Arah yang tidak bernorma satu tetap menghasilkan jarak dalam meter, bukan
    // dalam kelipatan panjang arahnya.
    RayHit scaled;
    REQUIRE(world.Raycast(Vec3(0.0f, 5.0f, 0.0f), Vec3(0.0f, -7.0f, 0.0f), 10.0f, scaled));
    CHECK(scaled.distance == doctest::Approx(4.0f).epsilon(0.001));

    // Jangkauan yang lebih pendek daripada sasarannya tidak menjangkaunya.
    RayHit tooShort;
    CHECK_FALSE(world.Raycast(Vec3(0.0f, 5.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), 3.0f, tooShort));

    // Arah bernorma nol ditolak alih-alih ditebak.
    RayHit degenerate;
    CHECK_FALSE(world.Raycast(Vec3(0.0f, 5.0f, 0.0f), Vec3(0.0f), 10.0f, degenerate));
}

TEST_CASE("raycast semua menjawab berurutan dari yang terdekat") {
    if (!Available()) {
        return;
    }
    PhysicsWorld world;
    REQUIRE(world.Create(DefaultWorld()));
    // Tiga bola bertumpuk di sepanjang ray, sengaja ditambahkan tidak berurutan
    // supaya urutan jawabannya benar-benar hasil pengurutan, bukan kebetulan
    // urutan penambahan.
    AddSphereAt(world, Vec3(0.0f, 2.0f, 0.0f), 0.5f);
    AddSphereAt(world, Vec3(0.0f, 6.0f, 0.0f), 0.5f);
    AddSphereAt(world, Vec3(0.0f, 4.0f, 0.0f), 0.5f);

    std::vector<RayHit> hits;
    CHECK(world.RaycastAll(Vec3(0.0f, 10.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), 20.0f, hits) == 3);
    REQUIRE(hits.size() == 3);
    CHECK(hits[0].distance < hits[1].distance);
    CHECK(hits[1].distance < hits[2].distance);
    CHECK(hits[0].distance == doctest::Approx(3.5f).epsilon(0.001));
    CHECK(hits[1].distance == doctest::Approx(5.5f).epsilon(0.001));
    CHECK(hits[2].distance == doctest::Approx(7.5f).epsilon(0.001));

    // Yang terdekat harus sama dengan jawaban raycast tunggal.
    RayHit nearest;
    REQUIRE(world.Raycast(Vec3(0.0f, 10.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), 20.0f, nearest));
    CHECK(nearest.distance == doctest::Approx(hits[0].distance));
}

TEST_CASE("filter lapisan benar-benar menyaring") {
    if (!Available()) {
        return;
    }
    PhysicsWorld world;
    REQUIRE(world.Create(DefaultWorld()));

    constexpr LayerMask kWall = 1u << 0;
    constexpr LayerMask kEnemy = 1u << 1;
    const BodyHandle wall = AddSphereAt(world, Vec3(0.0f, 2.0f, 0.0f), 0.5f, BodyKind::Static, kWall);
    const BodyHandle enemy =
        AddSphereAt(world, Vec3(0.0f, 5.0f, 0.0f), 0.5f, BodyKind::Static, kEnemy);

    const Vec3 origin(0.0f, 8.0f, 0.0f);
    const Vec3 down(0.0f, -1.0f, 0.0f);
    RayHit hit;

    SUBCASE("mask kosong tidak pernah kena") {
        // Kriteria terima P2, dan alasannya bukan kerapian: mask kosong yang
        // diam-diam berarti "semuanya" membuat peluru menembus rekan setim
        // justru pada kode yang lupa mengisi filternya.
        QueryFilter filter;
        filter.layers = kNoLayers;
        CHECK_FALSE(world.Raycast(origin, down, 20.0f, hit, filter));

        std::vector<RayHit> all;
        CHECK(world.RaycastAll(origin, down, 20.0f, all, filter) == 0);

        std::vector<BodyHandle> overlaps;
        CHECK(world.OverlapSphere(Vec3(0.0f, 5.0f, 0.0f), 2.0f, overlaps, filter) == 0);
    }

    SUBCASE("satu lapisan melewatkan yang lain") {
        QueryFilter filter;
        filter.layers = kWall;
        // Musuh berada lebih dekat ke asal ray, jadi jawaban yang benar hanya
        // bisa datang dari penyaringan — bukan dari urutan.
        REQUIRE(world.Raycast(origin, down, 20.0f, hit, filter));
        CHECK(hit.body == wall);

        filter.layers = kEnemy;
        REQUIRE(world.Raycast(origin, down, 20.0f, hit, filter));
        CHECK(hit.body == enemy);

        filter.layers = kWall | kEnemy;
        REQUIRE(world.Raycast(origin, down, 20.0f, hit, filter));
        CHECK(hit.body == enemy);  // yang terdekat
    }

    SUBCASE("statis dan dinamis dipisah tanpa lewat lapisan") {
        const BodyHandle falling =
            AddSphereAt(world, Vec3(0.0f, 6.5f, 0.0f), 0.5f, BodyKind::Dynamic, kWall);
        REQUIRE(falling != BodyHandle::Invalid);

        QueryFilter filter;
        filter.hitDynamic = false;
        REQUIRE(world.Raycast(origin, down, 20.0f, hit, filter));
        CHECK(hit.body != falling);

        filter = QueryFilter{};
        filter.hitStatic = false;
        REQUIRE(world.Raycast(origin, down, 20.0f, hit, filter));
        CHECK(hit.body == falling);
    }
}

TEST_CASE("sweep menemukan yang dilewati ray tak bertebal") {
    if (!Available()) {
        return;
    }
    // **Inilah sebabnya sweep ada.** Sebuah celah yang lebih sempit daripada
    // bendanya dilewati ray begitu saja, dan gejalanya adalah peluru yang
    // sesekali menembus dinding tipis.
    PhysicsWorld world;
    REQUIRE(world.Create(DefaultWorld()));
    // Dua bola dengan celah 1 m di antaranya, tepat di jalur ray.
    AddSphereAt(world, Vec3(-1.0f, 2.0f, 0.0f), 0.5f);
    AddSphereAt(world, Vec3(1.0f, 2.0f, 0.0f), 0.5f);

    const Vec3 origin(0.0f, 8.0f, 0.0f);
    const Vec3 down(0.0f, -1.0f, 0.0f);

    RayHit rayHit;
    CHECK_FALSE(world.Raycast(origin, down, 20.0f, rayHit));

    // Bola berjari-jari 0,75 tidak muat di celah selebar 1 m.
    RayHit sweepHit;
    REQUIRE(world.SweepSphere(0.75f, origin, down, 20.0f, sweepHit));
    CHECK(sweepHit.body != BodyHandle::Invalid);

    // Yang cukup kecil tetap lolos, sama seperti ray-nya.
    RayHit thin;
    CHECK_FALSE(world.SweepSphere(0.1f, origin, down, 20.0f, thin));
}

TEST_CASE("overlap menjawab benda, bukan bentuk") {
    if (!Available()) {
        return;
    }
    PhysicsWorld world;
    REQUIRE(world.Create(DefaultWorld()));
    const BodyHandle near1 = AddSphereAt(world, Vec3(0.0f, 0.0f, 0.0f), 0.5f);
    const BodyHandle near2 = AddSphereAt(world, Vec3(1.0f, 0.0f, 0.0f), 0.5f);
    AddSphereAt(world, Vec3(20.0f, 0.0f, 0.0f), 0.5f);

    std::vector<BodyHandle> hits;
    CHECK(world.OverlapSphere(Vec3(0.5f, 0.0f, 0.0f), 2.0f, hits) == 2);
    CHECK(std::find(hits.begin(), hits.end(), near1) != hits.end());
    CHECK(std::find(hits.begin(), hits.end(), near2) != hits.end());

    // Tidak ada handle yang muncul dua kali.
    std::vector<BodyHandle> sorted = hits;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());

    // Radius yang tidak menyentuh apa pun menjawab kosong, bukan gagal.
    CHECK(world.OverlapSphere(Vec3(0.0f, 50.0f, 0.0f), 1.0f, hits) == 0);
    CHECK(hits.empty());
}

TEST_CASE("query dari banyak thread menjawab hal yang sama") {
    if (!Available()) {
        return;
    }
    // Kriteria terima P2. PhysX mengizinkan banyak pembaca sekaligus asalkan
    // tidak ada langkah yang berjalan; uji ini menjalankan batas itu alih-alih
    // mempercayainya.
    PhysicsWorld world;
    REQUIRE(world.Create(DefaultWorld()));
    AddSphereAt(world, Vec3(0.0f), 1.0f);

    TaskPool pool(4);
    std::atomic<int> agreed{0};
    std::atomic<int> disagreed{0};
    constexpr int kQueries = 512;
    for (int i = 0; i < kQueries; ++i) {
        pool.Submit([&world, &agreed, &disagreed]() {
            RayHit hit;
            if (world.Raycast(Vec3(0.0f, 5.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), 10.0f, hit) &&
                std::abs(hit.distance - 4.0f) < 0.001f) {
                agreed.fetch_add(1, std::memory_order_relaxed);
            } else {
                disagreed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    pool.WaitIdle();

    INFO("sepakat " << agreed.load() << ", tidak " << disagreed.load());
    CHECK(agreed.load() == kQueries);
    CHECK(disagreed.load() == 0);
}

TEST_CASE("query yang menabrak langkah simulasi ditolak, bukan dijawab separuh") {
    if (!Available()) {
        return;
    }
    // Batas PhysX ditegakkan, bukan diharapkan. Thread pembaca menembakkan ray
    // terus-menerus sementara main thread melangkah; yang tertolak sah, tetapi
    // **tidak boleh ada satu pun jawaban yang salah** — dan jawaban yang salah
    // adalah persis yang akan muncul kalau penjagaannya tidak ada.
    PhysicsWorld world;
    REQUIRE(world.Create(DefaultWorld()));
    AddSphereAt(world, Vec3(0.0f), 1.0f, BodyKind::Static);

    std::atomic<bool> running{true};
    std::atomic<int> answered{0};
    std::atomic<int> refused{0};
    std::atomic<int> wrong{0};

    std::thread reader([&]() {
        while (running.load(std::memory_order_acquire)) {
            RayHit hit;
            if (!world.Raycast(Vec3(0.0f, 5.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), 10.0f, hit)) {
                refused.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            // Bolanya statik: jawabannya tidak boleh berubah, langkah atau tidak.
            if (std::abs(hit.distance - 4.0f) > 0.001f) {
                wrong.fetch_add(1, std::memory_order_relaxed);
            } else {
                answered.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    // Langkah satu per satu, seperti frame sungguhan — bukan `Step(200)`, yang
    // menahan penjagaannya sepanjang seluruh rangkaian dan tidak menyisakan
    // satu pun sela bagi pembacanya.
    for (int i = 0; i < 200; ++i) {
        world.Step(1);
    }
    running.store(false, std::memory_order_release);
    reader.join();

    INFO("dijawab " << answered.load() << ", ditolak " << refused.load() << ", salah "
                    << wrong.load());
    CHECK(wrong.load() == 0);
    CHECK(answered.load() + refused.load() > 0);

    // Dan penjagaannya tidak menolak selamanya: sesudah langkah terakhir selesai
    // ia harus terbuka lagi. Diperiksa dari main thread supaya jawabannya tidak
    // bergantung pada thread pembaca kebetulan mendapat sela — sebuah uji yang
    // lulusnya bergantung penjadwalan bukan uji.
    RayHit after;
    REQUIRE(world.Raycast(Vec3(0.0f, 5.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), 10.0f, after));
    CHECK(after.distance == doctest::Approx(4.0f).epsilon(0.001));
}
