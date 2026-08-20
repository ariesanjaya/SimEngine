#include "Sim/RHI/Device.h"

#include "Sim/Core/Assert.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>

namespace sim::rhi {
namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void* /*userData*/) {
    const char* message = data->pMessage != nullptr ? data->pMessage : "(kosong)";
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        SIM_ERROR("RHI", "[validation] {}", message);
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        SIM_WARN("RHI", "[validation] {}", message);
    } else {
        SIM_DEBUG_LOG("RHI", "[validation] {}", message);
    }
    // Selalu VK_FALSE: mengembalikan VK_TRUE akan membatalkan panggilan yang
    // memicunya, yang membuat perilaku Debug berbeda dari Release.
    return VK_FALSE;
}

bool HasLayer(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    return std::any_of(layers.begin(), layers.end(), [name](const VkLayerProperties& layer) {
        return std::strcmp(layer.layerName, name) == 0;
    });
}

bool HasInstanceExtension(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
    return std::any_of(extensions.begin(), extensions.end(),
                       [name](const VkExtensionProperties& extension) {
                           return std::strcmp(extension.extensionName, name) == 0;
                       });
}

/// Sama, untuk ekstensi yang disediakan sebuah lapisan alih-alih implementasi.
bool HasLayerExtension(const char* layer, const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(layer, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateInstanceExtensionProperties(layer, &count, extensions.data());
    return std::any_of(extensions.begin(), extensions.end(),
                       [name](const VkExtensionProperties& extension) {
                           return std::strcmp(extension.extensionName, name) == 0;
                       });
}

VkDebugUtilsMessengerCreateInfoEXT MakeDebugMessengerInfo() {
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = DebugCallback;
    return info;
}

}  // namespace

std::string_view ResultToString(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        default: return "unknown VkResult";
    }
}

Device::~Device() {
    Destroy();
}

bool Device::Create(const DeviceDesc& desc) {
    if (!CreateInstance(desc)) {
        return false;
    }
    if (!SelectPhysicalDevice()) {
        return false;
    }
    if (!CreateLogicalDevice()) {
        return false;
    }

    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.physicalDevice = physicalDevice_;
    allocatorInfo.device = device_;
    allocatorInfo.instance = instance_;
    allocatorInfo.vulkanApiVersion = apiVersion_;
    SIM_VK_CHECK(vmaCreateAllocator(&allocatorInfo, &allocator_));

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = graphicsQueueFamily_;
    SIM_VK_CHECK(vkCreateCommandPool(device_, &poolInfo, nullptr, &oneShotPool_));

    pipelineCachePath_ = desc.pipelineCachePath;
    CreatePipelineCache();

    return true;
}

void Device::CreatePipelineCache() {
    std::vector<uint8_t> initial;
    if (!pipelineCachePath_.empty()) {
        std::ifstream file(pipelineCachePath_, std::ios::binary);
        if (file) {
            initial.assign(std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>());
        }
    }

    // **Isinya diperiksa, bukan dipercaya.** Cache pipeline terikat pada
    // vendor, model kartu, dan versi driver; menyerahkan cache milik salah
    // satunya yang lain kepada `vkCreatePipelineCache` adalah perilaku tak
    // terdefinisi menurut spesifikasi — bukan cache yang lambat, melainkan
    // apa pun. Driver *boleh* menolaknya sendiri, dan sebagian memang begitu,
    // tapi "boleh" bukan jaminan yang layak dipakai pada berkas yang isinya
    // bisa berasal dari mesin lain lewat folder yang disinkronkan.
    //
    // Kepalanya sudah cukup untuk memeriksa: panjang, versi, vendorID,
    // deviceID, lalu UUID cache milik driver.
    bool usable = false;
    if (initial.size() > 32) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
        uint32_t headerLength = 0;
        uint32_t headerVersion = 0;
        uint32_t vendorId = 0;
        uint32_t deviceId = 0;
        std::memcpy(&headerLength, initial.data(), sizeof(uint32_t));
        std::memcpy(&headerVersion, initial.data() + 4, sizeof(uint32_t));
        std::memcpy(&vendorId, initial.data() + 8, sizeof(uint32_t));
        std::memcpy(&deviceId, initial.data() + 12, sizeof(uint32_t));
        usable = headerLength > 0 && headerLength <= initial.size() &&
                 headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
                 vendorId == properties.vendorID && deviceId == properties.deviceID &&
                 std::memcmp(initial.data() + 16, properties.pipelineCacheUUID,
                             VK_UUID_SIZE) == 0;
        if (!usable) {
            // Dicatat, bukan didiamkan: cache yang selalu ditolak berarti setiap
            // peluncuran membayar kompilasi penuh, dan yang mengalaminya tidak
            // punya cara menebak sebabnya.
            SIM_INFO("RHI", "pipeline cache on disk belongs to another device or driver; "
                            "starting empty");
        }
    }

    VkPipelineCacheCreateInfo cacheInfo{};
    cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    if (usable) {
        cacheInfo.initialDataSize = initial.size();
        cacheInfo.pInitialData = initial.data();
    }
    SIM_VK_CHECK(vkCreatePipelineCache(device_, &cacheInfo, nullptr, &pipelineCache_));
    if (usable) {
        SIM_INFO("RHI", "pipeline cache loaded ({} KB) from {}", initial.size() / 1024,
                 pipelineCachePath_.string());
    }
}

void Device::SavePipelineCache() const {
    if (pipelineCache_ == VK_NULL_HANDLE || pipelineCachePath_.empty()) {
        return;
    }
    std::size_t size = 0;
    if (vkGetPipelineCacheData(device_, pipelineCache_, &size, nullptr) != VK_SUCCESS ||
        size == 0) {
        return;
    }
    std::vector<uint8_t> data(size);
    if (vkGetPipelineCacheData(device_, pipelineCache_, &size, data.data()) != VK_SUCCESS) {
        return;
    }

    std::error_code error;
    std::filesystem::create_directories(pipelineCachePath_.parent_path(), error);

    // **Ditulis ke berkas sementara lalu dipindah.** Menulis langsung ke
    // tempatnya berarti sebuah proses yang mati di tengah penulisan meninggalkan
    // cache setengah jadi — yang kepalanya sah, isinya tidak, dan yang
    // membacanya besok menyerahkannya ke driver. `rename` di dalam satu
    // filesystem bersifat atomik; itulah yang membuat berkasnya selalu utuh
    // atau tidak ada sama sekali.
    std::filesystem::path temporary = pipelineCachePath_;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            SIM_WARN("RHI", "cannot write pipeline cache to {}", temporary.string());
            return;
        }
        file.write(reinterpret_cast<const char*>(data.data()),
                   static_cast<std::streamsize>(data.size()));
    }
    std::filesystem::rename(temporary, pipelineCachePath_, error);
    if (error) {
        SIM_WARN("RHI", "cannot move pipeline cache into place: {}", error.message());
        std::filesystem::remove(temporary, error);
        return;
    }
    SIM_INFO("RHI", "pipeline cache saved ({} KB)", data.size() / 1024);
}

bool Device::CreateInstance(const DeviceDesc& desc) {
    // Tanpa satu pun ekstensi instance berarti tidak ada jendela — dan karena
    // itu tidak ada surface, dan karena itu tidak boleh ada swapchain. Dicatat
    // di sini karena hanya di sini `desc` terlihat.
    headless_ = desc.instanceExtensions.empty();
    // Ditaut langsung ke loader, jadi simbolnya selalu ada (Vulkan 1.1+).
    apiVersion_ = VK_API_VERSION_1_0;
    SIM_VK_CHECK(vkEnumerateInstanceVersion(&apiVersion_));
    // Loader boleh lebih baru dari header yang kita compile; membatasi ke
    // VK_HEADER_VERSION_COMPLETE mencegah kita meminta versi yang struct-nya
    // tidak kita kenal.
    apiVersion_ = std::min(apiVersion_, static_cast<uint32_t>(VK_HEADER_VERSION_COMPLETE));

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = desc.applicationName.c_str();
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "SimEngine";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = apiVersion_;

    std::vector<const char*> extensions = desc.instanceExtensions;
    std::vector<const char*> layers;

    validationEnabled_ = desc.enableValidation && HasLayer(kValidationLayer) &&
                         HasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (desc.enableValidation && !validationEnabled_) {
        SIM_WARN("RHI", "Validation layer requested but unavailable; running without validation");
    }
    if (validationEnabled_) {
        layers.push_back(kValidationLayer);
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    // `VkValidationFeaturesEXT` di pNext hanya dibaca bila ekstensi ini ikut
    // diminta. Tanpanya struktur itu diabaikan **diam-diam** — dan diamnya
    // sempurna: validasi sinkronisasi yang tidak menyala melaporkan persis
    // sebanyak yang menyala dan tidak menemukan apa-apa, yaitu nol.
    //
    // Ditanyakan dengan nama lapisannya, bukan dengan `nullptr`: ekstensi ini
    // datang **dari** lapisan validasi, jadi daftar ekstensi implementasi —
    // yang dikembalikan `vkEnumerateInstanceExtensionProperties(nullptr, ...)`
    // — tidak pernah menyebutkannya. Menanyakannya di sana selalu menjawab
    // "tidak ada", pada mesin yang sebenarnya punya.
    const bool syncValidationRequested =
        validationEnabled_ && desc.enableSyncValidation &&
        HasLayerExtension(kValidationLayer, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    if (validationEnabled_ && desc.enableSyncValidation && !syncValidationRequested) {
        SIM_WARN("RHI", "sync validation requested but {} is missing",
                 VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    }
    if (syncValidationRequested) {
        extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    }
    if (HasInstanceExtension(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    }

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
    instanceInfo.ppEnabledLayerNames = layers.data();
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instanceInfo.ppEnabledExtensionNames = extensions.data();

    // Messenger dipasang lewat pNext supaya error selama vkCreateInstance
    // sendiri ikut tertangkap — messenger biasa baru hidup setelah instance ada.
    VkDebugUtilsMessengerCreateInfoEXT debugInfo = MakeDebugMessengerInfo();
    if (validationEnabled_) {
        instanceInfo.pNext = &debugInfo;
    }

    // Dinyalakan dari sini, bukan lewat variabel lingkungan: nama setelan
    // lapisan berganti antar-versi SDK — `VK_LAYER_ENABLES` yang lama sudah
    // dilaporkan usang oleh lapisan 1.4 — dan cara yang bergantung pada nama
    // yang berganti adalah cara yang suatu hari berhenti menyala tanpa memberi
    // tahu siapa pun.
    const VkValidationFeatureEnableEXT syncFeature =
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validationFeatures{};
    validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    validationFeatures.enabledValidationFeatureCount = 1;
    validationFeatures.pEnabledValidationFeatures = &syncFeature;
    if (syncValidationRequested) {
        validationFeatures.pNext = instanceInfo.pNext;
        instanceInfo.pNext = &validationFeatures;
        syncValidationEnabled_ = true;
    }

    const VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance_);
    if (result != VK_SUCCESS) {
        SIM_CRITICAL("RHI", "vkCreateInstance failed: {}", ResultToString(result));
        return false;
    }

    if (validationEnabled_) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (create != nullptr) {
            SIM_VK_CHECK(create(instance_, &debugInfo, nullptr, &debugMessenger_));
        }
    }

    SIM_INFO("RHI", "Vulkan instance {}.{}.{}{}{}", VK_VERSION_MAJOR(apiVersion_),
             VK_VERSION_MINOR(apiVersion_), VK_VERSION_PATCH(apiVersion_),
             validationEnabled_ ? " (validation on" : "",
             validationEnabled_ ? (syncValidationEnabled_ ? ", sync)" : ")") : "");
    return true;
}

bool Device::SelectPhysicalDevice() {
    uint32_t count = 0;
    SIM_VK_CHECK(vkEnumeratePhysicalDevices(instance_, &count, nullptr));
    if (count == 0) {
        SIM_CRITICAL("RHI", "No Vulkan device found");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    SIM_VK_CHECK(vkEnumeratePhysicalDevices(instance_, &count, devices.data()));

    auto scoreOf = [](VkPhysicalDevice device) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        switch (properties.deviceType) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 3;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 2;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 1;
            default: return 0;  // CPU/llvmpipe hanya dipakai kalau tidak ada lain
        }
    };

    physicalDevice_ = *std::max_element(
        devices.begin(), devices.end(),
        [&scoreOf](VkPhysicalDevice a, VkPhysicalDevice b) { return scoreOf(a) < scoreOf(b); });

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
    deviceName_ = properties.deviceName;
    apiVersion_ = std::min(apiVersion_, properties.apiVersion);
    supportsVulkan13_ = properties.apiVersion >= VK_API_VERSION_1_3;
    SIM_INFO("RHI", "GPU: {} (driver {}.{}.{})", deviceName_,
             VK_VERSION_MAJOR(properties.driverVersion), VK_VERSION_MINOR(properties.driverVersion),
             VK_VERSION_PATCH(properties.driverVersion));

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &familyCount, families.data());

    for (uint32_t i = 0; i < familyCount; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            graphicsQueueFamily_ = i;
            break;
        }
    }
    if (graphicsQueueFamily_ == UINT32_MAX) {
        SIM_CRITICAL("RHI", "No graphics-capable queue family");
        return false;
    }

    // **Antrean compute yang dicari adalah yang TIDAK bisa menggambar.** Sebuah
    // keluarga yang punya kedua bit hampir selalu keluarga grafis itu sendiri,
    // dan menjalankan dua antrean di atasnya tidak membeli tumpang tindih apa
    // pun — pekerjaannya tetap mengantre di perangkat keras yang sama. Yang
    // memberi tumpang tindih adalah keluarga terpisah, yang pada kartu modern
    // memang ada justru untuk itu.
    for (uint32_t i = 0; i < familyCount; ++i) {
        const bool compute = (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
        const bool graphics = (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        if (compute && !graphics) {
            computeQueueFamily_ = i;
            break;
        }
    }
    capabilities_.asyncCompute = computeQueueFamily_ != UINT32_MAX;
    return true;
}

bool Device::CreateLogicalDevice() {
    const float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    {
        VkDeviceQueueCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        info.queueFamilyIndex = graphicsQueueFamily_;
        info.queueCount = 1;
        info.pQueuePriorities = &priority;
        queueInfos.push_back(info);
    }
    if (computeQueueFamily_ != UINT32_MAX) {
        VkDeviceQueueCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        info.queueFamilyIndex = computeQueueFamily_;
        info.queueCount = 1;
        info.pQueuePriorities = &priority;
        queueInfos.push_back(info);
    }

    // Ray query hanya **dideteksi** di sini, tidak diaktifkan. Mengaktifkannya
    // menuntut acceleration structure beserta seluruh rantai pembangunannya, dan
    // itu pekerjaan M7 rencana GI. Yang dibutuhkan sekarang hanya jawaban atas
    // "backend mana yang akan dipilih di mesin ini" — dan jawaban itu harus ada
    // sejak M0, supaya jalur SDF tidak diam-diam berhenti diuji.
    {
        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> available(extensionCount);
        vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extensionCount,
                                             available.data());
        bool rayQuery = false;
        bool accelerationStructure = false;
        for (const VkExtensionProperties& extension : available) {
            const std::string_view name(extension.extensionName);
            rayQuery = rayQuery || name == VK_KHR_RAY_QUERY_EXTENSION_NAME;
            accelerationStructure = accelerationStructure ||
                                    name == VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME;
        }
        // Keduanya, bukan salah satu: ray query tanpa acceleration structure
        // tidak punya apa pun untuk ditelusuri.
        supportsRayQuery_ = rayQuery && accelerationStructure;
    }

    // **Swapchain hanya kalau ada surface.** `VK_KHR_swapchain` menuntut
    // `VK_KHR_surface` di sisi instance, dan yang meminta device tanpa ekstensi
    // instance sama sekali — jalur headless: uji unggahan tekstur, baker, mesin
    // build tanpa jendela — tidak pernah punya keduanya. Memintanya di sana
    // menghasilkan galat validation layer pada `vkCreateDevice` yang tidak
    // berhubungan dengan apa pun yang sedang dikerjakan, dan yang lalu
    // menenggelamkan galat sungguhan di antara derau.
    std::vector<const char*> deviceExtensions;
    if (!headless_) {
        deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }

    VkPhysicalDeviceFeatures features{};
    features.fillModeNonSolid = VK_TRUE;  // wireframe di viewport editor
    features.wideLines = VK_TRUE;         // garis gizmo & grid
    // Pass probe GI menulis ke cache radiansi dari tahap fragment, dan merebut
    // slotnya dengan atomik. Tanpa fitur ini setiap storage buffer yang tidak
    // ditandai NonWritable ditolak di pembuatan pipeline — ditemukan validation
    // layer, bukan dengan membaca kode.
    features.fragmentStoresAndAtomics = VK_TRUE;
    // Format blok BCn — BC7 untuk albedo, BC5 untuk normal, BC4 untuk roughness,
    // BC6H untuk HDR. **Diminta, bukan diandaikan:** setiap GPU desktop sepuluh
    // tahun terakhir punya, tetapi mengunggah `VK_FORMAT_BC7_...` ke perangkat
    // yang tidak mendukungnya adalah galat validation layer di setiap tekstur,
    // bukan gambar yang jelek.
    features.textureCompressionBC = VK_TRUE;

    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(physicalDevice_, &supported);
    features.fillModeNonSolid &= supported.fillModeNonSolid;
    features.wideLines &= supported.wideLines;
    features.fragmentStoresAndAtomics &= supported.fragmentStoresAndAtomics;
    features.textureCompressionBC &= supported.textureCompressionBC;
    supportsBlockCompression_ = features.textureCompressionBC == VK_TRUE;

    // **Ketiadaannya dicatat, bukan didiamkan.** Perangkat tanpa BC tetap
    // menjalankan editor — teksturnya jatuh ke RGBA8 — tetapi itu berarti empat
    // kali memori tekstur, dan yang menemukannya lewat "kok VRAM-nya penuh"
    // akan mencari di tempat yang salah selama berjam-jam.
    if (!supportsBlockCompression_) {
        SIM_WARN("RHI",
                 "this GPU has no BC texture compression; textures fall back to RGBA8 and use "
                 "about four times the memory");
    }

    // Fitur Vulkan 1.3 yang kita aktifkan sejak sekarang:
    //   - shaderDemoteToHelperInvocation: dibutuhkan segera, karena glslc
    //     menerjemahkan `discard` menjadi OpDemoteToHelperInvocation saat
    //     --target-env=vulkan1.3, dan shader grid memakai discard.
    //   - dynamicRendering & synchronization2: belum dipakai di E1, tapi
    //     mengaktifkannya sekarang membuat E8 tidak perlu menyentuh pembuatan
    //     device lagi.
    //   - shaderDrawParameters: dituntut shader material, dan asalnya sebuah
    //     keputusan yang sengaja diambil. `SV_InstanceID` di HLSL/Slang berarti
    //     nomor instance **relatif terhadap `firstInstance`**, jadi slangc
    //     menerjemahkannya menjadi `gl_InstanceIndex - gl_BaseInstance` — dan
    //     `gl_BaseInstance` itulah yang menuntut kapabilitas DrawParameters.
    //     Ditemukan validation layer, bukan dengan membaca kode.
    VkPhysicalDeviceVulkan11Features supported11{};
    supported11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    // **Ditanyakan sekarang walau belum dinyalakan.** Yang dibutuhkan sekarang
    // bukan fiturnya melainkan jawabannya: G5 sampai G7 masing-masing berdiri
    // atau jatuh di atas salah satu dari medan-medan ini, dan jawaban yang baru
    // dicari pada hari milestone-nya dimulai adalah jawaban yang datang setelah
    // rencananya terlanjur disusun.
    VkPhysicalDeviceVulkan12Features supported12{};
    supported12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    supported12.pNext = &supported11;
    VkPhysicalDeviceVulkan13Features supported13{};
    supported13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    supported13.pNext = &supported12;
    VkPhysicalDeviceFeatures2 supported2{};
    supported2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    supported2.pNext = &supported13;
    if (supportsVulkan13_) {
        vkGetPhysicalDeviceFeatures2(physicalDevice_, &supported2);
    }

    VkPhysicalDeviceVulkan11Features enable11{};
    enable11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    enable11.shaderDrawParameters = supported11.shaderDrawParameters;

    // Bindless (G5). **Tujuh medan, dan ketujuhnya wajib bersama-sama** — jalur
    // material memakai semuanya sekaligus, jadi menyalakan sebagiannya
    // menghasilkan device yang mengaku bindless lalu gagal di pembuatan
    // descriptor set layout, bukan di sini:
    //
    //   - `runtimeDescriptorArray` — larik `Texture2D t[]` tanpa batas yang
    //     ditulis di shader.
    //   - `shaderSampledImageArrayNonUniformIndexing` — indeksnya datang dari
    //     data material, jadi ia berbeda antar-fragmen dalam satu warp.
    //   - `descriptorBindingSampledImageUpdateAfterBind` dan
    //     `descriptorBindingUniformBufferUpdateAfterBind` — tekstur dan material
    //     baru masuk ke slotnya sementara frame sebelumnya masih terbang.
    //     Keduanya wajib, dan yang kedua ditemukan validation layer: blok
    //     parameter material tinggal di binding uniform buffer, dan medan
    //     tekstur tidak menutupinya.
    //   - `descriptorBindingPartiallyBound` — hanya slot yang sudah terisi yang
    //     ditulis; sisanya tidak pernah dibaca siapa pun, dan tanpa medan ini
    //     mereka tetap harus sah.
    //   - dua medan `...ArrayDynamicIndexing` inti 1.0 — indeks larik datang
    //     dari push constant dan dari blok parameter, yaitu nilai yang baru
    //     diketahui saat menggambar.
    //
    // `descriptorBindingVariableDescriptorCount` **tidak** ada di daftar ini,
    // dan itu disengaja: kapasitas larik ditetapkan descriptor set layout, jadi
    // yang dibutuhkan hanya larik tak berbatas di sisi shader. Menuntut medan
    // yang tidak dipakai berarti menolak bindless di perangkat yang sebenarnya
    // mampu.
    const bool bindlessSupported =
        supported12.descriptorIndexing != 0 && supported12.runtimeDescriptorArray != 0 &&
        supported12.shaderSampledImageArrayNonUniformIndexing != 0 &&
        supported12.descriptorBindingSampledImageUpdateAfterBind != 0 &&
        supported12.descriptorBindingUniformBufferUpdateAfterBind != 0 &&
        supported12.descriptorBindingPartiallyBound != 0 &&
        supported.shaderSampledImageArrayDynamicIndexing != 0 &&
        supported.shaderUniformBufferArrayDynamicIndexing != 0;

    // Kapasitas larik bindless diambil dari perangkat, bukan ditulis mati.
    //
    // **Dua batas, dan yang mengikat adalah yang per-tahap.** Batas per-set
    // hampir selalu jauh lebih besar; yang menolak descriptor set layout pada
    // GPU 6 GB justru batas per-tahap. Keduanya diambil minimumnya, lalu
    // dibatasi lagi ke `kBindlessCeiling` — sebuah adegan yang butuh lebih dari
    // itu punya masalah yang tidak diselesaikan larik yang lebih besar, dan
    // larik sebesar batas driver (sebagian melaporkan jutaan) membuat pool
    // descriptor menuntut memori yang tidak pernah dipakai.
    constexpr uint32_t kBindlessCeiling = 4096;
    uint32_t bindlessCapacity = 0;
    if (supportsVulkan13_) {
        VkPhysicalDeviceDescriptorIndexingProperties indexingProperties{};
        indexingProperties.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES;
        VkPhysicalDeviceProperties2 properties2{};
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties2.pNext = &indexingProperties;
        vkGetPhysicalDeviceProperties2(physicalDevice_, &properties2);
        bindlessCapacity =
            std::min({kBindlessCeiling,
                      indexingProperties.maxDescriptorSetUpdateAfterBindSampledImages,
                      indexingProperties.maxPerStageDescriptorUpdateAfterBindSampledImages,
                      indexingProperties.maxPerStageDescriptorUpdateAfterBindSamplers,
                      indexingProperties.maxDescriptorSetUpdateAfterBindSamplers});
    }

    VkPhysicalDeviceVulkan12Features enable12{};
    enable12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    enable12.pNext = &enable11;
    // Kapasitas nol berarti pertanyaannya tidak pernah sampai ke driver
    // (perangkat pra-1.3). Menyalakan fiturnya tetap sah di sana, tetapi larik
    // berkapasitas nol bukan jalur bindless melainkan jalur yang gagal pada
    // tekstur pertama — jadi ia dihitung tidak didukung, sekarang, bukan nanti.
    if (bindlessSupported && bindlessCapacity > 0) {
        enable12.descriptorIndexing = VK_TRUE;
        enable12.runtimeDescriptorArray = VK_TRUE;
        enable12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        enable12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        enable12.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
        enable12.descriptorBindingPartiallyBound = VK_TRUE;
        features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
        features.shaderUniformBufferArrayDynamicIndexing = VK_TRUE;
    }

    VkPhysicalDeviceVulkan13Features enable13{};
    enable13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    enable13.pNext = &enable12;
    enable13.shaderDemoteToHelperInvocation = supported13.shaderDemoteToHelperInvocation;
    enable13.dynamicRendering = supported13.dynamicRendering;
    enable13.synchronization2 = supported13.synchronization2;

    VkPhysicalDeviceFeatures2 enable2{};
    enable2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    enable2.pNext = &enable13;
    enable2.features = features;

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
    deviceInfo.pQueueCreateInfos = queueInfos.data();
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
    // Fitur diberikan lewat pNext (features2) atau lewat pEnabledFeatures —
    // tidak boleh keduanya sekaligus.
    if (supportsVulkan13_) {
        deviceInfo.pNext = &enable2;
    } else {
        deviceInfo.pEnabledFeatures = &features;
    }

    const VkResult result = vkCreateDevice(physicalDevice_, &deviceInfo, nullptr, &device_);
    if (result != VK_SUCCESS) {
        SIM_CRITICAL("RHI", "vkCreateDevice failed: {}", ResultToString(result));
        return false;
    }
    vkGetDeviceQueue(device_, graphicsQueueFamily_, 0, &graphicsQueue_);
    if (computeQueueFamily_ != UINT32_MAX) {
        vkGetDeviceQueue(device_, computeQueueFamily_, 0, &computeQueue_);
    }

    capabilities_.vulkan13 = supportsVulkan13_;
    capabilities_.dynamicRendering = enable13.dynamicRendering != 0;
    capabilities_.synchronization2 = enable13.synchronization2 != 0;
    capabilities_.blockCompression = supportsBlockCompression_;
    capabilities_.rayQuery = supportsRayQuery_;
    capabilities_.descriptorIndexing = enable12.descriptorIndexing != 0;
    capabilities_.bindlessTextureCapacity =
        capabilities_.descriptorIndexing ? bindlessCapacity : 0;
    capabilities_.bufferDeviceAddress = supported12.bufferDeviceAddress != 0;
    capabilities_.timelineSemaphore = supported12.timelineSemaphore != 0;
    capabilities_.drawIndirectCount = supported12.drawIndirectCount != 0;
    capabilities_.shaderFloat16 = supported12.shaderFloat16 != 0;
    capabilities_.multiDrawIndirect = supported.multiDrawIndirect != 0;

    // Satu baris untuk yang dipakai sekarang, satu untuk yang menentukan jalur
    // di depan. Dipisah karena keduanya dibaca oleh orang yang berbeda: yang
    // pertama saat sesuatu tidak tergambar, yang kedua saat memutuskan apakah
    // sebuah milestone bisa dikerjakan di mesin ini sama sekali.
    SIM_INFO("RHI", "Device ready (Vulkan 1.3: {}, dynamic rendering: {}, sync2: {}, BC: {})",
             capabilities_.vulkan13 ? "yes" : "no",
             capabilities_.dynamicRendering ? "yes" : "no",
             capabilities_.synchronization2 ? "yes" : "no",
             capabilities_.blockCompression ? "yes" : "no");
    SIM_INFO("RHI",
             "Capabilities: bindless {}, buffer address {}, timeline {}, indirect {}/{}, "
             "fp16 {}, async compute {}, ray query {}",
             capabilities_.descriptorIndexing
                 ? std::format("yes ({} slots)", capabilities_.bindlessTextureCapacity)
                 : std::string("no"),
             capabilities_.bufferDeviceAddress ? "yes" : "no",
             capabilities_.timelineSemaphore ? "yes" : "no",
             capabilities_.multiDrawIndirect ? "yes" : "no",
             capabilities_.drawIndirectCount ? "yes" : "no",
             capabilities_.shaderFloat16 ? "yes" : "no",
             capabilities_.asyncCompute ? "yes" : "no",
             capabilities_.rayQuery ? "yes" : "no");
    return true;
}

void Device::Destroy() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
    // Sebelum cache-nya dihancurkan, dan sesudah GPU diam: isi yang dipungut
    // dari cache yang masih dipakai pipeline yang sedang berjalan tidak dijamin
    // lengkap.
    SavePipelineCache();
    for (TransientSubmit& slot : transients_) {
        vkDestroyFence(device_, slot.fence, nullptr);
        vkDestroyCommandPool(device_, slot.pool, nullptr);
    }
    transients_.clear();
    if (pipelineCache_ != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
        pipelineCache_ = VK_NULL_HANDLE;
    }
    if (oneShotPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, oneShotPool_, nullptr);
        oneShotPool_ = VK_NULL_HANDLE;
    }
    if (allocator_ != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator_);
        allocator_ = VK_NULL_HANDLE;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (debugMessenger_ != VK_NULL_HANDLE) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy != nullptr) {
            destroy(instance_, debugMessenger_, nullptr);
        }
        debugMessenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

VkSurfaceKHR Device::CreateSurface(SDL_Window* window) const {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window, instance_, nullptr, &surface)) {
        SIM_CRITICAL("RHI", "SDL_Vulkan_CreateSurface failed: {}", SDL_GetError());
        return VK_NULL_HANDLE;
    }
    return surface;
}

void Device::DestroySurface(VkSurfaceKHR surface) const {
    if (surface != VK_NULL_HANDLE) {
        SDL_Vulkan_DestroySurface(instance_, surface, nullptr);
    }
}

VkCommandBuffer Device::BeginOneShot() const {
    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = oneShotPool_;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    SIM_VK_CHECK(vkAllocateCommandBuffers(device_, &allocateInfo, &commandBuffer));

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    SIM_VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));
    return commandBuffer;
}

void Device::EndOneShot(VkCommandBuffer commandBuffer) const {
    SIM_VK_CHECK(vkEndCommandBuffer(commandBuffer));

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    SIM_VK_CHECK(vkQueueSubmit(graphicsQueue_, 1, &submitInfo, VK_NULL_HANDLE));
    // Setup jarang terjadi dan selalu di luar frame, jadi menunggu queue idle
    // di sini lebih sederhana daripada mengelola fence sekali pakai.
    SIM_VK_CHECK(vkQueueWaitIdle(graphicsQueue_));
    vkFreeCommandBuffers(device_, oneShotPool_, 1, &commandBuffer);
}

Device::TransientSubmit Device::CreateTransient() const {
    TransientSubmit slot;

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = graphicsQueueFamily_;
    SIM_VK_CHECK(vkCreateCommandPool(device_, &poolInfo, nullptr, &slot.pool));

    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = slot.pool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    SIM_VK_CHECK(vkAllocateCommandBuffers(device_, &allocateInfo, &slot.commandBuffer));

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    SIM_VK_CHECK(vkCreateFence(device_, &fenceInfo, nullptr, &slot.fence));
    return slot;
}

VkCommandBuffer Device::BeginTransient() const {
    TransientSubmit* slot = nullptr;
    for (TransientSubmit& candidate : transients_) {
        if (!candidate.pending) {
            slot = &candidate;
            break;
        }
        // vkGetFenceStatus tidak memblokir: slot yang GPU-nya sudah selesai
        // langsung bisa dipakai ulang, yang belum dilewati begitu saja.
        if (vkGetFenceStatus(device_, candidate.fence) == VK_SUCCESS) {
            SIM_VK_CHECK(vkResetFences(device_, 1, &candidate.fence));
            SIM_VK_CHECK(vkResetCommandPool(device_, candidate.pool, 0));
            candidate.pending = false;
            // Nomor submit lama dilepas di sini. Itulah yang membuat
            // WaitTransient() aman: nomor yang tidak lagi ditemukan berarti
            // pekerjaannya sudah selesai, bukan berarti belum pernah ada.
            candidate.submitId = 0;
            slot = &candidate;
            break;
        }
    }

    if (slot == nullptr) {
        // Kolam tumbuh sampai sebanyak submit yang bisa in-flight sekaligus,
        // biasanya berhenti di 2-3. Batas ini murni jaring pengaman kalau ada
        // kebocoran: tanpa itu, submit yang fence-nya tak pernah di-signal akan
        // menambah slot tiap frame sampai kehabisan memori.
        SIM_VERIFY(transients_.size() < 64, "Transient command buffer pool grew unbounded");
        transients_.push_back(CreateTransient());
        slot = &transients_.back();
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    SIM_VK_CHECK(vkBeginCommandBuffer(slot->commandBuffer, &beginInfo));
    return slot->commandBuffer;
}

uint64_t Device::SubmitTransient(VkCommandBuffer commandBuffer) const {
    TransientSubmit* slot = nullptr;
    for (TransientSubmit& candidate : transients_) {
        if (candidate.commandBuffer == commandBuffer) {
            slot = &candidate;
            break;
        }
    }
    SIM_VERIFY(slot != nullptr, "SubmitTransient called with a foreign command buffer");

    SIM_VK_CHECK(vkEndCommandBuffer(commandBuffer));

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    SIM_VK_CHECK(vkQueueSubmit(graphicsQueue_, 1, &submitInfo, slot->fence));
    slot->pending = true;
    slot->submitId = nextSubmitId_++;
    return slot->submitId;
}

void Device::WaitTransient(uint64_t submitId) const {
    if (submitId == 0) {
        return;
    }
    for (const TransientSubmit& candidate : transients_) {
        if (candidate.submitId != submitId) {
            continue;
        }
        if (candidate.pending) {
            SIM_VK_CHECK(vkWaitForFences(device_, 1, &candidate.fence, VK_TRUE, UINT64_MAX));
        }
        return;
    }
    // Tidak ditemukan: slotnya sudah dipakai ulang, dan itu hanya terjadi
    // setelah fence-nya di-signal. Pekerjaannya sudah selesai.
}

void Device::WaitIdle() const {
    if (device_ != VK_NULL_HANDLE) {
        SIM_VK_CHECK(vkDeviceWaitIdle(device_));
    }
}

}  // namespace sim::rhi
