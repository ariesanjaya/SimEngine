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
#include "Sim/RHI/Device.h"
#include "Sim/RHI/TextureRegistry.h"
#include "Sim/Render/RendererFactory.h"

#if SIM_WITH_LUA
#include "Sim/Script/ScriptRuntime.h"
#endif

#include <glm/gtx/quaternion.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

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
        "  --mcp-port <port>             bawaan 7777\n"
        "  --mcp-permission <mode>       read-only | ask | auto (bawaan auto)\n"
        "  --render <width>x<height>     ukuran viewport offscreen, bawaan 1280x720\n"
        "  --no-render                   tanpa perender; viewport.capture tidak didaftarkan\n"
        "  --headless                    diterima dan diabaikan; program ini selalu headless\n",
        stderr);
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
        if (!device.Create(deviceDesc)) {
            SIM_WARN("Headless", "no Vulkan device; running without a renderer");
            wantRenderer = false;
        } else {
            render::StubRendererDesc rendererDesc;
            rendererDesc.shaderDirectory =
                std::filesystem::path(argv[0]).parent_path() / "Shaders";
            rendererDesc.initialWidth = renderWidth;
            rendererDesc.initialHeight = renderHeight;
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

    editor::Selection selection;
    editor::SceneView sceneView;
    // Kamera milik loop ini, bukan milik panel — tidak ada panel. Ia hanya
    // berpindah kalau agen memintanya lewat `viewport.capture`.
    render::Camera camera;

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
