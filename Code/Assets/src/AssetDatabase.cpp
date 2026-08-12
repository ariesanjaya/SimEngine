#include "Sim/Assets/AssetDatabase.h"

#include "Sim/Assets/Importer.h"
#include "Sim/Core/Log.h"
#include "Sim/Core/TaskPool.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace sim::assets {
namespace {

constexpr const char* kMetaExtension = ".meta";

/// Berkas pendamping yang bukan aset.
///
/// **Berkas pengaturan bukan sesuatu yang dirujuk siapa pun.** `.simmeshcfg`
/// menyimpan penandaan sebuah mesh dan hanya dibaca lewat mesh-nya; kalau ia
/// ikut terindeks, ia mendapat GUID dan `.meta`-nya sendiri, muncul di Asset
/// Browser sebagai berkas tak dikenal, dan `.meta` itu tertinggal sebagai
/// yatim begitu penandaan terakhirnya dilepas — karena berkas pengaturan yang
/// tidak mengatur apa pun memang dihapus.
bool IsSidecar(const std::filesystem::path& path) {
    const std::string extension = path.extension().string();
    return extension == kMetaExtension || extension == ".simmeshcfg";
}
constexpr int kMetaVersion = 1;

/// Path relatif dengan '/' sebagai pemisah, apa pun platformnya.
///
/// Path disimpan sebagai teks dan ikut masuk ke berkas favorit serta indeks;
/// membiarkan pemisah asli platform akan membuat berkas itu tidak bisa dibawa
/// pindah dari Linux ke Windows.
/// Memotong prefiks akar, bukan memakai std::filesystem::relative().
///
/// Terukur pada pohon 10.000 berkas: relative() memakan 269 ms dari 284 ms
/// seluruh pemindaian — 95%-nya — karena ia menormalkan kedua path dan
/// mengalokasi berkali-kali untuk pekerjaan yang di sini hanya "buang awalan
/// yang sudah pasti ada". Versi ini menyelesaikan pemindaian yang sama dalam
/// 14 ms. Aman karena setiap path memang berasal dari iterasi di bawah `root`;
/// bila ternyata tidak, path utuhnya dikembalikan apa adanya.
std::string ToRelativeString(const std::string& rootText, const std::filesystem::path& path) {
    std::string text = path.string();
    if (text.size() > rootText.size() + 1 &&
        text.compare(0, rootText.size(), rootText) == 0) {
        text.erase(0, rootText.size() + 1);  // buang akar beserta pemisahnya
    }
#ifdef _WIN32
    // Path relatif ikut masuk berkas favorit dan indeks, jadi pemisahnya
    // diseragamkan ke '/' supaya berkas itu bisa dibawa lintas platform.
    std::replace(text.begin(), text.end(), '\\', '/');
#endif
    return text;
}

std::int64_t ModifiedSeconds(const std::filesystem::directory_entry& entry) {
    std::error_code error;
    const auto time = entry.last_write_time(error);
    if (error) {
        return 0;
    }
    return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
}

/// Membaca GUID dari berkas `.meta`, atau membuatkan yang baru bila belum ada.
///
/// Berkas `.meta` inilah yang membuat identitas aset bertahan. Kalau hilang,
/// aset mendapat GUID baru dan setiap level yang memakainya kehilangan
/// rujukannya — karena itu berkas ini harus ikut masuk kontrol versi bersama
/// asetnya, dan itu dicatat di dalam berkasnya sendiri.
Uuid ReadOrCreateMeta(const std::filesystem::path& assetPath, bool& outCreated) {
    const std::filesystem::path metaPath = assetPath.string() + kMetaExtension;
    outCreated = false;

    std::ifstream input(metaPath);
    if (input) {
        try {
            nlohmann::json document;
            input >> document;
            const Uuid guid = Uuid::Parse(document.value("guid", std::string{}));
            if (guid.IsValid()) {
                return guid;
            }
        } catch (const nlohmann::json::exception& exception) {
            SIM_WARN("Assets", "Damaged meta file {}: {}", metaPath.string(), exception.what());
        }
    }

    const Uuid guid = Uuid::Generate();
    nlohmann::ordered_json document;
    document["version"] = kMetaVersion;
    document["guid"] = guid.ToString();
    document["note"] =
        "Keep this file next to its asset and commit it. Losing it gives the asset a new "
        "identity and breaks every level that references it.";

    std::ofstream output(metaPath);
    if (output) {
        output << document.dump(2) << '\n';
        outCreated = true;
    } else {
        SIM_WARN("Assets", "Cannot write meta file {}", metaPath.string());
    }
    return guid;
}

}  // namespace

bool AssetDatabase::Initialize(Config config) {
    root_ = config.root;
    tasks_ = config.tasks;
    pollInterval_ = config.pollIntervalSeconds;

    std::error_code error;
    std::filesystem::create_directories(root_, error);
    if (error) {
        SIM_ERROR("Assets", "Cannot create asset root {}: {}", root_.string(), error.message());
        return false;
    }

    ImporterRegistry::RegisterBuiltIn();
    pollInterval_ = config.pollIntervalSeconds;
    safetyNet_ = config.safetyNetSeconds;
    ScanNow();

    // Dipasang setelah pemindaian pertama: memasangnya lebih dulu berarti
    // menumpuk event untuk berkas yang toh sudah terbaca pemindaian itu.
    if (!watcher_.Watch(root_)) {
        SIM_INFO("Assets",
                 "File watcher unavailable; falling back to polling every {:.1f}s",
                 pollInterval_);
    }
    SIM_INFO("Assets", "Asset database ready: {} assets in {}", records_.size(), root_.string());
    return true;
}

void AssetDatabase::Shutdown() {
    watcher_.Stop();
    records_.clear();
    folders_.clear();
    byGuid_.clear();
    byPath_.clear();
    // **Cuplikan pemindaian ikut dibuang, dan ini yang membuat indeks bisa
    // dibuka ulang pada akar yang lain.** Ia adalah keadaan berkas dari
    // pemindaian sebelumnya; membiarkannya berarti pemindaian pertama akar baru
    // dibandingkan dengan isi akar lama — setiap berkas project sebelumnya
    // terbaca sebagai "dihapus", dan setiap berkas project ini sebagai "baru",
    // pada indeks yang toh akan benar sesudahnya. Yang tidak benar adalah
    // efek sampingnya: berkas `.meta` diimpor ulang, dan pemakainya diberi tahu
    // bahwa aset yang tidak ia sentuh baru saja berubah.
    snapshot_ = {};
}

void AssetDatabase::ScanNow() {
    Apply(Scan(root_, snapshot_));
}

void AssetDatabase::Update(float deltaSeconds) {
    // Daftar perubahan hanya berlaku untuk satu frame. Dikosongkan di sini —
    // bukan di Apply — supaya frame yang tidak menerapkan apa pun tetap
    // melaporkan "tidak ada yang berubah" alih-alih mengulang isi frame lalu.
    changed_.clear();

    ScanResult ready;
    bool hasReady = false;
    {
        const std::lock_guard<std::mutex> lock(handoffMutex_);
        if (hasPendingResult_) {
            ready = std::move(pendingResult_);
            hasPendingResult_ = false;
            hasReady = true;
        }
    }
    // Apply dijalankan di luar kunci. Ia hanya menyentuh indeks yang khusus
    // main thread, dan menahan kunci selama itu cuma membuat pemindai
    // berikutnya menunggu tanpa alasan.
    if (hasReady) {
        Apply(std::move(ready));
    }

    // Pemantau berkas menggantikan polling sebagai mekanisme utama. Yang
    // tersisa hanyalah jaring pengaman berjeda panjang, karena kedua platform
    // sama-sama bisa kehilangan event dan diam-diam ketinggalan.
    bool changed = false;
    if (watcher_.IsWatching()) {
        watcherEvents_.clear();
        const bool complete = watcher_.Poll(watcherEvents_);
        changed = !complete || !watcherEvents_.empty();
    }

    sinceLastScan_ += deltaSeconds;
    const float interval = watcher_.IsWatching() ? safetyNet_ : pollInterval_;
    if (!changed && sinceLastScan_ < interval) {
        return;
    }
    sinceLastScan_ = 0.0f;

    if (tasks_ == nullptr) {
        ScanNow();
        return;
    }
    // Satu pemindaian dalam satu waktu. Tanpa penjaga ini, folder besar yang
    // pemindaiannya lebih lama daripada jeda poll akan menumpuk tugas sampai
    // seluruh kolam thread terisi olehnya.
    bool expected = false;
    if (!scanInFlight_.compare_exchange_strong(expected, true)) {
        return;
    }
    // Cuplikan diambil di main thread, bukan di dalam tugas: menyalin pointernya
    // di sini menjamin tugas memegang versi yang sah walau indeksnya ditukar.
    tasks_->Submit([this, previous = snapshot_]() {
        ScanResult result = Scan(root_, previous);
        {
            const std::lock_guard<std::mutex> lock(handoffMutex_);
            pendingResult_ = std::move(result);
            hasPendingResult_ = true;
        }
        scanInFlight_.store(false);
    });
}

AssetDatabase::ScanResult AssetDatabase::Scan(const std::filesystem::path& root,
                                             const SnapshotPtr& previous) {
    ScanResult result;
    std::error_code error;
    if (!std::filesystem::exists(root, error)) {
        return result;
    }

    // Dihitung sekali di luar gelung: dipakai 10.000 kali per pemindaian.
    const std::string rootText = root.lexically_normal().string();

    const ImporterRegistry& importers = ImporterRegistry::Get();
    for (std::filesystem::recursive_directory_iterator it(root, error), end; it != end;
         it.increment(error)) {
        if (error) {
            SIM_WARN("Assets", "Scan error under {}: {}", root.string(), error.message());
            break;
        }
        const std::filesystem::directory_entry& entry = *it;

        if (entry.is_directory(error)) {
            result.folders.push_back(ToRelativeString(rootText, entry.path()));
            continue;
        }
        if (!entry.is_regular_file(error)) {
            continue;
        }
        // Berkas .meta adalah milik database, bukan aset tersendiri.
        if (IsSidecar(entry.path())) {
            continue;
        }

        AssetRecord record;
        record.relativePath = ToRelativeString(rootText, entry.path());
        record.name = entry.path().filename().string();
        record.type = TypeFromExtension(entry.path().extension().string());
        record.fileSize = entry.file_size(error);
        record.modifiedSeconds = ModifiedSeconds(entry);

        // Berkas yang ukuran dan waktu ubahnya sama persis dengan pemindaian
        // sebelumnya dipakai ulang apa adanya. Inilah yang membuat pemindaian
        // mantap hanya sebiaya satu stat() per berkas.
        if (previous != nullptr) {
            const auto cached = previous->find(record.relativePath);
            if (cached != previous->end() &&
                cached->second.fileSize == record.fileSize &&
                cached->second.modifiedSeconds == record.modifiedSeconds) {
                result.records.push_back(cached->second);
                continue;
            }
        }

        bool created = false;
        record.guid = ReadOrCreateMeta(entry.path(), created);

        if (const IImporter* importer = importers.Find(record.type)) {
            const ImportResult imported = importer->Import(entry.path());
            record.state = imported.ok ? ImportState::Ready : ImportState::Failed;
            record.error = imported.error;
            record.width = imported.width;
            record.height = imported.height;
            record.channels = imported.channels;
            record.dependencies = imported.dependencies;
        } else {
            record.state = ImportState::Ready;
        }

        result.records.push_back(std::move(record));
    }

    // Urutan tetap: folder dulu menurut abjad, lalu berkas. Tanpa ini, urutan
    // yang diberikan sistem berkas bisa berubah antar pemindaian dan panel akan
    // terlihat mengacak isinya sendiri.
    std::sort(result.folders.begin(), result.folders.end());
    std::sort(result.records.begin(), result.records.end(),
              [](const AssetRecord& a, const AssetRecord& b) {
                  return a.relativePath < b.relativePath;
              });
    return result;
}

void AssetDatabase::Apply(ScanResult&& result) {
    // Dibandingkan lebih dulu supaya Version() hanya naik ketika ada yang
    // benar-benar berubah — panel memakainya untuk memutuskan menyusun ulang
    // tampilan, dan menaikkannya tiap detik akan membuat itu percuma.
    const bool sameFolders = folders_ == result.folders;
    bool sameRecords = records_.size() == result.records.size();
    if (sameRecords) {
        for (std::size_t i = 0; i < records_.size(); ++i) {
            const AssetRecord& a = records_[i];
            const AssetRecord& b = result.records[i];
            if (a.guid != b.guid || a.relativePath != b.relativePath ||
                a.fileSize != b.fileSize || a.modifiedSeconds != b.modifiedSeconds ||
                a.state != b.state) {
                sameRecords = false;
                break;
            }
        }
    }
    if (sameFolders && sameRecords) {
        return;
    }

    // Dihitung selagi records_ masih memuat versi lama. Yang dicari bukan
    // "daftarnya berbeda" melainkan berkas mana yang isinya berganti — path
    // yang sama dengan ukuran atau waktu ubah yang bergeser, dan path yang baru
    // muncul. Berkas yang cuma berpindah tempat tidak masuk: isinya sama, dan
    // memuat ulang skrip yang tidak berubah hanya membuang state-nya.
    for (const AssetRecord& fresh : result.records) {
        const auto it = byPath_.find(fresh.relativePath);
        if (it == byPath_.end()) {
            changed_.push_back(fresh.guid);
        } else if (const AssetRecord& previous = records_[it->second];
                   previous.fileSize != fresh.fileSize ||
                   previous.modifiedSeconds != fresh.modifiedSeconds) {
            changed_.push_back(fresh.guid);
        }
    }

    records_ = std::move(result.records);
    folders_ = std::move(result.folders);
    Reindex();

    auto snapshot = std::make_shared<Snapshot>();
    snapshot->reserve(records_.size());
    for (const AssetRecord& record : records_) {
        snapshot->emplace(record.relativePath, record);
    }
    snapshot_ = std::move(snapshot);
    ++version_;
}

void AssetDatabase::Reindex() {
    byGuid_.clear();
    byPath_.clear();
    byGuid_.reserve(records_.size());
    byPath_.reserve(records_.size());
    for (std::size_t i = 0; i < records_.size(); ++i) {
        byGuid_.emplace(records_[i].guid, i);
        byPath_.emplace(records_[i].relativePath, i);
    }

    // Ketergantungan disaring di sini, bukan di importer: importer berjalan di
    // thread latar dan tidak boleh melihat indeks. GUID yang tidak dikenal —
    // termasuk GUID entity di dalam berkas level, yang bentuknya sama persis —
    // dibuang pada tahap ini.
    for (AssetRecord& record : records_) {
        std::vector<Uuid> resolved;
        for (const Uuid& guid : record.dependencies) {
            if (guid != record.guid && byGuid_.find(guid) != byGuid_.end()) {
                resolved.push_back(guid);
            }
        }
        record.dependencies = std::move(resolved);
    }
}

const AssetRecord* AssetDatabase::Find(const Uuid& guid) const {
    const auto it = byGuid_.find(guid);
    return it == byGuid_.end() ? nullptr : &records_[it->second];
}

const AssetRecord* AssetDatabase::FindByRelativePath(std::string_view relativePath) const {
    const auto it = byPath_.find(std::string(relativePath));
    return it == byPath_.end() ? nullptr : &records_[it->second];
}

std::vector<const AssetRecord*> AssetDatabase::InFolder(std::string_view folder,
                                                        bool recursive) const {
    std::vector<const AssetRecord*> result;
    const std::string prefix = folder.empty() ? std::string{} : std::string(folder) + "/";

    for (const AssetRecord& record : records_) {
        if (!record.relativePath.starts_with(prefix)) {
            continue;
        }
        if (!recursive) {
            // Hanya isi langsung: sisa path setelah prefix tidak boleh
            // mengandung pemisah lagi.
            const std::string_view remainder(record.relativePath);
            if (remainder.substr(prefix.size()).find('/') != std::string_view::npos) {
                continue;
            }
        }
        result.push_back(&record);
    }
    return result;
}

std::vector<Uuid> AssetDatabase::UsersOf(const Uuid& guid) const {
    std::vector<Uuid> users;
    for (const AssetRecord& record : records_) {
        if (std::find(record.dependencies.begin(), record.dependencies.end(), guid) !=
            record.dependencies.end()) {
            users.push_back(record.guid);
        }
    }
    return users;
}

std::filesystem::path AssetDatabase::AbsolutePath(const AssetRecord& record) const {
    return root_ / std::filesystem::path(record.relativePath);
}

bool AssetDatabase::Rename(const Uuid& guid, std::string_view newName, std::string& error) {
    const AssetRecord* record = Find(guid);
    if (record == nullptr) {
        error = "asset not found";
        return false;
    }
    if (newName.empty() || newName.find('/') != std::string_view::npos) {
        error = "invalid name";
        return false;
    }

    const std::filesystem::path from = AbsolutePath(*record);
    const std::filesystem::path to = from.parent_path() / std::filesystem::path(newName);
    if (from == to) {
        return true;
    }
    std::error_code code;
    if (std::filesystem::exists(to, code)) {
        error = "a file with that name already exists";
        return false;
    }

    std::filesystem::rename(from, to, code);
    if (code) {
        error = code.message();
        return false;
    }
    // Berkas .meta ikut berpindah. Kalau tertinggal, aset yang dinamai ulang
    // akan dianggap baru pada pemindaian berikutnya dan mendapat GUID baru —
    // memutus tepat referensi yang seharusnya dijaga utuh.
    std::filesystem::rename(from.string() + kMetaExtension, to.string() + kMetaExtension, code);

    ScanNow();
    return true;
}

bool AssetDatabase::Move(const Uuid& guid, std::string_view newFolder, std::string& error) {
    const AssetRecord* record = Find(guid);
    if (record == nullptr) {
        error = "asset not found";
        return false;
    }

    const std::filesystem::path from = AbsolutePath(*record);
    const std::filesystem::path targetDir =
        newFolder.empty() ? root_ : root_ / std::filesystem::path(newFolder);
    const std::filesystem::path to = targetDir / from.filename();
    if (from == to) {
        return true;
    }

    std::error_code code;
    std::filesystem::create_directories(targetDir, code);
    if (std::filesystem::exists(to, code)) {
        error = "a file with that name already exists in the target folder";
        return false;
    }
    std::filesystem::rename(from, to, code);
    if (code) {
        error = code.message();
        return false;
    }
    std::filesystem::rename(from.string() + kMetaExtension, to.string() + kMetaExtension, code);

    ScanNow();
    return true;
}

bool AssetDatabase::Delete(const Uuid& guid, std::string& error) {
    const AssetRecord* record = Find(guid);
    if (record == nullptr) {
        error = "asset not found";
        return false;
    }
    const std::filesystem::path path = AbsolutePath(*record);

    std::error_code code;
    std::filesystem::remove(path, code);
    if (code) {
        error = code.message();
        return false;
    }
    std::filesystem::remove(path.string() + kMetaExtension, code);

    ScanNow();
    return true;
}

}  // namespace sim::assets
