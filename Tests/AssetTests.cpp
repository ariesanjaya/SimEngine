#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Assets/Cook.h"
#include "Sim/Assets/Importer.h"
#include "Sim/Assets/MaterialImport.h"
#include "Sim/Assets/BlockCompress.h"
#include "Sim/Assets/TextureBake.h"
#include "Sim/Assets/TextureBakery.h"
#include "Sim/Assets/TextureSettings.h"
#include "Sim/Assets/MeshData.h"
#include "Sim/Assets/MeshSdfBake.h"
#include "Sim/Assets/MeshSdfBakery.h"
#include "Sim/Assets/MeshSettings.h"
#include "Sim/Assets/Thumbnail.h"
#include "Sim/Material/MaterialGraph.h"
#include "Sim/Material/MaterialValidation.h"

#include "Sim/Volume/SdfBake.h"

#include "Sim/ImageIO/ImageIO.h"
#include "Sim/RHI/Ktx2.h"

#include "Sim/Core/FileWatcher.h"
#include "Sim/Core/TaskPool.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <cctype>
#include <unistd.h>
#include <fstream>
#include <string>
#include <thread>

using namespace sim;
using namespace sim::assets;

namespace {

/// Folder sementara yang membersihkan dirinya sendiri.
class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("simassets_" + std::to_string(counter.fetch_add(1)) + "_" +
                 std::to_string(::getpid()));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    const std::filesystem::path& Path() const { return path_; }

private:
    std::filesystem::path path_;
};

void WriteFile(const std::filesystem::path& path, std::string_view text) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path);
    out << text;
}

}  // namespace

TEST_CASE("aset mendapat GUID yang bertahan lintas pemindaian") {
    TempDir temp;
    WriteFile(temp.Path() / "Textures" / "brick.png", "not really a png");

    AssetDatabase first;
    REQUIRE(first.Initialize({temp.Path(), nullptr, 1.0f}));
    const AssetRecord* record = first.FindByRelativePath("Textures/brick.png");
    REQUIRE(record != nullptr);
    const Uuid guid = record->guid;
    CHECK(guid.IsValid());

    // Berkas .meta di sebelah aset itulah yang menyimpan identitasnya. Database
    // baru harus membaca GUID yang sama, bukan membuat yang baru.
    CHECK(std::filesystem::exists(temp.Path() / "Textures" / "brick.png.meta"));

    AssetDatabase second;
    REQUIRE(second.Initialize({temp.Path(), nullptr, 1.0f}));
    const AssetRecord* again = second.FindByRelativePath("Textures/brick.png");
    REQUIRE(again != nullptr);
    CHECK(again->guid == guid);
}

TEST_CASE("berkas .meta tidak ikut terdaftar sebagai aset") {
    TempDir temp;
    WriteFile(temp.Path() / "a.txt", "hello");

    AssetDatabase db;
    REQUIRE(db.Initialize({temp.Path(), nullptr, 1.0f}));
    CHECK(db.All().size() == 1);
    CHECK(db.FindByRelativePath("a.txt.meta") == nullptr);
}

TEST_CASE("tipe aset diturunkan dari ekstensi tanpa peduli huruf besar-kecil") {
    CHECK(TypeFromExtension(".PNG") == AssetType::Texture);
    CHECK(TypeFromExtension(".lua") == AssetType::Script);
    CHECK(TypeFromExtension(".simlevel") == AssetType::Level);
    CHECK(TypeFromExtension(".xyz") == AssetType::Unknown);
}

TEST_CASE("mengganti nama aset mempertahankan GUID-nya") {
    TempDir temp;
    WriteFile(temp.Path() / "old.txt", "content");

    AssetDatabase db;
    REQUIRE(db.Initialize({temp.Path(), nullptr, 1.0f}));
    const Uuid guid = db.FindByRelativePath("old.txt")->guid;

    std::string error;
    REQUIRE(db.Rename(guid, "new.txt", error));
    CHECK(error.empty());

    // Inilah yang membuat level tidak pernah putus: berkasnya berpindah nama,
    // identitasnya tidak.
    CHECK(db.FindByRelativePath("old.txt") == nullptr);
    const AssetRecord* renamed = db.FindByRelativePath("new.txt");
    REQUIRE(renamed != nullptr);
    CHECK(renamed->guid == guid);
    CHECK(std::filesystem::exists(temp.Path() / "new.txt.meta"));
    CHECK_FALSE(std::filesystem::exists(temp.Path() / "old.txt.meta"));
}

TEST_CASE("memindahkan aset antar folder mempertahankan GUID-nya") {
    TempDir temp;
    WriteFile(temp.Path() / "mesh.obj", "v 0 0 0");

    AssetDatabase db;
    REQUIRE(db.Initialize({temp.Path(), nullptr, 1.0f}));
    const Uuid guid = db.FindByRelativePath("mesh.obj")->guid;

    std::string error;
    REQUIRE(db.Move(guid, "Models/Props", error));
    const AssetRecord* moved = db.FindByRelativePath("Models/Props/mesh.obj");
    REQUIRE(moved != nullptr);
    CHECK(moved->guid == guid);
}

TEST_CASE("mengganti nama menolak nama yang sudah dipakai") {
    TempDir temp;
    WriteFile(temp.Path() / "a.txt", "a");
    WriteFile(temp.Path() / "b.txt", "b");

    AssetDatabase db;
    REQUIRE(db.Initialize({temp.Path(), nullptr, 1.0f}));
    const Uuid guid = db.FindByRelativePath("a.txt")->guid;

    std::string error;
    CHECK_FALSE(db.Rename(guid, "b.txt", error));
    CHECK_FALSE(error.empty());
    // Yang lama harus tetap utuh, bukan hilang separuh jalan.
    CHECK(db.FindByRelativePath("a.txt") != nullptr);
}

TEST_CASE("ketergantungan antar-aset ditemukan dan disaring ke GUID yang dikenal") {
    TempDir temp;
    WriteFile(temp.Path() / "brick.png", "png");

    AssetDatabase db;
    REQUIRE(db.Initialize({temp.Path(), nullptr, 1.0f}));
    const Uuid textureGuid = db.FindByRelativePath("brick.png")->guid;

    // Level yang merujuk tekstur itu, ditambah satu GUID acak yang bukan aset —
    // meniru GUID entity, yang bentuknya sama persis dan harus tersaring.
    const std::string level = R"({"schemaVersion":3,"entities":[{"guid":")" +
                              Uuid::Generate().ToString() +
                              R"(","components":{"MeshRenderer":{"material":")" +
                              textureGuid.ToString() + R"("}}}]})";
    WriteFile(temp.Path() / "arena.simlevel", level);
    db.ScanNow();

    const AssetRecord* levelRecord = db.FindByRelativePath("arena.simlevel");
    REQUIRE(levelRecord != nullptr);
    CHECK(levelRecord->dependencies.size() == 1);
    CHECK(levelRecord->dependencies.front() == textureGuid);

    const std::vector<Uuid> users = db.UsersOf(textureGuid);
    REQUIRE(users.size() == 1);
    CHECK(users.front() == levelRecord->guid);
}

TEST_CASE("akar relatif ber-\"./\" tetap menghasilkan jalur yang bisa dibuka") {
    // **Bukan kasus buatan.** Sebuah program yang dijalankan dengan `./Program`
    // menurunkan folder asetnya dari `argv[0]`, jadi akar yang diterimanya
    // memang berbentuk `./Resources` — dan itu cara setiap executable di folder
    // build dijalankan. Bug-nya: awalan akar dipotong dengan perbandingan
    // string, akarnya dinormalkan lebih dulu sementara jalur hasil iterasi
    // tidak, dan yang tersimpan sebagai "relatif" justru jalur utuhnya.
    // `AbsolutePath` lalu menempelkan akarnya untuk kedua kalinya.
    TempDir temp;
    WriteFile(temp.Path() / "Meshes" / "cube.obj", "o");

    const std::filesystem::path previous = std::filesystem::current_path();
    std::filesystem::current_path(temp.Path().parent_path());
    const std::filesystem::path relativeRoot =
        std::filesystem::path(".") / temp.Path().filename();

    AssetDatabase db;
    REQUIRE(db.Initialize({relativeRoot, nullptr, 1.0f}));
    const AssetRecord* record = db.FindByRelativePath("Meshes/cube.obj");
    REQUIRE(record != nullptr);
    // Yang dikunci bukan bentuk stringnya melainkan kontraknya: jalur yang
    // dikembalikan indeks harus benar-benar bisa dibuka.
    CHECK(std::filesystem::exists(db.AbsolutePath(*record)));

    std::filesystem::current_path(previous);
}

TEST_CASE("isi folder bisa diambil bertingkat maupun langsung") {
    TempDir temp;
    WriteFile(temp.Path() / "Textures" / "a.png", "a");
    WriteFile(temp.Path() / "Textures" / "Sub" / "b.png", "b");
    WriteFile(temp.Path() / "root.txt", "r");

    AssetDatabase db;
    REQUIRE(db.Initialize({temp.Path(), nullptr, 1.0f}));

    CHECK(db.InFolder("Textures", true).size() == 2);
    CHECK(db.InFolder("Textures", false).size() == 1);
    CHECK(db.InFolder("", false).size() == 1);  // hanya root.txt
    CHECK(db.InFolder("", true).size() == 3);
}

TEST_CASE("berkas baru dari luar editor muncul lewat pemindaian latar") {
    TempDir temp;
    WriteFile(temp.Path() / "first.txt", "1");

    TaskPool pool(2);
    AssetDatabase db;
    // Jeda poll dipendekkan supaya test tidak menunggu satu detik penuh.
    REQUIRE(db.Initialize({temp.Path(), &pool, 0.05f}));
    CHECK(db.All().size() == 1);

    // Ditulis dari luar, tanpa memberi tahu database sama sekali.
    WriteFile(temp.Path() / "second.txt", "2");

    // Meniru gelung frame: Update dipanggil berulang sampai perubahannya
    // terlihat, dengan batas waktu supaya kegagalan tidak menggantung.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (db.All().size() < 2 && std::chrono::steady_clock::now() < deadline) {
        db.Update(0.1f);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(db.All().size() == 2);
    CHECK(db.FindByRelativePath("second.txt") != nullptr);
}

TEST_CASE("pemantau berkas melaporkan berkas baru tanpa pemindaian penuh") {
    TempDir temp;
    WriteFile(temp.Path() / "sudah_ada.txt", "1");

    FileWatcher watcher;
    if (!watcher.Watch(temp.Path())) {
        MESSAGE("pemantau berkas tidak tersedia di platform ini; uji dilewati");
        return;
    }
    REQUIRE(watcher.IsWatching());

    std::vector<FileWatcher::Event> events;
    // Sebelum ada perubahan, tidak ada yang dilaporkan dan tidak ada yang hilang.
    CHECK(watcher.Poll(events));
    CHECK(events.empty());

    WriteFile(temp.Path() / "baru.txt", "2");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool seen = false;
    while (!seen && std::chrono::steady_clock::now() < deadline) {
        events.clear();
        watcher.Poll(events);
        for (const FileWatcher::Event& event : events) {
            if (event.path == "baru.txt") {
                seen = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(seen);
}

TEST_CASE("pemantau meminta pemindaian ulang saat direktori baru muncul") {
    TempDir temp;
    FileWatcher watcher;
    if (!watcher.Watch(temp.Path())) {
        return;
    }

    // Berkas bisa masuk ke direktori baru sebelum watch-nya sempat terpasang,
    // dan itu tidak akan pernah terlaporkan. Satu-satunya jawaban jujur adalah
    // meminta pemindaian ulang — Poll() harus mengembalikan false.
    std::filesystem::create_directories(temp.Path() / "Sub");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool askedForRescan = false;
    while (!askedForRescan && std::chrono::steady_clock::now() < deadline) {
        std::vector<FileWatcher::Event> events;
        askedForRescan = !watcher.Poll(events);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(askedForRescan);
}

TEST_CASE("versi hanya naik ketika isinya benar-benar berubah") {
    TempDir temp;
    WriteFile(temp.Path() / "a.txt", "a");

    AssetDatabase db;
    REQUIRE(db.Initialize({temp.Path(), nullptr, 1.0f}));
    const uint64_t before = db.Version();

    db.ScanNow();
    db.ScanNow();
    // Memindai ulang tanpa perubahan tidak boleh menaikkan versi: panel memakai
    // nilai ini untuk memutuskan menyusun ulang tampilannya.
    CHECK(db.Version() == before);

    WriteFile(temp.Path() / "b.txt", "b");
    db.ScanNow();
    CHECK(db.Version() > before);
}

TEST_CASE("menghapus aset ikut menghapus berkas metanya") {
    TempDir temp;
    WriteFile(temp.Path() / "gone.txt", "x");

    AssetDatabase db;
    REQUIRE(db.Initialize({temp.Path(), nullptr, 1.0f}));
    const Uuid guid = db.FindByRelativePath("gone.txt")->guid;

    std::string error;
    REQUIRE(db.Delete(guid, error));
    CHECK(db.Find(guid) == nullptr);
    CHECK_FALSE(std::filesystem::exists(temp.Path() / "gone.txt"));
    CHECK_FALSE(std::filesystem::exists(temp.Path() / "gone.txt.meta"));
}

TEST_CASE("kolam tugas menyelesaikan seluruh pekerjaan yang diantre") {
    TaskPool pool(4);
    std::atomic<int> counter{0};
    for (int i = 0; i < 500; ++i) {
        pool.Submit([&counter]() { counter.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.WaitIdle();
    CHECK(counter.load() == 500);
    CHECK(pool.Pending() == 0);
}

TEST_CASE("tugas yang melempar pengecualian tidak mematikan worker") {
    TaskPool pool(2);
    std::atomic<int> after{0};
    pool.Submit([]() { throw std::runtime_error("boom"); });
    for (int i = 0; i < 50; ++i) {
        pool.Submit([&after]() { after.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.WaitIdle();
    // Kalau worker mati bersama pengecualiannya, tugas sesudahnya tidak akan
    // pernah selesai dan WaitIdle menggantung selamanya.
    CHECK(after.load() == 50);
}

TEST_CASE("berkas yang ditimpa dilaporkan sebagai berubah, yang diam tidak") {
    TempDir temp;
    WriteFile(temp.Path() / "spin.lua", "return {}");
    WriteFile(temp.Path() / "diam.lua", "return {}");

    TaskPool pool(2);
    AssetDatabase db;
    REQUIRE(db.Initialize({temp.Path(), &pool, 0.05f}));
    REQUIRE(db.All().size() == 2);

    const AssetRecord* spin = db.FindByRelativePath("spin.lua");
    const AssetRecord* quiet = db.FindByRelativePath("diam.lua");
    REQUIRE(spin != nullptr);
    REQUIRE(quiet != nullptr);
    const Uuid spinGuid = spin->guid;
    const Uuid quietGuid = quiet->guid;

    // Waktu ubah berkas bergranularitas detik di beberapa sistem berkas, jadi
    // isinya dibuat berbeda panjang supaya ukurannya saja sudah cukup untuk
    // membedakan — persis kondisi yang harus ditangani di dunia nyata ketika
    // pengguna menyimpan dua kali dalam satu detik.
    WriteFile(temp.Path() / "spin.lua", "return { OnUpdate = function() end }");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::vector<Uuid> changed;
    while (changed.empty() && std::chrono::steady_clock::now() < deadline) {
        db.Update(0.1f);
        changed = db.ChangedThisUpdate();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(changed.size() == 1);
    CHECK(changed[0] == spinGuid);
    // Berkas yang tidak disentuh tidak boleh ikut dilaporkan: memuat ulang
    // skrip yang tidak berubah membuang tabel `state`-nya tanpa alasan.
    CHECK(std::find(changed.begin(), changed.end(), quietGuid) == changed.end());
    // GUID-nya tidak berubah walau isinya diganti — identitas aset melekat pada
    // berkas .meta, bukan pada isinya.
    CHECK(db.FindByRelativePath("spin.lua")->guid == spinGuid);

    // Daftar perubahan hanya berlaku satu frame.
    db.Update(0.1f);
    CHECK(db.ChangedThisUpdate().empty());
}

// --- E8.4: impor mesh ---------------------------------------------------------

TEST_CASE("indeks mesh menyatukan vertex kembar tanpa melunakkan tepi tajam") {
    using namespace sim::assets;

    // Dua segitiga yang berbagi sebuah tepi, tapi normalnya berbeda — persis
    // keadaan di rusuk sebuah kubus.
    //
    // **Perbandingannya bit-per-bit, bukan bertoleransi.** Toleransi menyatukan
    // kedua sisi rusuk itu menjadi satu vertex bernormal rata-rata, dan yang
    // terlihat adalah tepi kotak yang membulat sendiri. Yang dicari di sini
    // hanyalah vertex yang benar-benar sama, yaitu yang muncul karena satu titik
    // dipakai beberapa segitiga.
    const Vec3 shared0(0.0f, 0.0f, 0.0f);
    const Vec3 shared1(1.0f, 0.0f, 0.0f);
    const Vec3 up(0.0f, 1.0f, 0.0f);
    const Vec3 forward(0.0f, 0.0f, 1.0f);

    std::vector<MeshVertex> soup;
    soup.push_back(MeshVertex{shared0, up, Vec2(0.0f)});
    soup.push_back(MeshVertex{shared1, up, Vec2(0.0f)});
    soup.push_back(MeshVertex{Vec3(0.0f, 0.0f, 1.0f), up, Vec2(0.0f)});
    // Segitiga kedua memakai dua titik yang sama persis, bernormal sama.
    soup.push_back(MeshVertex{shared0, up, Vec2(0.0f)});
    soup.push_back(MeshVertex{shared1, up, Vec2(0.0f)});
    soup.push_back(MeshVertex{Vec3(1.0f, 0.0f, 1.0f), up, Vec2(0.0f)});
    // Segitiga ketiga memakai titik yang sama tapi bernormal lain — tepi tajam.
    soup.push_back(MeshVertex{shared0, forward, Vec2(0.0f)});
    soup.push_back(MeshVertex{shared1, forward, Vec2(0.0f)});
    soup.push_back(MeshVertex{Vec3(0.0f, 1.0f, 0.0f), forward, Vec2(0.0f)});

    const MeshData mesh = BuildIndexedMesh(soup);
    REQUIRE(mesh.IsValid());
    // Segitiganya utuh: sembilan sudut, tiga segitiga.
    CHECK(mesh.indices.size() == 9);
    CHECK(mesh.TriangleCount() == 3);
    // Dua vertex kembar bernormal sama disatukan; yang bernormal beda tidak.
    // 9 sudut - 2 kembar = 7 vertex.
    CHECK(mesh.vertices.size() == 7);

    // Setiap indeks menunjuk vertex yang sah.
    for (const uint32_t index : mesh.indices) {
        CHECK(index < mesh.vertices.size());
    }
}

TEST_CASE("batas mesh dihitung dari vertexnya, dan aman saat kosong") {
    using namespace sim::assets;

    MeshData mesh;
    mesh.ComputeBounds();
    CHECK(mesh.boundsMin.x == doctest::Approx(0.0f));
    CHECK(mesh.boundsMax.x == doctest::Approx(0.0f));

    mesh.vertices.push_back(MeshVertex{Vec3(-2.0f, 1.0f, 0.5f), Vec3(0, 1, 0), Vec2(0.0f)});
    mesh.vertices.push_back(MeshVertex{Vec3(3.0f, -4.0f, 7.0f), Vec3(0, 1, 0), Vec2(0.0f)});
    mesh.ComputeBounds();
    CHECK(mesh.boundsMin.x == doctest::Approx(-2.0f));
    CHECK(mesh.boundsMin.y == doctest::Approx(-4.0f));
    CHECK(mesh.boundsMin.z == doctest::Approx(0.5f));
    CHECK(mesh.boundsMax.x == doctest::Approx(3.0f));
    CHECK(mesh.boundsMax.y == doctest::Approx(1.0f));
    CHECK(mesh.boundsMax.z == doctest::Approx(7.0f));
}

TEST_CASE("segitiga yang tidak genap tiga ditolak, bukan dipotong") {
    using namespace sim::assets;

    // Dipotong diam-diam berarti mesh yang kehilangan segitiga terakhirnya tanpa
    // ada yang tahu — dan yang terlihat adalah lubang di permukaan, bukan galat.
    std::vector<MeshVertex> soup(4);
    CHECK(BuildIndexedMesh(soup).IsValid() == false);
    CHECK(BuildIndexedMesh({}).IsValid() == false);
}

TEST_CASE("berkas yang tidak ada menjawab galat, bukan mesh separuh jadi") {
    using namespace sim::assets;

    std::string error;
    const MeshData mesh = LoadMesh("tidak-ada-berkas-ini.fbx", error);
    CHECK(mesh.IsValid() == false);
    CHECK(!error.empty());
}

TEST_CASE("shaderBall.fbx terbaca sebagai geometri yang masuk akal") {
    using namespace sim::assets;

    const std::filesystem::path path =
        std::filesystem::path(SIM_MESH_DIR) / "shaderBall.fbx";
    if (!std::filesystem::exists(path)) {
        return;  // aset opsional; ketiadaannya bukan kegagalan uji
    }

    std::string error;
    const MeshData mesh = LoadMesh(path, error);
    REQUIRE(mesh.IsValid());
    CHECK(error.empty());
    CHECK(mesh.TriangleCount() > 1000);

    // **Satuannya meter, dan itu yang paling mudah salah.** FBX menyimpan
    // satuannya sendiri di dalam berkas — sentimeter pada berkas dari banyak
    // DCC — dan mesh yang tidak dikonversi muncul seratus kali terlalu besar.
    // Itu tidak terlihat sebagai galat melainkan sebagai kamera yang berada di
    // dalam objek.
    const Vec3 size = mesh.boundsMax - mesh.boundsMin;
    CHECK(size.x > 0.5f);
    CHECK(size.x < 20.0f);
    CHECK(size.y > 0.5f);
    CHECK(size.y < 20.0f);

    // Berdiri di atas titik asal, bukan terkubur di bawahnya: alasnya di sekitar
    // nol. Sumbu yang tertukar akan memindahkannya ke samping.
    CHECK(mesh.boundsMin.y > -0.5f);
    CHECK(mesh.boundsMin.y < 0.5f);

    // Setiap indeks sah, dan normalnya ternormalisasi.
    for (const uint32_t index : mesh.indices) {
        REQUIRE(index < mesh.vertices.size());
    }
    for (std::size_t i = 0; i < mesh.vertices.size(); i += 97) {
        CHECK(glm::length(mesh.vertices[i].normal) == doctest::Approx(1.0f).epsilon(0.01f));
    }
}


// --- E8.4: rangka dan bobot skin ----------------------------------------------

TEST_CASE("bobot skin dinormalkan, dan vertex tanpa pengaruh tidak runtuh") {
    using namespace sim::assets;

    SkinInfluence influence;
    influence.bones = {3, 7, 0, 0};
    influence.weights = {0.5f, 0.25f, 0.0f, 0.0f};
    influence.Normalize();
    // **Bobot yang tidak berjumlah satu menyusutkan vertexnya ke arah titik
    // asal**, dan yang terlihat adalah permukaan yang berkerut di tempat
    // tertentu — bukan galat.
    CHECK(influence.WeightSum() == doctest::Approx(1.0f));
    CHECK(influence.weights[0] == doctest::Approx(2.0f / 3.0f));
    CHECK(influence.weights[1] == doctest::Approx(1.0f / 3.0f));

    // Nol di seluruh bobot berarti vertex itu runtuh ke titik asal. Ia harus
    // mengikuti bone pertama sepenuhnya alih-alih hilang.
    SkinInfluence empty;
    empty.Normalize();
    CHECK(empty.WeightSum() == doctest::Approx(1.0f));
    CHECK(empty.weights[0] == doctest::Approx(1.0f));
}

TEST_CASE("rangka menolak urutan yang bukan topologis") {
    using namespace sim::assets;

    SkeletonData skeleton;
    skeleton.bones.push_back(SkeletonBone{"Root", -1, {}, {}, {}});
    skeleton.bones.push_back(SkeletonBone{"Spine", 0, {}, {}, {}});
    CHECK(skeleton.IsTopological());
    CHECK(skeleton.Find("Spine") == 1);
    CHECK(skeleton.Find("Tidak Ada") == -1);

    // Induk yang menyusul anaknya membuat transform global tidak bisa dihitung
    // satu lintasan maju — dan satu lintasan maju itulah alasan urutannya
    // dituntut sejak awal.
    skeleton.bones.push_back(SkeletonBone{"Salah", 3, {}, {}, {}});
    CHECK(skeleton.IsTopological() == false);
}

TEST_CASE("vertex yang sama tapi beda bobot skin tidak disatukan") {
    using namespace sim::assets;

    // **Menyatukannya berarti separuh permukaan mengikuti bone yang salah** —
    // yang terlihat sebagai kulit yang tertarik ke arah yang tidak masuk akal,
    // bukan sebagai galat.
    const MeshVertex vertex{Vec3(1.0f, 2.0f, 3.0f), Vec3(0.0f, 1.0f, 0.0f), Vec2(0.0f)};
    std::vector<MeshVertex> soup(6, vertex);

    SkinInfluence hip;
    hip.bones = {0, 0, 0, 0};
    hip.weights = {1.0f, 0.0f, 0.0f, 0.0f};
    SkinInfluence knee;
    knee.bones = {5, 0, 0, 0};
    knee.weights = {1.0f, 0.0f, 0.0f, 0.0f};
    std::vector<SkinInfluence> influences{hip, hip, hip, knee, knee, knee};

    const MeshData mesh = BuildIndexedMesh(soup, influences);
    REQUIRE(mesh.IsValid());
    CHECK(mesh.vertices.size() == 2);
    CHECK(mesh.influences.size() == 2);
    CHECK(mesh.indices.size() == 6);

    // Tanpa pengaruh, keenamnya memang satu vertex — itu perilaku lama yang
    // harus tetap berlaku untuk mesh statis.
    const MeshData plain = BuildIndexedMesh(soup);
    CHECK(plain.vertices.size() == 1);
    CHECK(plain.influences.empty());
    CHECK(plain.IsSkinned() == false);
}

TEST_CASE("palet satuan adalah bind pose, dan bind pose tidak memindahkan apa pun") {
    using namespace sim::assets;

    // **Inilah kriteria yang menjaga seluruh jalur GPU skinning.** Matriks kulit
    // adalah `global × invers bind`; pada bind pose keduanya saling meniadakan,
    // jadi paletnya satuan. Vertex yang bergeser sedikit pun di sini berarti ada
    // transpose, urutan perkalian, atau normalisasi bobot yang salah — dan di
    // viewport hal yang sama muncul sebagai karakter yang meledak, tanpa satu
    // pun galat.
    const std::vector<Mat4> palette(3, Mat4(1.0f));

    SkinInfluence influence;
    influence.bones = {0, 1, 2, 0};
    influence.weights = {0.5f, 0.25f, 0.25f, 0.0f};

    const Vec3 point(1.5f, -2.0f, 0.75f);
    const Vec3 skinned = SkinPoint(influence, palette, point);
    CHECK(skinned.x == doctest::Approx(point.x));
    CHECK(skinned.y == doctest::Approx(point.y));
    CHECK(skinned.z == doctest::Approx(point.z));

    // Arah memakai bagian 3x3 saja: translasi bone tidak boleh menggeser normal.
    std::vector<Mat4> shifted(1, glm::translate(Mat4(1.0f), Vec3(10.0f, 0.0f, 0.0f)));
    SkinInfluence single;
    single.bones = {0, 0, 0, 0};
    single.weights = {1.0f, 0.0f, 0.0f, 0.0f};
    const Vec3 normal = SkinDirection(single, shifted, Vec3(0.0f, 1.0f, 0.0f));
    CHECK(normal.x == doctest::Approx(0.0f));
    CHECK(normal.y == doctest::Approx(1.0f));
    CHECK(SkinPoint(single, shifted, Vec3(0.0f)).x == doctest::Approx(10.0f));
}

TEST_CASE("bobot mencampur matriksnya, bukan hasil tiap bone sendiri-sendiri") {
    using namespace sim::assets;

    // Dua bone yang menarik ke arah berlawanan dengan bobot sama menaruh vertex
    // tepat di tengah. Yang diuji bukan angka tengahnya melainkan bahwa yang
    // dijumlahkan adalah matriksnya — bentuk yang dijalankan `skinMatrix` di
    // `Shaders/skin_common.slang`, dan yang membuat satu perkalian
    // matriks-vektor cukup untuk keempat pengaruh.
    std::vector<Mat4> palette;
    palette.push_back(glm::translate(Mat4(1.0f), Vec3(-4.0f, 0.0f, 0.0f)));
    palette.push_back(glm::translate(Mat4(1.0f), Vec3(4.0f, 0.0f, 0.0f)));

    SkinInfluence influence;
    influence.bones = {0, 1, 0, 0};
    influence.weights = {0.5f, 0.5f, 0.0f, 0.0f};
    CHECK(SkinPoint(influence, palette, Vec3(0.0f)).x == doctest::Approx(0.0f));

    influence.weights = {0.25f, 0.75f, 0.0f, 0.0f};
    CHECK(SkinPoint(influence, palette, Vec3(0.0f)).x == doctest::Approx(2.0f));

    // Rotasi 90° di sekitar Y, seluruh bobot pada satu bone: titik di +X pindah
    // ke -Z. Kalau matriksnya tertranspose, ia pindah ke +Z — arah yang salah,
    // tanpa panjang yang berubah, dan karena itu tidak terlihat sebagai cacat
    // melainkan sebagai animasi yang "terbalik".
    std::vector<Mat4> turned(1, glm::rotate(Mat4(1.0f), glm::radians(90.0f),
                                            Vec3(0.0f, 1.0f, 0.0f)));
    SkinInfluence single;
    single.bones = {0, 0, 0, 0};
    single.weights = {1.0f, 0.0f, 0.0f, 0.0f};
    const Vec3 rotated = SkinPoint(single, turned, Vec3(1.0f, 0.0f, 0.0f));
    CHECK(rotated.x == doctest::Approx(0.0f).epsilon(1e-5f));
    CHECK(rotated.z == doctest::Approx(-1.0f).epsilon(1e-5f));
}

TEST_CASE("indeks bone di luar palet tidak membaca memori orang lain") {
    using namespace sim::assets;

    // Importir menjamin indeksnya di dalam batas, dan shader karena itu tidak
    // memeriksanya — memeriksa batas di lingkaran terdalam adalah ongkos yang
    // dibayar setiap vertex setiap frame. Yang di sini adalah jaring pengaman
    // untuk sisi CPU, supaya rig cacat menghasilkan pose yang salah alih-alih
    // pembacaan di luar batas.
    const std::vector<Mat4> palette(1, glm::translate(Mat4(1.0f), Vec3(0.0f, 5.0f, 0.0f)));
    SkinInfluence influence;
    influence.bones = {0, 9, 0, 0};
    influence.weights = {0.5f, 0.5f, 0.0f, 0.0f};

    // Setengah mengikuti bone nol, setengahnya matriks satuan.
    CHECK(SkinPoint(influence, palette, Vec3(0.0f)).y == doctest::Approx(2.5f));
}

TEST_CASE("rangka tanpa geometri sama persis dengan rangka hasil impor mesh") {
    using namespace sim::assets;

    const char* rigPath = std::getenv("SIM_RIG_FBX");
    if (rigPath == nullptr || !std::filesystem::exists(rigPath)) {
        return;
    }

    std::string meshError;
    std::string skeletonError;
    const MeshData mesh = LoadMesh(rigPath, meshError);
    const SkeletonData skeleton = LoadSkeleton(rigPath, skeletonError);
    REQUIRE(mesh.skeleton.IsValid());
    REQUIRE(skeleton.IsValid());

    // **Urutannya yang harus sama, bukan sekadar isinya.** Indeks bone di dalam
    // `SkinInfluence` menunjuk ke urutan yang dihasilkan `LoadMesh`; rangka yang
    // sama tapi berurutan lain menghasilkan pose yang benar untuk tulang yang
    // salah, dan yang terlihat adalah kulit yang terpelintir — bukan galat.
    REQUIRE(skeleton.bones.size() == mesh.skeleton.bones.size());
    for (std::size_t i = 0; i < skeleton.bones.size(); ++i) {
        INFO("bone " << i);
        CHECK(skeleton.bones[i].name == mesh.skeleton.bones[i].name);
        CHECK(skeleton.bones[i].parent == mesh.skeleton.bones[i].parent);
        CHECK(skeleton.bones[i].translation.x ==
              doctest::Approx(mesh.skeleton.bones[i].translation.x));
        CHECK(skeleton.bones[i].translation.y ==
              doctest::Approx(mesh.skeleton.bones[i].translation.y));
        CHECK(skeleton.bones[i].scale.x == doctest::Approx(mesh.skeleton.bones[i].scale.x));
    }
}

TEST_CASE("rig ber-skin terbaca dengan satuan yang sejalan antara mesh dan rangka") {
    using namespace sim::assets;

    // Berkas rig tidak ikut di repo, jadi ujinya dijalankan hanya bila
    // ditunjuk: `SIM_RIG_FBX=/path/ke/rig.fbx ctest`.
    const char* rigPath = std::getenv("SIM_RIG_FBX");
    if (rigPath == nullptr || !std::filesystem::exists(rigPath)) {
        return;
    }

    std::string error;
    const MeshData mesh = LoadMesh(rigPath, error);
    REQUIRE(mesh.IsValid());
    REQUIRE(mesh.IsSkinned());
    CHECK(mesh.skeleton.IsTopological());

    for (const SkinInfluence& influence : mesh.influences) {
        CHECK(influence.WeightSum() == doctest::Approx(1.0f).epsilon(1e-4f));
        for (int i = 0; i < kMaxInfluences; ++i) {
            if (influence.weights[i] > 0.0f) {
                CHECK(influence.bones[i] < mesh.skeleton.bones.size());
            }
        }
    }

    // **Ini uji yang menentukan, dan yang menangkap satu bug sungguhan.**
    // Konversi satuan yang dipanggang ke transform node root — yang dilakukan
    // `FbxSystemUnit::ConvertScene`, dan karena itu tidak dipakai importirnya —
    // meninggalkan transform lokal anak-anaknya dalam satuan asli. Bind pose yang
    // diambil apa adanya dari sana menghasilkan rangka seratus kali lebih besar
    // daripada kulitnya — dan kulit yang diulit rangka seratus kali terlalu
    // besar tidak menghasilkan satu pun galat, ia menghasilkan karakter yang
    // lenyap. Yang membuktikan keduanya sejalan: posisi global bone harus
    // berada di dalam kotak batas mesh-nya.
    std::vector<Mat4> global(mesh.skeleton.bones.size());
    Vec3 lowest(1e9f);
    Vec3 highest(-1e9f);
    for (std::size_t i = 0; i < mesh.skeleton.bones.size(); ++i) {
        const SkeletonBone& bone = mesh.skeleton.bones[i];
        const Mat4 local = glm::translate(Mat4(1.0f), bone.translation) *
                           glm::mat4_cast(bone.rotation) * glm::scale(Mat4(1.0f), bone.scale);
        global[i] = bone.parent >= 0 ? global[static_cast<std::size_t>(bone.parent)] * local : local;
        const Vec3 position(global[i][3]);
        lowest = glm::min(lowest, position);
        highest = glm::max(highest, position);
    }
    // Longgar sepersepuluh meter: ujung jari dan rambut boleh sedikit melewati
    // bone terluar, tapi seratus kali lipat tidak mungkin lolos.
    const float slack = 0.1f;
    CHECK(lowest.x >= mesh.boundsMin.x - slack);
    CHECK(lowest.y >= mesh.boundsMin.y - slack);
    CHECK(highest.x <= mesh.boundsMax.x + slack);
    CHECK(highest.y <= mesh.boundsMax.y + slack);
}

TEST_CASE("Mesh ber-material terbagi menjadi ruas yang berurutan tanpa celah") {
    using namespace sim::assets;

    const char* rigPath = std::getenv("SIM_RIG_FBX");
    if (rigPath == nullptr || !std::filesystem::exists(rigPath)) {
        return;
    }

    std::string error;
    const MeshData mesh = LoadMesh(rigPath, error);
    REQUIRE(mesh.IsValid());

    // Rig Mixamo punya dua node bermaterial berbeda, dan importir menggabungkan
    // seluruh node menjadi satu mesh — jadi mesh gabungannya tidak bisa lagi
    // digambar dengan satu panggilan.
    REQUIRE(mesh.materials.size() >= 2);
    REQUIRE(mesh.parts.size() >= 2);

    // **Ruasnya harus menutupi seluruh indeks, berurutan, tanpa celah maupun
    // tumpang tindih.** Celah berarti segitiga yang tidak pernah digambar;
    // tumpang tindih berarti segitiga yang digambar dua kali dengan dua
    // material — dan yang kedua terlihat sebagai z-fighting, bukan sebagai
    // kesalahan pembagian.
    uint32_t cursor = 0;
    for (const SubMesh& part : mesh.parts) {
        CHECK(part.firstIndex == cursor);
        CHECK(part.indexCount > 0);
        CHECK(part.indexCount % 3 == 0);
        CHECK(part.material >= 0);
        CHECK(part.material < static_cast<int>(mesh.materials.size()));
        cursor += part.indexCount;
    }
    CHECK(cursor == mesh.indices.size());

    // Satu ruas per material, tidak lebih: segitiga bermaterial sama harus sudah
    // dikumpulkan bersebelahan, kalau tidak dua material menjadi ratusan draw.
    CHECK(mesh.parts.size() == mesh.materials.size());

    // Indeksnya tetap menunjuk vertex yang ada sesudah disusun ulang.
    for (const uint32_t index : mesh.indices) {
        REQUIRE(index < mesh.vertices.size());
    }

    for (const MeshMaterial& material : mesh.materials) {
        INFO("material " << material.name);
        CHECK_FALSE(material.name.empty());
        CHECK(material.roughness >= 0.0f);
        CHECK(material.roughness <= 1.0f);
    }
}

TEST_CASE("Jalur tekstur dinormalkan supaya bisa dibaca di mesin lain") {
    using namespace sim::assets;

    // shaderBall memakai pemisah Windows dan jalur naik satu tingkat —
    // `..\checkerA.tga` — dan std::filesystem di Linux membaca seluruhnya
    // sebagai satu nama berkas kalau pemisahnya tidak dinormalkan.
    const std::filesystem::path ball = std::filesystem::path(SIM_MESH_DIR) / "shaderBall.fbx";
    if (!std::filesystem::exists(ball)) {
        return;
    }
    std::string error;
    const MeshData mesh = LoadMesh(ball, error);
    REQUIRE(mesh.IsValid());
    REQUIRE(mesh.materials.size() >= 2);

    int withTexture = 0;
    for (const MeshMaterial& material : mesh.materials) {
        if (material.baseColorTexture.empty()) {
            continue;
        }
        ++withTexture;
        INFO("tekstur " << material.baseColorTexture);
        CHECK(material.baseColorTexture.find('\\') == std::string::npos);
        // Relatif, bukan absolut: jalur absolut di dalam FBX menunjuk mesin
        // tempat ia diekspor dan hampir tidak pernah ada di mesin yang
        // membukanya.
        CHECK_FALSE(std::filesystem::path(material.baseColorTexture).is_absolute());
    }
    CHECK(withTexture > 0);
}

TEST_CASE("Penandaan kain disimpan di sebelah mesh dan dikunci nama material") {
    using namespace sim::assets;

    TempDir temp;
    const std::filesystem::path mesh = temp.Path() / "rig.fbx";
    WriteFile(mesh, "bukan fbx sungguhan");

    // Berkas yang belum ada berarti pengaturan kosong, bukan galat — itu keadaan
    // setiap mesh yang baru diimpor.
    MeshSettings settings;
    CHECK_FALSE(LoadMeshSettings(settings, mesh));
    CHECK(settings.Empty());
    CHECK_FALSE(settings.ClothFor("Alpha_Body_MAT"));

    settings.SetCloth("Jubah", true);
    settings.SetCloth("Alpha_Body_MAT", false);
    REQUIRE(SaveMeshSettings(settings, mesh));
    CHECK(std::filesystem::exists(MeshSettingsPath(mesh)));
    // Namanya ditempel di belakang nama lengkapnya, bukan mengganti ekstensinya:
    // rig.fbx dan rig.obj di satu folder adalah dua aset yang berbeda.
    CHECK(MeshSettingsPath(mesh).filename() == "rig.fbx.simmeshcfg");

    MeshSettings loaded;
    REQUIRE(LoadMeshSettings(loaded, mesh));
    CHECK(loaded.ClothFor("Jubah"));
    // Yang tidak menandai apa pun dibuang, supaya berkasnya tidak tumbuh oleh
    // baris yang seluruhnya bernilai bawaan.
    CHECK(loaded.parts.size() == 1);
    CHECK_FALSE(loaded.ClothFor("Alpha_Body_MAT"));
    CHECK_FALSE(loaded.ClothFor("tidak pernah disebut"));

    // Menyimpan dua kali menghasilkan byte yang sama.
    std::string first;
    std::string second;
    {
        std::ifstream in(MeshSettingsPath(mesh));
        first.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    REQUIRE(SaveMeshSettings(loaded, mesh));
    {
        std::ifstream in(MeshSettingsPath(mesh));
        second.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    CHECK(first == second);

    // Material bawaan per ruas dan nilai bawaan komponen ikut bertahan: inilah
    // yang mengisi MeshRendererComponent begitu mesh-nya ditetapkan.
    const Uuid material = Uuid::Generate();
    loaded.SetMaterial("Jubah", material);
    loaded.lodBias = 1.5f;
    loaded.castShadows = false;
    REQUIRE(SaveMeshSettings(loaded, mesh));

    MeshSettings again;
    REQUIRE(LoadMeshSettings(again, mesh));
    CHECK(again.MaterialFor("Jubah") == material);
    CHECK(again.lodBias == doctest::Approx(1.5f));
    CHECK_FALSE(again.castShadows);
    CHECK(again.receiveShadows);
    CHECK(again.MaterialFor("tidak pernah disebut").IsValid() == false);

    // Melepas penanda terakhir menghapus berkasnya: berkas pengaturan yang tidak
    // mengatur apa pun hanya menambah satu berkas yang harus ikut kontrol versi.
    // Nilai bawaan komponen ikut ditimbang — berkas yang hanya berisi lodBias
    // bukan berkas kosong.
    again.SetCloth("Jubah", false);
    again.SetMaterial("Jubah", Uuid{});
    again.lodBias = 0.0f;
    again.castShadows = true;
    REQUIRE(SaveMeshSettings(again, mesh));
    CHECK_FALSE(std::filesystem::exists(MeshSettingsPath(mesh)));
}

TEST_CASE("glTF terbaca beserta material dan ruasnya") {
    using namespace sim::assets;

    // Berkasnya tidak ikut di repo: SIM_GLTF=/path/DamagedHelmet.glb ctest
    const char* gltfPath = std::getenv("SIM_GLTF");
    if (gltfPath == nullptr || !std::filesystem::exists(gltfPath)) {
        return;
    }

    std::string error;
    const MeshData mesh = LoadMesh(gltfPath, error);
    INFO("error: " << error);
    REQUIRE(mesh.IsValid());
    CHECK(error.empty());

    // **Satu ruas per primitive**, dan itu yang membuat glTF lebih mudah
    // daripada FBX: materialnya per primitive, bukan per muka, jadi tidak ada
    // yang perlu disusun ulang.
    REQUIRE_FALSE(mesh.parts.empty());
    REQUIRE_FALSE(mesh.materials.empty());
    uint32_t cursor = 0;
    for (const SubMesh& part : mesh.parts) {
        CHECK(part.firstIndex == cursor);
        CHECK(part.indexCount % 3 == 0);
        cursor += part.indexCount;
    }
    CHECK(cursor == mesh.indices.size());
    for (const uint32_t index : mesh.indices) {
        REQUIRE(index < mesh.vertices.size());
    }

    // **Satuannya meter tanpa konversi apa pun.** glTF menetapkan tangan-kanan,
    // Y ke atas, dan meter di dalam spesifikasinya — jadi model seukuran benda
    // sungguhan harus keluar seukuran benda sungguhan, tanpa faktor seratus yang
    // dua kali menjatuhkan importir FBX.
    const Vec3 size = mesh.boundsMax - mesh.boundsMin;
    CHECK(size.x > 0.01f);
    CHECK(size.x < 100.0f);
    CHECK(size.y < 100.0f);

    // Materialnya ikut, bukan hanya geometrinya.
    bool named = false;
    bool textured = false;
    for (const MeshMaterial& material : mesh.materials) {
        INFO("material " << material.name);
        named = named || !material.name.empty();
        textured = textured || material.HasTexture();
        CHECK(material.roughness >= 0.0f);
        CHECK(material.roughness <= 1.0f);
        CHECK(material.metalness >= 0.0f);
        CHECK(material.metalness <= 1.0f);
        // Jalur teksturnya tidak pernah absolut: yang tertanam di GLB tidak
        // punya jalur sama sekali, dan yang punya URI relatif terhadap
        // berkasnya.
        CHECK_FALSE(std::filesystem::path(material.baseColorTexture).is_absolute());
    }
    CHECK(named);
    CHECK(textured);
}

TEST_CASE("Tekstur glTF yang tertanam dikeluarkan dengan nama yang dirujuk materialnya") {
    using namespace sim::assets;

    const char* gltfPath = std::getenv("SIM_GLTF");
    if (gltfPath == nullptr || !std::filesystem::exists(gltfPath)) {
        return;
    }

    TempDir temp;
    std::vector<std::string> written;
    std::string error;
    REQUIRE(ExtractGltfTextures(gltfPath, temp.Path(), written, error));
    INFO("error: " << error);
    REQUIRE_FALSE(written.empty());

    // **Namanya harus sama persis dengan yang dicatat materialnya**, karena
    // keduanya berasal dari satu fungsi penamaan. Kalau berselisih, material
    // menunjuk berkas yang tidak pernah ada — dan itu tidak menghasilkan galat,
    // hanya tekstur yang diam-diam tidak muncul.
    std::string loadError;
    const MeshData mesh = LoadMesh(gltfPath, loadError);
    REQUIRE(mesh.IsValid());
    int matched = 0;
    for (const MeshMaterial& material : mesh.materials) {
        for (const std::string* texture :
             {&material.baseColorTexture, &material.normalTexture, &material.roughnessTexture,
              &material.emissiveTexture}) {
            if (texture->empty()) {
                continue;
            }
            INFO("tekstur " << *texture);
            CHECK(std::filesystem::exists(temp.Path() / *texture));
            ++matched;
        }
    }
    CHECK(matched > 0);

    // Berkas yang sudah ada tidak ditimpa: tekstur yang sudah disunting orang
    // tidak boleh hilang karena mesh-nya diimpor ulang.
    const std::filesystem::path first = temp.Path() / written.front();
    WriteFile(first, "sudah disunting");
    std::vector<std::string> again;
    REQUIRE(ExtractGltfTextures(gltfPath, temp.Path(), again, error));
    CHECK(again.empty());
    std::ifstream check(first);
    std::string contents;
    check >> contents;
    CHECK(contents == "sudah");
}

TEST_CASE("Nama berkas dari dalam aset tidak bisa keluar dari foldernya") {
    using namespace sim::assets;

    // **Nama di dalam berkas aset adalah masukan yang tidak dipercaya.** Sebuah
    // .glb yang diunduh bisa menamai gambarnya "../../../.bashrc", dan nama itu
    // menjadi nama berkas saat gambarnya dikeluarkan — jadi membuka sebuah model
    // tidak boleh berarti mengizinkan model itu menulis ke mana pun.
    CHECK(SafeAssetFileName("albedo.png") == "albedo.png");
    CHECK(SafeAssetFileName("../../../.bashrc").empty() == false);
    CHECK(SafeAssetFileName("../../../.bashrc") == ".bashrc");
    CHECK(SafeAssetFileName("textures/albedo.png") == "albedo.png");
    // Pemisah gaya Windows disamakan lebih dulu: std::filesystem di Linux
    // membaca "..\rahasia" sebagai satu nama berkas utuh, jadi penyaring yang
    // hanya memeriksa '/' melewatkannya.
    CHECK(SafeAssetFileName("..\\..\\rahasia.png") == "rahasia.png");
    CHECK(SafeAssetFileName("/etc/passwd") == "passwd");
    CHECK(SafeAssetFileName("..").empty());
    CHECK(SafeAssetFileName(".").empty());
    CHECK(SafeAssetFileName("").empty());

    // Apa pun yang dikembalikan harus benar-benar satu komponen: menggabungnya
    // dengan sebuah folder tidak boleh menghasilkan jalur di luar folder itu.
    for (const char* hostile :
         {"../../../.bashrc", "textures/../../x.png", "..\\..\\rahasia.png", "/etc/passwd"}) {
        const std::string safe = SafeAssetFileName(hostile);
        if (safe.empty()) {
            continue;
        }
        const std::filesystem::path joined = std::filesystem::path("/tmp/project") / safe;
        INFO(hostile << " -> " << joined.string());
        CHECK(joined.parent_path() == std::filesystem::path("/tmp/project"));
    }
}

TEST_CASE("Berkas pengaturan mesh tidak ikut terdaftar sebagai aset") {
    TempDir temp;
    WriteFile(temp.Path() / "rig.fbx", "bukan fbx");
    WriteFile(temp.Path() / "rig.fbx.simmeshcfg", "{\"version\":1,\"parts\":[]}");

    AssetDatabase db;
    REQUIRE(db.Initialize({temp.Path(), nullptr, 1.0f}));
    // Berkas pengaturan hanya dibaca lewat mesh-nya. Kalau ia ikut terindeks, ia
    // mendapat GUID dan .meta sendiri, muncul di Asset Browser sebagai berkas
    // tak dikenal, dan .meta itu tertinggal yatim begitu penandaan terakhirnya
    // dilepas — karena berkas pengaturan yang kosong memang dihapus.
    CHECK(db.All().size() == 1);
    CHECK(db.FindByRelativePath("rig.fbx.simmeshcfg") == nullptr);
    CHECK_FALSE(std::filesystem::exists(temp.Path() / "rig.fbx.simmeshcfg.meta"));
}

TEST_CASE("unitCube.usda terbaca dalam meter dan Y-atas") {
    using namespace sim::assets;

    const std::filesystem::path path =
        std::filesystem::path(SIM_MESH_DIR) / "unitCube.usda";
    if (!std::filesystem::exists(path)) {
        return;  // aset opsional; ketiadaannya bukan kegagalan uji
    }

    std::string error;
    const MeshData mesh = LoadMesh(path, error);

    // Build tanpa OpenUSD menolak berkasnya dengan alasan yang menyebut mesinnya.
    // Itu jawaban yang benar untuk build itu, bukan kegagalan uji.
    if (!mesh.IsValid() && error.find("no USD support") != std::string::npos) {
        return;
    }

    REQUIRE(mesh.IsValid());
    CHECK(error.empty());
    // Enam muka segi empat, masing-masing jadi dua segitiga.
    CHECK(mesh.TriangleCount() == 12);

    // **Tiga konversi sekaligus, dan berkasnya dibuat supaya tidak ada yang bisa
    // dilewati.** Panggungnya sentimeter, sumbu atasnya Z, dan kubusnya digeser
    // oleh Xform induknya. Yang benar: kubus satu meter yang berdiri di atas
    // bidang nol. Satuan yang tidak dikonversi membuatnya seratus kali lebih
    // besar, sumbu yang tidak diputar menidurkannya, dan transform yang tidak
    // dipanggang menguburnya setengah di bawah nol.
    const Vec3 size = mesh.boundsMax - mesh.boundsMin;
    CHECK(size.x == doctest::Approx(1.0f));
    CHECK(size.y == doctest::Approx(1.0f));
    CHECK(size.z == doctest::Approx(1.0f));
    CHECK(mesh.boundsMin.y == doctest::Approx(0.0f));
    CHECK(mesh.boundsMax.y == doctest::Approx(1.0f));

    // Material terikatnya ikut terbaca, dengan nilai yang memang ada di berkasnya.
    REQUIRE(mesh.materials.size() == 1);
    CHECK(mesh.materials[0].name == "Paint");
    CHECK(mesh.materials[0].baseColor.x == doctest::Approx(0.8f));
    CHECK(mesh.materials[0].metalness == doctest::Approx(0.25f));
    CHECK(mesh.materials[0].roughness == doctest::Approx(0.6f));
}

TEST_CASE("OBJ tanpa satuan terbaca sebagai meter, bukan sentimeter") {
    using namespace sim::assets;

    const std::filesystem::path path =
        std::filesystem::path(SIM_MESH_DIR) / "unitTriangle.obj";
    if (!std::filesystem::exists(path)) {
        return;  // aset opsional; ketiadaannya bukan kegagalan uji
    }

    std::string error;
    const MeshData mesh = LoadMesh(path, error);
    REQUIRE(mesh.IsValid());
    CHECK(error.empty());
    CHECK(mesh.TriangleCount() == 1);

    // **Seratus kali, dan itu satu-satunya angka yang menarik di sini.** OBJ
    // tidak menyebut satuannya di mana pun, dan FBX SDK melaporkan panggung yang
    // tidak menyebutnya sebagai sentimeter — bawaan pustakanya, bukan sesuatu
    // yang ada di berkasnya. Importir yang mengalikannya seperti satuan
    // sungguhan menghasilkan segitiga 2 x 3 sentimeter dari berkas yang jelas
    // menuliskan 2 dan 3.
    CHECK((mesh.boundsMax.x - mesh.boundsMin.x) == doctest::Approx(2.0f));
    CHECK((mesh.boundsMax.y - mesh.boundsMin.y) == doctest::Approx(3.0f));
}

TEST_CASE("Tangent tiap vertex membentuk bingkai yang sah untuk peta normal") {
    using namespace sim::assets;

    // Ketiganya melewati importir yang berbeda — FBX, USD, dan OBJ lewat FBX
    // SDK — dan bingkai tangent harus sah di ketiganya. Peta normal dibaca di
    // ruang tangent, jadi bingkai yang miring atau merosot membelokkan cahaya
    // ke arah yang salah tanpa menghasilkan satu pun galat.
    for (const char* name : {"unitCube.usda", "unitTriangle.obj", "shaderBall.fbx"}) {
        const std::filesystem::path path = std::filesystem::path(SIM_MESH_DIR) / name;
        if (!std::filesystem::exists(path)) {
            continue;  // aset opsional
        }
        std::string error;
        const MeshData mesh = LoadMesh(path, error);
        if (!mesh.IsValid() && error.find("no USD support") != std::string::npos) {
            continue;  // build tanpa OpenUSD
        }
        INFO("mesh " << name << " error '" << error << "'");
        REQUIRE(mesh.IsValid());

        for (const MeshVertex& vertex : mesh.vertices) {
            const Vec3 tangent(vertex.tangent);
            // Sepanjang satu: bingkai yang tidak ternormalkan menskalakan arah
            // yang dibaca dari peta normal, dan yang nol menghitamkannya.
            CHECK(glm::length(tangent) == doctest::Approx(1.0f).epsilon(1e-3f));
            // Tegak lurus normalnya. Toleransinya longgar karena normal yang
            // dirata-rata di sudut tajam tidak pernah tegak lurus sempurna.
            CHECK(std::abs(glm::dot(glm::normalize(vertex.normal), tangent)) < 1e-3f);
            // Arah tangan hanya boleh +1 atau −1; nilai lain berarti `w` dipakai
            // untuk hal lain, dan bitangent yang diturunkan darinya salah skala.
            CHECK(std::abs(std::abs(vertex.tangent.w) - 1.0f) < 1e-6f);
        }
    }
}

TEST_CASE("Material impor menjadi instance dari satu induk bersama") {
    using namespace sim::assets;

    // Induknya ikut di repo, dan GUID-nya tertulis di dua tempat: berkas `.meta`
    // di sebelahnya, dan tetapan di `MaterialImport.h`. Keduanya harus sama —
    // instance yang menunjuk GUID yang tidak ada tidak menghasilkan galat, ia
    // menghasilkan material yang diam-diam memakai bawaan.
    const std::filesystem::path parentPath =
        std::filesystem::path(SIM_BUILTIN_DIR) / kImportedMaterialAsset;
    REQUIRE(std::filesystem::exists(parentPath));

    std::ifstream metaFile(parentPath.string() + ".meta");
    REQUIRE(metaFile.good());
    const std::string meta((std::istreambuf_iterator<char>(metaFile)),
                           std::istreambuf_iterator<char>());
    INFO("meta " << meta);
    CHECK(meta.find(std::string(kImportedMaterialGuid)) != std::string::npos);

    const Uuid parent = Uuid::Parse(kImportedMaterialGuid);
    REQUIRE(parent.IsValid());

    // Induknya harus benar-benar sah dan benar-benar mengekspos kelima parameter
    // yang ditimpa konverter. Instance yang menimpa parameter yang tidak ada di
    // induknya tidak menghasilkan galat — `ResolveParameters` membuangnya diam-
    // diam, dan materialnya memakai bawaan seolah impornya tidak pernah terjadi.
    sim::material::MaterialGraph master;
    const sim::material::MaterialIoResult masterIo =
        sim::material::LoadMaterialFromFile(master, parentPath);
    INFO("muat induk: " << masterIo.error);
    REQUIRE(masterIo.ok);
    CHECK(sim::material::ValidateMaterial(master).ok);
    for (const char* name : {"baseColor", "metalness", "roughness", "emissive", "opacity"}) {
        INFO("parameter " << name);
        CHECK(master.FindParameter(name) != nullptr);
    }

    MeshMaterial source;
    source.name = "Cat Merah";
    source.baseColor = Vec3(0.8f, 0.1f, 0.05f);
    source.metalness = 0.25f;
    source.roughness = 0.4f;
    source.emissive = Vec3(0.0f, 0.5f, 0.0f);
    source.opacity = 0.75f;

    const sim::material::MaterialInstance instance = MaterialInstanceFromMesh(source, parent);
    CHECK(instance.parent == parent);
    // Kelimanya ditimpa apa adanya: nilai itu dibaca dari berkas mesh-nya, jadi
    // ia pernyataan tentang materialnya — bukan medan yang dibiarkan kosong.
    REQUIRE(instance.overrides.size() == 5);

    const auto* roughness = instance.Find("roughness");
    REQUIRE(roughness != nullptr);
    // **Disalin, bukan dikuadratkan.** `specularRoughness` OpenPBR perseptual,
    // sama seperti `roughnessFactor` glTF; yang mengkuadratkannya di sini
    // membuat setiap permukaan impor terlalu mengkilap.
    CHECK(roughness->value.components[0] == doctest::Approx(0.4f));

    const auto* baseColor = instance.Find("baseColor");
    REQUIRE(baseColor != nullptr);
    CHECK(baseColor->value.kind == sim::material::ValueKind::Float3);
    CHECK(baseColor->value.components[0] == doctest::Approx(0.8f));
    CHECK(baseColor->value.components[1] == doctest::Approx(0.1f));

    const auto* opacity = instance.Find("opacity");
    REQUIRE(opacity != nullptr);
    CHECK(opacity->value.components[0] == doctest::Approx(0.75f));
}

TEST_CASE("Tiap material mesh ditulis sebagai satu berkas .simmatinst") {
    using namespace sim::assets;

    TempDir temp;
    MeshData mesh;
    MeshMaterial a;
    a.name = "lambert1";
    a.roughness = 0.2f;
    MeshMaterial b;
    b.name = "lambert1";  // nama yang sama: tidak boleh saling menimpa
    b.roughness = 0.9f;
    MeshMaterial c;
    c.name = "../../lolos";  // nama dari berkas orang lain, bukan jalur
    mesh.materials = {a, b, c};

    const Uuid parent = Uuid::Parse(kImportedMaterialGuid);
    std::vector<std::string> written;
    std::string error;
    REQUIRE(WriteMaterialInstances(mesh, temp.Path(), parent, written, error));
    CHECK(error.empty());
    REQUIRE(written.size() == 3);

    CHECK(written[0] == "lambert1.simmatinst");
    CHECK(written[1] == "lambert1 2.simmatinst");
    // Nama yang mengandung `../` tidak boleh menjadi jalur naik.
    CHECK(written[2].find('/') == std::string::npos);
    CHECK(written[2].find("..") == std::string::npos);

    for (const std::string& name : written) {
        const std::filesystem::path path = temp.Path() / name;
        INFO("berkas " << path.string());
        REQUIRE(std::filesystem::exists(path));
        sim::material::MaterialInstance loaded;
        const sim::material::MaterialIoResult result =
            sim::material::LoadInstanceFromFile(loaded, path);
        REQUIRE(result.ok);
        CHECK(loaded.parent == parent);
    }

    // Isinya benar-benar berbeda, bukan dua salinan berkas yang sama.
    sim::material::MaterialInstance first;
    sim::material::MaterialInstance second;
    REQUIRE(sim::material::LoadInstanceFromFile(first, temp.Path() / written[0]).ok);
    REQUIRE(sim::material::LoadInstanceFromFile(second, temp.Path() / written[1]).ok);
    CHECK(first.Find("roughness")->value.components[0] == doctest::Approx(0.2f));
    CHECK(second.Find("roughness")->value.components[0] == doctest::Approx(0.9f));
}

TEST_CASE("thumbnail dibuat dari berkas gambar dan di-cache") {
    // Titik dekode ini dialihkan lewat `Sim::ImageIO` di I0. Sebelumnya tidak
    // ada uji yang menyentuhnya sama sekali, jadi tidak ada yang bisa
    // membedakan "masih bekerja" dari "sudah lama rusak".
    const std::filesystem::path source =
        std::filesystem::path(SIM_IMAGE_DIR) / "checker.png";
    TempDir cache;

    const ThumbnailImage thumbnail = MakeThumbnail(source, cache.Path(), 4);
    REQUIRE(thumbnail.IsValid());
    // Sumbernya 8x8; dikecilkan ke kotak 4 menghasilkan 4x4 dengan proporsi
    // terjaga.
    CHECK(thumbnail.width == 4);
    CHECK(thumbnail.height == 4);
    // Selalu RGBA8, apa pun jumlah kanal berkas asalnya — berkas ini RGB.
    CHECK(thumbnail.rgba.size() == 4u * 4u * 4u);

    // Gambar RGB tanpa alfa harus keluar opak. Alfa nol di sini membuat setiap
    // thumbnail tekstur tanpa alfa menghilang tanpa satu pun galat.
    for (std::size_t i = 0; i < 16; ++i) {
        CHECK(thumbnail.rgba[i * 4 + 3] == 255);
    }

    SUBCASE("ukuran yang tidak mengecilkan disalin apa adanya") {
        const ThumbnailImage full = MakeThumbnail(source, cache.Path(), 16);
        REQUIRE(full.IsValid());
        // maxSize di atas ukuran sumbernya tidak memperbesar: skalanya dijepit
        // ke 1. Thumbnail yang diperbesar hanya membuang memori.
        CHECK(full.width == 8);
        CHECK(full.height == 8);
        // piksel(x,y) = (x*32, y*32, (x+y) genap ? 255 : 0), lolos tanpa
        // penskalaan sama sekali.
        CHECK(full.rgba[0] == 0);
        CHECK(full.rgba[2] == 255);
        CHECK(full.rgba[4] == 32);
        CHECK(full.rgba[6] == 0);
    }

    SUBCASE("panggilan kedua dilayani cache dan hasilnya identik") {
        // Kunci cache adalah hash isi berkas. Yang diuji: jalur cache
        // mengembalikan piksel yang sama, bukan piksel yang mirip.
        const ThumbnailImage cached = MakeThumbnail(source, cache.Path(), 4);
        REQUIRE(cached.IsValid());
        CHECK(cached.width == thumbnail.width);
        CHECK(cached.height == thumbnail.height);
        CHECK(cached.rgba == thumbnail.rgba);
    }

    SUBCASE("berkas yang bukan gambar ditolak, bukan crash") {
        const std::filesystem::path text = cache.Path() / "bukan-gambar.png";
        std::ofstream(text) << "ini teks biasa";
        CHECK_FALSE(MakeThumbnail(text, cache.Path(), 4).IsValid());
    }
}

TEST_CASE("unitSphere.obj cocok dengan collider Sphere yang dipasangkan padanya") {
    using namespace sim::assets;

    // **Aset yang dikirim, dan sebuah prefab bergantung padanya.** "Physics
    // Sphere" menggambar mesh ini sambil menabrak dengan `ColliderComponent`
    // berjari-jari 0,5 — dan bentuk yang digambar tidak cocok dengan yang
    // ditabrak tidak pernah tampak sebagai galat, hanya sebagai benda yang
    // berhenti melayang atau setengah tenggelam.
    const std::filesystem::path path = std::filesystem::path(SIM_MESH_DIR) / "unitSphere.obj";
    REQUIRE(std::filesystem::exists(path));

    std::string error;
    const MeshData mesh = LoadMesh(path, error);
    REQUIRE(mesh.IsValid());
    CHECK(error.empty());
    CHECK(mesh.TriangleCount() > 500);

    // Diameter satu meter di ketiga sumbu: bola, bukan telur.
    const Vec3 size = mesh.boundsMax - mesh.boundsMin;
    CHECK(size.x == doctest::Approx(1.0f).epsilon(0.02));
    CHECK(size.y == doctest::Approx(1.0f).epsilon(0.02));
    CHECK(size.z == doctest::Approx(1.0f).epsilon(0.02));

    // Berpusat di titik asal, karena `ColliderComponent::offset` bawaannya nol.
    // Mesh yang berdiri di atas nol akan tampak tertanam separuh di lantai.
    CHECK((mesh.boundsMin.y + mesh.boundsMax.y) == doctest::Approx(0.0f).epsilon(0.02));
    CHECK((mesh.boundsMin.x + mesh.boundsMax.x) == doctest::Approx(0.0f).epsilon(0.02));
}

TEST_CASE("unitCylinder.obj cocok dengan collider Cylinder yang dipasangkan padanya") {
    using namespace sim::assets;

    // Aset yang dikirim, dan sebuah prefab bergantung padanya. Sumbunya +X —
    // sama seperti kapsul dan silinder di `ColliderComponent` — jadi ia panjang
    // pada X dan bundar pada YZ. Sumbu yang tertukar tidak terlihat sebagai
    // galat, hanya sebagai tabung yang berbaring ke arah yang salah.
    const std::filesystem::path path = std::filesystem::path(SIM_MESH_DIR) / "unitCylinder.obj";
    REQUIRE(std::filesystem::exists(path));

    std::string error;
    const MeshData mesh = LoadMesh(path, error);
    REQUIRE(mesh.IsValid());
    CHECK(error.empty());
    CHECK(mesh.TriangleCount() > 100);

    const Vec3 size = mesh.boundsMax - mesh.boundsMin;
    CHECK(size.x == doctest::Approx(1.0f).epsilon(0.02));   // tinggi penuh
    CHECK(size.y == doctest::Approx(1.0f).epsilon(0.02));   // diameter
    CHECK(size.z == doctest::Approx(1.0f).epsilon(0.02));

    // Berpusat di titik asal, karena `ColliderComponent::offset` bawaannya nol.
    CHECK((mesh.boundsMin.x + mesh.boundsMax.x) == doctest::Approx(0.0f).epsilon(0.02));
    CHECK((mesh.boundsMin.y + mesh.boundsMax.y) == doctest::Approx(0.0f).epsilon(0.02));
}

TEST_CASE("L6c: material yang dipakai layer terrain terhitung terpakai") {
    using namespace sim::assets;

    // **Ditemukan lewat smoke test, bukan lewat membaca kode.** `.simterrain`
    // menyebut material tiap layernya sebagai GUID, tetapi importir dokumen
    // tidak mengenal jenis aset itu — jadi materialnya tidak punya satu pun
    // pemakai yang tercatat, dan menghapusnya tidak memicu peringatan apa pun.
    TempDir temp;
    WriteFile(temp.Path() / "Batu.simmat", R"({"nodes":[]})");

    AssetDatabase db;
    REQUIRE(db.Initialize({temp.Path(), nullptr, 1.0f}));
    const AssetRecord* material = db.FindByRelativePath("Batu.simmat");
    REQUIRE(material != nullptr);
    const Uuid materialGuid = material->guid;

    // Terrain yang salah satu layernya memakai material itu.
    const std::string terrain = R"({"version":2,"name":"Padang","tileSamples":16,)"
                                R"("tilesX":1,"tilesY":1,"sampleSpacing":1.0,)"
                                R"("minHeight":0.0,"maxHeight":100.0,"baseHeight":0.0,)"
                                R"("heightmap":"Padang_height.png","layers":[)"
                                R"({"name":"Base","material":")" +
                                materialGuid.ToString() + R"("}]})";
    WriteFile(temp.Path() / "Padang.simterrain", terrain);
    db.ScanNow();

    const AssetRecord* record = db.FindByRelativePath("Padang.simterrain");
    REQUIRE(record != nullptr);
    CHECK(record->type == AssetType::Terrain);

    const std::vector<Uuid> users = db.UsersOf(materialGuid);
    INFO(users.size() << " pemakai");
    REQUIRE(users.size() == 1);
    CHECK(users.front() == record->guid);
}

TEST_CASE("E8.4: statistik mesh dilaporkan dari yang memuatnya dan bertahan lintas pemindaian") {
    using namespace sim::assets;

    // **Bukan dihitung importir.** Mengurai FBX saat impor berarti 118 ms per
    // berkas, dan pemindaian pertama sebuah project berjalan sinkron — seratus
    // mesh berarti sepuluh detik sebelum jendela pertama muncul.
    TempDir temp;
    WriteFile(temp.Path() / "batu.obj", "v 0 0 0\n");

    AssetDatabase db;
    REQUIRE(db.Initialize({temp.Path(), nullptr, 1.0f}));
    const AssetRecord* record = db.FindByRelativePath("batu.obj");
    REQUIRE(record != nullptr);
    const Uuid guid = record->guid;

    // Belum pernah dimuat: nol berarti "belum diketahui", bukan nol segitiga.
    CHECK(record->triangleCount == 0);
    CHECK(record->vertexCount == 0);

    db.ReportMeshStats(guid, 67832, 35897);
    // Terlihat frame ini juga, bukan setelah pemindaian berikutnya kebetulan
    // berjalan.
    REQUIRE(db.Find(guid) != nullptr);
    CHECK(db.Find(guid)->triangleCount == 67832);
    CHECK(db.Find(guid)->vertexCount == 35897);

    // **Dan bertahan.** Isi indeks ditukar utuh tiap pemindaian; angka yang
    // hanya ditulis ke record akan hilang beberapa detik kemudian tanpa ada yang
    // menyadarinya. Berkas kedua memaksa pemindaian menghasilkan daftar baru.
    WriteFile(temp.Path() / "kayu.obj", "v 1 0 0\n");
    db.ScanNow();
    REQUIRE(db.Find(guid) != nullptr);
    CHECK(db.Find(guid)->triangleCount == 67832);
    CHECK(db.Find(guid)->vertexCount == 35897);
    // Yang lain tetap kosong — laporan satu mesh tidak mewarnai seluruh indeks.
    const AssetRecord* other = db.FindByRelativePath("kayu.obj");
    REQUIRE(other != nullptr);
    CHECK(other->triangleCount == 0);

    // Laporan yang tidak masuk akal ditolak diam-diam: nol segitiga berarti
    // "tidak tahu", dan menuliskannya akan menghapus angka yang sudah benar.
    db.ReportMeshStats(guid, 0, 0);
    CHECK(db.Find(guid)->triangleCount == 67832);
}

TEST_CASE("Rekomendasi 3: material impor menyebut teksturnya lewat parameter induk") {
    using namespace sim::assets;

    // Jalur teksturnya diselesaikan pemanggil, bukan di sini: ia relatif
    // terhadap berkas mesh dan kerap naik satu tingkat, jadi menyelesaikannya
    // menuntut tahu di mana berkas mesh itu berada.
    MeshData mesh;
    mesh.vertices.resize(3);
    mesh.indices = {0, 1, 2};
    MeshMaterial withTexture;
    withTexture.name = "Aspal";
    withTexture.baseColor = Vec3(0.5f, 0.5f, 0.5f);
    withTexture.baseColorTexture = "../tekstur/aspal.png";
    MeshMaterial plain;
    plain.name = "Kaca";
    mesh.materials = {withTexture, plain};

    const Uuid parent = Uuid::Parse(kImportedMaterialGuid);
    const Uuid image = Uuid::Generate();

    std::string asked;
    const TextureResolver resolver = [&](std::string_view path) {
        asked = std::string(path);
        return image;
    };

    const material::MaterialInstance textured =
        MaterialInstanceFromMesh(withTexture, parent, resolver);
    CHECK(asked == "../tekstur/aspal.png");
    CHECK(textured.Texture(kBaseColorTextureParameter) == image);
    // Parameter skalarnya tetap ikut — tekstur menambah, bukan menggantikan.
    CHECK(textured.overrides.size() == 5);

    // Material tanpa tekstur tidak menulis parameter itu sama sekali: yang
    // kosong berarti "pakai bawaan induk", yaitu putih — dan itu persis
    // perilaku material tanpa tekstur.
    const material::MaterialInstance untouched = MaterialInstanceFromMesh(plain, parent, resolver);
    CHECK(untouched.textures.empty());

    // Tanpa resolver sama sekali — jalur yang dipakai pemanggil yang tidak bisa
    // menyalin apa pun — materialnya tetap ditulis, hanya tanpa tekstur.
    const material::MaterialInstance noResolver = MaterialInstanceFromMesh(withTexture, parent);
    CHECK(noResolver.textures.empty());
    CHECK(noResolver.overrides.size() == 5);

    // Resolver yang gagal menyelesaikan jalurnya juga tidak menulis apa pun.
    const material::MaterialInstance missing =
        MaterialInstanceFromMesh(withTexture, parent, [](std::string_view) { return Uuid{}; });
    CHECK(missing.textures.empty());

    // Dan berkasnya benar-benar membawanya.
    TempDir temp;
    std::vector<std::string> written;
    std::string error;
    REQUIRE_MESSAGE(
        WriteMaterialInstances(mesh, temp.Path(), parent, written, error, resolver), error);
    REQUIRE(written.size() == 2);
    material::MaterialInstance reloaded;
    REQUIRE(material::LoadInstanceFromFile(reloaded, temp.Path() / written[0]).ok);
    CHECK(reloaded.Texture(kBaseColorTextureParameter) == image);
}

TEST_CASE("T1: pengaturan tekstur bolak-balik lewat berkas tanpa berubah") {
    using namespace sim::assets;

    TempDir temp;
    const std::filesystem::path texture = temp.Path() / "batu.png";
    WriteFile(texture, "bukan png sungguhan");

    // **Yang belum punya berkas pengaturan memakai bawaan, dan itu bukan
    // galat** — itu keadaan setiap tekstur yang baru diimpor.
    TextureSettings loaded;
    CHECK(LoadTextureSettings(loaded, texture));
    CHECK(loaded.usage == TextureUsage::Color);
    CHECK(loaded.compress);
    CHECK(loaded.generateMips);
    CHECK(loaded == DefaultTextureSettings(texture));
    CHECK_FALSE(std::filesystem::exists(TextureSettingsPath(texture)));

    // Menyimpan yang seluruhnya bawaan **tidak menulis berkas**: berkas yang
    // tidak mengatur apa pun hanya menambah satu berkas yang harus ikut kontrol
    // versi.
    CHECK(SaveTextureSettings(DefaultTextureSettings(texture), texture));
    CHECK_FALSE(std::filesystem::exists(TextureSettingsPath(texture)));

    TextureSettings settings;
    settings.usage = TextureUsage::NormalMap;
    settings.quality = TextureQuality::Best;
    settings.alpha = TextureAlpha::PunchThrough;
    settings.compress = false;
    settings.generateMips = false;
    REQUIRE(SaveTextureSettings(settings, texture));
    REQUIRE(std::filesystem::exists(TextureSettingsPath(texture)));

    TextureSettings back;
    REQUIRE(LoadTextureSettings(back, texture));
    CHECK(back == settings);

    // Menyimpan dua kali menghasilkan byte yang sama: berkas yang urutannya
    // berubah tiap simpan menghasilkan diff yang tidak membawa informasi.
    const auto readAll = [](const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
    };
    const std::string first = readAll(TextureSettingsPath(texture));
    REQUIRE(SaveTextureSettings(back, texture));
    CHECK(readAll(TextureSettingsPath(texture)) == first);

    // Nilainya ditulis sebagai **nama**, bukan angka: berkas yang ikut kontrol
    // versi dibaca manusia, dan nomor yang bergeser saat sebuah nilai disisipkan
    // mengubah arti setiap berkas yang sudah ada tanpa satu pun tanda.
    INFO(first);
    CHECK(first.find("\"normal\"") != std::string::npos);
    CHECK(first.find("\"best\"") != std::string::npos);

    // Kembali ke bawaan **menghapus** berkasnya, meninggalkan folder seperti
    // sebelum ada yang menyentuhnya.
    REQUIRE(SaveTextureSettings(DefaultTextureSettings(texture), texture));
    CHECK_FALSE(std::filesystem::exists(TextureSettingsPath(texture)));

    // Dua tekstur bernama sama berbeda ekstensi adalah dua aset, dan pengaturan
    // yang dibagi keduanya akan membuat yang satu diam-diam mengubah yang lain.
    CHECK(TextureSettingsPath(temp.Path() / "batu.png") !=
          TextureSettingsPath(temp.Path() / "batu.tga"));
}

TEST_CASE("T1: usage ditebak dari nama sebagai tebakan, bukan sebagai kebenaran") {
    using namespace sim::assets;

    CHECK(GuessUsageFromName("batu_n.png") == TextureUsage::NormalMap);
    CHECK(GuessUsageFromName("batu_normal.png") == TextureUsage::NormalMap);
    CHECK(GuessUsageFromName("Batu_NRM.TGA") == TextureUsage::NormalMap);
    CHECK(GuessUsageFromName("batu-normal.png") == TextureUsage::NormalMap);
    CHECK(GuessUsageFromName("batu_rough.png") == TextureUsage::Mask);
    CHECK(GuessUsageFromName("batu_ao.png") == TextureUsage::Mask);
    CHECK(GuessUsageFromName("batu_orm.png") == TextureUsage::Mask);
    CHECK(GuessUsageFromName("batu_height.png") == TextureUsage::Height);
    CHECK(GuessUsageFromName("langit_hdr.exr") == TextureUsage::Hdr);

    // Yang tidak berakhiran apa pun adalah warna — bawaan yang benar untuk
    // kebanyakan tekstur.
    CHECK(GuessUsageFromName("batu.png") == TextureUsage::Color);
    CHECK(GuessUsageFromName("kayu_albedo.png") == TextureUsage::Color);

    // **Akhiran dicocokkan sebagai kata, bukan sebagai potongan huruf.** `_n`
    // yang dicocokkan apa adanya akan ikut mengenai nama yang kebetulan
    // berakhiran huruf itu — dan yang tertandai normal map adalah tekstur warna
    // yang lalu tampak biru pekat.
    CHECK(GuessUsageFromName("kayu_batan.png") == TextureUsage::Color);
    CHECK(GuessUsageFromName("beton.png") == TextureUsage::Color);
    CHECK(GuessUsageFromName("gerobak_karam.png") == TextureUsage::Color);

    // Tanpa ekstensi pun terbaca: yang menebak dari nama utuh tidak akan pernah
    // mengenali satu pun akhiran, karena semuanya berakhiran ".png".
    CHECK(GuessUsageFromName("batu_n") == TextureUsage::NormalMap);
}

TEST_CASE("T1: tebakan nama bisa ditolak, dan penolakannya bertahan") {
    using namespace sim::assets;

    // **Yang tidak bisa ditolak bukan tebakan lagi.** Tekstur bernama
    // `batu_n.png` ditebak normal map; pengguna yang tahu itu sebenarnya warna
    // harus bisa menyetelnya ke `Color` — dan setelan itu harus bertahan.
    //
    // Versi pertama mekanisme ini membandingkan terhadap bawaan struct, bukan
    // terhadap bawaan berkasnya: menyetel ke `Color` berarti "sama dengan
    // bawaan", berkasnya dihapus, dan tebakan namanya kembali memaksa
    // `NormalMap` pada pemuatan berikutnya.
    TempDir temp;
    const std::filesystem::path texture = temp.Path() / "batu_n.png";
    WriteFile(texture, "bukan png sungguhan");

    CHECK(DefaultTextureSettings(texture).usage == TextureUsage::NormalMap);

    TextureSettings loaded;
    REQUIRE(LoadTextureSettings(loaded, texture));
    CHECK(loaded.usage == TextureUsage::NormalMap);

    // Ditolak: pengguna menyetelnya ke warna.
    TextureSettings asColor = loaded;
    asColor.usage = TextureUsage::Color;
    REQUIRE(SaveTextureSettings(asColor, texture));
    // Berkasnya **harus** ada — inilah yang membedakan "sama dengan bawaan"
    // dari "sama dengan bawaan struct".
    CHECK(std::filesystem::exists(TextureSettingsPath(texture)));

    TextureSettings back;
    REQUIRE(LoadTextureSettings(back, texture));
    CHECK(back.usage == TextureUsage::Color);

    // Dan mengembalikannya ke tebakan menghapus berkasnya lagi.
    REQUIRE(SaveTextureSettings(DefaultTextureSettings(texture), texture));
    CHECK_FALSE(std::filesystem::exists(TextureSettingsPath(texture)));
    REQUIRE(LoadTextureSettings(back, texture));
    CHECK(back.usage == TextureUsage::NormalMap);
}

// ---------------------------------------------------------------------------
// T2 — baker tekstur
// ---------------------------------------------------------------------------

namespace {

std::filesystem::path TextureFixture(const char* name) {
    return std::filesystem::path(SIM_IMAGE_DIR) / name;
}

/// Menyalin sebuah berkas, dipakai uji yang perlu mengubah isinya.
void CopyFile(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code error;
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, error);
    REQUIRE_MESSAGE(!error, error.message());
}

}  // namespace

TEST_CASE("T2: hasil bake dibaca utuh oleh pembaca KTX2 yang lain") {
    // **Yang menulis libktx, yang membaca implementasi tangan di Sim::RHI.**
    // Round-trip terhadap penulis sendiri hanya membuktikan konsistensi dengan
    // diri sendiri — berkas yang bukan KTX2 sah pun akan lulus. Yang dibuktikan
    // di sini adalah bahwa kedua sisi sepakat tentang tata letak berkasnya.
    TempDir temp;
    TextureSettings settings;
    settings.usage = TextureUsage::Color;
    // Tanpa kompresi di sini, supaya yang diperiksa adalah tata letak RGBA8 yang
    // ukurannya bisa dihitung tangan. Varian ber-BC7-nya diuji terpisah di
    // bawah, dan keduanya perlu: jalur blok dan jalur mentah menempuh cabang
    // yang berbeda di dalam libktx.
    settings.compress = false;

    const BakeResult result = BakeTexture(TextureFixture("checker.png"), settings, temp.Path());
    REQUIRE_MESSAGE(result.ok, result.error);
    CHECK_FALSE(result.fromCache);
    CHECK(result.width == 8);
    CHECK(result.height == 8);
    // 8x8 turun sampai 1x1: 8, 4, 2, 1.
    CHECK(result.levelCount == 4);

    rhi::Ktx2Texture texture;
    const rhi::Ktx2Result read = rhi::ReadKtx2(result.path, texture);
    REQUIRE_MESSAGE(read.ok, read.error);
    CHECK(texture.width == 8);
    CHECK(texture.height == 8);
    CHECK(texture.levels.size() == 4);
    CHECK(texture.format == result.vkFormat);

    // Tiap level punya byte sebanyak yang dituntut dimensinya, dan rentangnya
    // benar-benar di dalam berkas. Level yang menunjuk ke luar dikembalikan
    // sebagai span kosong oleh pembacanya, jadi ukuran nol di sini berarti
    // penulisnya menaruh offset yang salah.
    for (std::size_t level = 0; level < texture.levels.size(); ++level) {
        const uint32_t side = 8u >> level;
        INFO("level " << level);
        CHECK(texture.levels[level].width == side);
        CHECK(texture.levels[level].height == side);
        CHECK(texture.LevelBytes(level).size() == static_cast<std::size_t>(side) * side * 4);
    }
}

TEST_CASE("T2: bake kedua tidak mengerjakan apa pun, dan pengaturan yang berubah mengerjakannya lagi") {
    // Dihitung, bukan diukur. Pemanggilan kedua yang lebih cepat juga dihasilkan
    // cache halaman sistem berkas, dan itu membuktikan hal yang lain.
    TempDir temp;
    const std::filesystem::path source = temp.Path() / "batu.png";
    CopyFile(TextureFixture("checker.png"), source);

    TextureSettings settings;
    settings.usage = TextureUsage::Color;
    const std::filesystem::path cache = temp.Path() / "cache";

    const uint64_t before = TextureBakeCount();
    const BakeResult first = BakeTexture(source, settings, cache);
    REQUIRE_MESSAGE(first.ok, first.error);
    CHECK_FALSE(first.fromCache);
    CHECK(TextureBakeCount() == before + 1);

    const BakeResult second = BakeTexture(source, settings, cache);
    REQUIRE_MESSAGE(second.ok, second.error);
    CHECK(second.fromCache);
    CHECK(second.path == first.path);
    CHECK(TextureBakeCount() == before + 1);

    // Pengaturan yang berubah adalah kunci yang berbeda. Inilah yang membuat
    // "tandai perlu di-bake ulang" tidak perlu ditulis sama sekali — bendera
    // terpisah hanya akan menjadi keadaan kedua yang bisa berselisih dengan yang
    // pertama.
    settings.quality = TextureQuality::Best;
    const BakeResult reconfigured = BakeTexture(source, settings, cache);
    REQUIRE_MESSAGE(reconfigured.ok, reconfigured.error);
    CHECK_FALSE(reconfigured.fromCache);
    CHECK(reconfigured.path != first.path);
    CHECK(TextureBakeCount() == before + 2);

    // Dan isi berkas yang berubah juga, meski namanya sama persis. Cache
    // berkunci jalur akan mengembalikan tekstur basi di sini, dan yang basi itu
    // terlihat benar.
    settings.quality = TextureQuality::Balanced;
    CopyFile(TextureFixture("albedo-srgb.png"), source);
    const BakeResult replaced = BakeTexture(source, settings, cache);
    REQUIRE_MESSAGE(replaced.ok, replaced.error);
    CHECK_FALSE(replaced.fromCache);
    CHECK(replaced.path != first.path);
    CHECK(TextureBakeCount() == before + 3);
}

TEST_CASE("T2: base color memakai format sRGB, normal map tidak pernah") {
    // Jebakan nomor dua di rencananya: `_SRGB` dan `_UNORM` berisi bit yang
    // identik, dan yang membedakan hanya tafsir sampler. Normal map yang
    // ber-format `_SRGB` menghasilkan pencahayaan yang salah sedikit di seluruh
    // permukaan — tanpa satu pun peringatan.
    TempDir temp;
    const std::filesystem::path source = TextureFixture("checker.png");

    // Jalur tanpa kompresi: `_SRGB` versus `_UNORM` pada format RGBA8 yang sama.
    // Pasangan ber-BC-nya diuji di "tabel format" di bawah.
    TextureSettings color;
    color.usage = TextureUsage::Color;
    color.compress = false;
    const BakeResult asColor = BakeTexture(source, color, temp.Path() / "color");
    REQUIRE_MESSAGE(asColor.ok, asColor.error);

    TextureSettings normal;
    normal.usage = TextureUsage::NormalMap;
    normal.compress = false;
    const BakeResult asNormal = BakeTexture(source, normal, temp.Path() / "normal");
    REQUIRE_MESSAGE(asNormal.ok, asNormal.error);

    // VK_FORMAT_R8G8B8A8_SRGB dan VK_FORMAT_R8G8B8A8_UNORM. Angkanya ditulis
    // apa adanya karena spesifikasi KTX2 memang menyimpan nomor VkFormat, dan
    // uji ini akan gagal kalau nomornya bergeser — yang memang harus.
    CHECK(asColor.vkFormat == 43);
    CHECK(asNormal.vkFormat == 37);
    CHECK(asColor.vkFormat != asNormal.vkFormat);
}

// ---------------------------------------------------------------------------
// T2 — kompresi blok
// ---------------------------------------------------------------------------

namespace {

/// Pola RGBA8 yang tepi kanan dan bawahnya jauh berbeda dari bagian dalamnya —
/// justru bagian yang menentukan apa yang terjadi pada blok tepi.
uint8_t EdgePattern(uint32_t x, uint32_t y, uint32_t channel) {
    const uint32_t base = (x >= 4 ? 230u : 25u) + (y >= 2 ? 15u : 0u);
    return static_cast<uint8_t>(channel == 3 ? 255u : base + channel * 7u);
}

std::vector<uint8_t> MakePattern(uint32_t width, uint32_t height, uint32_t sourceWidth,
                                 uint32_t sourceHeight) {
    std::vector<uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            // Dijepit ke ukuran sumbernya: inilah "ulangi tepinya", ditulis di
            // sisi uji supaya harapannya tidak datang dari kode yang diuji.
            const uint32_t sx = std::min(x, sourceWidth - 1);
            const uint32_t sy = std::min(y, sourceHeight - 1);
            for (uint32_t c = 0; c < 4; ++c) {
                pixels[(static_cast<std::size_t>(y) * width + x) * 4 + c] = EdgePattern(sx, sy, c);
            }
        }
    }
    return pixels;
}

}  // namespace

TEST_CASE("T2: blok tepi mengulang piksel tepinya, bukan menghitamkannya") {
    // Jebakan nomor lima di rencananya. Dimensi bukan kelipatan empat
    // menyisakan blok yang sebagian pikselnya di luar gambar, dan isi piksel itu
    // ikut menentukan endpoint bloknya — jadi ikut menentukan piksel yang
    // benar-benar terlihat. Hitam di sana menarik endpoint ke bawah dan
    // menggelapkan tepi kanan dan bawah setiap tekstur berukuran ganjil.
    constexpr uint32_t kWidth = 5;
    constexpr uint32_t kHeight = 3;
    const std::vector<uint8_t> source = MakePattern(kWidth, kHeight, kWidth, kHeight);
    // Gambar yang sama, sudah dijepit tangan sampai batas bloknya.
    const std::vector<uint8_t> extended = MakePattern(8, 4, kWidth, kHeight);

    CompressOptions options;
    options.format = BlockFormat::Bc7;
    options.quality = TextureQuality::Balanced;

    std::vector<uint8_t> fromSource;
    std::vector<uint8_t> fromExtended;
    REQUIRE(CompressBlocks(options, source, kWidth, kHeight, 4, fromSource));
    REQUIRE(CompressBlocks(options, extended, 8, 4, 4, fromExtended));

    // Dua blok mendatar, satu menurun, enam belas byte masing-masing.
    CHECK(fromSource.size() == 32);
    CHECK(CompressedSize(BlockFormat::Bc7, kWidth, kHeight) == 32);
    // Byte per byte. Encoder yang memadatkan dengan nol menghasilkan endpoint
    // yang berbeda, jadi blok yang berbeda — dan perbandingan ini tidak bisa
    // lulus karena toleransi, karena tidak ada toleransi.
    CHECK(fromSource == fromExtended);
}

TEST_CASE("T2: kompresi bolak-balik menjaga gambarnya") {
    // Encoder yang menghasilkan blok berukuran benar tapi isinya kacau lolos
    // setiap pemeriksaan ukuran. Yang memeriksanya adalah menguraikannya kembali
    // dan membandingkan angkanya.
    constexpr uint32_t kSide = 16;
    std::vector<uint8_t> source(static_cast<std::size_t>(kSide) * kSide * 4);
    for (uint32_t y = 0; y < kSide; ++y) {
        for (uint32_t x = 0; x < kSide; ++x) {
            uint8_t* pixel = source.data() + (static_cast<std::size_t>(y) * kSide + x) * 4;
            pixel[0] = static_cast<uint8_t>(x * 16);
            pixel[1] = static_cast<uint8_t>(y * 16);
            pixel[2] = static_cast<uint8_t>(128);
            pixel[3] = 255;
        }
    }

    CompressOptions options;
    options.format = BlockFormat::Bc7;
    options.quality = TextureQuality::Best;

    std::vector<uint8_t> blocks;
    REQUIRE(CompressBlocks(options, source, kSide, kSide, 4, blocks));
    CHECK(blocks.size() == 16 * 16);  // 4x4 blok, 16 byte

    std::vector<uint8_t> decoded;
    REQUIRE(DecompressBlocks(BlockFormat::Bc7, blocks, kSide, kSide, decoded));
    REQUIRE(decoded.size() == source.size());

    int worst = 0;
    for (std::size_t i = 0; i < source.size(); ++i) {
        worst = std::max(worst, std::abs(static_cast<int>(decoded[i]) - source[i]));
    }
    INFO("selisih kanal terbesar " << worst);
    // Dua puluh, bukan lebih ketat: merah berubah menurut x dan hijau menurut y,
    // jadi setiap blok memuat variasi dua arah — dan sebuah blok BC7 menyimpan
    // satu garis di ruang warna. Yang tidak bisa diwakili garis itu adalah
    // selisih yang tersisa, dan besarnya memang belasan.
    //
    // Batasnya tetap berarti: blok yang tertukar tempat menggeser gradien satu
    // blok penuh — enam puluh empat tingkat — dan baris yang tergeser lebih
    // jauh lagi.
    CHECK(worst <= 20);
}

TEST_CASE("T2: tabel format — warna ke BC7, normal ke BC5, mask ke BC4") {
    TempDir temp;
    const std::filesystem::path color = TextureFixture("checker.png");

    TextureSettings settings;
    settings.compress = true;

    settings.usage = TextureUsage::Color;
    const BakeResult asColor = BakeTexture(color, settings, temp.Path() / "c");
    REQUIRE_MESSAGE(asColor.ok, asColor.error);
    // VK_FORMAT_BC7_SRGB_BLOCK. Warna, jadi sRGB — dan itu tafsir sampler,
    // bukan konversi: bitnya identik dengan varian UNORM-nya.
    CHECK(asColor.vkFormat == 146);

    settings.usage = TextureUsage::NormalMap;
    const BakeResult asNormal = BakeTexture(color, settings, temp.Path() / "n");
    REQUIRE_MESSAGE(asNormal.ok, asNormal.error);
    // VK_FORMAT_BC5_UNORM_BLOCK — dan **tidak ada varian sRGB-nya sama sekali**,
    // yang persis kenapa BC5 adalah jawaban yang benar untuk normal map.
    CHECK(asNormal.vkFormat == 141);

    settings.usage = TextureUsage::Mask;
    const BakeResult asMask = BakeTexture(color, settings, temp.Path() / "m");
    REQUIRE_MESSAGE(asMask.ok, asMask.error);
    CHECK(asMask.vkFormat == 139);  // VK_FORMAT_BC4_UNORM_BLOCK

    // Dimatikan per aset, seperti yang dijanjikan `TextureSettings` sejak T1.
    settings.usage = TextureUsage::Color;
    settings.compress = false;
    const BakeResult plain = BakeTexture(color, settings, temp.Path() / "p");
    REQUIRE_MESSAGE(plain.ok, plain.error);
    CHECK(plain.vkFormat == 43);  // VK_FORMAT_R8G8B8A8_SRGB
}

TEST_CASE("T2: berkas ber-BC7 dibaca pembaca lain dengan ukuran level yang benar") {
    // Bahwa libktx menghitung ukuran level dari format bloknya — dan bahwa
    // pembaca tangan di Sim::RHI membaca offset yang sama.
    TempDir temp;
    TextureSettings settings;
    settings.usage = TextureUsage::Color;
    settings.compress = true;

    const BakeResult result = BakeTexture(TextureFixture("checker.png"), settings, temp.Path());
    REQUIRE_MESSAGE(result.ok, result.error);
    CHECK(result.vkFormat == 146);

    rhi::Ktx2Texture texture;
    const rhi::Ktx2Result read = rhi::ReadKtx2(result.path, texture);
    REQUIRE_MESSAGE(read.ok, read.error);
    CHECK(texture.format == 146);
    REQUIRE(texture.levels.size() == 4);

    // 8x8 → 2x2 blok → 64 byte. 4x4 → 1 blok. 2x2 dan 1x1 tetap satu blok utuh
    // masing-masing: format blok tidak punya cara menyimpan setengah blok.
    const std::size_t expected[] = {64, 16, 16, 16};
    for (std::size_t level = 0; level < 4; ++level) {
        INFO("level " << level);
        CHECK(texture.LevelBytes(level).size() == expected[level]);
    }
    // Dan seluruh muatannya lebih kecil daripada level 0 saja tanpa kompresi.
    // Yang dibandingkan muatannya, bukan `texture.bytes` — itu isi berkas utuh,
    // dan pada tekstur sekecil ini header beserta DFD-nya lebih besar daripada
    // gambarnya sendiri.
    std::size_t payload = 0;
    for (std::size_t level = 0; level < texture.levels.size(); ++level) {
        payload += texture.LevelBytes(level).size();
    }
    CHECK(payload == 112);
    CHECK(payload < 8 * 8 * 4);
}

TEST_CASE("T2: metrik perseptual benar-benar sampai ke encoder") {
    // Jebakan nomor tiga di rencananya menuntut normal map dikompresi **tanpa**
    // metrik warna. Perlindungan sebenarnya struktural — normal map memakai BC5,
    // yang tidak punya metrik warna sama sekali — tetapi bendera itu tetap ada
    // untuk siapa pun yang memampatkan data ke BC7, dan bendera yang tidak
    // sampai ke encoder adalah janji yang tidak ditepati tanpa satu pun tanda.
    constexpr uint32_t kSide = 8;
    std::vector<uint8_t> source(static_cast<std::size_t>(kSide) * kSide * 4);
    for (uint32_t y = 0; y < kSide; ++y) {
        for (uint32_t x = 0; x < kSide; ++x) {
            uint8_t* pixel = source.data() + (static_cast<std::size_t>(y) * kSide + x) * 4;
            // Biru berayun jauh sementara merah dan hijau nyaris diam: itulah
            // kanal yang paling dihemat metrik perseptual, jadi di sinilah kedua
            // metrik paling berbeda hasilnya.
            pixel[0] = static_cast<uint8_t>(120 + x);
            pixel[1] = static_cast<uint8_t>(120 + y);
            pixel[2] = static_cast<uint8_t>(((x + y) % 2) == 0 ? 10 : 245);
            pixel[3] = 255;
        }
    }

    CompressOptions perceptual;
    perceptual.format = BlockFormat::Bc7;
    perceptual.quality = TextureQuality::Balanced;
    perceptual.perceptual = true;

    CompressOptions linear = perceptual;
    linear.perceptual = false;

    std::vector<uint8_t> withPerceptual;
    std::vector<uint8_t> withLinear;
    REQUIRE(CompressBlocks(perceptual, source, kSide, kSide, 4, withPerceptual));
    REQUIRE(CompressBlocks(linear, source, kSide, kSide, 4, withLinear));

    REQUIRE(withPerceptual.size() == withLinear.size());
    CHECK(withPerceptual != withLinear);
}

TEST_CASE("T2: BC7 pada kualitas seimbang tetap di atas ambang PSNR-nya") {
    // **Yang dijaga di sini adalah keputusan, bukan implementasi.** Parameter
    // `seimbang` diturunkan setelah diukur — 4K dari 8,7 detik menjadi 5,3 —
    // dan godaan berikutnya adalah menurunkannya sekali lagi dengan menolkan
    // partisinya, yang memang tiga detik lebih cepat lagi. Harganya lima
    // desibel, dan lima desibel terlihat sebagai blok pada setiap tepi tajam.
    //
    // Diukur, bukan diwaktukan: PSNR tidak bergantung pada beban mesin, jadi uji
    // ini tidak akan pernah gagal hanya karena tiga build berjalan bersamaan.
    constexpr uint32_t kSide = 512;
    std::vector<uint8_t> source(static_cast<std::size_t>(kSide) * kSide * 4);
    uint32_t seed = 12345;
    for (uint32_t y = 0; y < kSide; ++y) {
        for (uint32_t x = 0; x < kSide; ++x) {
            // Gradien, tepi tajam, dan sedikit derau — ketiganya sekaligus,
            // karena encoder blok bisa unggul pada satu dan gagal pada yang lain.
            seed = seed * 1664525u + 1013904223u;
            const uint32_t noise = (seed >> 24) % 24;
            uint8_t* pixel = source.data() + (static_cast<std::size_t>(y) * kSide + x) * 4;
            pixel[0] = static_cast<uint8_t>((x * 255 / kSide + noise) & 0xFF);
            pixel[1] = static_cast<uint8_t>((y * 255 / kSide) ^ ((x / 16) * 17));
            pixel[2] = static_cast<uint8_t>(((x / 8 + y / 8) % 2) ? 200 + noise : 40 + noise);
            pixel[3] = 255;
        }
    }

    auto psnr = [&](TextureQuality quality) {
        CompressOptions options;
        options.format = BlockFormat::Bc7;
        options.quality = quality;
        std::vector<uint8_t> blocks;
        std::vector<uint8_t> back;
        REQUIRE(CompressBlocks(options, source, kSide, kSide, 4, blocks));
        REQUIRE(DecompressBlocks(BlockFormat::Bc7, blocks, kSide, kSide, back));
        double sum = 0.0;
        for (std::size_t i = 0; i < source.size(); ++i) {
            const double difference = static_cast<double>(back[i]) - source[i];
            sum += difference * difference;
        }
        return 10.0 * std::log10(255.0 * 255.0 / (sum / static_cast<double>(source.size())));
    };

    const double balanced = psnr(TextureQuality::Balanced);
    const double best = psnr(TextureQuality::Best);
    INFO("seimbang " << balanced << " dB, terbaik " << best << " dB");

    // Terukur 46,2 dB. Ambangnya 44 supaya ada ruang bagi versi encoder yang
    // berbeda, dan tetap di atas 41,4 dB yang dihasilkan partisi nol — yaitu
    // tetap menangkap persis penurunan yang ingin dijaga.
    CHECK(balanced > 44.0);
    // Dan `terbaik` memang lebih baik. Tingkat kualitas yang urutannya terbalik
    // adalah bug yang tidak pernah terlihat: yang meminta kualitas terbaik
    // menerima gambar yang lebih buruk dan membayar empat kali lipat waktunya.
    CHECK(best >= balanced);
}

// ---------------------------------------------------------------------------
// T3 — bakery: dari berkas sumber ke .ktx2
// ---------------------------------------------------------------------------

TEST_CASE("T3: bakery menjawab .ktx2, dan hanya sekali mengerjakannya") {
    TempDir temp;
    const std::filesystem::path source = temp.Path() / "batu.png";
    CopyFile(TextureFixture("checker.png"), source);

    // Tanpa TaskPool bake dikerjakan di tempat — itu jalur uji dan jalur
    // headless, keduanya tidak punya frame yang bisa terlihat membeku.
    TextureBakery bakery(temp.Path() / "cache", nullptr);

    const uint64_t before = TextureBakeCount();
    const BakedTextureRef first = bakery.Request(source);
    CHECK(first.state == BakeState::Ready);
    CHECK(first.path.extension() == ".ktx2");
    CHECK(std::filesystem::exists(first.path));
    CHECK(TextureBakeCount() == before + 1);

    // Permintaan kedua tidak menyentuh baker sama sekali — bukan sekadar
    // menemukan berkas cache-nya, tetapi tidak bertanya.
    const BakedTextureRef second = bakery.Request(source);
    CHECK(second.state == BakeState::Ready);
    CHECK(second.path == first.path);
    CHECK(TextureBakeCount() == before + 1);

    // Dan yang dijawabnya benar-benar sebuah KTX2 yang bisa dibaca.
    rhi::Ktx2Texture texture;
    const rhi::Ktx2Result read = rhi::ReadKtx2(first.path, texture);
    REQUIRE_MESSAGE(read.ok, read.error);
    CHECK(texture.width == 8);
    CHECK(texture.levels.size() == 4);
}

TEST_CASE("T3: berkas yang tidak bisa dibaca menjawab gagal, dan tidak dicoba lagi") {
    TempDir temp;
    const std::filesystem::path broken = temp.Path() / "rusak.png";
    WriteFile(broken, "ini bukan PNG");

    TextureBakery bakery(temp.Path() / "cache", nullptr);
    const uint64_t before = TextureBakeCount();

    CHECK(bakery.Request(broken).state == BakeState::Failed);
    CHECK(bakery.Request(broken).state == BakeState::Failed);
    // Nol: yang gagal tidak menghasilkan berkas cache.
    CHECK(TextureBakeCount() == before);

    // **Dan ia benar-benar diingat, bukan sekadar gagal lagi dengan cara yang
    // sama.** Berkasnya diganti dengan yang sah; jawabannya tetap gagal, karena
    // yang sudah dicoba tidak dicoba ulang. Itu yang mencegah berkas rusak
    // diurai enam puluh kali per detik — dan sekaligus alasan `Invalidate` ada.
    CopyFile(TextureFixture("checker.png"), broken);
    CHECK(bakery.Request(broken).state == BakeState::Failed);
    bakery.Invalidate(broken);
    CHECK(bakery.Request(broken).state == BakeState::Ready);

    // Yang tidak ada sama sekali juga gagal, bukan menggantung di `Pending`.
    CHECK(bakery.Request(temp.Path() / "tidak-ada.png").state == BakeState::Failed);
}

TEST_CASE("T3: dengan TaskPool jawabannya menunggu, bukan memblokir") {
    // **Inilah alasan bakery ada.** Bake sebuah tekstur 4K adalah detik, bukan
    // milidetik, dan pekerjaan sebesar itu di dalam jalur gambar berarti editor
    // yang membeku. Yang diminta frame ini menjawab `Pending`, dan frame itu
    // menggambar placeholder.
    TempDir temp;
    const std::filesystem::path source = temp.Path() / "batu.png";
    CopyFile(TextureFixture("albedo-srgb.png"), source);

    TaskPool tasks;
    TextureBakery bakery(temp.Path() / "cache", &tasks);

    const uint64_t before = TextureBakeCount();
    const BakedTextureRef first = bakery.Request(source);
    CHECK(first.state == BakeState::Pending);
    CHECK(first.path.empty());
    // Permintaan kedua pada frame yang sama **tidak** mengantre tugas kedua.
    // Tanpa itu, satu tekstur yang dipakai lima ruas mesh menjalankan lima
    // encoder BC7 sekaligus untuk menghasilkan berkas yang identik.
    CHECK(bakery.Request(source).state == BakeState::Pending);
    CHECK(bakery.Request(source).state == BakeState::Pending);

    tasks.WaitIdle();
    CHECK(TextureBakeCount() == before + 1);

    const BakedTextureRef done = bakery.Request(source);
    CHECK(done.state == BakeState::Ready);
    CHECK(std::filesystem::exists(done.path));
    CHECK(bakery.PendingCount() == 0);

    // Melupakannya membuat permintaan berikutnya membangunnya lagi — itu yang
    // dipanggil panel ketika pengaturan teksturnya berubah.
    bakery.Invalidate(source);
    CHECK(bakery.Request(source).state == BakeState::Pending);
    tasks.WaitIdle();
    CHECK(bakery.Request(source).state == BakeState::Ready);
}

TEST_CASE("T3: BC7 memakai seperempat VRAM RGBA8, dan angkanya diukur") {
    // Rencananya menuntut penghematannya **terukur**, bukan diyakini. Yang
    // dijumlahkan adalah muatan tiap level — persis byte yang diunggah ke GPU —
    // bukan dimensi dikali tebakan bytes-per-texel.
    //
    // Sumbernya ditulis di sini, bukan diambil dari `Resources/Images`: fixture
    // di sana semuanya delapan piksel, dan pada ukuran itu level terdalam yang
    // tetap menempati satu blok utuh justru mendominasi hasilnya.
    TempDir temp;
    constexpr uint32_t kSide = 256;
    imageio::ImageDesc desc;
    desc.width = kSide;
    desc.height = kSide;
    desc.channels = 1;
    desc.type = imageio::PixelType::UInt8;
    imageio::Image image;
    image.Allocate(desc);
    for (uint32_t y = 0; y < kSide; ++y) {
        for (uint32_t x = 0; x < kSide; ++x) {
            image.bytes[static_cast<std::size_t>(y) * kSide + x] =
                static_cast<uint8_t>((x ^ y) | ((x / 32 + y / 32) % 2 ? 128 : 0));
        }
    }
    const std::filesystem::path source = temp.Path() / "besar.png";
    const imageio::ImageIoResult written = imageio::Write(source, image);
    REQUIRE_MESSAGE(written.ok, written.error);

    auto payload = [&](bool compress, const char* folder) {
        TextureSettings settings;
        settings.usage = TextureUsage::Color;
        settings.compress = compress;
        const BakeResult result = BakeTexture(source, settings, temp.Path() / folder);
        REQUIRE_MESSAGE(result.ok, result.error);
        rhi::Ktx2Texture texture;
        const rhi::Ktx2Result read = rhi::ReadKtx2(result.path, texture);
        REQUIRE_MESSAGE(read.ok, read.error);
        std::size_t bytes = 0;
        for (std::size_t level = 0; level < texture.levels.size(); ++level) {
            bytes += texture.LevelBytes(level).size();
        }
        return bytes;
    };

    const std::size_t plain = payload(false, "plain");
    const std::size_t block = payload(true, "block");
    INFO("RGBA8 " << plain << " byte, BC7 " << block << " byte");
    REQUIRE(plain > 0);
    CHECK(block < plain);

    // BC7 satu byte per teksel, RGBA8 empat — jadi seperempat, dan sisa
    // beberapa blok di level terdalam. Batasnya 0,3 supaya tetap menangkap
    // format yang diam-diam kembali ke RGBA8, tanpa terikat pada digit terakhir
    // hasil sebuah versi encoder.
    CHECK(static_cast<double>(block) <= static_cast<double>(plain) * 0.30);
}

// --- cook: memangkas yang tidak terjangkau -------------------------------------

TEST_CASE("GuidsIn menemukan yang berbentuk GUID dan menolak yang mirip") {
    // Dicari sebagai teks karena setiap aset di engine ini JSON dan menyebut
    // aset lain lewat GUID. Yang harus dijaga adalah batasnya: teks yang
    // *hampir* GUID tidak boleh ikut, karena setiap yang ikut adalah satu aset
    // yang dikirim tanpa alasan.
    const std::string guid = "3f2504e0-4f89-41d3-9a0c-0305e82c3301";

    SUBCASE("di dalam JSON") {
        const std::vector<Uuid> found =
            assets::GuidsIn(R"({"mesh":")" + guid + R"(","name":"Kubus"})");
        REQUIRE(found.size() == 1);
        CHECK(found[0].ToString() == guid);
    }

    SUBCASE("huruf besar diterima dan dinormalkan") {
        std::string upper = guid;
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        const std::vector<Uuid> found = assets::GuidsIn(upper);
        REQUIRE(found.size() == 1);
        CHECK(found[0].ToString() == guid);
    }

    SUBCASE("duplikat hanya sekali") {
        CHECK(assets::GuidsIn(guid + " " + guid + " " + guid).size() == 1);
    }

    SUBCASE("yang mirip tapi bukan ditolak") {
        // Kelompok yang panjangnya salah, huruf non-heksadesimal, dan hash yang
        // lebih panjang: ketiganya pernah lolos pemeriksa yang lebih longgar.
        CHECK(assets::GuidsIn("3f2504e0-4f89-41d3-9a0c-0305e82c33").empty());
        CHECK(assets::GuidsIn("3f2504e0-4f89-41d3-9a0c-0305e82c330g").empty());
        CHECK(assets::GuidsIn("3f2504e04f8941d39a0c0305e82c3301").empty());
        // Diikuti heksadesimal lagi: ini bagian dari sesuatu yang lebih panjang.
        CHECK(assets::GuidsIn(guid + "ab").empty());
    }

    SUBCASE("dua GUID berturut-turut keduanya terbaca") {
        const std::string second = "11111111-2222-4333-8444-555555555555";
        const std::vector<Uuid> found = assets::GuidsIn(guid + "\n" + second);
        REQUIRE(found.size() == 2);
    }
}

TEST_CASE("PlanCook menelusuri dari level dan memisahkan yang tidak terjangkau") {
    // Aset dibuat sungguhan di disk lalu diindeks: yang diuji adalah
    // penelusuran atas indeks yang sama yang dipakai editor, bukan atas struct
    // yang dikarang uji.
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("simcook_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Assets");
    std::filesystem::create_directories(root / "Levels");

    const auto write = [](const std::filesystem::path& path, const std::string& text) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
    };

    // Rantai: level -> material -> tekstur. Ditambah satu tekstur yatim yang
    // tidak dirujuk siapa pun.
    write(root / "Assets" / "batu.png", "\x89PNG fake");
    write(root / "Assets" / "yatim.png", "\x89PNG fake");

    assets::AssetDatabase database;
    assets::AssetDatabase::Config config;
    config.root = root / "Assets";
    REQUIRE(database.Initialize(config));
    database.ScanNow();

    const assets::AssetRecord* stone = database.FindByRelativePath("batu.png");
    const assets::AssetRecord* orphan = database.FindByRelativePath("yatim.png");
    REQUIRE(stone != nullptr);
    REQUIRE(orphan != nullptr);

    write(root / "Assets" / "batu.simmat",
          R"({"version":1,"nodes":[{"type":"input.texture","pins":{"texture":")" +
              stone->guid.ToString() + R"("}}]})");
    database.ScanNow();
    const assets::AssetRecord* material = database.FindByRelativePath("batu.simmat");
    REQUIRE(material != nullptr);

    const std::filesystem::path level = root / "Levels" / "utama.simlevel";
    write(level, R"({"entities":[{"material":")" + material->guid.ToString() + R"("}]})");

    const assets::CookPlan plan = assets::PlanCook(database, {level});

    SUBCASE("terjangkau transitif lewat material") {
        // Level menyebut materialnya saja; teksturnya ikut karena material
        // menyebutnya. Tanpa penelusuran transitif, permainan kehilangan
        // teksturnya dan yang terlihat adalah material putih.
        std::vector<std::string> paths;
        for (const Uuid& guid : plan.reachable) {
            paths.push_back(database.Find(guid)->relativePath);
        }
        CHECK(std::find(paths.begin(), paths.end(), "batu.simmat") != paths.end());
        CHECK(std::find(paths.begin(), paths.end(), "batu.png") != paths.end());
        CHECK(std::find(paths.begin(), paths.end(), "yatim.png") == paths.end());
    }

    SUBCASE("yang yatim masuk daftar tidak terjangkau, beserta ukurannya") {
        std::vector<std::string> paths;
        for (const Uuid& guid : plan.unreachable) {
            paths.push_back(database.Find(guid)->relativePath);
        }
        CHECK(std::find(paths.begin(), paths.end(), "yatim.png") != paths.end());
        CHECK(plan.unreachableBytes > 0);
    }

    SUBCASE("GUID entity di level tidak menyeret apa pun") {
        // **Regresi.** Setiap entity di berkas level punya GUID sendiri,
        // bentuknya sama persis dengan GUID aset. Yang tidak ada di indeks
        // dilewati diam-diam; percobaan pertama melaporkannya sebagai referensi
        // menggantung, dan sebelas dari sebelas laporannya salah.
        const std::filesystem::path withEntities = root / "Levels" / "berentity.simlevel";
        write(withEntities,
              R"({"entities":[{"guid":"00000000-0000-0000-0000-00000000000a"},)"
              R"({"guid":"00000000-0000-0000-0000-00000000000b"}]})");
        const assets::CookPlan onlyEntities = assets::PlanCook(database, {withEntities});
        CHECK(onlyEntities.reachable.empty());
    }

    SUBCASE("hasilnya stabil antar-jalan") {
        const assets::CookPlan again = assets::PlanCook(database, {level});
        CHECK(again.reachable == plan.reachable);
        CHECK(again.unreachable == plan.unreachable);
    }

    database.Shutdown();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("Tekstur yang sudah dipanggang tetap bertipe Texture") {
    // Keluaran cook adalah `.ktx2`, dan ia harus berarti hal yang sama seperti
    // sumbernya — kalau tidak, setiap referensi material yang menunjuknya
    // berubah arti begitu project-nya dikirim.
    CHECK(assets::TypeFromExtension(".ktx2") == assets::AssetType::Texture);
    CHECK(assets::TypeFromExtension(".png") == assets::AssetType::Texture);
}

TEST_CASE("T-M1: sisi voxel diperbesar sampai muat, bukan mesh-nya ditolak") {
    MeshSdfSettings settings;
    settings.voxelSize = 0.1f;
    settings.maxVoxels = 1u << 20;

    // Mesh sebesar kursi muat apa adanya.
    CHECK(FitVoxelSize(Vec3(-0.5f), Vec3(0.5f), settings) == doctest::Approx(0.1f));

    // Mesh sebesar gedung tidak, dan yang berubah sisi voxelnya.
    const float coarse = FitVoxelSize(Vec3(-18.0f, -1.0f, -12.0f), Vec3(18.0f, 19.0f, 12.0f),
                                      settings);
    CHECK(coarse > 0.1f);
    // Dan sesudah diperbesar, ia benar-benar muat — inilah yang dipakai baker
    // untuk memutuskan menolak atau tidak.
    const float band = BandMeters(Vec3(-18.0f, -1.0f, -12.0f), Vec3(18.0f, 19.0f, 12.0f),
                                  settings, coarse);
    const Vec3 extent = Vec3(36.0f, 20.0f, 24.0f) + Vec3(2.0f * (band + coarse));
    const double voxels = std::ceil(extent.x / coarse) * std::ceil(extent.y / coarse) *
                          std::ceil(extent.z / coarse);
    CHECK(voxels <= static_cast<double>(settings.maxVoxels));
}

TEST_CASE("T-M1: pita sepadan dengan bendanya, bukan jumlah voxel tetap") {
    MeshSdfSettings settings;
    settings.bandVoxels = 4.0f;
    settings.bandFraction = 0.05f;

    // Benda kecil: yang berlaku batas bawahnya, empat voxel.
    CHECK(BandMeters(Vec3(-0.5f), Vec3(0.5f), settings, 0.1f) == doctest::Approx(0.4f));
    // Benda sebesar gedung: yang berlaku pecahannya. Tanpa ini pita 40 cm
    // membuat penelusur melangkah 40 cm di seluruh halaman yang lebarnya 20 m.
    CHECK(BandMeters(Vec3(0.0f), Vec3(36.0f, 20.0f, 24.0f), settings, 0.1f) ==
          doctest::Approx(1.8f));
}

TEST_CASE("T-M1: berkas .simsdf bolak-balik, dan yang terpotong ditolak") {
    TempDir temp;
    SdfGrid grid;
    grid.sizeX = 3;
    grid.sizeY = 4;
    grid.sizeZ = 5;
    grid.voxelSize = 0.25f;
    grid.band = 1.0f;
    grid.origin = Vec3(-1.0f, 2.0f, -3.0f);
    grid.distances.resize(grid.VoxelCount());
    for (std::size_t i = 0; i < grid.distances.size(); ++i) {
        grid.distances[i] = static_cast<float>(i) * 0.01f - 0.3f;
    }
    grid.palette = {Vec3(0.5f), Vec3(0.9f, 0.2f, 0.1f), Vec3(0.1f, 0.3f, 0.7f)};
    grid.materials.resize(grid.VoxelCount());
    for (std::size_t i = 0; i < grid.materials.size(); ++i) {
        grid.materials[i] = static_cast<uint8_t>(i % 3);
    }

    const std::filesystem::path file = temp.Path() / "field.simsdf";
    std::string error;
    REQUIRE(WriteMeshSdf(file, grid, error));

    SdfGrid loaded;
    REQUIRE(ReadMeshSdf(file, loaded, error));
    CHECK(loaded.sizeX == grid.sizeX);
    CHECK(loaded.HasAlbedo());
    CHECK(loaded.palette.size() == grid.palette.size());
    CHECK(loaded.materials == grid.materials);
    CHECK(loaded.palette[1].x == doctest::Approx(grid.palette[1].x));
    CHECK(loaded.sizeY == grid.sizeY);
    CHECK(loaded.sizeZ == grid.sizeZ);
    CHECK(loaded.voxelSize == doctest::Approx(grid.voxelSize));
    CHECK(loaded.band == doctest::Approx(grid.band));
    CHECK(loaded.origin.y == doctest::Approx(grid.origin.y));
    CHECK(loaded.distances == grid.distances);

    // **Berkas yang terpotong ditolak, bukan dipotong.** Proses yang mati di
    // tengah penulisan meninggalkan header yang menyebut ukuran penuh dan isi
    // yang tidak sampai; menerimanya berarti separuh medan jarak berisi nol,
    // dan nol berarti "permukaan di sini" — dinding hantu di tengah ruangan.
    const std::filesystem::path cut = temp.Path() / "cut.simsdf";
    std::filesystem::copy_file(file, cut);
    std::filesystem::resize_file(cut, std::filesystem::file_size(cut) - 8);
    SdfGrid broken;
    CHECK_FALSE(ReadMeshSdf(cut, broken, error));
    CHECK(broken.Empty());

    // Berkas lain yang panjangnya cukup pun ditolak, lewat magic-nya.
    const std::filesystem::path alien = temp.Path() / "alien.simsdf";
    {
        std::ofstream out(alien, std::ios::binary);
        const std::vector<char> junk(512, 'x');
        out.write(junk.data(), static_cast<std::streamsize>(junk.size()));
    }
    CHECK_FALSE(ReadMeshSdf(alien, broken, error));
}

TEST_CASE("T-M1: bake sebuah bola, dan jalan kedua dijawab cache") {
    if (!volume::Available()) {
        return;
    }
    const std::filesystem::path sphere = std::filesystem::path(SIM_MESH_DIR) / "unitSphere.obj";
    REQUIRE(std::filesystem::exists(sphere));

    TempDir temp;
    MeshSdfSettings settings;
    settings.voxelSize = 0.05f;

    const uint64_t before = MeshSdfBakeCount();
    const MeshSdfBakeResult baked = BakeMeshSdfFile(sphere, settings, temp.Path());
    REQUIRE_MESSAGE(baked.ok, baked.error);
    CHECK_FALSE(baked.fromCache);
    CHECK(MeshSdfBakeCount() == before + 1);

    SdfGrid grid;
    std::string error;
    REQUIRE(ReadMeshSdf(baked.path, grid, error));

    // **Yang diuji tandanya dan besarnya, bukan bentuknya.** Pusat bola berada
    // di dalam benda, jadi jaraknya negatif; sebuah titik jauh di luar berada di
    // luar pita, jadi ia jenuh di +band. Keduanya adalah sifat yang membedakan
    // medan jarak sungguhan dari kotak batas — kotak menjawab nol di pusat dan
    // nol di setiap titik di dalamnya.
    CHECK(grid.SampleLocal(Vec3(0.0f)) < 0.0f);
    CHECK(grid.SampleLocal(Vec3(0.0f, 100.0f, 0.0f)) == doctest::Approx(grid.band));

    // Permukaannya: jarak berubah tanda persis sekali sepanjang jari-jari, dan
    // titik nolnya jauh dari pusat maupun dari tepi kotak batasnya.
    float crossing = -1.0f;
    for (int i = 0; i <= 200; ++i) {
        const float t = static_cast<float>(i) / 200.0f * 2.0f;
        if (grid.SampleLocal(Vec3(t, 0.0f, 0.0f)) >= 0.0f) {
            crossing = t;
            break;
        }
    }
    CHECK(crossing > 0.05f);
    CHECK(crossing < 2.0f);

    // Jalan kedua tidak menyentuh mesh-nya sama sekali.
    const MeshSdfBakeResult again = BakeMeshSdfFile(sphere, settings, temp.Path());
    REQUIRE(again.ok);
    CHECK(again.fromCache);
    CHECK(again.path == baked.path);
    CHECK(MeshSdfBakeCount() == before + 1);
}

TEST_CASE("T-M1: bakery menjawab Pending lalu Ready, dan hanya membake sekali") {
    if (!volume::Available()) {
        return;
    }
    const std::filesystem::path sphere = std::filesystem::path(SIM_MESH_DIR) / "unitSphere.obj";
    TempDir temp;
    MeshSdfSettings settings;
    settings.voxelSize = 0.05f;

    TaskPool tasks(2);
    MeshSdfBakery bakery(temp.Path(), &tasks, settings);

    const uint64_t before = MeshSdfBakeCount();
    CHECK(bakery.Request(sphere).state == MeshSdfState::Pending);
    // Permintaan kedua pada frame yang sama tidak mengantre bake kedua: satu
    // mesh sebesar Sponza adalah 7 detik dan 1,5 GB, dan dua sekaligus adalah
    // dua-duanya.
    CHECK(bakery.Request(sphere).state == MeshSdfState::Pending);
    tasks.WaitIdle();
    CHECK(MeshSdfBakeCount() == before + 1);

    const MeshSdfRef ready = bakery.Request(sphere);
    REQUIRE(ready.state == MeshSdfState::Ready);
    REQUIRE(ready.grid != nullptr);
    CHECK(ready.grid->SampleLocal(Vec3(0.0f)) < 0.0f);
    CHECK(bakery.PendingCount() == 0);

    // Berkas yang tidak ada gagal — sesudah tugasnya berjalan, bukan seketika:
    // dengan `TaskPool` jawaban pertama selalu `Pending`, dan itu memang
    // kontraknya.
    const std::filesystem::path missing = temp.Path() / "tidak-ada.obj";
    CHECK(bakery.Request(missing).state == MeshSdfState::Pending);
    tasks.WaitIdle();
    CHECK(bakery.Request(missing).state == MeshSdfState::Failed);
}

