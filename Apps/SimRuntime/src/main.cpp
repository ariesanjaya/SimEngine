/// SimRuntime: player. Membuka jendela, memuat level, menjalankannya.
///
/// **Tidak ada editor di sini.** Tidak ada undo, tidak ada seleksi, tidak ada
/// panel — yang berjalan adalah dunia, skripnya, dan fisikanya, digambar oleh
/// perender yang sama persis dengan yang dipakai viewport editor. Itu yang
/// membuat apa yang dilihat orang di editor sama dengan apa yang dilihatnya di
/// sini.
///
/// **Utang lapisannya sudah dibayar.** Terjemahan `World` → `ViewportScene`
/// dulu tinggal di `SceneView` yang ada di `EditorFramework`, jadi player ini
/// menautkan ImGui, riwayat undo, dan `PanelManager` yang tidak pernah
/// dipakainya. Sekarang terjemahan itu punya modulnya sendiri, `Sim::SceneView`,
/// dan player dibangun juga di build tanpa editor — yang membuat klaim "player
/// tidak membawa editornya" bisa gagal, bukan sekadar diucapkan.
///
/// Dijalankan dulu, dirapikan kemudian: sebuah terjemahan yang dipindah sebelum
/// ada dua pemakai adalah terjemahan yang dipindah menurut tebakan.

#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Core/Log.h"
#include "Sim/Core/TaskPool.h"
#include "Sim/SceneView/SceneView.h"
#include "Sim/ImageIO/ImageIO.h"
#include "Sim/SceneView/Selection.h"
#include "Sim/Physics/PhysicsScene.h"
#include "Sim/Platform/Window.h"
#include "Sim/RHI/Device.h"
#include "Sim/RHI/Swapchain.h"
#include "Sim/RHI/TextureRegistry.h"
#include "Sim/Render/Presenter.h"
#include "Sim/Render/RendererFactory.h"
#include "Sim/Scene/Components.h"
#include "Sim/Scene/Project.h"
#include "Sim/Scene/Serialization.h"
#include "Sim/Scene/World.h"

#if SIM_WITH_LUA
#include "Sim/Script/ScriptRuntime.h"
#endif

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_vulkan.h>

#include <chrono>
#include <fstream>
#include <vector>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

/// Jembatan tekstur yang tidak menjembatani ke UI mana pun.
///
/// Perender menyerahkan image view-nya ke sini karena di editor yang menerimanya
/// ImGui. Di player yang menyalinnya ke layar adalah `render::Presenter`, yang
/// mengambil view-nya langsung dari perender — jadi handle-nya cukup angka yang
/// naik.
class NullTextureRegistry final : public sim::rhi::ITextureRegistry {
public:
    uint64_t Acquire(VkImageView, VkSampler) override { return ++next_; }
    void Release(uint64_t) override {}

private:
    uint64_t next_ = 0;
};

std::string_view FlagValue(int argc, char** argv, std::string_view name) {
    for (int at = 1; at + 1 < argc; ++at) {
        if (argv[at] != nullptr && std::string_view(argv[at]) == name) {
            return argv[at + 1] == nullptr ? std::string_view{} : std::string_view(argv[at + 1]);
        }
    }
    return {};
}

std::filesystem::path ExecutableDirectory(const char* argv0) {
    std::error_code ec;
    const std::filesystem::path self = std::filesystem::canonical(argv0, ec);
    return ec ? std::filesystem::current_path() : self.parent_path();
}

void PrintUsage() {
    std::fputs(
        "SimRuntime — memainkan sebuah project SimEngine.\n"
        "\n"
        "  --project <path>   wajib\n"
        "  --level <name>     level yang dimainkan; bawaan level pertama project\n"
        "  --width <px>       bawaan 1280\n"
        "  --height <px>      bawaan 720\n"
        "  --frames <n>       jalankan n frame lalu keluar; 0 berarti sampai ditutup\n"
        "  --screenshot <png> tulis frame terakhir ke berkas lalu keluar\n",
        stderr);
}

/// Kamera dari entity ber-CameraComponent pertama, atau pandangan bawaan.
///
/// **Level tanpa kamera tetap tergambar.** Layar hitam yang berarti "tidak ada
/// kamera" tidak bisa dibedakan dari layar hitam yang berarti "levelnya kosong",
/// dan keduanya menuntut perbaikan yang berbeda.
sim::render::Camera CameraFor(const sim::scene::World& world, bool& outFound) {
    using namespace sim;
    render::Camera camera;
    outFound = false;
    const auto view = world.Registry().view<scene::CameraComponent, scene::TransformComponent>();
    for (const auto raw : view) {
        const auto& settings = view.get<scene::CameraComponent>(raw);
        const auto& transform = view.get<scene::TransformComponent>(raw);
        camera.position = transform.position;
        camera.rotation = transform.rotation;
        camera.fovYRadians = settings.fovYRadians;
        camera.nearZ = settings.nearZ;
        camera.farZ = settings.farZ;
        camera.orthographic = settings.orthographic;
        camera.orthoHeight = settings.orthoHeight;
        outFound = true;
        break;
    }
    return camera;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace sim;

    const std::filesystem::path exeDir = ExecutableDirectory(argv[0]);
    Log::Init(exeDir / "Logs" / "runtime.log");
    SIM_INFO("Runtime", "SimRuntime 0.1.0 starting");

    const std::string_view projectFlag = FlagValue(argc, argv, "--project");
    if (projectFlag.empty()) {
        PrintUsage();
        return 2;
    }

    uint32_t width = 1280;
    uint32_t height = 720;
    if (const std::string_view value = FlagValue(argc, argv, "--width"); !value.empty()) {
        width = static_cast<uint32_t>(std::atoi(std::string(value).c_str()));
    }
    if (const std::string_view value = FlagValue(argc, argv, "--height"); !value.empty()) {
        height = static_cast<uint32_t>(std::atoi(std::string(value).c_str()));
    }
    if (width == 0 || height == 0) {
        SIM_ERROR("Runtime", "--width dan --height harus lebih dari nol");
        return 2;
    }

    scene::Project project;
    std::string projectError;
    if (!scene::LoadProject(project, std::filesystem::path(projectFlag), projectError)) {
        SIM_ERROR("Runtime", "cannot open project {}: {}", std::string(projectFlag),
                  projectError);
        return 1;
    }

    std::filesystem::path levelPath;
    if (const std::string_view level = FlagValue(argc, argv, "--level"); !level.empty()) {
        levelPath = project.LevelsDirectory() / (std::string(level) + ".simlevel");
    } else {
        // Level pertama menurut abjad. **Bukan tebakan yang disembunyikan**: ia
        // dicatat ke log, supaya yang menjalankan project berisi banyak level
        // tahu yang mana yang dimainkan tanpa harus menebak.
        std::error_code ec;
        for (const auto& entry :
             std::filesystem::directory_iterator(project.LevelsDirectory(), ec)) {
            if (entry.path().extension() == ".simlevel" &&
                (levelPath.empty() || entry.path() < levelPath)) {
                levelPath = entry.path();
            }
        }
    }
    if (levelPath.empty() || !std::filesystem::exists(levelPath)) {
        SIM_ERROR("Runtime", "no level to play in {}", project.LevelsDirectory().string());
        return 1;
    }
    SIM_INFO("Runtime", "playing {}", levelPath.string());

    if (!platform::InitPlatform()) {
        return 1;
    }
    platform::Window window;
    platform::WindowDesc windowDesc;
    windowDesc.title = project.name.empty() ? "SimEngine" : project.name;
    windowDesc.width = static_cast<int>(width);
    windowDesc.height = static_cast<int>(height);
    if (!window.Create(windowDesc)) {
        platform::ShutdownPlatform();
        return 1;
    }

    rhi::DeviceDesc deviceDesc;
    deviceDesc.applicationName = "SimRuntime";
    // Di sebelah executable, mengikuti letak log runtime: sebuah build yang
    // dikirim ke pemain tidak punya folder konfigurasi editor, dan yang
    // dituliskannya harus tetap berada di dalam yang dikirim.
    deviceDesc.pipelineCachePath = exeDir / "Cache" / "pipeline.bin";
    {
        uint32_t count = 0;
        const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
        deviceDesc.instanceExtensions.assign(names, names + count);
    }
    rhi::Device device;
    if (!device.Create(deviceDesc)) {
        return 1;
    }
    VkSurfaceKHR surface = device.CreateSurface(window.Handle());
    if (surface == VK_NULL_HANDLE) {
        return 1;
    }
    const UVec2 pixelSize = window.PixelSize();
    rhi::Swapchain swapchain;
    if (!swapchain.Create(device, surface, pixelSize.x, pixelSize.y, /*vsync=*/true)) {
        return 1;
    }

    NullTextureRegistry textures;
    render::StubRendererDesc rendererDesc;
    rendererDesc.shaderDirectory = exeDir / "Shaders";
    rendererDesc.initialWidth = pixelSize.x;
    rendererDesc.initialHeight = pixelSize.y;
    std::unique_ptr<render::IViewportRenderer> renderer =
        render::CreateVulkanRenderer(device, textures, rendererDesc);
    if (renderer == nullptr) {
        renderer = render::CreateStubRenderer(device, textures, rendererDesc);
    }
    if (renderer == nullptr) {
        SIM_ERROR("Runtime", "no renderer");
        return 1;
    }

    render::Presenter presenter;
    if (!presenter.Create(device, swapchain, rendererDesc.shaderDirectory)) {
        SIM_ERROR("Runtime", "present pass did not build");
        return 1;
    }

    TaskPool tasks;
    // Panggangan lingkungan tidak boleh menahan frame pemain.
    if (renderer != nullptr) {
        renderer->SetTaskPool(&tasks);
        // **Di sebelah binernya, bukan di folder setelan pengguna.** Player
        // yang dikirim membawa asetnya sendiri, dan cache di folder pengguna
        // berarti jalan pertama membayar unwrap untuk setiap mesh — di layar
        // pemuatan, bukan di editor.
        renderer->SetLightmapCacheDir(exeDir / "LightmapUvCache");
    }
    assets::AssetDatabase assetDatabase;
    assets::AssetDatabase::Config assetConfig;
    assetConfig.root = project.AssetsDirectory();
    assetConfig.tasks = &tasks;
    assetDatabase.Initialize(assetConfig);
    assetDatabase.ScanNow();

    scene::World world;
    if (const scene::LevelIoResult loaded = scene::LoadLevelFromFile(world, levelPath);
        !loaded.ok) {
        SIM_ERROR("Runtime", "cannot load {}: {}", levelPath.string(), loaded.error);
        return 1;
    }
    SIM_INFO("Runtime", "loaded {} entities", world.Count());

#if SIM_WITH_LUA
    script::ScriptRuntime scripts;
    scripts.Initialize(world, &assetDatabase);
    // **Start, bukan menunggu tombol Play.** Di editor Play adalah sebuah mode;
    // di sini ia satu-satunya mode.
    scripts.Start();
#endif

    physics::PhysicsScene physics;
    if (!physics.Build(world)) {
        // Bukan galat fatal: level tanpa rigid body sama sekali adalah level yang
        // sah, dan yang menghentikannya di sini adalah player yang menolak
        // memainkan setengah project.
        SIM_WARN("Runtime", "physics did not start: {}", physics.Error());
    }

    view::Selection selection;
    view::SceneView sceneView;

    // **Player yang bisa memotret dirinya sendiri.** Itu yang membuat regresi
    // visual bisa dijalankan di CI tanpa seorang pun menatap layar — dan yang
    // membuat "apakah ia benar-benar menggambar sesuatu" bisa dijawab tanpa
    // menangkap seluruh desktop orang yang sedang bekerja.
    const std::string_view screenshotPath = FlagValue(argc, argv, "--screenshot");
    int frameLimit = 0;
    if (const std::string_view value = FlagValue(argc, argv, "--frames"); !value.empty()) {
        frameLimit = std::atoi(std::string(value).c_str());
    }
    if (!screenshotPath.empty() && frameLimit <= 0) {
        // Beberapa frame supaya aset yang dimuat di latar sempat sampai. Satu
        // frame memotret adegan yang mesh-nya belum diunggah, dan yang keluar
        // adalah kubus satuan di tempat modelnya seharusnya.
        frameLimit = 60;
    }

    bool running = true;
    int framesDrawn = 0;
    auto previous = std::chrono::steady_clock::now();
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            // Menutup jendela adalah perintah di sini, bukan permintaan: tidak
            // ada perubahan yang belum disimpan untuk ditanyakan.
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                 event.window.windowID == window.Id())) {
                running = false;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        const float deltaSeconds = std::chrono::duration<float>(now - previous).count();
        previous = now;

        assetDatabase.Update(deltaSeconds);
#if SIM_WITH_LUA
        scripts.Update(deltaSeconds);
#endif
        physics.Advance(world, deltaSeconds);

        const UVec2 size = window.PixelSize();
        if (window.IsMinimized() || size.x == 0 || size.y == 0) {
            continue;  // terminimalkan; tidak ada yang perlu digambar
        }
        if (size.x != swapchain.Width() || size.y != swapchain.Height()) {
            device.WaitIdle();
            swapchain.Resize(size.x, size.y);
        }

        rhi::Swapchain::Frame frame;
        if (!swapchain.BeginFrame(frame)) {
            device.WaitIdle();
            swapchain.Resize(size.x, size.y);
            continue;
        }

        sceneView.Build(world, selection, &assetDatabase, renderer.get());

        render::ViewportDesc desc;
        desc.width = size.x;
        desc.height = size.y;
        // **Grid mati.** Ia alat bantu penyuntingan — sesuatu yang digambar
        // untuk orang yang sedang menempatkan benda — dan sebuah player yang
        // menampilkannya sedang menampilkan UI editor kepada pemain.
        desc.showGrid = false;
        // **Adegan disinari sebagaimana levelnya menyatakannya.** Player bukan
        // pengecualian: sebuah level yang dirancang dengan GI real-time harus
        // memakai tingkat itu di tangan pemain, tanpa satu klik pun — dan
        // langitnya sama, karena tingkat panggang tanpa langit adalah tingkat
        // yang tidak disinari apa pun.
        view::ApplySceneSky(world, desc);
        view::ApplyWorldSettings(world, desc);
        bool hasCamera = false;
        desc.camera = CameraFor(world, hasCamera);
        renderer->Render(desc, sceneView.Scene());

        // Warna clear tidak pernah terlihat: pass present menutupi seluruh
        // layar. Ia hitam supaya frame yang gagal dipresent terlihat sebagai
        // hitam pekat alih-alih sebagai sampah dari frame sebelumnya.
        swapchain.BeginRenderPass(frame, {0.0f, 0.0f, 0.0f, 1.0f});
        presenter.Draw(*renderer, swapchain);
        swapchain.EndRenderPass(frame);
        if (!swapchain.EndFrame(frame)) {
            device.WaitIdle();
            swapchain.Resize(size.x, size.y);
            continue;
        }

        ++framesDrawn;
        if (frameLimit > 0 && framesDrawn >= frameLimit) {
            running = false;
        }
    }

    if (!screenshotPath.empty()) {
        std::vector<uint8_t> rgba;
        uint32_t shotWidth = 0;
        uint32_t shotHeight = 0;
        std::string error;
        if (!swapchain.CaptureLastPresented(rgba, shotWidth, shotHeight, error)) {
            SIM_ERROR("Runtime", "screenshot failed: {}", error);
        } else {
            imageio::Image image;
            image.desc.width = shotWidth;
            image.desc.height = shotHeight;
            image.desc.channels = 4;
            image.desc.type = imageio::PixelType::UInt8;
            image.bytes = std::move(rgba);
            std::vector<uint8_t> png;
            const imageio::ImageIoResult encoded = imageio::Encode(image, ".png", png);
            if (!encoded.ok) {
                SIM_ERROR("Runtime", "screenshot encode failed: {}", encoded.error);
            } else {
                std::ofstream file(std::filesystem::path(screenshotPath),
                                   std::ios::binary | std::ios::trunc);
                file.write(reinterpret_cast<const char*>(png.data()),
                           static_cast<std::streamsize>(png.size()));
                SIM_INFO("Runtime", "screenshot {} ({}x{})", std::string(screenshotPath),
                         shotWidth, shotHeight);
            }
        }
    }

    device.WaitIdle();
#if SIM_WITH_LUA
    scripts.Stop();
    scripts.Shutdown();
#endif
    physics.Clear();
    presenter.Destroy();
    renderer.reset();
    swapchain.Destroy();
    device.DestroySurface(surface);
    device.Destroy();
    window.Destroy();
    platform::ShutdownPlatform();
    SIM_INFO("Runtime", "stopped");
    return 0;
}
