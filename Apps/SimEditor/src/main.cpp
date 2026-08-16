// SimEditor — composition root.
//
// Satu-satunya berkas yang boleh melihat seluruh modul sekaligus. Semua
// penyambungan antar-modul terjadi di sini; modul itu sendiri tidak pernah
// saling mencari lewat singleton.

#include "Sim/AIBridge/McpServer.h"
#include "Sim/AIBridge/ResourceRegistry.h"
#include "Sim/AIBridge/ToolRegistry.h"
#include "Sim/Core/FrameLimiter.h"
#include "Sim/Core/Log.h"
#include "Sim/Core/MainThreadQueue.h"
#include "Sim/Editor/AiTools.h"
#include "Sim/Editor/EditorApp.h"
#include "Sim/Editor/Icons.h"
#include "Sim/ImGuiIntegration/ImGuiLayer.h"
#include "Sim/ImageIO/ImageIO.h"
#include "Sim/Physics/PhysicsWorld.h"
#include "Sim/Platform/Display.h"
#include "Sim/Platform/FileDialog.h"
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

#include <algorithm>
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

/// Tempat project baru dibuat secara bawaan.
///
/// **Di Documents, bukan di folder konfigurasi.** Yang di folder konfigurasi
/// adalah milik editor; project adalah pekerjaan orang, dan pekerjaan orang
/// tinggal di tempat yang ia sendiri bisa temukan, backup, dan taruh di kontrol
/// versi tanpa harus tahu editor menyimpan apa pun di mana.
std::filesystem::path ProjectsRoot() {
    const char* home = SDL_getenv("HOME");
    std::filesystem::path base =
        home != nullptr ? std::filesystem::path(home) : std::filesystem::current_path();
    return base / "Documents" / "SimEngine";
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

int main(int argc, char** argv) {
    using namespace sim;

    MainThreadQueue::Get().BindMainThread();

    const std::filesystem::path configDir = ConfigDirectory();
    Log::Init(configDir / "Logs" / "editor.log");
    SIM_INFO("Editor", "SimEditor 0.1.0 starting");
    // Backend gambar dicatat beserta versinya. Berkas yang terbaca di satu
    // mesin dan ditolak di mesin lain hampir selalu berarti backendnya berbeda,
    // dan tanpa baris ini satu-satunya cara mengetahuinya adalah membangun ulang.
    SIM_INFO("Editor", "ImageIO backend: {}", imageio::BackendSummary());
    // Fisika dicatat dengan cara yang sama, dan ketiadaannya sama pentingnya
    // untuk dicatat: benda yang diam padahal seharusnya jatuh terbaca sebagai
    // bug simulasi, bukan sebagai build tanpa PhysX. Baris ini yang membedakan
    // keduanya tanpa perlu membangun ulang.
    if (physics::Available()) {
        SIM_INFO("Editor", "Physics: PhysX {} (CPU{})", physics::BackendVersion(),
                 physics::GpuAvailable() ? ", GPU tersedia" : "");
    } else {
        SIM_WARN("Editor", "Physics: PhysX tidak ada di build ini — simulasi mati");
    }

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

    // Pratinjau mesh: instance KEDUA `IViewportRenderer`, dengan target
    // rendernya sendiri. Tiap `VulkanRenderer` memiliki `RenderTarget`-nya
    // sendiri, jadi keduanya tidak bertabrakan — dan Mesh Editor karena itu
    // memakai jalur gambar yang sama persis dengan viewport utama, lengkap
    // dengan kulit, warna per ruas, dan pass garis untuk rangkanya.
    std::unique_ptr<render::IViewportRenderer> meshPreview =
        render::CreateVulkanRenderer(device, imguiLayer.Textures(), rendererDesc);
    if (meshPreview == nullptr) {
        SIM_WARN("Editor", "Mesh preview unavailable; the Mesh Editor will say so");
    }

    // Preview material: instance kedua dengan target rendernya sendiri.
    // Null bukan kegagalan fatal — menyunting graph material tidak menuntut
    // preview, dan panel menampilkan alasannya alih-alih menolak dibuka.
    std::unique_ptr<render::IMaterialPreview> materialPreview =
        render::CreateMaterialPreview(device, imguiLayer.Textures());
    if (materialPreview == nullptr) {
        SIM_WARN("Editor", "Material preview unavailable; the Material Editor will say so");
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
    appConfig.projectsRoot = ProjectsRoot();
    appConfig.resourceDir = ExecutableDirectory() / "Resources";
    appConfig.shaderDir = rendererDesc.shaderDirectory;
    appConfig.viewportRenderer = renderer.get();
    appConfig.materialPreview = materialPreview.get();
    appConfig.meshPreview = meshPreview.get();
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

    // Server MCP (track AI, A0). Dinyalakan sesudah editor siap dan dimatikan
    // sebelum apa pun dibongkar: sebuah permintaan yang masih ditangani saat
    // `World` dihancurkan adalah crash yang muncul di thread jaringan, jauh dari
    // baris mana pun yang menutup editor.
    //
    // Registry hidup di sini, bukan di dalam `EditorApp`, karena pemiliknya
    // adalah yang menyusun aplikasi — dan `SimHeadless` nanti menyusun himpunan
    // tool yang berbeda dari editor yang sama.
    ai::ToolRegistry mcpTools;
    ai::ResourceRegistry mcpResources;
    // Tangkapan layar dirakit di sini karena hanya di sini kedua sisinya
    // terlihat: swapchain milik RHI, penyandi PNG milik ImageIO, dan
    // EditorFramework tidak boleh melihat satu pun dari keduanya.
    editor::ScreenshotFn captureWindow;
    if (swapchain.CanCapture()) {
        captureWindow = [&swapchain](const editor::CaptureRect* crop, std::vector<uint8_t>& png,
                                     std::string& error) {
            std::vector<uint8_t> rgba;
            uint32_t width = 0;
            uint32_t height = 0;
            if (!swapchain.CaptureLastPresented(rgba, width, height, error)) {
                return false;
            }

            // Rect dijepit ke dalam gambar. Yang memintanya bekerja dalam satuan
            // logis ImGui dan menerjemahkannya sendiri ke piksel; selisih satu
            // piksel di tepi adalah pembulatan, bukan alasan menolak.
            uint32_t originX = 0;
            uint32_t originY = 0;
            uint32_t cropWidth = width;
            uint32_t cropHeight = height;
            if (crop != nullptr && crop->width > 0 && crop->height > 0) {
                originX = std::min(crop->x, width);
                originY = std::min(crop->y, height);
                cropWidth = std::min(crop->width, width - originX);
                cropHeight = std::min(crop->height, height - originY);
            }
            if (cropWidth == 0 || cropHeight == 0) {
                error = "the requested crop is outside the window";
                return false;
            }
            // **Alfa dibuang, bukan dibawa.** Jendela editor tidak punya alfa
            // yang berarti — swapchain-nya opaque — dan pembaca gambar di sini
            // meng-associate alfa saat membaca, jadi PNG berkanal empat yang
            // dibaca kembali warnanya bisa berbeda dari yang dikirim. Tiga kanal
            // menghilangkan seluruh pertanyaan itu, dan berkasnya seperempat
            // lebih kecil.
            std::vector<uint8_t> rgb(static_cast<std::size_t>(cropWidth) * cropHeight * 3u);
            for (uint32_t row = 0; row < cropHeight; ++row) {
                const std::size_t source =
                    (static_cast<std::size_t>(originY + row) * width + originX) * 4u;
                const std::size_t target = static_cast<std::size_t>(row) * cropWidth * 3u;
                for (uint32_t column = 0; column < cropWidth; ++column) {
                    rgb[target + column * 3u + 0u] = rgba[source + column * 4u + 0u];
                    rgb[target + column * 3u + 1u] = rgba[source + column * 4u + 1u];
                    rgb[target + column * 3u + 2u] = rgba[source + column * 4u + 2u];
                }
            }
            rgba = std::vector<uint8_t>();

            imageio::Image image;
            image.desc.width = cropWidth;
            image.desc.height = cropHeight;
            image.desc.channels = 3;
            image.desc.type = imageio::PixelType::UInt8;
            // Swapchain-nya UNORM dan ImGui menggambar dengan warna yang sudah
            // dalam ruang sRGB — jadi byte-nya memang sRGB, dan menyebutnya
            // linear akan membuat siapa pun yang membacanya mencerahkannya lagi.
            image.desc.colorSpace = imageio::ColorSpace::Srgb;
            image.bytes = std::move(rgb);
            const imageio::ImageIoResult result = imageio::Encode(image, ".png", png);
            if (!result) {
                error = result.error;
                return false;
            }
            return true;
        };
    }
    editor::RegisterEditorTools(mcpTools, mcpResources, app, std::move(captureWindow));
    editor::RegisterSceneTools(mcpTools, mcpResources, app);
    editor::RegisterEntityTools(mcpTools, app);
    editor::RegisterAssetTools(mcpTools, app);

    ai::McpServer mcpServer;
    ai::McpServerConfig mcpConfig;
    mcpConfig.advertisePath = configDir / "mcp.json";
    // Panel AI Bridge menampilkan keadaannya dan bisa mematikan-menyalakannya.
    // Closure-nya dipegang context, bukan panel: panel bisa ditutup, dan yang
    // ditutup tidak boleh membawa serta kemampuan menyalakan servernya lagi.
    app.Context().mcpServer = &mcpServer;
    app.Context().mcpStart = [&mcpServer, &mcpTools, &mcpResources, &mcpConfig]() {
        return mcpServer.Start(mcpTools, mcpResources, mcpConfig);
    };

    if (!mcpServer.Start(mcpTools, mcpResources, mcpConfig)) {
        // Editor tetap jalan tanpa server. Yang hilang adalah kendali agen,
        // bukan kemampuan menyunting — dan editor yang menolak dibuka karena
        // sebuah port sibuk akan sangat mengganggu.
        SIM_WARN("Editor", "MCP server tidak menyala — editor jalan tanpa kendali agen");
    }

    // Project dari baris perintah, seperti yang dijanjikan docs/PLAN-EDITOR.md.
    // **Bukan pengganti project manager melainkan jalan pintas ke dalamnya:**
    // yang gagal dibuka tetap mendarat di manager beserta pesannya, bukan pada
    // editor kosong yang tidak menjelaskan apa-apa.
    if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0') {
        app.OpenProject(std::filesystem::path(argv[1]));
    }

    // Runtime Lua tidak lagi dipasang di sini. Ia memegang pointer ke indeks
    // aset, dan indeks itu baru punya akar sesudah sebuah project dibuka — jadi
    // yang memasangnya adalah `EditorApp::OpenProject`, setiap kali project
    // berganti. Cache graph tetap di luar folder Assets: `.lua` hasil kompilasi
    // bukan aset yang dikarang pengguna, dan menaruhnya di Assets akan membuatnya
    // muncul di Asset Browser, ikut mendapat GUID, dan ikut masuk kontrol versi
    // sebagai berkas turunan.

    bool running = true;
    std::string windowTitle;

    // Dialog berkas sistem dibuat modal terhadap jendela utama. Dialog yang
    // tidak punya induk bisa muncul di belakang editor, dan yang terlihat adalah
    // editor yang membeku menunggu jendela yang tak seorang pun tahu ada.
    platform::SetDialogParentWindow(&window);

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
    // Sebelum apa pun yang lain. `Stop()` menunggu thread jaringannya selesai,
    // jadi sesudah baris ini dijamin tidak ada handler yang masih memegang
    // `World` — dan itulah kriteria terima A0 nomor 4.
    mcpServer.Stop();
    // Induk dialog dilepas sebelum jendelanya dihancurkan: sebuah dialog yang
    // masih terbuka saat editor ditutup akan menunjuk jendela yang sudah tidak
    // ada.
    platform::SetDialogParentWindow(nullptr);
    app.Shutdown();
    device.WaitIdle();

    renderer.reset();
    meshPreview.reset();
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
