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
#include "Sim/Assets/MeshSdfBakery.h"
#include "Sim/Core/Log.h"
#include "Sim/Core/MainThreadQueue.h"
#include "Sim/Core/TaskPool.h"
#include "Sim/Core/UserPaths.h"
#include "Sim/Editor/AiTools.h"
#include "Sim/Editor/EditorApp.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/SceneView/SceneView.h"
#include "Sim/SceneView/Selection.h"
#include "Sim/ImageIO/ImageIO.h"
#include "Sim/RHI/Device.h"
#include "Sim/RHI/TextureRegistry.h"
#include "Sim/Render/RendererFactory.h"

#if SIM_WITH_LUA
#include "Sim/Script/ScriptRuntime.h"
#endif

#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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
        "  --bench-cull-first <n>        gambar hanya permukaan bernomor >= n\n"
        "  --bench-async                 pass compute di antrean terpisah (G7)\n"
        "  --bench-settle-seconds <n>    batas menunggu material siap, bawaan 300\n"
        "  --bench-camera px,py,pz,tx,ty,tz  kamera tetap, bukan lintasan orbit\n"
        "  --bench-dump-depth <path>     depth buffer mentah (w, h, float per texel)\n"
        "  --bench-cull-limit <n>        gambar hanya permukaan bernomor < n\n"
        "  --bench-fixed-exposure        eksposur manual; wajib untuk membandingkan gambar\n"
        "  --bench-gi-debug <view>       off|albedo|normal|irradiance|raycount|steps|layers\n"
        "  --bench-draw-mode <mode>      material|unlit|clay|material+wireframe|wireframe\n"
        "  --bench-ev <ev100>            eksposur manual pada EV100 ini\n"
        "  --bench-no-screen-trace       matikan lapis screen-space; lihat SDF sendirian\n"
        "  --bench-furnace               uji tungku: langit seragam 1, albedo 1, tanpa matahari\n"
        "  --dump-mesh <file> --dump-out <json>  material dan ruas sebuah berkas mesh\n"
        "  --dump-uv <bin>               posisi+UV tiap titik, float32 x5\n"
        "  --dump-tri <bin>              posisi+UV tiap sudut segitiga, float32 x15\n"
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

    const std::filesystem::path configDir = sim::ConfigDirectory();
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
    // **Worker berhenti sebelum apa pun yang mengantre pekerjaan ke sana
    // dihancurkan.** `app` memiliki bakery tekstur, database aset, dan penjaga
    // shader; ketiganya mengirim tugas yang menangkap `this`. Dideklarasikan di
    // sini, sesudah `app`, jadi ia dihancurkan lebih dulu — pada jalur `return`
    // manapun, termasuk yang gagal di tengah.
    const TaskPool::StopGuard stopWorkers(tasks);

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

    // **Membaca isi sebuah berkas mesh, lalu keluar.** Bukan bagian dari jalur
    // ukur: ia alat untuk yang mengarang aset, dan yang dijawabnya pertanyaan
    // yang selama ini hanya bisa dijawab dengan menulis uji sementara — material
    // apa saja yang dibawa berkas ini, tekstur mana yang disebut masing-masing,
    // dan ruas ke berapa memakai yang mana. Larik `materials` di level diindeks
    // nomor ruas, jadi tanpa jawaban terakhir itu sebuah level hanya bisa
    // dikarang dengan menebak.
    if (const std::string_view file = FlagValue(argc, argv, "--dump-mesh"); !file.empty()) {
        std::string error;
        const assets::MeshData mesh = assets::LoadMesh(std::filesystem::path(file), error);
        if (!mesh.IsValid()) {
            SIM_ERROR("Headless", "cannot read {}: {}", std::string(file), error);
            app.Shutdown();
            return 1;
        }
        std::ostringstream json;
        json << "{\n  \"triangles\": " << mesh.TriangleCount() << ",\n";
        json << "  \"vertices\": " << mesh.vertices.size() << ",\n";
        json << "  \"boundsMin\": [" << mesh.boundsMin.x << ", " << mesh.boundsMin.y << ", "
             << mesh.boundsMin.z << "],\n";
        json << "  \"boundsMax\": [" << mesh.boundsMax.x << ", " << mesh.boundsMax.y << ", "
             << mesh.boundsMax.z << "],\n";
        json << "  \"materials\": [\n";
        for (std::size_t i = 0; i < mesh.materials.size(); ++i) {
            const assets::MeshMaterial& material = mesh.materials[i];
            json << "    {\"name\": \"" << material.name << "\", \"baseColor\": \""
                 << material.baseColorTexture << "\", \"normal\": \"" << material.normalTexture
                 << "\", \"roughness\": \"" << material.roughnessTexture
                 << "\", \"metalness\": \"" << material.metalnessTexture << "\"}"
                 << (i + 1 < mesh.materials.size() ? "," : "") << "\n";
        }
        json << "  ],\n  \"parts\": [";
        for (std::size_t i = 0; i < mesh.parts.size(); ++i) {
            json << (i == 0 ? "" : ", ") << mesh.parts[i].material;
        }
        json << "],\n";
        // **Statistik UV, karena yang ditanyakan "sama dengan aslinya atau
        // tidak" tidak bisa dijawab satu titik.** Konvensi V yang berlawanan
        // tidak muncul sebagai galat maupun sebagai UV di luar jangkauan; yang
        // muncul hanya `v` yang tercermin, dan itu hanya terlihat kalau dua
        // impor atas model yang sama dibandingkan berdampingan.
        Vec2 uvMin(std::numeric_limits<float>::max());
        Vec2 uvMax(std::numeric_limits<float>::lowest());
        double uSum = 0.0;
        double vSum = 0.0;
        std::size_t outside = 0;
        for (const assets::MeshVertex& vertex : mesh.vertices) {
            uvMin = glm::min(uvMin, vertex.uv);
            uvMax = glm::max(uvMax, vertex.uv);
            uSum += static_cast<double>(vertex.uv.x);
            vSum += static_cast<double>(vertex.uv.y);
            if (vertex.uv.x < 0.0f || vertex.uv.x > 1.0f || vertex.uv.y < 0.0f ||
                vertex.uv.y > 1.0f) {
                ++outside;
            }
        }
        const auto count = static_cast<double>(std::max<std::size_t>(mesh.vertices.size(), 1));
        json << "  \"uv\": {\"min\": [" << uvMin.x << ", " << uvMin.y << "], \"max\": ["
             << uvMax.x << ", " << uvMax.y << "], \"mean\": [" << uSum / count << ", "
             << vSum / count << "], \"outside01\": " << outside << "}\n}\n";

        // Posisi dan UV setiap titik, mentah. JSON tidak dipakai di sini: satu
        // Sponza adalah ratusan ribu titik, dan yang membacanya mencocokkan
        // titik demi posisi — pekerjaan numerik, bukan pekerjaan teks.
        if (const std::string_view uvOut = FlagValue(argc, argv, "--dump-uv"); !uvOut.empty()) {
            std::ofstream raw{std::filesystem::path(uvOut), std::ios::binary};
            for (const assets::MeshVertex& vertex : mesh.vertices) {
                const float row[5] = {vertex.position.x, vertex.position.y, vertex.position.z,
                                      vertex.uv.x, vertex.uv.y};
                raw.write(reinterpret_cast<const char*>(row), sizeof(row));
            }
            SIM_INFO("Headless", "{} titik posisi+UV ditulis ke {}", mesh.vertices.size(),
                     std::string(uvOut));
        }

        // Sudut demi sudut, dalam urutan segitiga.
        //
        // **Bukan sekadar lebih rinci daripada `--dump-uv`; ia menjawab
        // pertanyaan yang tidak bisa dijawab per titik.** Di jahitan UV satu
        // posisi membawa dua nilai atau lebih, jadi dua impor bisa sepakat soal
        // *himpunan* UV di sebuah posisi dan tetap berbeda soal sudut mana
        // mendapat yang mana — dan yang terlihat dari kekeliruan itu adalah
        // tekstur yang robek tepat di jahitannya.
        if (const std::string_view triOut = FlagValue(argc, argv, "--dump-tri"); !triOut.empty()) {
            std::ofstream raw{std::filesystem::path(triOut), std::ios::binary};
            for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                float row[15];
                for (int c = 0; c < 3; ++c) {
                    const assets::MeshVertex& v = mesh.vertices[mesh.indices[i + c]];
                    row[c * 5 + 0] = v.position.x;
                    row[c * 5 + 1] = v.position.y;
                    row[c * 5 + 2] = v.position.z;
                    row[c * 5 + 3] = v.uv.x;
                    row[c * 5 + 4] = v.uv.y;
                }
                raw.write(reinterpret_cast<const char*>(row), sizeof(row));
            }
            SIM_INFO("Headless", "{} segitiga sudut+UV ditulis ke {}", mesh.TriangleCount(),
                     std::string(triOut));
        }
        // **Ke berkas, bukan ke stdout.** Log proses ini juga menulis ke
        // stdout, jadi keluaran yang dicampur dengannya bukan JSON yang sah —
        // dan yang membacanya adalah skrip, bukan mata.
        const std::string_view out = FlagValue(argc, argv, "--dump-out");
        if (out.empty()) {
            SIM_ERROR("Headless", "--dump-mesh needs --dump-out <path>");
            app.Shutdown();
            return 2;
        }
        std::ofstream dump{std::filesystem::path(out)};
        if (!dump) {
            SIM_ERROR("Headless", "cannot write {}", std::string(out));
            app.Shutdown();
            return 1;
        }
        dump << json.str();
        SIM_INFO("Headless", "{} materials, {} parts written to {}", mesh.materials.size(),
                 mesh.parts.size(), std::string(out));
        app.Shutdown();
        return 0;
    }

    editor::Selection selection;
    editor::SceneView sceneView;
    // **Tanpa ini setiap material bertekstur digambar tanpa teksturnya, dan
    // tanpa satu pun pesan.** `SceneView::UploadedTexture` menjawab
    // "tekstur tidak ada" begitu bakery-nya null, materialnya tetap dikompilasi
    // — dengan tekstur kosong — dan hasilnya permukaan putih rata yang tidak
    // bisa dibedakan dari material yang memang tidak bertekstur. Sampai adegan
    // Sponza masuk, tidak ada adegan uji headless yang punya tekstur sama
    // sekali, jadi tidak ada yang menagihnya.
    sceneView.SetTextureBakery(app.Context().textureBakery);
    // Dan medan jarak mesh, dengan alasan yang sama: tanpanya clipmap GI
    // menyusun Sponza sebagai satu kotak pejal, dan yang terukur adalah
    // adegan yang setiap sinar probenya mengenai dinding di jarak nol.
    sceneView.SetMeshSdfBakery(app.Context().meshSdfBakery);
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

    // Depth buffer mentah, float per texel. Yang membacanya skrip di luar
    // program; menulisnya sebagai gambar akan membuang persis ketelitian yang
    // dicari.
    const auto writeDepthDump = [](render::IViewportRenderer& target, std::string_view path) {
        std::vector<float> depth;
        uint32_t width = 0;
        uint32_t height = 0;
        std::string dumpError;
        if (!target.CaptureDepth(depth, width, height, dumpError)) {
            SIM_ERROR("Bench", "cannot read the depth buffer: {}", dumpError);
            return;
        }
        std::ofstream file{std::filesystem::path(path), std::ios::binary};
        if (!file) {
            SIM_ERROR("Bench", "cannot write {}", std::string(path));
            return;
        }
        const uint32_t header[2]{width, height};
        file.write(reinterpret_cast<const char*>(header), sizeof(header));
        file.write(reinterpret_cast<const char*>(depth.data()),
                   static_cast<std::streamsize>(depth.size() * sizeof(float)));
        SIM_INFO("Bench", "depth buffer written to {} ({}x{})", std::string(path), width, height);
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
        bool asyncCompute = false;
        // Kamera tetap, menggantikan lintasan orbit.
        //
        // **Lintasan orbit menjawab pertanyaan "berapa biayanya dari segala
        // arah"; ia tidak menjawab "apakah cahayanya benar".** Yang kedua
        // menuntut satu sudut pandang yang sama persis tiap jalan, dan pada
        // adegan berbentuk bangunan ia harus berada di **dalam** — orbit selalu
        // memotret dindingnya dari luar.
        bool fixedCamera = false;
        Vec3 cameraEye{0.0f};
        Vec3 cameraTarget{0.0f};
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
        float manualEv = 0.0f;
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
            } else if (flag == "--bench-async") {
                asyncCompute = true;
            } else if (flag == "--bench-occlusion") {
                occlusion = true;
            } else if (flag == "--bench-fixed-exposure") {
                fixedExposure = true;
            } else if (flag == "--bench-furnace") {
                // Uji tungku, kriteria selesai M4.
                //
                // **Lapis layar tetap menyala.** Mematikannya juga mematikan
                // pass piramida HiZ, dan tampilan iradiansi membaca kedalaman
                // dari piramida itu untuk tahu piksel mana yang punya
                // permukaan — jadi seluruh layar terbaca langit dan yang
                // terukur bukan apa-apa. Yang membuat lapis layar tidak
                // mengotori tungkunya adalah shader: di bawah tungku setiap
                // jawaban yang *diketahui* bernilai satu, karena permukaan
                // ber-albedo satu di bawah cahaya seragam satu memang
                // berradiansi satu. Yang tersisa diuji hanyalah penutup
                // rekursinya.
                app.Context().gi.furnaceTest = true;
            } else if (flag == "--bench-no-screen-trace") {
                // **Lapis layar dimatikan supaya lapis SDF bisa dilihat
                // sendirian.** Kriteria selesai M1 menuntut penelusuran sphere
                // dari kamera cocok dengan depth buffer raster, dan selama
                // lapis layar menjawab lebih dulu yang terlihat selalu lapis
                // layar — termasuk ketika medan jaraknya kosong sama sekali.
                app.Context().gi.screenTrace = false;
            }
        }
        // EV100 manual. **Yang dibelinya bukan gambar yang lebih enak melainkan
        // bagian gelap yang bisa diukur:** sebuah tangkapan 8-bit menjepit
        // seluruh bayangan ke nol, dan dua adegan yang berbeda seratus kali di
        // sana tetap terlihat sama hitamnya. Menurunkan EV mengangkat bayangan
        // itu ke rentang yang masih punya angka.
        if (const std::string_view value = FlagValue(argc, argv, "--bench-ev"); !value.empty()) {
            manualEv = std::strtof(std::string(value).c_str(), nullptr);
            fixedExposure = true;
        }

        // **GI dipaksa dari baris perintah, bukan hanya dari level.** Anggaran
        // GI adalah 3,0 ms dari 16,6 ms — sepertiga dari seluruh pekerjaan yang
        // G0 ingin ukur — jadi garis dasar yang diam-diam mengukur adegan tanpa
        // GI adalah garis dasar yang menjawab pertanyaan lain. Levelnya sendiri
        // tetap yang menentukan bila benderanya tidak ada.
        if (const std::string_view value = FlagValue(argc, argv, "--bench-camera");
            !value.empty()) {
            std::array<float, 6> numbers{};
            std::size_t at = 0;
            std::size_t start = 0;
            const std::string text(value);
            while (at < numbers.size() && start <= text.size()) {
                const std::size_t comma = text.find(',', start);
                numbers[at++] =
                    std::strtof(text.substr(start, comma - start).c_str(), nullptr);
                if (comma == std::string::npos) {
                    break;
                }
                start = comma + 1;
            }
            if (at != numbers.size()) {
                SIM_ERROR("Bench", "--bench-camera wants px,py,pz,tx,ty,tz");
                app.Shutdown();
                return 2;
            }
            fixedCamera = true;
            cameraEye = Vec3(numbers[0], numbers[1], numbers[2]);
            cameraTarget = Vec3(numbers[3], numbers[4], numbers[5]);
        }
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

        // Tampilan diagnostik GI dari baris perintah. **Yang menjawab
        // pertanyaan "kenapa gelap" bukan gambar akhirnya melainkan lapis mana
        // yang menjawab tiap sinar**, dan tanpa bendera ini satu-satunya cara
        // melihatnya adalah menjalankan editor dan menekan tombolnya.
        if (const std::string_view value = FlagValue(argc, argv, "--bench-gi-debug");
            !value.empty()) {
            static constexpr std::array<std::pair<std::string_view, render::GiDebugView>, 7> kViews{
                {{"off", render::GiDebugView::Off},
                 {"albedo", render::GiDebugView::Albedo},
                 {"normal", render::GiDebugView::Normal},
                 {"irradiance", render::GiDebugView::Irradiance},
                 {"raycount", render::GiDebugView::RayCount},
                 {"steps", render::GiDebugView::MarchSteps},
                 {"layers", render::GiDebugView::TraceLayers}}};
            bool found = false;
            for (const auto& [name, view] : kViews) {
                if (value == name) {
                    app.Context().gi.debugView = view;
                    found = true;
                    break;
                }
            }
            if (!found) {
                SIM_ERROR("Bench", "--bench-gi-debug tidak mengenal \"{}\"", std::string(value));
                app.Shutdown();
                return 2;
            }
        }

        // Saringan tampilan viewport dari baris perintah, dengan alasan yang
        // sama seperti `--bench-gi-debug` di atas: tanpa bendera ini
        // satu-satunya cara melihat rangka kawat adalah menjalankan editor dan
        // menekan tombolnya, dan itu berarti tidak ada satu pun gambar rangka
        // kawat yang bisa dibandingkan dengan gambar rangka kawat kemarin.
        render::DrawMode drawMode = render::DrawMode::Material;
        if (const std::string_view value = FlagValue(argc, argv, "--bench-draw-mode");
            !value.empty()) {
            static constexpr std::array<std::pair<std::string_view, render::DrawMode>, 5> kModes{
                {{"material", render::DrawMode::Material},
                 {"unlit", render::DrawMode::Unlit},
                 {"clay", render::DrawMode::Clay},
                 {"material+wireframe", render::DrawMode::MaterialWireframe},
                 {"wireframe", render::DrawMode::Wireframe}}};
            bool found = false;
            for (const auto& [name, mode] : kModes) {
                if (value == name) {
                    drawMode = mode;
                    found = true;
                    break;
                }
            }
            if (!found) {
                SIM_ERROR("Bench", "--bench-draw-mode tidak mengenal \"{}\"", std::string(value));
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
        SampleTable frameGpu;
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
            // **Batasnya waktu, bukan jumlah frame.** Yang ditunggu di sini
            // bukan frame melainkan kompilasi shader dan pemanggangan tekstur,
            // dan keduanya berjalan di thread lain dengan kecepatan yang tidak
            // ada hubungannya dengan berapa kali frame disusun. Batas 3.000
            // frame dulu berarti enam detik — cukup untuk adegan uji yang
            // materialnya delapan, dan jauh dari cukup untuk adegan yang
            // teksturnya tujuh puluh dua lembar 4K: yang terukur lalu adegan
            // yang setengah materialnya masih jalur mundur.
            // Bisa dipendekkan: sebuah jalan yang sengaja dimulai sebelum
            // materialnya siap adalah satu-satunya cara menguji apa yang terjadi
            // pada pekerjaan yang masih berjalan saat program ditutup.
            auto settleBudget = std::chrono::seconds(300);
            // **Batas yang disebut sendiri berarti tunggu selama itu, bukan
            // paling lama selama itu.** Syarat berhenti di bawah — tidak ada
            // kompilasi yang tertunda — juga terpenuhi *sebelum* mesh-nya
            // selesai dimuat, karena yang menerbitkan permintaan material
            // adalah bagian-bagian mesh yang belum ada. Sebuah jalan dengan
            // `--bench-settle-seconds 8` lalu berhenti di frame pertama dan
            // mengukur adegan kosong. Ketika batasnya diminta secara eksplisit,
            // yang diminta adalah keadaan setengah siap itu sendiri, jadi
            // syarat berhenti lebih awal tidak berlaku.
            bool settleFixed = false;
            if (const std::string_view value = FlagValue(argc, argv, "--bench-settle-seconds");
                !value.empty()) {
                settleBudget =
                    std::chrono::seconds(std::strtol(std::string(value).c_str(), nullptr, 10));
                settleFixed = true;
            }
            const auto settleStart = std::chrono::steady_clock::now();
            uint32_t settle = 0;
            for (; !gStopping.load(); ++settle) {
                if (std::chrono::steady_clock::now() - settleStart > settleBudget) {
                    break;
                }
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
                // Medan jarak mesh ikut ditunggu, dan bukan demi kerapian:
                // bake Sponza memakan tujuh detik, dan frame yang diukur
                // sebelum ia siap adalah frame yang clipmap GI-nya masih
                // menyusun gedung itu sebagai kotak pejal.
                const std::size_t sdfPending = app.Context().meshSdfBakery != nullptr
                                                   ? app.Context().meshSdfBakery->PendingCount()
                                                   : 0;
                if (!settleFixed && settle > 0 && sdfPending == 0 &&
                    app.Context().materialPrograms->PendingCount() == 0) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (app.Context().meshSdfBakery != nullptr &&
                app.Context().meshSdfBakery->PendingCount() > 0) {
                SIM_WARN("Bench", "{} mesh distance fields are still baking after {} frames",
                         app.Context().meshSdfBakery->PendingCount(), settle);
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
            const Vec3 orbit(centre.x + radius * std::cos(angle),
                             centre.y + extent.y * 0.55f,
                             centre.z + radius * std::sin(angle));
            const Vec3 eye = fixedCamera ? cameraEye : orbit;
            const Vec3 forward = (fixedCamera ? cameraTarget : centre) - eye;

            render::ViewportDesc desc;
            desc.width = renderWidth;
            desc.height = renderHeight;
            desc.mode = drawMode;
            desc.gi = app.Context().gi;
            desc.computeGradient = computeGradient;
            desc.gpuClusters = !cpuClusters;
            desc.gpuSdf = !cpuSdf;
            desc.gpuCull = !cpuCull;
            desc.gpuOcclusion = occlusion;
            desc.asyncCompute = asyncCompute;
            desc.cullDebug = !FlagValue(argc, argv, "--bench-dump-cull").empty();
            if (const std::string_view value = FlagValue(argc, argv, "--bench-cull-first");
                !value.empty()) {
                desc.cullFirst =
                    static_cast<uint32_t>(std::strtoul(std::string(value).c_str(), nullptr, 10));
            }
            if (const std::string_view value = FlagValue(argc, argv, "--bench-cull-limit");
                !value.empty()) {
                desc.cullLimit =
                    static_cast<uint32_t>(std::strtoul(std::string(value).c_str(), nullptr, 10));
            }
            if (fixedExposure) {
                desc.post.exposureMode = render::ExposureMode::Manual;
                desc.post.manualEv100 = manualEv;
            }
            // Delta yang sama dengan yang diberikan ke `app.Tick`, dan karena
            // alasan yang sama: yang maju menurut waktu — eksposur, awan,
            // akumulasi temporal — harus maju sama jauhnya di tiap jalan, kalau
            // tidak dua gambar dari binary yang identik pun berbeda.
            // **Langit datang dari adegan, sama seperti di viewport editor.**
            // Tanpa baris ini setiap pengukuran headless menggambar langit
            // bawaan berapa pun angka yang tertulis di level — dan dua level
            // yang langitnya berbeda seratus kali menghasilkan gambar yang sama
            // persis. Ditemukan begitu.
            editor::ApplySceneSky(*app.Context().world, desc);
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
                if (const std::string_view dump = FlagValue(argc, argv, "--bench-dump-depth");
                    !dump.empty()) {
                    writeDepthDump(*renderer, dump);
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
                // **Frame diukur, bukan dijumlahkan.** Sejak G7 sebuah pass
                // boleh berjalan di antrean lain, dan dua pass yang berjalan
                // bersamaan dihitung dua kali oleh jumlah baris di bawah tabel.
                frameGpu.Add("frame", renderer->FrameGpuMilliseconds());
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
        report += FormatTable("Waktu GPU frame (ms)", frameGpu);
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
