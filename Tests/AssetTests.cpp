#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Assets/Importer.h"
#include "Sim/Assets/MeshData.h"
#include "Sim/Core/FileWatcher.h"
#include "Sim/Core/TaskPool.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
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
