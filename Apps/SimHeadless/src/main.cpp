/// SimEngine tanpa jendela: editor yang sama, dikendalikan hanya lewat MCP.
///
/// **Bukan program kedua yang meniru editor.** Ia menyusun `EditorApp` yang
/// sama, `ToolRegistry` yang sama, dan `McpServer` yang sama — yang tidak
/// dipasangnya hanyalah ImGui, jendela, dan panel. Itu yang membuat sebuah tool
/// menjawab hal yang sama di kedua mode: tidak ada salinan kedua yang bisa
/// tertinggal.
///
/// Gunanya tugas batch yang dijalankan agen di CI: konversi aset, validasi
/// seluruh level, regresi visual — semuanya di mesin tanpa display.

#include "Sim/AIBridge/McpServer.h"
#include "Sim/AIBridge/ResourceRegistry.h"
#include "Sim/AIBridge/ToolRegistry.h"
#include "Sim/Core/Log.h"
#include "Sim/Core/MainThreadQueue.h"
#include "Sim/Core/TaskPool.h"
#include "Sim/Editor/AiTools.h"
#include "Sim/Editor/EditorApp.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/SceneView.h"
#include "Sim/Editor/Selection.h"
#include "Sim/ImageIO/ImageIO.h"
#include "Sim/RHI/Device.h"
#include "Sim/RHI/TextureRegistry.h"
#include "Sim/Render/RendererFactory.h"

#if SIM_WITH_LUA
#include "Sim/Script/ScriptRuntime.h"
#endif

#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

/// Jembatan tekstur yang tidak menjembatani ke mana-mana.
///
/// `IViewportRenderer` menuntutnya karena di editor ia menyerahkan target render
/// ke ImGui. Di sini tidak ada yang menggambar hasilnya ke layar — yang
/// membacanya adalah `CapturePixels`, langsung dari target rendernya — jadi
/// handle-nya cukup angka yang naik. Mengembalikan nol berarti "tidak ada
/// tekstur", dan itu arti yang berbeda.
class NullTextureRegistry final : public sim::rhi::ITextureRegistry {
public:
    uint64_t Acquire(VkImageView, VkSampler) override { return ++next_; }
    void Release(uint64_t) override {}

private:
    uint64_t next_ = 0;
};

std::atomic<bool> gStopping{false};

void OnSignal(int) { gStopping.store(true); }

/// Nilai sebuah bendera `--nama nilai`. Kosong bila benderanya tidak ada.
std::string_view FlagValue(int argc, char** argv, std::string_view name) {
    for (int at = 1; at + 1 < argc; ++at) {
        if (argv[at] != nullptr && std::string_view(argv[at]) == name) {
            return argv[at + 1] == nullptr ? std::string_view{} : std::string_view(argv[at + 1]);
        }
    }
    return {};
}

void PrintUsage() {
    std::fputs(
        "SimHeadless — SimEngine dikendalikan lewat MCP, tanpa jendela.\n"
        "\n"
        "  --project <path>              wajib\n"
        "  --level <name>                level yang dibuka saat start\n"
        "  --level-file <path>           level dari jalur berkas, di luar project\n"
        "  --mcp-port <port>             bawaan 7777\n"
        "  --mcp-permission <mode>       read-only | ask | auto (bawaan auto)\n"
        "  --render <width>x<height>     ukuran viewport offscreen, bawaan 1280x720\n"
        "  --no-render                   tanpa perender; viewport.capture tidak didaftarkan\n"
        "  --headless                    diterima dan diabaikan; program ini selalu headless\n"
        "\n"
        "Mode ukur (G0, docs/PLAN-GPU-OPTIM.md) — tanpa server MCP:\n"
        "  --bench                       jalankan lintasan kamera terkunci lalu keluar\n"
        "  --bench-frames <n>            frame yang diukur, bawaan 240\n"
        "  --bench-warmup <n>            frame pemanasan yang dibuang, bawaan 90\n"
        "  --bench-out <path>            tulis tabelnya sebagai Markdown\n"
        "  --bench-gi <on|off>           paksa GI; bawaannya mengikuti level\n"
        "  --bench-capture <path.png>    simpan sebuah frame; kameranya deterministik\n"
        "  --bench-capture-frame <n>     frame mana yang disimpan, bawaan yang terakhir\n"
        "  --bench-fixed-exposure        eksposur manual; wajib untuk membandingkan gambar\n"
        "  --bench-compute               ganti gambarnya dengan pass compute uji (G3)\n"
        "  --validate-sync               nyalakan validasi sinkronisasi; lambat, sengaja\n"
        "  --bench-cpu-clusters          penetapan cluster di CPU, bukan GPU (G4)\n"
        "  --bench-cpu-sdf               komposit clipmap SDF di CPU, bukan GPU (G4)\n"
        "  --bench-cpu-cull              culling dan perintah gambar di CPU (G6)\n"
        "  --bench-occlusion             nyalakan occlusion culling; belum tepat, lihat G6\n"
        "  --bench-dump-cull <path>      angka antara uji occlusion tiap permukaan\n"
        "  --no-bindless                 material lewat set per ruas, bukan bindless (G5)\n"
        "  --bench-dump-sdf <path>       simpan isi voxel kaskade SDF; untuk dibandingkan\n",
        stderr);
}

/// Satu deret angka milidetik untuk satu nama pass.
struct Samples {
    std::string name;
    std::vector<float> milliseconds;
    /// Primitif yang diserahkan pass ini, per frame. Kosong untuk tahap CPU.
    std::vector<uint64_t> primitives;

    /// Median primitif. **Median dan bukan rata-rata, alasan yang sama dengan
    /// waktu** — dan di sini alasannya lebih tajam lagi: satu frame yang
    /// hasilnya belum siap dipungut sebagai nol, dan satu nol menyeret
    /// rata-rata sementara median tidak bergerak sama sekali.
    uint64_t MedianPrimitives() const {
        if (primitives.empty()) {
            return 0;
        }
        std::vector<uint64_t> sorted = primitives;
        std::sort(sorted.begin(), sorted.end());
        return sorted[sorted.size() / 2];
    }

    double Mean() const {
        if (milliseconds.empty()) {
            return 0.0;
        }
        double total = 0.0;
        for (const float value : milliseconds) {
            total += static_cast<double>(value);
        }
        return total / static_cast<double>(milliseconds.size());
    }

    /// **Median, bukan hanya rata-rata.** Satu frame yang tersendat karena
    /// kompilasi pipeline atau penjadwal OS menggeser rata-rata dan tidak
    /// menggeser median; dua angka yang berjauhan adalah tanda bahwa yang
    /// diukur belum stabil, dan itu informasi yang justru dibutuhkan.
    double Median() const {
        if (milliseconds.empty()) {
            return 0.0;
        }
        std::vector<float> sorted = milliseconds;
        std::sort(sorted.begin(), sorted.end());
        const std::size_t middle = sorted.size() / 2;
        if (sorted.size() % 2 == 1) {
            return static_cast<double>(sorted[middle]);
        }
        return 0.5 * (static_cast<double>(sorted[middle - 1]) +
                      static_cast<double>(sorted[middle]));
    }

    double Max() const {
        double peak = 0.0;
        for (const float value : milliseconds) {
            peak = std::max(peak, static_cast<double>(value));
        }
        return peak;
    }
};

/// Kumpulan deret yang urutannya adalah urutan kemunculan pertama.
///
/// Peta ber-hash akan mengurutkannya sesuka hash-nya, dan tabel yang barisnya
/// berpindah tempat antar-jalan adalah tabel yang tidak bisa dibandingkan
/// dengan mata.
class SampleTable {
public:
    void Add(std::string_view name, float milliseconds, uint64_t primitives = 0) {
        for (Samples& entry : entries_) {
            if (entry.name == name) {
                entry.milliseconds.push_back(milliseconds);
                entry.primitives.push_back(primitives);
                return;
            }
        }
        entries_.push_back(Samples{std::string(name), {milliseconds}, {primitives}});
    }

    const std::vector<Samples>& Entries() const { return entries_; }
    bool Empty() const { return entries_.empty(); }

private:
    std::vector<Samples> entries_;
};

std::string FormatTable(std::string_view title, const SampleTable& table) {
    if (table.Empty()) {
        return std::string("### ") + std::string(title) + "\n\n(tidak ada angka)\n";
    }
    std::string out = "### ";
    out += title;
    // **Kolom primitif hanya muncul kalau ada yang mengisinya.** Tabel CPU
    // tidak punya angka itu, dan kolom kosong di sebelah angka yang berarti
    // adalah kolom yang membuat pembacanya mengira ada yang hilang.
    bool anyPrimitives = false;
    for (const Samples& entry : table.Entries()) {
        anyPrimitives = anyPrimitives || entry.MedianPrimitives() > 0;
    }
    out += anyPrimitives ? "\n\n| Pass | Median | Rata-rata | Puncak | Primitif |\n"
                           "|---|---:|---:|---:|---:|\n"
                         : "\n\n| Pass | Median | Rata-rata | Puncak |\n|---|---:|---:|---:|\n";
    double medianTotal = 0.0;
    double meanTotal = 0.0;
    uint64_t primitiveTotal = 0;
    for (const Samples& entry : table.Entries()) {
        char line[256];
        if (anyPrimitives) {
            std::snprintf(line, sizeof(line), "| `%s` | %.3f | %.3f | %.3f | %llu |\n",
                          entry.name.c_str(), entry.Median(), entry.Mean(), entry.Max(),
                          static_cast<unsigned long long>(entry.MedianPrimitives()));
        } else {
            std::snprintf(line, sizeof(line), "| `%s` | %.3f | %.3f | %.3f |\n",
                          entry.name.c_str(), entry.Median(), entry.Mean(), entry.Max());
        }
        out += line;
        primitiveTotal += entry.MedianPrimitives();
        // **Lingkup yang namanya berakhiran `-total` tidak ikut dijumlahkan.**
        // `cpu-total` membungkus seluruh baris lain; memasukkannya ke dalam
        // jumlah berarti menghitung frame yang sama dua kali dan menghasilkan
        // angka yang tidak berarti apa-apa — dan angka yang tidak berarti
        // apa-apa di baris paling tebal adalah angka yang paling sering dikutip.
        if (entry.name.size() >= 6 &&
            entry.name.compare(entry.name.size() - 6, 6, "-total") == 0) {
            continue;
        }
        medianTotal += entry.Median();
        meanTotal += entry.Mean();
    }
    char total[192];
    if (anyPrimitives) {
        std::snprintf(total, sizeof(total),
                      "| **jumlah baris di atas** | **%.3f** | **%.3f** | | **%llu** |\n",
                      medianTotal, meanTotal,
                      static_cast<unsigned long long>(primitiveTotal));
    } else {
        std::snprintf(total, sizeof(total),
                      "| **jumlah baris di atas** | **%.3f** | **%.3f** | |\n", medianTotal,
                      meanTotal);
    }
    out += total;
    return out;
}

/// Kotak batas seluruh mesh yang benar-benar dikirim ke perender.
///
/// Dipakai menurunkan lintasan kamera dari adegannya sendiri, supaya satu
/// perintah yang sama bekerja untuk level mana pun tanpa ada yang perlu
/// mengarang koordinat. Dihitung **sekali**, dari frame pertama: menghitungnya
/// ulang tiap frame membuat lintasannya bergantung pada apa yang kebetulan
/// terlihat, dan lintasan yang berubah adalah pengukuran yang tidak bisa
/// diulang.
bool SceneBounds(const sim::render::ViewportScene& scene, sim::Vec3& outMin,
                 sim::Vec3& outMax) {
    bool any = false;
    for (const sim::render::MeshInstance& mesh : scene.meshes) {
        // Delapan sudut, bukan dua titik yang ikut ditransformasi: kotak yang
        // diputar punya sudut yang tidak lagi menjadi min/max-nya.
        for (int corner = 0; corner < 8; ++corner) {
            const sim::Vec3 local((corner & 1) != 0 ? mesh.boundsMax.x : mesh.boundsMin.x,
                                  (corner & 2) != 0 ? mesh.boundsMax.y : mesh.boundsMin.y,
                                  (corner & 4) != 0 ? mesh.boundsMax.z : mesh.boundsMin.z);
            const sim::Vec4 world = mesh.transform * sim::Vec4(local, 1.0f);
            const sim::Vec3 point(world.x, world.y, world.z);
            outMin = any ? glm::min(outMin, point) : point;
            outMax = any ? glm::max(outMax, point) : point;
            any = true;
        }
    }
    return any;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace sim;

    MainThreadQueue::Get().BindMainThread();

    const std::filesystem::path configDir =
        std::filesystem::path(std::getenv("HOME") == nullptr ? "." : std::getenv("HOME")) /
        ".simengine";
    Log::Init(configDir / "Logs" / "headless.log");

    const std::string_view projectFlag = FlagValue(argc, argv, "--project");
    if (projectFlag.empty()) {
        PrintUsage();
        return 2;
    }

    // **Mode izin diurai sebelum apa pun dinyalakan.** Bendera yang salah ketik
    // yang diam-diam jatuh ke bawaan adalah persis cara sebuah sesi berjalan
    // dengan izin yang lebih longgar daripada yang diminta.
    ai::PermissionMode permission = ai::PermissionMode::Auto;
    if (const std::string_view mode = FlagValue(argc, argv, "--mcp-permission"); !mode.empty()) {
        if (!ai::PermissionModeFromString(mode, permission)) {
            SIM_ERROR("Headless", "unknown --mcp-permission \"{}\"; known: read-only, ask, auto",
                      std::string(mode));
            return 2;
        }
    }

    uint32_t renderWidth = 1280;
    uint32_t renderHeight = 720;
    if (const std::string_view size = FlagValue(argc, argv, "--render"); !size.empty()) {
        const std::size_t cross = size.find('x');
        if (cross == std::string_view::npos) {
            SIM_ERROR("Headless", "--render wants <width>x<height>, got \"{}\"",
                      std::string(size));
            return 2;
        }
        renderWidth = static_cast<uint32_t>(std::atoi(std::string(size.substr(0, cross)).c_str()));
        renderHeight =
            static_cast<uint32_t>(std::atoi(std::string(size.substr(cross + 1)).c_str()));
        if (renderWidth == 0 || renderHeight == 0) {
            SIM_ERROR("Headless", "--render wants two positive numbers");
            return 2;
        }
    }

    bool wantRenderer = true;
    for (int at = 1; at < argc; ++at) {
        if (argv[at] != nullptr && std::string_view(argv[at]) == "--no-render") {
            wantRenderer = false;
        }
    }

    SIM_INFO("Headless", "SimHeadless 0.1.0 starting");

    // **Device tanpa satu pun ekstensi instance.** Itulah yang membuatnya tidak
    // menyentuh surface, dan karena itu tidak menuntut display server. Jalur ini
    // sudah dipakai uji unggahan tekstur sejak sebelum ada mode headless.
    rhi::Device device;
    NullTextureRegistry textures;
    std::unique_ptr<render::IViewportRenderer> renderer;
    if (wantRenderer) {
        rhi::DeviceDesc deviceDesc;
        deviceDesc.pipelineCachePath = configDir / "Cache" / "pipeline-headless.bin";
        // Validasi sinkronisasi: mahal, jadi diminta sendiri. Dipakai sesudah
        // sebuah pass baru ditambahkan — barrier yang hilang tidak melanggar
        // satu pun aturan layout, dan tanpa lapisan ini ia hanya terlihat
        // sebagai hasil yang berubah-ubah di mesin orang lain.
        for (int at = 1; at < argc; ++at) {
            if (argv[at] != nullptr && std::string_view(argv[at]) == "--validate-sync") {
                deviceDesc.enableValidation = true;
                deviceDesc.enableSyncValidation = true;
            }
        }
        if (!device.Create(deviceDesc)) {
            SIM_WARN("Headless", "no Vulkan device; running without a renderer");
            wantRenderer = false;
        } else {
            render::StubRendererDesc rendererDesc;
            rendererDesc.shaderDirectory =
                std::filesystem::path(argv[0]).parent_path() / "Shaders";
            rendererDesc.initialWidth = renderWidth;
            rendererDesc.initialHeight = renderHeight;
            // **Sakelar yang menjaga jalur mundur tetap dijalankan.** Kriteria
            // selesai G5 menuntut kedua jalur menghasilkan gambar yang sama,
            // dan perbandingan itu hanya bisa dilakukan kalau keduanya bisa
            // diminta pada mesin yang sama, pada adegan yang sama.
            for (int at = 1; at < argc; ++at) {
                if (argv[at] != nullptr && std::string_view(argv[at]) == "--no-bindless") {
                    rendererDesc.materialBinding =
                        render::MaterialBindingPreference::ForceClassic;
                }
            }
            renderer = render::CreateVulkanRenderer(device, textures, rendererDesc);
            if (renderer == nullptr) {
                // Jatuh kembali, alasan yang sama dengan editor: yang menolak
                // jalan di mesin lama tidak bisa dipakai mengerjakan data.
                renderer = render::CreateStubRenderer(device, textures, rendererDesc);
            }
            wantRenderer = renderer != nullptr;
        }
    }

    TaskPool tasks;
#if SIM_WITH_LUA
    script::ScriptRuntime scripts;
#endif

    editor::EditorApp app;
    editor::EditorApp::Config config;
    config.configDir = configDir;
    config.resourceDir = std::filesystem::path(argv[0]).parent_path() / "Resources";
    config.shaderDir = std::filesystem::path(argv[0]).parent_path() / "Shaders";
    config.tasks = &tasks;
    config.viewportRenderer = renderer.get();
#if SIM_WITH_LUA
    config.scripts = &scripts;
#endif
    // **Tanpa skrip editor.** Yang di folder itu menambah menu dan panel, dan
    // keduanya menyentuh ImGui yang tidak ada di sini. Skrip gameplay tetap
    // berjalan lewat Play, dengan runtime yang sama.
    config.headless = true;
    if (!app.Initialize(config)) {
        SIM_ERROR("Headless", "editor core did not start");
        return 1;
    }

    if (!app.OpenProject(std::filesystem::path(projectFlag))) {
        SIM_ERROR("Headless", "cannot open project {}", std::string(projectFlag));
        app.Shutdown();
        return 1;
    }
    if (const std::string_view level = FlagValue(argc, argv, "--level"); !level.empty()) {
        const std::filesystem::path path =
            app.LevelsDirectory() / (std::string(level) + ".simlevel");
        if (!app.LoadLevel(path)) {
            SIM_ERROR("Headless", "cannot open level {}", std::string(level));
            app.Shutdown();
            return 1;
        }
    }
    // **Level dari jalur berkas, bukan dari nama di dalam project.** Adegan uji
    // G0 hidup di `Resources/Levels` — isi bawaan yang sama untuk setiap project
    // — supaya garis dasarnya tidak bergantung pada project siapa pun yang
    // kebetulan ada di mesin yang mengukur. Asetnya tetap terjangkau:
    // `SceneView` mencari GUID yang tidak ada di project ke indeks bawaan.
    if (const std::string_view file = FlagValue(argc, argv, "--level-file"); !file.empty()) {
        if (!app.LoadLevel(std::filesystem::path(file))) {
            SIM_ERROR("Headless", "cannot open level file {}", std::string(file));
            app.Shutdown();
            return 1;
        }
    }

    editor::Selection selection;
    editor::SceneView sceneView;
    // **Material sungguhan ikut diukur, bukan hanya jalur mundur.**
    //
    // Sampai G5 sambungan ini hanya dipasang `ViewportPanel`, dan SimHeadless
    // tidak punya panel — jadi seluruh adegan uji digambar `box.frag` dan
    // pipeline material tidak pernah tersentuh alat ukurnya sendiri. Itu
    // menyembunyikan persis apa yang G5 urus: ikatan descriptor per ruas datang
    // dari keragaman material, dan adegan yang seluruhnya memakai satu shader
    // jalur mundur tidak punya keragaman itu.
    //
    // Akibatnya angka G5 tidak sebanding dengan baris "sesudah" G4 — dan itu
    // disebutkan di dokumen rencananya, bukan didiamkan.
    sceneView.SetMaterialPrograms(app.Context().materialPrograms);
    // Kamera milik kedua loop di bawah, bukan milik panel — tidak ada panel. Di
    // mode MCP ia hanya berpindah kalau agen memintanya lewat
    // `viewport.capture`; di mode ukur ia mengikuti lintasan terkunci.
    render::Camera camera;

    // Menyimpan isi target render ke berkas. Dipakai mode ukur, dan dipisah
    // supaya frame di tengah lintasan dan frame terakhir memakai jalur yang sama.
    const auto writeCapture = [](render::IViewportRenderer& target, std::string_view path) {
        std::vector<uint8_t> rgba;
        uint32_t width = 0;
        uint32_t height = 0;
        std::string readbackError;
        if (!target.CapturePixels(rgba, width, height, readbackError)) {
            SIM_ERROR("Bench", "cannot read back the render target: {}", readbackError);
            return;
        }
        imageio::Image image;
        image.desc.width = width;
        image.desc.height = height;
        image.desc.channels = 4;
        image.desc.type = imageio::PixelType::UInt8;
        image.bytes = std::move(rgba);
        const imageio::ImageIoResult written =
            imageio::Write(std::filesystem::path(path), image);
        if (!written.ok) {
            SIM_ERROR("Bench", "cannot write {}: {}", std::string(path), written.error);
        } else {
            SIM_INFO("Bench", "frame written to {}", std::string(path));
        }
    };

    // Menyimpan angka antara uji occlusion. Dipisah karena alasan yang sama
    // dengan `writeCapture`: frame di tengah lintasan dan frame terakhir harus
    // memakai jalur yang sama.
    const auto writeCullDump = [](render::IViewportRenderer& target, std::string_view path) {
        std::string text;
        std::string dumpError;
        if (!target.CaptureCullDebug(text, dumpError)) {
            SIM_ERROR("Bench", "cannot read the cull data: {}", dumpError);
            return;
        }
        std::ofstream file{std::filesystem::path(path)};
        if (!file) {
            SIM_ERROR("Bench", "cannot write {}", std::string(path));
            return;
        }
        file << text;
        SIM_INFO("Bench", "cull data written to {}", std::string(path));
    };

    // --- Mode ukur (G0) ------------------------------------------------------
    //
    // **Sebelum server MCP dinyalakan, dan keluar tanpa pernah menyalakannya.**
    // Yang mengukur tidak boleh berbagi proses dengan yang menunggu soket:
    // permintaan yang datang di tengah lintasan mengubah adegan yang sedang
    // diukur, dan angkanya lalu menjelaskan frame yang berbeda dari yang
    // dijelaskan judul tabelnya.
    bool bench = false;
    for (int at = 1; at < argc; ++at) {
        if (argv[at] != nullptr && std::string_view(argv[at]) == "--bench") {
            bench = true;
        }
    }
    if (bench) {
        if (renderer == nullptr) {
            SIM_ERROR("Bench", "--bench needs a renderer; --no-render was given or Vulkan is absent");
            app.Shutdown();
            return 1;
        }

        uint32_t measured = 240;
        uint32_t warmup = 90;
        if (const std::string_view value = FlagValue(argc, argv, "--bench-frames");
            !value.empty()) {
            measured = static_cast<uint32_t>(std::atoi(std::string(value).c_str()));
        }
        if (const std::string_view value = FlagValue(argc, argv, "--bench-warmup");
            !value.empty()) {
            warmup = static_cast<uint32_t>(std::atoi(std::string(value).c_str()));
        }
        if (measured == 0) {
            SIM_ERROR("Bench", "--bench-frames must be at least 1");
            app.Shutdown();
            return 2;
        }

        // Pemeriksa jalur compute (G3). Sebuah bendera dan bukan hanya sakelar
        // di editor, karena yang harus lulus adalah dua hal yang tidak bisa
        // dilihat dari editor tanpa dibaca satu per satu: pass-nya muncul di
        // tabel waktu GPU, dan validation layer tidak mengeluh sekali pun.
        // Keduanya diperiksa dari satu perintah di sini.
        bool computeGradient = false;
        // Jalur CPU penetapan cluster, untuk membandingkan gambarnya dengan
        // jalur GPU (G4). Dua implementasi yang harus sepakat adalah cara
        // termurah menemukan yang mana yang salah — dan perbandingannya harus
        // bisa dijalankan dari satu perintah, bukan dari dua build.
        bool cpuClusters = false;
        bool cpuSdf = false;
        bool cpuCull = false;
        bool occlusion = false;
        // Frame mana yang ditangkap. **Bawaannya yang terakhir, dan itu tidak
        // selalu berguna:** lintasan kamera menutup satu putaran penuh, jadi
        // frame terakhir selalu berdiri di tempat yang sama dengan frame
        // pertama — dan pada sebagian adegan tempat itu tidak memperlihatkan
        // apa pun. Perbandingan gambar yang hanya bisa dilakukan dari satu sudut
        // adalah perbandingan yang mudah lulus tanpa berarti.
        uint32_t captureFrame = 0;
        bool captureFrameSet = false;
        // **Eksposur otomatis adalah gelung umpan balik, dan itu merusak setiap
        // perbandingan gambar.** Ia mengukur adegan lalu beradaptasi terhadap
        // waktu, jadi selisih sekecil apa pun di sebuah frame — satu benda di
        // tepi frustum yang lolos di satu jalur dan tidak di jalur lain —
        // menggeser eksposur frame berikutnya, dan geseran itu menumpuk. Yang
        // terlihat di ujung bukan selisih isinya melainkan seluruh gambar yang
        // lebih terang. Dengan eksposur manual, yang dibandingkan kembali isinya.
        bool fixedExposure = false;
        for (int at = 1; at < argc; ++at) {
            if (argv[at] == nullptr) {
                continue;
            }
            const std::string_view flag(argv[at]);
            if (flag == "--bench-compute") {
                computeGradient = true;
            } else if (flag == "--bench-cpu-clusters") {
                cpuClusters = true;
            } else if (flag == "--bench-cpu-sdf") {
                cpuSdf = true;
            } else if (flag == "--bench-cpu-cull") {
                cpuCull = true;
            } else if (flag == "--bench-occlusion") {
                occlusion = true;
            } else if (flag == "--bench-fixed-exposure") {
                fixedExposure = true;
            }
        }

        // **GI dipaksa dari baris perintah, bukan hanya dari level.** Anggaran
        // GI adalah 3,0 ms dari 16,6 ms — sepertiga dari seluruh pekerjaan yang
        // G0 ingin ukur — jadi garis dasar yang diam-diam mengukur adegan tanpa
        // GI adalah garis dasar yang menjawab pertanyaan lain. Levelnya sendiri
        // tetap yang menentukan bila benderanya tidak ada.
        if (const std::string_view value = FlagValue(argc, argv, "--bench-capture-frame");
            !value.empty()) {
            captureFrame = static_cast<uint32_t>(std::strtoul(std::string(value).c_str(), nullptr, 10));
            captureFrameSet = true;
        }
        if (const std::string_view value = FlagValue(argc, argv, "--bench-gi"); !value.empty()) {
            if (value == "on") {
                app.Context().gi.enabled = true;
            } else if (value == "off") {
                app.Context().gi.enabled = false;
            } else {
                SIM_ERROR("Bench", "--bench-gi wants on or off, got \"{}\"", std::string(value));
                app.Shutdown();
                return 2;
            }
        }

        const uint32_t total = warmup + measured;
        // **Delta tetap, bukan jam dinding.** Animasi, fisika, dan adaptasi
        // eksposur semuanya maju menurut delta; memberi mereka waktu sungguhan
        // berarti mesin yang lebih cepat mensimulasikan adegan yang berbeda,
        // dan dua jalan di mesin yang sama tidak pernah mengukur hal yang sama.
        constexpr float kFixedDelta = 1.0f / 60.0f;

        SampleTable gpu;
        SampleTable cpu;
        uint64_t lastSerial = 0;
        bool haveBounds = false;
        Vec3 boundsMin(0.0f);
        Vec3 boundsMax(0.0f);
        uint32_t gpuSamples = 0;

        SIM_INFO("Bench", "{} ({}x{}), {} warmup + {} measured frames", device.DeviceName(),
                 renderWidth, renderHeight, warmup, measured);

        // **Material harus selesai dikompilasi sebelum frame pertama diukur.**
        //
        // slangc berjalan di `TaskPool` dan memakan detik, sementara seluruh
        // jalan — pemanasan sekaligus pengukuran — selesai dalam waktu yang jauh
        // lebih pendek. Tanpa menunggu di sini, sebagian frame digambar jalur
        // mundur `box.frag` dan sebagian lagi lewat pipeline material, dan
        // pembagiannya bergantung pada berapa cepat mesinnya mengompilasi. Dua
        // jalan berturut-turut lalu tidak akan pernah sepakat — yaitu justru
        // kriteria selesai G0.
        //
        // Frame di sini tidak digambar, hanya disusun: yang menerbitkan
        // permintaan kompilasi adalah `SceneView::Build`, dan yang mengambil
        // hasilnya juga ia.
        if (app.Context().materialPrograms != nullptr) {
            constexpr uint32_t kMaxSettleFrames = 3000;
            uint32_t settle = 0;
            for (; settle < kMaxSettleFrames && !gStopping.load(); ++settle) {
                // **Delta nol, dan itu bukan detail.** Berapa banyak frame yang
                // dibutuhkan di sini bergantung pada apakah `slangc` menjawab
                // dari cache atau harus benar-benar berjalan — jadi ia berbeda
                // antara jalan pertama dan jalan kedua di mesin yang sama.
                // Memajukan waktu dunia sebanyak itu berarti frame yang diukur
                // mulai dari keadaan yang berbeda, dan dua jalan dari binary
                // yang identik menghasilkan gambar yang berbeda. Ditemukan
                // begitu: dua tangkapan yang seharusnya sama byte demi byte
                // ternyata tidak.
                app.Tick(0.0f);
                MainThreadQueue::Get().Drain();
                if (app.Context().world == nullptr) {
                    break;
                }
                sceneView.Build(*app.Context().world, selection, app.Context().assets,
                                renderer.get(), app.Context().animation,
                                app.Context().builtinAssets, app.Context().whiteboxes);
                if (settle > 0 && app.Context().materialPrograms->PendingCount() == 0) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            const std::size_t pending = app.Context().materialPrograms->PendingCount();
            if (pending > 0) {
                // Disebutkan, bukan didiamkan: angka dari jalan yang materialnya
                // belum siap adalah angka untuk adegan yang lain.
                SIM_WARN("Bench", "{} materials are still compiling after {} frames", pending,
                         settle);
            } else {
                SIM_INFO("Bench", "materials ready after {} settle frames ({} compiled)", settle,
                         app.Context().materialPrograms->CompileCount());
            }
        }

        const auto wallStart = std::chrono::steady_clock::now();
        for (uint32_t frame = 0; frame < total && !gStopping.load(); ++frame) {
            app.Tick(kFixedDelta);
            MainThreadQueue::Get().Drain();
            if (app.Context().world == nullptr) {
                SIM_ERROR("Bench", "no world to render");
                app.Shutdown();
                return 1;
            }

            sceneView.Build(*app.Context().world, selection, app.Context().assets, renderer.get(),
                            app.Context().animation, app.Context().builtinAssets,
                            app.Context().whiteboxes);

            if (!haveBounds) {
                haveBounds = SceneBounds(sceneView.Scene(), boundsMin, boundsMax);
                if (!haveBounds) {
                    // Adegan tanpa satu pun mesh tetap bisa diukur — langit,
                    // grid, dan post-process punya biayanya sendiri — jadi ini
                    // bukan kegagalan, hanya lintasan bawaan.
                    boundsMin = Vec3(-5.0f);
                    boundsMax = Vec3(5.0f);
                    haveBounds = true;
                    SIM_WARN("Bench", "scene has no meshes; using a default camera orbit");
                }
                SIM_INFO("Bench", "scene bounds [{:.2f} {:.2f} {:.2f}]..[{:.2f} {:.2f} {:.2f}]",
                         boundsMin.x, boundsMin.y, boundsMin.z, boundsMax.x, boundsMax.y,
                         boundsMax.z);
            }

            // Lintasan: satu putaran penuh sepanjang seluruh jalan, diturunkan
            // dari nomor frame dan bukan dari waktu. Satu putaran penuh supaya
            // tiap sudut pandang ikut terukur — biaya culling dan kaskade
            // bayangan sangat bergantung arah, dan mengukur satu sudut saja
            // menghasilkan angka yang benar untuk satu sudut itu saja.
            const Vec3 centre = (boundsMin + boundsMax) * 0.5f;
            const Vec3 extent = glm::max(boundsMax - boundsMin, Vec3(1.0f));
            const float radius = glm::length(Vec2(extent.x, extent.z)) * 0.9f + 2.0f;
            const float angle = 6.2831853f * static_cast<float>(frame) / static_cast<float>(total);
            const Vec3 eye(centre.x + radius * std::cos(angle),
                           centre.y + extent.y * 0.55f,
                           centre.z + radius * std::sin(angle));
            const Vec3 forward = centre - eye;

            render::ViewportDesc desc;
            desc.width = renderWidth;
            desc.height = renderHeight;
            desc.gi = app.Context().gi;
            desc.computeGradient = computeGradient;
            desc.gpuClusters = !cpuClusters;
            desc.gpuSdf = !cpuSdf;
            desc.gpuCull = !cpuCull;
            desc.gpuOcclusion = occlusion;
            desc.cullDebug = !FlagValue(argc, argv, "--bench-dump-cull").empty();
            if (fixedExposure) {
                desc.post.exposureMode = render::ExposureMode::Manual;
            }
            // Delta yang sama dengan yang diberikan ke `app.Tick`, dan karena
            // alasan yang sama: yang maju menurut waktu — eksposur, awan,
            // akumulasi temporal — harus maju sama jauhnya di tiap jalan, kalau
            // tidak dua gambar dari binary yang identik pun berbeda.
            desc.fixedDeltaSeconds = kFixedDelta;
            camera.position = eye;
            camera.rotation = glm::quatLookAt(glm::normalize(forward), Vec3(0.0f, 1.0f, 0.0f));
            desc.camera = camera;
            renderer->Render(desc, sceneView.Scene());

            if (captureFrameSet && frame == captureFrame) {
                if (const std::string_view shot = FlagValue(argc, argv, "--bench-capture");
                    !shot.empty()) {
                    writeCapture(*renderer, shot);
                }
                if (const std::string_view dump = FlagValue(argc, argv, "--bench-dump-cull");
                    !dump.empty()) {
                    writeCullDump(*renderer, dump);
                }
            }

            if (frame < warmup) {
                continue;
            }

            // **Angka GPU dicatat hanya saat serialnya berpindah.** Pemungutan
            // timestamp melewati hasil yang belum siap alih-alih menunggunya,
            // jadi dua frame berturut-turut bisa membaca angka yang sama persis
            // — dan yang merata-ratakan akan menghitung frame itu dua kali.
            const uint64_t serial = renderer->TimingSerial();
            if (serial != lastSerial) {
                lastSerial = serial;
                for (const render::PassTiming& timing : renderer->PassTimings()) {
                    gpu.Add(timing.name, timing.milliseconds, timing.primitives);
                }
                ++gpuSamples;
            }
            // Yang CPU segar tiap frame; tidak ada yang perlu ditunggu.
            for (const render::PassTiming& timing : renderer->CpuTimings()) {
                cpu.Add(timing.name, timing.milliseconds);
            }
        }
        const double wallSeconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();

        // **Gambar frame terakhir, dan itu bukan pelengkap.** Setiap milestone
        // sesudah G0 mengklaim "lebih cepat dengan gambar yang sama", dan klaim
        // separuhnya tidak bisa dibuktikan angka. Lintasan kameranya
        // deterministik, jadi frame terakhir dua jalan yang berbeda adalah sudut
        // pandang yang sama persis dan bisa dibandingkan piksel per piksel.
        // **Isi voxelnya, bukan gambarnya.** Satu-satunya pembaca kaskade SDF
        // adalah penelusuran GI, dan GI tidak deterministik — jadi dua jalur
        // komposit tidak bisa dibandingkan lewat piksel. Yang bisa: kedua jalan
        // dijalankan dengan lintasan kamera yang sama, lalu berkas ini
        // dibandingkan byte demi byte.
        if (const std::string_view dump = FlagValue(argc, argv, "--bench-dump-sdf");
            !dump.empty()) {
            std::vector<uint8_t> voxels;
            std::string dumpError;
            if (!renderer->CaptureSdf(voxels, dumpError)) {
                SIM_ERROR("Bench", "cannot read back the SDF clipmap: {}", dumpError);
            } else {
                std::ofstream file(std::filesystem::path(dump), std::ios::binary);
                if (!file) {
                    SIM_ERROR("Bench", "cannot write {}", std::string(dump));
                } else {
                    file.write(reinterpret_cast<const char*>(voxels.data()),
                               static_cast<std::streamsize>(voxels.size()));
                    SIM_INFO("Bench", "{} SDF voxels written to {}", voxels.size(),
                             std::string(dump));
                }
            }
        }

        if (const std::string_view shot = FlagValue(argc, argv, "--bench-capture");
            !shot.empty() && !captureFrameSet) {
            writeCapture(*renderer, shot);
        }

        if (const std::string_view dump = FlagValue(argc, argv, "--bench-dump-cull");
            !dump.empty() && !captureFrameSet) {
            writeCullDump(*renderer, dump);
        }

        std::string report = "# Garis dasar G0\n\n";
        report += "- Perangkat: " + device.DeviceName() + "\n";
        report += "- Perender: " + std::string(renderer->Name()) + "\n";
        {
            char line[256];
            std::snprintf(line, sizeof(line), "- Viewport: %ux%u\n", renderWidth, renderHeight);
            report += line;
            std::snprintf(line, sizeof(line),
                          "- Frame: %u pemanasan + %u diukur (%u sampel GPU)\n", warmup,
                          measured, gpuSamples);
            report += line;
            std::snprintf(line, sizeof(line), "- GI: %s\n",
                          app.Context().gi.enabled ? "menyala" : "mati");
            report += line;
            // **Jalur material dan harganya, di baris yang sama.** Kriteria
            // selesai G5 adalah sebuah hitungan, bukan sebuah waktu — dan
            // hitungan yang tidak ikut tercetak di laporan adalah hitungan yang
            // harus dicari ulang setiap kali seseorang bertanya.
            const render::RenderStats stats = renderer->Stats();
            std::snprintf(line, sizeof(line),
                          "- Material: %s — %u ikatan descriptor, %u draw per frame\n",
                          renderer->UsesBindlessMaterials() ? "bindless" : "set per ruas",
                          stats.descriptorSetBinds, stats.drawCalls);
            report += line;
            std::snprintf(line, sizeof(line), "- Jalan selesai dalam %.2f s\n\n", wallSeconds);
            report += line;
        }
        report += FormatTable("Waktu GPU per pass (ms)", gpu);
        report += "\n";
        report += FormatTable("Waktu CPU per tahap (ms)", cpu);

        std::fputs(report.c_str(), stdout);
        if (const std::string_view out = FlagValue(argc, argv, "--bench-out"); !out.empty()) {
            std::ofstream file{std::filesystem::path(out)};
            if (!file) {
                SIM_ERROR("Bench", "cannot write {}", std::string(out));
            } else {
                file << report;
                SIM_INFO("Bench", "report written to {}", std::string(out));
            }
        }

        app.Shutdown();
        return 0;
    }

    ai::ToolRegistry mcpTools;
    ai::ResourceRegistry mcpResources;
    // **Tanpa `ScreenshotFn`.** Tidak ada jendela untuk dipotret, jadi
    // `editor.screenshot` tidak didaftarkan sama sekali — sedangkan
    // `viewport.capture` tetap ada, karena ia membaca target render.
    editor::RegisterEditorTools(mcpTools, mcpResources, app);
    editor::RegisterSceneTools(mcpTools, mcpResources, app);
    editor::RegisterEntityTools(mcpTools, app);
    editor::RegisterAssetTools(mcpTools, app);
    editor::RegisterAuthoringTools(mcpTools, app);

    ai::McpServer server;
    ai::McpServerConfig serverConfig;
    serverConfig.advertisePath = configDir / "mcp.json";
    serverConfig.permissionMode = permission;
    serverConfig.serverName = "simengine-headless";
    if (const std::string_view port = FlagValue(argc, argv, "--mcp-port"); !port.empty()) {
        serverConfig.preferredPort =
            static_cast<uint16_t>(std::atoi(std::string(port).c_str()));
    }
    if (!server.Start(mcpTools, mcpResources, serverConfig)) {
        SIM_ERROR("Headless", "MCP server did not start");
        app.Shutdown();
        return 1;
    }
    SIM_INFO("Headless", "listening on {} ({} tools, permission {})", server.Url(),
             mcpTools.All().size(), ai::ToString(permission));

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    // Loop tetap, bukan menunggu peristiwa. Yang harus maju tiap detak bukan
    // hanya permintaan MCP: pemindaian aset, animasi, skrip, dan fisika juga —
    // dan agen yang memanggil `scene.describe` sesudah mengimpor sesuatu berhak
    // melihat hasil impornya.
    constexpr auto kTickInterval = std::chrono::milliseconds(16);
    auto previous = std::chrono::steady_clock::now();
    while (!gStopping.load() && !app.WantsExit()) {
        const auto now = std::chrono::steady_clock::now();
        const float deltaSeconds =
            std::chrono::duration<float>(now - previous).count();
        previous = now;

        app.Tick(deltaSeconds);
        MainThreadQueue::Get().Drain();

        if (renderer != nullptr && app.Context().world != nullptr) {
            sceneView.Build(*app.Context().world, selection, app.Context().assets,
                            renderer.get(), app.Context().animation,
                            app.Context().builtinAssets, app.Context().whiteboxes);

            render::ViewportDesc desc;
            desc.width = renderWidth;
            desc.height = renderHeight;
            desc.gi = app.Context().gi;
            // Kamera datang dari `viewport.capture`, atau tetap di tempatnya.
            // Tidak ada orbit di sini: tidak ada mouse untuk menggerakkannya.
            editor::EditorContext::ViewportCameraRequest& request =
                app.Context().cameraRequest;
            if (request.pending) {
                const Vec3 forward = request.lookAt - request.from;
                if (glm::length(forward) > 1e-4f) {
                    camera.position = request.from;
                    camera.rotation =
                        glm::quatLookAt(glm::normalize(forward), Vec3(0.0f, 1.0f, 0.0f));
                }
                request.pending = false;
            }
            desc.camera = camera;
            renderer->Render(desc, sceneView.Scene());

            // Diumumkan dengan arti yang sama seperti di editor: "viewport
            // digambar frame lalu, seukuran ini". `viewport.capture` memakainya
            // untuk menolak mengirim target yang belum pernah disentuh.
            app.Context().viewportRect.position = Vec2(0.0f, 0.0f);
            app.Context().viewportRect.size =
                Vec2(static_cast<float>(renderWidth), static_cast<float>(renderHeight));
            app.Context().viewportRect.mainSize = app.Context().viewportRect.size;
        }

        std::this_thread::sleep_for(kTickInterval);
    }

    SIM_INFO("Headless", "stopping");
    server.Stop();
    app.Shutdown();
    return 0;
}
