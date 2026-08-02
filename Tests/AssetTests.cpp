#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Assets/Importer.h"
#include "Sim/Core/FileWatcher.h"
#include "Sim/Core/TaskPool.h"

#include <doctest/doctest.h>

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
