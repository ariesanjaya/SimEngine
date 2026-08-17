/// SimCook: menyiapkan sebuah project untuk dikirim.
///
/// **Yang dikirim adalah yang terjangkau, bukan yang ada.** Folder aset sebuah
/// project berisi percobaan, tekstur yang sudah diganti, dan model yang dipakai
/// sekali lalu dilupakan — semuanya sah selama pengembangan dan semuanya tidak
/// punya alasan ikut ke tangan pemain. Yang menentukan mana yang ikut adalah
/// graf ketergantungan E5, ditelusuri dari level yang benar-benar dimainkan.
///
/// Tidak menautkan perender maupun editor: ini alat berkas, dan alat berkas yang
/// menuntut GPU adalah alat yang tidak bisa dijalankan di CI.

#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Assets/AssetTypes.h"
#include "Sim/Assets/Cook.h"
#include "Sim/Assets/TextureBakery.h"
#include "Sim/Core/Log.h"
#include "Sim/Core/TaskPool.h"
#include "Sim/Scene/Project.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string_view FlagValue(int argc, char** argv, std::string_view name) {
    for (int at = 1; at + 1 < argc; ++at) {
        if (argv[at] != nullptr && std::string_view(argv[at]) == name) {
            return argv[at + 1] == nullptr ? std::string_view{} : std::string_view(argv[at + 1]);
        }
    }
    return {};
}

bool HasFlag(int argc, char** argv, std::string_view name) {
    for (int at = 1; at < argc; ++at) {
        if (argv[at] != nullptr && std::string_view(argv[at]) == name) {
            return true;
        }
    }
    return false;
}

/// Semua `--level` yang disebut, bukan hanya yang pertama.
std::vector<std::string> LevelFlags(int argc, char** argv) {
    std::vector<std::string> levels;
    for (int at = 1; at + 1 < argc; ++at) {
        if (argv[at] != nullptr && std::string_view(argv[at]) == "--level" &&
            argv[at + 1] != nullptr) {
            levels.emplace_back(argv[at + 1]);
        }
    }
    return levels;
}

void PrintUsage() {
    std::fputs(
        "SimCook — menyiapkan project untuk dikirim.\n"
        "\n"
        "  --project <path>   wajib\n"
        "  --out <dir>        folder keluaran; wajib kecuali --dry-run\n"
        "  --level <name>     boleh diulang; bawaan seluruh level project\n"
        "  --dry-run          hanya laporkan rencananya, jangan tulis apa pun\n"
        "  --no-bake          salin tekstur apa adanya, jangan panggang ke .ktx2\n",
        stderr);
}

std::string Megabytes(std::uintmax_t bytes) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f MB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buffer;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace sim;

    Log::Init({});
    const std::string_view projectFlag = FlagValue(argc, argv, "--project");
    const std::string_view outFlag = FlagValue(argc, argv, "--out");
    const bool dryRun = HasFlag(argc, argv, "--dry-run");
    if (projectFlag.empty() || (outFlag.empty() && !dryRun)) {
        PrintUsage();
        return 2;
    }

    scene::Project project;
    std::string projectError;
    if (!scene::LoadProject(project, std::filesystem::path(projectFlag), projectError)) {
        SIM_ERROR("Cook", "cannot open project {}: {}", std::string(projectFlag), projectError);
        return 1;
    }

    TaskPool tasks;
    assets::AssetDatabase database;
    assets::AssetDatabase::Config config;
    config.root = project.AssetsDirectory();
    config.tasks = &tasks;
    if (!database.Initialize(config)) {
        SIM_ERROR("Cook", "cannot index {}", project.AssetsDirectory().string());
        return 1;
    }
    database.ScanNow();

    std::vector<std::filesystem::path> levels;
    const std::vector<std::string> requested = LevelFlags(argc, argv);
    if (requested.empty()) {
        std::error_code ec;
        for (const auto& entry :
             std::filesystem::directory_iterator(project.LevelsDirectory(), ec)) {
            if (entry.path().extension() == ".simlevel") {
                levels.push_back(entry.path());
            }
        }
    } else {
        for (const std::string& name : requested) {
            const std::filesystem::path path =
                project.LevelsDirectory() / (name + ".simlevel");
            if (!std::filesystem::exists(path)) {
                SIM_ERROR("Cook", "no level called {}", name);
                return 1;
            }
            levels.push_back(path);
        }
    }
    if (levels.empty()) {
        SIM_ERROR("Cook", "no levels in {}", project.LevelsDirectory().string());
        return 1;
    }

    const assets::CookPlan plan = assets::PlanCook(database, levels);

    std::printf("project   %s\n", project.name.c_str());
    std::printf("levels    %zu\n", plan.levels.size());
    std::printf("keep      %zu assets, %s\n", plan.reachable.size(),
                Megabytes(plan.reachableBytes).c_str());
    std::printf("prune     %zu assets, %s\n", plan.unreachable.size(),
                Megabytes(plan.unreachableBytes).c_str());
    // Yang dipangkas disebut satu per satu. Sebuah angka saja tidak bisa
    // dibantah maupun dibenarkan oleh siapa pun yang membacanya — dan aset yang
    // hilang dari permainan adalah hal yang orang ingin periksa sendiri.
    for (const Uuid& guid : plan.unreachable) {
        if (const assets::AssetRecord* record = database.Find(guid)) {
            std::printf("  - %s\n", record->relativePath.c_str());
        }
    }
    if (dryRun) {
        return 0;
    }

    const std::filesystem::path out(outFlag);
    std::error_code ec;
    std::filesystem::create_directories(out / "Assets", ec);
    std::filesystem::create_directories(out / "Levels", ec);
    if (ec) {
        SIM_ERROR("Cook", "cannot create {}: {}", out.string(), ec.message());
        return 1;
    }

    // **Bake di tempat, bukan di TaskPool.** Cook adalah proses yang keluar
    // begitu selesai; pekerjaan yang diantrekan ke kolam adalah pekerjaan yang
    // bisa belum selesai saat berkasnya hendak disalin.
    assets::TextureBakery bakery(out / ".cook-cache", nullptr);
    const bool bakeTextures = !HasFlag(argc, argv, "--no-bake");

    std::uintmax_t copied = 0;
    std::uintmax_t bakedCount = 0;
    std::uintmax_t bakedBytes = 0;
    std::uintmax_t sourceBytes = 0;
    for (const Uuid& guid : plan.reachable) {
        const assets::AssetRecord* record = database.Find(guid);
        if (record == nullptr) {
            continue;
        }
        const std::filesystem::path source = database.AbsolutePath(*record);
        std::filesystem::path destination = out / "Assets" / record->relativePath;
        std::filesystem::create_directories(destination.parent_path(), ec);

        // **Tekstur dikirim sebagai `.ktx2`, sumbernya tidak ikut.** Itu arti
        // "siap-pakai": yang di tangan pemain tidak perlu decoder PNG maupun
        // encoder BC7, dan tidak menghabiskan detik pertama permainan untuk
        // memanggang sesuatu yang bisa dipanggang sekali di sini.
        bool wroteBaked = false;
        if (bakeTextures && record->type == assets::AssetType::Texture &&
            source.extension() != ".ktx2") {
            const assets::BakedTextureRef baked = bakery.Request(source);
            if (baked.state == assets::BakeState::Ready) {
                destination.replace_extension(".ktx2");
                std::filesystem::copy_file(baked.path, destination,
                                           std::filesystem::copy_options::overwrite_existing,
                                           ec);
                if (!ec) {
                    wroteBaked = true;
                    ++bakedCount;
                    sourceBytes += record->fileSize;
                    bakedBytes += std::filesystem::file_size(destination, ec);
                }
            } else {
                // Dikatakan, bukan didiamkan: tekstur yang gagal dipanggang tetap
                // ikut sebagai sumbernya, dan yang membacanya berhak tahu kenapa
                // hasil cook-nya lebih besar dari yang ia harapkan.
                SIM_WARN("Cook", "cannot bake {}, shipping the source", record->relativePath);
            }
        }
        if (!wroteBaked) {
            std::filesystem::copy_file(source, destination,
                                       std::filesystem::copy_options::overwrite_existing, ec);
        }
        if (ec) {
            SIM_ERROR("Cook", "cannot write {}: {}", record->relativePath, ec.message());
            return 1;
        }
        ++copied;
        // **`.meta` ikut, dan itu bukan kerapian.** GUID sebuah aset tersimpan
        // di sana; tanpanya, indeks di sisi pemain memberi GUID baru dan setiap
        // referensi di setiap level menunjuk ke tempat yang tidak ada lagi.
        //
        // Namanya mengikuti berkas yang benar-benar ditulis, bukan sumbernya:
        // `batu.png.meta` di sebelah `batu.ktx2` tidak dilihat indeks mana pun,
        // dan teksturnya lalu mendapat GUID baru — yang berarti setiap material
        // yang menunjuknya menunjuk ke tempat yang tidak ada lagi.
        const std::filesystem::path meta = source.string() + ".meta";
        if (std::filesystem::exists(meta, ec)) {
            std::filesystem::copy_file(meta, destination.string() + ".meta",
                                       std::filesystem::copy_options::overwrite_existing, ec);
        }
    }

    for (const std::filesystem::path& level : plan.levels) {
        std::filesystem::copy_file(level, out / "Levels" / level.filename(),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            SIM_ERROR("Cook", "cannot copy {}: {}", level.string(), ec.message());
            return 1;
        }
    }

    const std::filesystem::path projectFile = project.root / "project.simproj";
    if (std::filesystem::exists(projectFile, ec)) {
        std::filesystem::copy_file(projectFile, out / "project.simproj",
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }

    // Cache bake tidak ikut dikirim: ia hanya perantara.
    std::filesystem::remove_all(out / ".cook-cache", ec);

    if (bakedCount > 0) {
        // **Angkanya sering naik, dan itu bukan kesalahan.** BC7 dengan rantai
        // mip lebih besar di disk daripada JPG; yang ditukar adalah ukuran unduh
        // dengan waktu muat dan VRAM yang bisa diperkirakan — pemain tidak lagi
        // menunggu decoder dan encoder bekerja di detik pertama. Separuh yang
        // hilang adalah supercompression KTX2 (Zstd), yang belum ditulis baker.
        // `--no-bake` untuk yang lebih memilih unduhan kecil.
        std::printf("baked     %ju textures, %s of sources became %s of .ktx2\n", bakedCount,
                    Megabytes(sourceBytes).c_str(), Megabytes(bakedBytes).c_str());
        if (bakedBytes > sourceBytes) {
            std::printf("          (larger on disk, faster to load; KTX2 supercompression "
                        "would close the gap)\n");
        }
    }
    std::printf("wrote     %ju assets and %zu levels to %s\n",
                static_cast<std::uintmax_t>(copied), plan.levels.size(), out.string().c_str());
    return 0;
}
