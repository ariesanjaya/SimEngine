#include "Sim/ImGuiIntegration/ImGuiLayer.h"

#include "Sim/Core/Assert.h"
#include "Sim/ImGuiIntegration/Theme.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_stdinc.h>

#include <cmath>

namespace sim::imguix {
namespace {

void CheckVkResult(VkResult result) {
    if (result != VK_SUCCESS) {
        SIM_ERROR("ImGui", "Backend Vulkan mengembalikan {}", rhi::ResultToString(result));
    }
}

/// Descriptor untuk font atlas ditambah tekstur yang didaftarkan
/// TextureBridge (viewport, thumbnail, preview). Backend membuat pool-nya
/// sendiri dari angka ini, jadi RHI tidak perlu menyediakan pool khusus UI.
constexpr uint32_t kDescriptorPoolSize = 1024;

}  // namespace

bool ImGuiLayer::Initialize(rhi::Device& device, const ImGuiLayerDesc& desc) {
    SIM_VERIFY(desc.window != nullptr, "ImGuiLayer requires a window");
    SIM_VERIFY(desc.renderPass != VK_NULL_HANDLE, "ImGuiLayer requires a render pass");
    device_ = &device;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ConfigureIo(desc);
    ApplyDarkTheme();

    if (!ImGui_ImplSDL3_InitForVulkan(desc.window)) {
        SIM_CRITICAL("ImGui", "ImGui_ImplSDL3_InitForVulkan failed");
        return false;
    }

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion = device.ApiVersion();
    initInfo.Instance = device.Instance();
    initInfo.PhysicalDevice = device.PhysicalDevice();
    initInfo.Device = device.Handle();
    initInfo.QueueFamily = device.GraphicsQueueFamily();
    initInfo.Queue = device.GraphicsQueue();
    initInfo.DescriptorPool = VK_NULL_HANDLE;
    initInfo.DescriptorPoolSize = kDescriptorPoolSize;
    initInfo.MinImageCount = desc.minImageCount;
    initInfo.ImageCount = desc.imageCount;
    initInfo.PipelineCache = device.PipelineCache();
    initInfo.PipelineInfoMain.RenderPass = desc.renderPass;
    initInfo.PipelineInfoMain.Subpass = 0;
    initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.CheckVkResultFn = CheckVkResult;

    if (!ImGui_ImplVulkan_Init(&initInfo)) {
        SIM_CRITICAL("ImGui", "ImGui_ImplVulkan_Init failed");
        return false;
    }

    LoadFonts(desc);
    textures_.Initialize(device);
    initialized_ = true;

    const ImGuiIO& io = ImGui::GetIO();
    SIM_INFO("ImGui", "Dear ImGui {} — docking {}, multi-viewport {}", IMGUI_VERSION,
             (io.ConfigFlags & ImGuiConfigFlags_DockingEnable) != 0 ? "on" : "off",
             (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0 ? "on" : "off");
    return true;
}

void ImGuiLayer::ConfigureIo(const ImGuiLayerDesc& desc) {
    ImGuiIO& io = ImGui::GetIO();

    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Multi-viewport dimatikan untuk sementara — lihat ImGuiLayerDesc.
    // Environment override ada supaya perbaikannya bisa diuji tanpa membangun
    // ulang; ia hanya bisa MENYALAKAN, tidak mematikan, sehingga tidak ada cara
    // tidak sengaja mematikannya di mesin yang sudah benar.
    const char* forceViewports = SDL_getenv("SIM_ENABLE_VIEWPORTS");
    const bool viewports =
        desc.enableViewports || (forceViewports != nullptr && forceViewports[0] == '1');
    if (viewports) {
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    }
    SIM_INFO("ImGui", "Multi-viewport {}", viewports ? "on" : "off (panels stay in the main window)");

    // Panel hanya menempel ke dockspace eksplisit. Tanpa ini, menyeret panel ke
    // tengah viewport 3D akan menempelkannya ke jendela mana pun yang kebetulan
    // ada di bawah kursor, yang membuat tata letak mudah kacau tanpa sengaja.
    io.ConfigDockingWithShift = false;
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    // Field angka (Translate/Rotate/Scale dan seterusnya) bisa diketik dengan
    // sekali klik-lepas, bukan hanya Ctrl+Klik. Menyeret tetap bekerja seperti
    // biasa karena ImGui membedakan keduanya dari ada-tidaknya gerakan mouse.
    //
    // Di editor 3D ini penting: menyetel posisi lewat drag itu cepat tapi kasar,
    // dan angka pasti (0, 90, 1.5) tetap sering dibutuhkan. Memaksa Ctrl+Klik
    // untuk itu membuat cara mengetik nilai jadi tersembunyi.
    io.ConfigDragClickToInputText = true;
    // Enter mengaktifkan kembali field dan menyorot isinya, sehingga mengisi
    // X lalu Tab lalu Y berjalan tanpa mengangkat tangan dari keyboard.
    io.ConfigInputTextEnterKeepActive = true;

    // Inti dukungan multi-monitor: saat jendela berpindah ke monitor dengan DPI
    // berbeda, ImGui membangun ulang atlas font dan menskalakan jendela
    // platform. Tanpa keduanya, panel yang diseret ke monitor HiDPI akan
    // tampil buram atau kekecilan.
    io.ConfigDpiScaleFonts = true;
    io.ConfigDpiScaleViewports = true;

    if (!desc.iniPath.empty()) {
        iniPathStorage_ = desc.iniPath.string();
        io.IniFilename = iniPathStorage_.c_str();
    } else {
        io.IniFilename = nullptr;
    }
}

void ImGuiLayer::LoadFonts(const ImGuiLayerDesc& desc) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::GetStyle().FontSizeBase = desc.fontSize;

    const bool wantIcons =
        !desc.iconFontPath.empty() && std::filesystem::exists(desc.iconFontPath);
    if (!desc.iconFontPath.empty() && !wantIcons) {
        SIM_WARN("ImGui", "Icon font not found: {}", desc.iconFontPath.string());
    }

    // Rentang harus tetap hidup selama atlas hidup (lihat komentar pada
    // ImFontConfig::GlyphRanges), jadi disimpan statis, bukan di stack.
    static ImWchar iconRange[3] = {};
    iconRange[0] = static_cast<ImWchar>(desc.iconRangeMin);
    iconRange[1] = static_cast<ImWchar>(desc.iconRangeMax);
    iconRange[2] = 0;

    ImFontConfig textConfig;
    if (wantIcons) {
        // Inter memetakan 745 glyph ke Private Use Area — wilayah yang sama
        // dengan tempat font ikon berada. Saat merge, sumber pertama yang punya
        // sebuah codepoint yang menang, jadi tanpa pengecualian ini sebagian
        // ikon diam-diam tergantikan glyph Latin milik Inter: chevron-down
        // muncul sebagai "3", save sebagai "9", undo sebagai "ï". Gejalanya
        // menyesatkan karena sebagian ikon lain tetap benar.
        textConfig.GlyphExcludeRanges = iconRange;
    }

    bool textFontLoaded = false;
    if (!desc.fontPath.empty() && std::filesystem::exists(desc.fontPath)) {
        if (io.Fonts->AddFontFromFileTTF(desc.fontPath.string().c_str(), desc.fontSize,
                                         &textConfig) != nullptr) {
            SIM_INFO("ImGui", "Text font: {}", desc.fontPath.string());
            textFontLoaded = true;
        } else {
            SIM_WARN("ImGui", "Failed to load font {}, using built-in", desc.fontPath.string());
        }
    }
    if (!textFontLoaded) {
        io.Fonts->AddFontDefault();
    }

    if (!wantIcons) {
        return;
    }

    ImFontConfig config;
    // MergeMode: glyph ikon masuk ke ImFont yang sama dengan teks, sehingga
    // ikon bisa ditulis di tengah string biasa tanpa PushFont/PopFont.
    config.MergeMode = true;
    // Lebar seragam sebesar tinggi font: tanpa ini setiap ikon punya advance
    // sendiri dan kolom ikon di Outliner maupun Console jadi bergerigi.
    config.GlyphMinAdvanceX = desc.fontSize;
    config.GlyphMaxAdvanceX = desc.fontSize;
    // Ikon Lucide duduk sedikit lebih tinggi dari baseline teks; turunkan
    // supaya sejajar dengan huruf di sebelahnya.
    config.GlyphOffset = ImVec2(0.0f, 2.0f);
    config.PixelSnapH = true;

    if (io.Fonts->AddFontFromFileTTF(desc.iconFontPath.string().c_str(), desc.fontSize, &config,
                                     iconRange) != nullptr) {
        SIM_INFO("ImGui", "Icon font: {} (U+{:04X}..U+{:04X})", desc.iconFontPath.string(),
                 desc.iconRangeMin, desc.iconRangeMax);
    } else {
        SIM_WARN("ImGui", "Failed to load icon font {}", desc.iconFontPath.string());
    }
}

bool ImGuiLayer::ProcessEvent(const SDL_Event& event) {
    return ImGui_ImplSDL3_ProcessEvent(&event);
}

void ImGuiLayer::BeginFrame() {
    // Sebelum apa pun menggambar: bebaskan tekstur yang sudah lewat masa
    // tunggunya. Lihat catatan di TextureBridge.h.
    textures_.BeginFrame();

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::EndFrame() {
    ImGui::Render();
}

void ImGuiLayer::RenderDrawData(VkCommandBuffer commandBuffer) {
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
}

void ImGuiLayer::RenderPlatformWindows() {
    const ImGuiIO& io = ImGui::GetIO();
    if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) == 0) {
        return;
    }
    ImGui::UpdatePlatformWindows();
    ImGui::RenderPlatformWindowsDefault();
}

void ImGuiLayer::SetMinImageCount(uint32_t minImageCount) {
    if (initialized_) {
        ImGui_ImplVulkan_SetMinImageCount(minImageCount);
    }
}

void ImGuiLayer::UpdateDisplayScale(float scale) {
    if (scale <= 0.0f) {
        return;
    }
    // Ambang 1% supaya jitter pelaporan skala dari compositor tidak memicu
    // pembangunan ulang gaya setiap frame.
    if (std::abs(scale - currentScale_) < 0.01f) {
        return;
    }
    currentScale_ = scale;
    ApplyScale(scale);
    SIM_DEBUG_LOG("ImGui", "UI scale is now {:.2f}", scale);
}

void ImGuiLayer::Shutdown() {
    if (!initialized_) {
        return;
    }
    if (device_ != nullptr) {
        device_->WaitIdle();
    }
    textures_.Shutdown();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    initialized_ = false;
    device_ = nullptr;
}

}  // namespace sim::imguix
