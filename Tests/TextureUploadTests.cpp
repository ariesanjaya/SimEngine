#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Assets/TextureBake.h"
#include "Sim/Assets/TextureSettings.h"
#include "Sim/Core/Log.h"
#include "Sim/RHI/Device.h"
#include "Sim/RHI/Ktx2.h"
#include "Sim/RHI/Texture.h"

#include "TestProcess.h"

#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

using namespace sim;

/// Unggahan tekstur ke GPU sungguhan.
///
/// **Satu-satunya uji di repo ini yang menuntut Vulkan**, dan itu disengaja:
/// yang diperiksa di sini persis hal yang tidak bisa diperiksa tanpa driver —
/// bahwa mengunggah rantai mip ber-BC7 tidak menghasilkan satu pun galat
/// validation layer. Barrier yang lupa menyebut seluruh level, `imageExtent`
/// yang dihitung dari level nol, `bufferRowLength` yang diisi tangan untuk
/// format blok: ketiganya lolos setiap uji CPU dan ditangkap validation layer
/// dalam sekali jalan.
///
/// **Mesin tanpa Vulkan tidak menggagalkannya.** Uji ini melaporkan bahwa ia
/// dilewati dan berhenti — menggagalkan build di mesin yang memang tidak punya
/// GPU akan membuat seluruh suite tidak bisa dipercaya di sana, dan yang
/// dikorbankan lebih besar daripada yang dijaga.
namespace {

/// Folder sementara yang membersihkan dirinya sendiri.
class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("simupload_" + std::to_string(counter.fetch_add(1)) + "_" +
                 std::to_string(sim::tests::ProcessId()));
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

/// Galat yang dilaporkan validation layer sejak `Clear()` terakhir.
std::vector<std::string> ValidationErrors() {
    std::vector<std::string> errors;
    for (const LogEntry& entry : LogRing::Get().Snapshot()) {
        if (entry.level >= spdlog::level::err && entry.message.find("[validation]") != std::string::npos) {
            errors.push_back(entry.message);
        }
    }
    return errors;
}

}  // namespace

TEST_CASE("T3: rantai mip ber-BC7 diunggah tanpa satu pun galat validation layer") {
    TempDir temp;
    // Sink LogRing dipasang `Log::Init`; tanpanya pesan validation layer tidak
    // pernah sampai ke tempat yang bisa diperiksa uji ini.
    Log::Init(temp.Path() / "upload.log");

    rhi::DeviceDesc desc;
    desc.applicationName = "SimTextureUploadTests";
    // Tanpa ekstensi surface: uji ini tidak punya jendela, dan tidak butuh satu
    // pun. Yang diperiksa berhenti di `VkImage`.
    desc.enableValidation = true;

    rhi::Device device;
    if (!device.Create(desc)) {
        MESSAGE("Vulkan tidak tersedia di mesin ini — unggahan tekstur tidak diperiksa");
        Log::Shutdown();
        return;
    }
    // Pembuatan device-nya sendiri juga harus bersih. Sebelum T3 ia tidak:
    // `VK_KHR_swapchain` diminta tanpa `VK_KHR_surface` di sisi instance, dan
    // galat yang dihasilkannya menenggelamkan galat sungguhan di antara derau.
    CHECK(ValidationErrors().empty());

    if (!device.SupportsBlockCompression()) {
        MESSAGE("perangkat tanpa dukungan kompresi blok — BC7 tidak diperiksa");
        device.Destroy();
        Log::Shutdown();
        return;
    }

    assets::TextureSettings settings;
    settings.usage = assets::TextureUsage::Color;
    settings.compress = true;
    const assets::BakeResult baked = assets::BakeTexture(
        std::filesystem::path(SIM_IMAGE_DIR) / "checker.png", settings, temp.Path() / "cache");
    REQUIRE_MESSAGE(baked.ok, baked.error);

    rhi::Ktx2Texture image;
    const rhi::Ktx2Result read = rhi::ReadKtx2(baked.path, image);
    REQUIRE_MESSAGE(read.ok, read.error);
    REQUIRE(image.levels.size() == 4);

    LogRing::Get().Clear();

    rhi::Texture2D texture;
    REQUIRE(texture.CreateFromKtx2(device, image));
    CHECK(texture.Width() == 8);
    CHECK(texture.Height() == 8);
    CHECK(texture.LevelCount() == 4);
    // 8x8 → 2x2 blok → 64 byte, lalu tiga level yang masing-masing satu blok.
    CHECK(texture.GpuBytes() == 64 + 16 + 16 + 16);

    const std::vector<std::string> errors = ValidationErrors();
    for (const std::string& error : errors) {
        INFO(error);
        CHECK_MESSAGE(false, "validation layer melaporkan galat saat unggah");
    }
    CHECK(errors.empty());

    // Dilepas sebelum device-nya, kalau tidak yang dihancurkan adalah image
    // milik device yang sudah tidak ada.
    texture.Destroy();
    device.Destroy();
    Log::Shutdown();
}
