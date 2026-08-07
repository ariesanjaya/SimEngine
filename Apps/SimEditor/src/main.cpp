// SimEditor — composition root.
//
// Satu-satunya berkas yang boleh melihat seluruh modul sekaligus. Semua
// penyambungan antar-modul terjadi di sini; modul itu sendiri tidak pernah
// saling mencari lewat singleton.

#include "Sim/Core/FrameLimiter.h"
#include "Sim/Core/Log.h"
#include "Sim/Core/MainThreadQueue.h"
#include "Sim/Editor/EditorApp.h"
#include "Sim/Editor/Icons.h"
#include "Sim/ImGuiIntegration/ImGuiLayer.h"
#include "Sim/Platform/Display.h"
#include "Sim/Platform/Window.h"
#include "Sim/RHI/Device.h"
#include "Sim/RHI/Swapchain.h"
#include "Sim/Core/TaskPool.h"
#include "Sim/Render/RendererFactory.h"
#include "Sim/Render/ThumbnailCache.h"

#if SIM_WITH_LUA
#include "Sim/Script/ScriptRuntime.h"
#endif

#include <imgui.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <filesystem>
#include <string>
#include <vector>

namespace {

/// Folder tempat berkas pendamping executable berada (shader, resource).
std::filesystem::path ExecutableDirectory() {
    const char* base = SDL_GetBasePath();
    return base != nullptr ? std::filesystem::path(base) : std::filesystem::current_path();
}

/// Font UI editor.
///
/// Inter dibundel di Resources/Fonts, bukan mengandalkan font sistem. Dua
/// alasan: font bawaan Dear ImGui hanya memuat ASCII sehingga karakter seperti
/// em-dash tampil sebagai kotak, dan font sistem berbeda-beda antar-distribusi
/// sehingga tata letak panel bisa bergeser di mesin lain. Inter dipilih karena
/// dirancang untuk teks UI berukuran kecil: tinggi-x besar, celah huruf lebar,
/// dan angka bertabular — yang terakhir penting supaya kolom X/Y/Z di Inspector
/// tidak bergoyang saat nilainya berubah.
///
/// Font sistem tetap dipakai sebagai cadangan bila Resources tidak ikut
/// tersalin.
std::filesystem::path FindUiFont() {
    const std::filesystem::path bundled =
        ExecutableDirectory() / "Resources" / "Fonts" / "Inter-Regular.ttf";
    if (std::filesystem::exists(bundled)) {
        return bundled;
    }

    static const std::filesystem::path kFallbacks[] = {
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
    };
    for (const std::filesystem::path& candidate : kFallbacks) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

/// Folder konfigurasi per-pengguna: layout dock, preferensi, log.
std::filesystem::path ConfigDirectory() {
    const char* home = SDL_getenv("HOME");
    std::filesystem::path base =
        home != nullptr ? std::filesystem::path(home) : std::filesystem::current_path();
    return base / ".simengine";
}

/// Menghitung laju frame yang dipakai mengunci editor.
///
/// Diambil dari monitor dengan refresh rate terendah, bukan monitor utama.
/// Dengan multi-viewport, panel bisa berada di monitor mana pun dan ikut
/// digambar pada frame yang sama; menyamakan ke monitor terlambat membuat
/// semua panel bergerak konsisten alih-alih setengahnya patah-patah.
struct FrameLock {
    float hz = 60.0f;
    std::string reason;
};

FrameLock ComputeFrameLock() {
    const std::vector<sim::platform::DisplayInfo> displays = sim::platform::EnumerateDisplays();

    FrameLock lock;
    lock.hz = sim::platform::LowestRefreshRate(60.0f);

    const sim::platform::DisplayInfo* slowest = nullptr;
    for (const sim::platform::DisplayInfo& display : displays) {
        if (display.refreshRate > 1.0f &&
            (slowest == nullptr || display.refreshRate < slowest->refreshRate)) {
            slowest = &display;
        }
    }

    if (slowest != nullptr) {
        lock.reason = slowest->name + " @ " + std::to_string(static_cast<int>(lock.hz)) + " Hz";
        if (displays.size() > 1) {
            lock.reason += " (lowest of " + std::to_string(displays.size()) + " monitors)";
        }
    } else {
        lock.reason = "no refresh rate reported, using 60 Hz";
    }
    return lock;
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
    using namespace sim;

    MainThreadQueue::Get().BindMainThread();

    const std::filesystem::path configDir = ConfigDirectory();
    Log::Init(configDir / "Logs" / "editor.log");
    SIM_INFO("Editor", "SimEditor 0.1.0 starting");

    if (!platform::InitPlatform()) {
        return 1;
    }

    platform::Window window;
    platform::WindowDesc windowDesc;
    windowDesc.title = "SimEngine Editor";
    windowDesc.width = 1600;
    windowDesc.height = 900;
    windowDesc.maximized = true;
    if (!window.Create(windowDesc)) {
        platform::ShutdownPlatform();
        return 1;
    }

    rhi::DeviceDesc deviceDesc;
    deviceDesc.applicationName = "SimEditor";
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

    imguix::ImGuiLayerDesc layerDesc;
    layerDesc.window = window.Handle();
    layerDesc.renderPass = swapchain.RenderPass();
    layerDesc.minImageCount = swapchain.MinImageCount();
    layerDesc.imageCount = swapchain.ImageCount();
    layerDesc.iniPath = configDir / "layout.ini";
    layerDesc.fontPath = FindUiFont();
    // 13 px adalah ukuran kerja Inter untuk UI padat: masih terbaca nyaman,
    // tapi memberi ruang jauh lebih banyak untuk daftar entity dan baris log
    // dibanding 16 px. Skala DPI ditambahkan di atas nilai ini oleh ImGui.
    layerDesc.fontSize = 13.0f;
    layerDesc.iconFontPath = ExecutableDirectory() / "Resources" / "Fonts" / "lucide.ttf";
    layerDesc.iconRangeMin = ICON_MIN_LC;
    layerDesc.iconRangeMax = ICON_MAX_LC;
    // Ditulis eksplisit meski sudah menjadi bawaan: mematikan panel yang bisa
    // keluar jadi jendela OS adalah keputusan yang harus terlihat di tempat
    // seluruh modul disambungkan, bukan hanya di nilai bawaan sebuah struct.
    layerDesc.enableViewports = false;

    std::error_code ec;
    std::filesystem::create_directories(configDir, ec);

    imguix::ImGuiLayer imguiLayer;
    if (!imguiLayer.Initialize(device, layerDesc)) {
        return 1;
    }

    render::StubRendererDesc rendererDesc;
    rendererDesc.shaderDirectory = ExecutableDirectory() / "Shaders";
    // Renderer sungguhan lebih dulu; stub adalah jalur mundurnya, bukan
    // sebaliknya. `CreateVulkanRenderer` mengembalikan nullptr kalau perangkatnya
    // tidak memenuhi syarat — dan editor yang menolak jalan di mesin lama tidak
    // bisa dipakai menyunting data, padahal seluruh E2..E7 memang tidak menuntut
    // renderer sungguhan.
    std::unique_ptr<render::IViewportRenderer> renderer =
        render::CreateVulkanRenderer(device, imguiLayer.Textures(), rendererDesc);
    if (renderer == nullptr) {
        renderer = render::CreateStubRenderer(device, imguiLayer.Textures(), rendererDesc);
    }
    if (renderer == nullptr) {
        SIM_CRITICAL("Editor", "Failed to create viewport renderer");
        return 1;
    }

    // Kolam dan cache dideklarasikan di sini, sebelum EditorApp, supaya
    // keduanya dihancurkan belakangan: editor menjadwalkan pekerjaan ke kolam
    // dan meminta thumbnail dari cache sepanjang hidupnya.
    TaskPool tasks;
    std::unique_ptr<render::IThumbnailCache> thumbnails = render::CreateThumbnailCache(
        device, imguiLayer.Textures(), tasks, configDir / "ThumbnailCache");

#if SIM_WITH_LUA
    script::ScriptRuntime scripts;
#endif

    FrameLock frameLock = ComputeFrameLock();
    FrameLimiter frameLimiter(static_cast<double>(frameLock.hz));
    SIM_INFO("Editor", "Frame rate locked to {:.0f} Hz — {}", frameLock.hz, frameLock.reason);

    editor::EditorApp app;
    editor::EditorApp::Config appConfig;
    appConfig.configDir = configDir;
    appConfig.resourceDir = ExecutableDirectory() / "Resources";
    appConfig.viewportRenderer = renderer.get();
    appConfig.frameLimiter = &frameLimiter;
    appConfig.lockedFps = frameLock.hz;
    appConfig.frameLockReason = frameLock.reason;
    appConfig.tasks = &tasks;
    appConfig.thumbnails = thumbnails.get();
#if SIM_WITH_LUA
    appConfig.scripts = &scripts;
#endif
    if (!app.Initialize(appConfig)) {
        return 1;
    }

#if SIM_WITH_LUA
    // Setelah app.Initialize(): runtime butuh World dan AssetDatabase yang baru
    // dibuat di dalamnya.
    //
    // Cache graph berada di luar folder Assets, dan itu disengaja: `.lua` hasil
    // kompilasi bukan aset yang dikarang pengguna, dan menaruhnya di Assets akan
    // membuatnya muncul di Asset Browser, ikut mendapat GUID, dan ikut masuk
    // kontrol versi sebagai berkas turunan.
    scripts.Initialize(app.GetWorld(), app.Context().assets, configDir / "GraphCache");
#endif

    bool running = true;
    std::string windowTitle;

    window.CenterOnPrimaryDisplay();
    window.Show();

    bool swapchainDirty = false;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            imguiLayer.ProcessEvent(event);
            switch (event.type) {
                // Menutup jendela adalah permintaan, bukan perintah: kalau ada
                // perubahan yang belum disimpan, editor bertanya lebih dulu.
                // Jalur keluar hanya satu, lewat EditorApp.
                case SDL_EVENT_QUIT:
                    app.RequestExit();
                    break;
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    if (event.window.windowID == window.Id()) {
                        app.RequestExit();
                    }
                    break;
                // Hanya jendela utama. Panel yang ditarik keluar dockspace punya
                // jendela — dan swapchain — sendiri yang diurus backend ImGui.
                // Tanpa saringan ini, mengubah ukuran panel mengambang membangun
                // ulang swapchain jendela utama pada ukuran yang sama persis:
                // terukur 11 dan 27 ms hilang untuk pekerjaan yang hasilnya
                // identik dengan yang sudah ada.
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                    if (event.window.windowID == window.Id()) {
                        swapchainDirty = true;
                    }
                    break;
                // Konfigurasi monitor bisa berubah saat editor berjalan; kunci
                // laju dihitung ulang supaya tetap mengikuti monitor terlambat
                // yang sekarang terpasang.
                case SDL_EVENT_DISPLAY_ADDED:
                case SDL_EVENT_DISPLAY_REMOVED:
                case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED: {
                    frameLock = ComputeFrameLock();
                    frameLimiter.SetTargetFps(static_cast<double>(frameLock.hz));
                    app.SetFrameLock(frameLock.hz, frameLock.reason);
                    SIM_INFO("Editor", "Monitor configuration changed; frame lock is now {:.0f} Hz — {}",
                             frameLock.hz, frameLock.reason);
                    break;
                }
                default:
                    break;
            }
        }

        // Titik tetap eksekusi pekerjaan lintas-thread (seam #5). Nanti request
        // MCP dari track AI juga dijalankan di sini.
        MainThreadQueue::Get().Drain();

        if (window.IsMinimized()) {
            SDL_Delay(10);
            continue;
        }

        // Membangun ulang swapchain terukur 25–50 ms di mesin ini, hampir
        // seluruhnya di dalam vkCreateSwapchainKHR — lebih lama dari satu frame,
        // dan di luar jangkauan kita. Yang bisa kita kendalikan hanya seberapa
        // sering ia dipanggil, jadi permintaan yang tidak mengubah apa pun
        // dibuang di sini.
        //
        // Ukuran yang sudah cocok terjadi setelah present menjawab SUBOPTIMAL,
        // dan sebelumnya juga setiap kali panel mengambang berubah ukuran.
        //
        // Yang sengaja TIDAK dilakukan: membatasi laju bangun-ulang selama
        // seretan berlangsung. Terlihat masuk akal, tapi diukur justru lebih
        // buruk — driver di sini menjawab OUT_OF_DATE, bukan SUBOPTIMAL, untuk
        // swapchain yang ukurannya meleset, sehingga frame di antaranya tidak
        // tergambar sama sekali: 21 dari 27 frame per 500 ms hilang, dibanding 2
        // dari 26 tanpa pembatasan. Isi jendela membeku selama seretan.
        const UVec2 size = window.PixelSize();
        if (swapchainDirty && size.x > 0 && size.y > 0) {
            if (size.x != swapchain.Width() || size.y != swapchain.Height()) {
                swapchain.Resize(size.x, size.y);
                imguiLayer.SetMinImageCount(swapchain.MinImageCount());
            }
            swapchainDirty = false;
        }

        imguiLayer.UpdateDisplayScale(window.DisplayScale());
        imguiLayer.BeginFrame();

        app.DrawFrame(static_cast<float>(frameLimiter.LastDeltaSeconds()));
        if (app.WantsExit()) {
            running = false;
        }

        // Judul jendela ikut menandai perubahan yang belum disimpan. Diperbarui
        // hanya saat berubah: SDL_SetWindowTitle memicu lalu-lintas ke window
        // manager, dan melakukannya tiap frame terlihat di profiler X11.
        if (std::string title = app.WindowTitle(); title != windowTitle) {
            windowTitle = std::move(title);
            SDL_SetWindowTitle(window.Handle(), windowTitle.c_str());
        }

        // Menyusun draw list selalu dilakukan, bahkan kalau frame jendela utama
        // batal. Keduanya di bawah ini sengaja tidak berada di dalam cabang
        // sukses swapchain: melewatkan UpdatePlatformWindows() satu frame saja
        // membuat ImGui melihat viewport yang belum punya jendela platform dan
        // langsung membuangnya. Gejalanya jauh dari penyebabnya — panel yang
        // dipulihkan di monitor kedua tertarik kembali ke jendela utama dengan
        // ukuran menciut, karena frame-frame pertama setelah jendela ditampilkan
        // memang hampir selalu gagal acquire.
        imguiLayer.EndFrame();

        rhi::Swapchain::Frame frame;
        if (swapchain.BeginFrame(frame)) {
            swapchain.BeginRenderPass(frame, {0.10f, 0.11f, 0.12f, 1.0f});
            imguiLayer.RenderDrawData(frame.commandBuffer);
            swapchain.EndRenderPass(frame);
            if (!swapchain.EndFrame(frame)) {
                swapchainDirty = true;
            }
        } else {
            swapchainDirty = true;
        }

        // Jendela multi-viewport punya swapchain sendiri masing-masing.
        imguiLayer.RenderPlatformWindows();

        frameLimiter.EndFrame();
    }

    SIM_INFO("Editor", "SimEditor stopping");
    app.Shutdown();
    device.WaitIdle();

    renderer.reset();
#if SIM_WITH_LUA
#endif
    imguiLayer.Shutdown();
    swapchain.Destroy();
    device.DestroySurface(surface);
    device.Destroy();
    window.Destroy();
    platform::ShutdownPlatform();
    Log::Shutdown();
    return 0;
}
