#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Assets/Importer.h"
#include "Sim/Assets/MaterialImport.h"
#include "Sim/Assets/MeshData.h"
#include "Sim/Assets/MeshSettings.h"
#include "Sim/Assets/Thumbnail.h"
#include "Sim/Material/MaterialGraph.h"
#include "Sim/Material/MaterialValidation.h"

#include "Sim/Core/FileWatcher.h"
#include "Sim/Core/TaskPool.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
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
