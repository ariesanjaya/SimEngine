#include "Sim/Render/RendererFactory.h"

#include "DepthPyramid.h"
#include "PostProcess.h"
#include "SkyAtmosphere.h"
#include "FrameGraphExecutor.h"
#include "ProbeField.h"
#include "SdfClipmapResource.h"
#include "Sim/Core/Log.h"
#include "Sim/RHI/Buffer.h"
#include "Sim/RHI/Device.h"
#include "Sim/RHI/GpuProfiler.h"
#include "Sim/RHI/RenderTarget.h"
#include "Sim/RHI/TextureRegistry.h"
#include "Sim/Render/FrameGraph.h"
#include "Sim/Render/Frustum.h"
#include "Sim/Render/LightCluster.h"
#include "Sim/Render/RadianceCache.h"
#include "Sim/Render/ShadowAtlas.h"
#include "Sim/Render/TraceBackend.h"
#include "Sim/Render/ShadowCascades.h"

#include <algorithm>
#include <chrono>
#include <array>
#include <limits>
#include <cstddef>
#include <fstream>
#include <vector>

namespace sim::render {
namespace {

/// Harus sama persis dengan blok push_constant di Shaders/box.vert.
struct BoxPush {
    Mat4 viewProj;
};

/// Harus sama persis dengan blok push_constant di Shaders/sdf_debug.{vert,frag}.
struct SdfDebugPush {
    Mat4 invViewProj{1.0f};
    /// xyz posisi kamera, w depth bidang dekat.
    Vec4 cameraPosition{0.0f};
    /// x jenis debug view, y jangkauan trace maksimum.
    Vec4 params{0.0f};
};

/// Harus sama persis dengan blok `ShadowParams` di Shaders/shadow_common.glsl.
///
/// **ABI, dan std140.** Selisih satu sisipan tidak menghasilkan galat apa pun —
/// hanya bayangan yang jatuh di tempat yang salah, yang paling mudah dikira
/// masalah matriks cascade dan dicari berjam-jam di tempat yang keliru.
struct ShadowUniforms {
    std::array<Mat4, kMaxCascades> cascadeViewProj{};
    Vec4 cascadeSplitFar{0.0f};
    Vec4 cascadeBlendBegin{0.0f};
    Vec4 cascadeTexelSize{0.0f};
    /// xyz arah ke cahaya, w jumlah cascade.
    Vec4 lightDirection{0.0f};
    /// xyz posisi kamera, w 1 kalau bayangan menyala.
    Vec4 cameraPosition{0.0f};
    /// rgb radiance matahari — warna dikali intensitas dikali eksposur.
    Vec4 sunRadiance{0.0f};
    /// xyz arah pandang, w kekuatan bias normal dalam satuan texel.
    Vec4 cameraForward{0.0f};
    /// xyz ubin dan irisan cluster, w jumlah lampu.
    Vec4 clusterCounts{0.0f};
    /// x skala irisan, y bias irisan, z near, w far.
    Vec4 clusterDepth{0.0f};
    /// xy ukuran viewport dalam piksel.
    Vec4 viewportSize{0.0f};
    /// Kaskade SDF: xyz titik asal dunia, w ukuran voxel.
    std::array<Vec4, 4> sdfOrigin{};
    /// x resolusi, y jumlah kaskade, z lebar pita dalam voxel, w langkah maks.
    Vec4 sdfParams{0.0f};
    /// Dunia → clip. Dipakai lapis screen-space untuk memproyeksikan sinar ke
    /// layar. Di sini, bukan di push constant: batas push constant yang dijamin
    /// Vulkan hanya 128 byte, dan dua matriks saja sudah menghabiskannya.
    Mat4 viewProj{1.0f};
    /// Clip → dunia. Di UBO supaya penelusur screen-space tidak bergantung pada
    /// push constant pemakainya — dan ia dipakai lebih dari satu pass.
    Mat4 invViewProj{1.0f};
    /// x ketebalan yang diandaikan (meter), y dorongan awal (meter),
    /// z langkah maks screen-space, w jumlah tingkat HiZ. Nol berarti lapis
    /// screen-space mati.
    Vec4 screenTrace{0.0f};
    /// x kapasitas cache (pangkat dua), y ukuran sel terhalus, z jarak LOD,
    /// w langkah probing maksimum. Kapasitas nol berarti cache mati.
    Vec4 cacheParams{0.0f};
    /// x frame akumulasi, y frame sebelum entri boleh direbut, z nomor frame.
    Vec4 cacheDecay{0.0f};
    /// Dunia → clip frame sebelumnya.
    Mat4 previousViewProj{1.0f};
    /// x jendela akumulasi, y jarak bidang penolakan riwayat, z kosinus normal
    /// minimum, w skala penjepitan riwayat.
    Vec4 denoise{0.0f};
    /// x 1 kalau iradiansi GI berlaku, y ukuran ubin probe dalam piksel.
    Vec4 giParams{0.0f};
};
// 7 mat4 + 20 vec4. Angkanya ditulis eksplisit supaya menambah medan tanpa
// memperbarui shader-nya menjadi galat kompilasi, bukan bayangan yang bergeser.
static_assert(sizeof(ShadowUniforms) == 7 * 64 + 20 * 16,
              "ShadowUniforms harus cocok dengan blok ShadowParams di shadow_common.slang");

/// Cermin dari `GpuLight` di Shaders/cluster_common.glsl. std430.
///
/// **Yang bisa dihitung CPU dihitung di CPU.** Bukan demi kecepatan — beberapa
/// pembagian per lampu tidak berarti apa-apa — melainkan supaya shader tidak
/// punya keputusan aritmetika sendiri yang bisa menyimpang dari sisi C++. Itu
/// disiplin yang sama dengan skala dan bias irisan cluster, dan alasannya sama:
/// dua rumus yang setara secara matematis tapi ditulis terpisah akan berselisih
/// di tepinya, dan selisih itu muncul sebagai lampu yang berperilaku aneh pada
/// jarak tertentu saja.
///
/// Yang ikut ke sini karena itu bukan `range` melainkan `1/range²`, dan bukan
/// `sourceRadius` melainkan `sourceRadius²` — bentuk yang benar-benar dipakai
/// shader, sehingga tidak ada lagi kuadrat atau pembagian yang bisa ditulis
/// berbeda di sana.
struct GpuLight {
    /// xyz posisi, w 1/range² — nol berarti lampu tanpa jangkauan berhingga.
    Vec4 positionInvRangeSq{0.0f};
    Vec4 directionCosOuter{0.0f};
    Vec4 colorCosInner{0.0f};
    /// x: 0 point, 1 spot. y: jarak minimum kuadrat, yaitu `sourceRadius²`.
    /// z dan w cadangan — std430 menyisipkan sampai kelipatan 16, jadi keduanya
    /// tidak memakan apa pun.
    Vec4 kind{0.0f};
};
static_assert(sizeof(GpuLight) == 64, "GpuLight harus cocok dengan std430-nya");

/// Cermin dari `GpuShadowFace` di Shaders/cluster_common.glsl. std430.
struct GpuShadowFace {
    Mat4 viewProjection{1.0f};
    /// xy sudut kiri-atas ubin dalam UV atlas, zw ukurannya.
    Vec4 tile{0.0f};
};
static_assert(sizeof(GpuShadowFace) == 80, "GpuShadowFace harus cocok dengan std430-nya");

/// Peta bayangan cascade: satu image D32 berlapis, satu lapis per cascade.
///
/// **Larik berlapis, bukan atlas di satu tekstur besar.** Atlas menuntut shader
/// menggeser dan menskalakan koordinat per cascade, dan penyaringan PCF-nya lalu
/// bisa mengambil texel milik cascade tetangga di tepi ubin. Lapisan memberi
/// tiap cascade ruang koordinatnya sendiri, dan penjepitan sampler bekerja apa
/// adanya.
struct ShadowMap {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView arrayView = VK_NULL_HANDLE;
    std::array<VkImageView, kMaxCascades> layerViews{};
    VkSampler sampler = VK_NULL_HANDLE;
    uint32_t resolution = 0;
    uint32_t layers = 0;

    bool IsValid() const { return image != VK_NULL_HANDLE; }
};

/// Harus sama persis dengan blok push_constant di Shaders/grid.{vert,frag}.
struct GridPush {
    Mat4 invViewProj;
    Vec4 cameraPos;
    Vec4 params;
};

/// Harus sama persis dengan blok push_constant di Shaders/line.vert.
struct LinePush {
    Mat4 viewProj;
};

struct LineVertex {
    Vec3 position;
    Vec4 color;
};

struct BoxVertex {
    Vec3 position;
    Vec3 normal;
};

/// Satu instance kotak. Tata letaknya harus sama persis dengan atribut instance
/// di Shaders/box.vert.
struct BoxInstance {
    Vec4 row0;
    Vec4 row1;
    Vec4 row2;
    Vec4 row3;
    Vec4 color;
    /// Bit 0: menerima bayangan. Sebuah bitmask, bukan float bernilai 0/1 —
    /// bendera per-instance berikutnya tinggal mengambil bit berikutnya alih-alih
    /// menuntut atribut vertex baru.
    uint32_t flags;
};

/// Bit 0 dari `BoxInstance::flags`. Harus sama dengan `kReceiveShadows` di
/// Shaders/box.frag.
constexpr uint32_t kInstanceReceiveShadows = 1u;

constexpr Vec4 kSelectedColor{1.0f, 0.62f, 0.20f, 1.0f};

constexpr VkFormat kShadowFormat = VK_FORMAT_D32_SFLOAT;
constexpr uint32_t kShadowResolution = 2048;
/// Atlas point/spot. Terpisah dari cascade karena keduanya punya bentuk yang
/// berbeda — cascade larik berlapis dengan resolusi seragam, atlas satu bidang
/// dengan ubin beragam ukuran — dan menyatukannya berarti salah satunya harus
/// mengalah pada bentuk yang bukan miliknya.
constexpr uint32_t kAtlasResolution = 2048;
/// Kekuatan bias normal, dalam satuan texel dunia cascade yang bersangkutan.
/// Bukan angka ajaib per-cascade: ia dikalikan `texelWorldSize`, jadi cascade
/// yang lebih kasar otomatis mendapat pergeseran yang lebih besar.
constexpr float kNormalBiasTexels = 1.5f;
/// Bias normal atlas, dalam satuan texel ubin. Lebih besar daripada milik
/// cascade karena ubin atlas jauh lebih kecil dan frustumnya perspektif — texel
/// di ujung jauh sebuah spot mewakili wilayah yang jauh lebih lebar.
constexpr float kAtlasNormalBiasTexels = 3.0f;

std::vector<uint32_t> ReadSpirv(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        SIM_ERROR("Render", "Cannot open shader: {}", path.string());
        return {};
    }
    const std::streamsize size = file.tellg();
    if (size <= 0 || size % 4 != 0) {
        SIM_ERROR("Render", "Invalid SPIR-V size ({} bytes): {}", size, path.string());
        return {};
    }
    std::vector<uint32_t> code(static_cast<std::size_t>(size) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), size);
    return code;
}

VkShaderModule CreateShaderModule(VkDevice device, const std::filesystem::path& path) {
    const std::vector<uint32_t> code = ReadSpirv(path);
    if (code.empty()) {
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size() * sizeof(uint32_t);
    info.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    SIM_VK_CHECK(vkCreateShaderModule(device, &info, nullptr, &module));
    return module;
}

/// Enam sisi kubus satuan, masing-masing dua segitiga, dengan normal per sisi.
///
/// Ditulis apa adanya alih-alih diindeks: normal per sisi berarti sudut kubus
/// tidak bisa dibagi antar-sisi, jadi indeks tidak menghemat apa pun — dan 36
/// vertex yang dibuat sekali saat start bukan tempat yang layak dioptimasi.
std::vector<BoxVertex> BuildUnitCube() {
    const std::array<Vec3, 6> normals{Vec3(0, 0, 1),  Vec3(0, 0, -1), Vec3(1, 0, 0),
                                      Vec3(-1, 0, 0), Vec3(0, 1, 0),  Vec3(0, -1, 0)};
    const std::array<Vec3, 6> tangents{Vec3(1, 0, 0),  Vec3(-1, 0, 0), Vec3(0, 0, -1),
                                       Vec3(0, 0, 1),  Vec3(1, 0, 0),  Vec3(1, 0, 0)};
    std::vector<BoxVertex> vertices;
    vertices.reserve(36);
    for (std::size_t face = 0; face < 6; ++face) {
        const Vec3 n = normals[face];
        const Vec3 t = tangents[face];
        const Vec3 b = glm::cross(n, t);
        const Vec3 centre = n * 0.5f;
        const Vec3 a0 = centre - t * 0.5f - b * 0.5f;
        const Vec3 a1 = centre + t * 0.5f - b * 0.5f;
        const Vec3 a2 = centre + t * 0.5f + b * 0.5f;
        const Vec3 a3 = centre - t * 0.5f + b * 0.5f;
        vertices.push_back({a0, n});
        vertices.push_back({a1, n});
        vertices.push_back({a2, n});
        vertices.push_back({a0, n});
        vertices.push_back({a2, n});
        vertices.push_back({a3, n});
    }
    return vertices;
}

BoxInstance MakeInstance(const Mat4& model, const Vec4& color, bool receiveShadows) {
    BoxInstance instance;
    // Kolom glm ditulis apa adanya sebagai empat atribut. Shader menyusunnya
    // kembali dengan `mat4(...)`, yang juga kolom-mayor — jadi keduanya cocok
    // tanpa transpose di mana pun.
    instance.row0 = model[0];
    instance.row1 = model[1];
    instance.row2 = model[2];
    instance.row3 = model[3];
    instance.color = color;
    instance.flags = receiveShadows ? kInstanceReceiveShadows : 0u;
    return instance;
}

/// Renderer Vulkan E8.1.
///
/// **Yang digambar masih kotak, dan itu bukan penambal sementara yang malas.**
/// `ViewportScene::MeshInstance` belum membawa geometri sama sekali — hanya
/// transform, kotak batas, dan warna — karena importir mesh baru datang di E8.4.
/// Yang sedang dibuktikan di E8.1 bukan "mesh terlihat benar", melainkan bahwa
/// frame graph benar-benar menjalankan pass Vulkan dengan barrier yang
/// disimpulkannya sendiri, dengan depth prepass, transparansi tersortir, dan
/// reversed-Z. Kotak adalah geometri yang cukup untuk membuktikan semua itu, dan
/// begitu mesh sungguhan masuk, yang berganti hanya sumber vertex-nya.
class VulkanRenderer final : public IViewportRenderer {
public:
    VulkanRenderer(rhi::Device& device, rhi::ITextureRegistry& textures)
        : device_(device), textures_(textures) {}

    ~VulkanRenderer() override { Shutdown(); }

    bool Initialize(const StubRendererDesc& desc) {
        if (!device_.SupportsVulkan13()) {
            // Dynamic rendering dan synchronization2 keduanya inti 1.3. Jalur
            // mundurnya adalah render pass tradisional — dan itu jalur kedua
            // yang harus ikut diuji setiap kali pass berubah. Selama belum ada
            // yang menuntutnya, lebih jujur menolak daripada menyediakan jalur
            // yang tidak pernah dijalankan siapa pun.
            SIM_WARN("Render", "VulkanRenderer needs Vulkan 1.3; falling back to StubRenderer");
            return false;
        }
        if (!target_.Create(device_, desc.initialWidth, desc.initialHeight)) {
            return false;
        }
        shaderDirectory_ = desc.shaderDirectory;
        if (!CreateCube() || !CreateShadowMap() || !CreateShadowAtlas() ||
            !CreatePipelines(desc.shaderDirectory) ||
            !CreateOverlayPipelines(desc.shaderDirectory)) {
            return false;
        }
        for (InstanceSlot& slot : slots_) {
            if (!slot.buffer.Create(device_, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                    sizeof(BoxInstance) * 256) ||
                !slot.lineBuffer.Create(device_, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                        sizeof(LineVertex) * 1024) ||
                !slot.shadowUniform.Create(device_, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                           sizeof(ShadowUniforms)) ||
                !slot.lightBuffer.Create(device_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                         sizeof(GpuLight) * 64) ||
                !slot.clusterRangeBuffer.Create(device_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                sizeof(uint32_t) * 2 * 4096) ||
                !slot.clusterIndexBuffer.Create(device_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                sizeof(uint32_t) * 8192) ||
                !slot.shadowFaceBuffer.Create(device_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                              sizeof(GpuShadowFace) * 64)) {
                return false;
            }
        }
        // Profiler boleh gagal dibuat; renderer tetap jalan tanpa tabel waktu.
        profiler_.Create(device_);

        // 64³ dan bukan 128³ yang diminta rencana. Batasnya bukan memori
        // melainkan komposit CPU: medan jaraknya dievaluasi per voxel di sini,
        // dan 128³ berarti delapan kali pekerjaan itu. 128³ menunggu komposit
        // compute — dan biayanya sekarang terukur, jadi keputusan itu punya
        // angka untuk bersandar.
        SdfClipmapSettings sdf;
        sdf.resolution = 64;
        sdf.cascadeCount = 3;
        sdf.finestVoxelSize = 0.1f;
        if (!sdfClipmap_.Create(device_, sdf)) {
            SIM_WARN("Render", "SDF clipmap unavailable; GI tracing will have nothing to read");
        } else {
            // Dipesan sekali sebesar kasus terburuknya — seluruh isi setiap
            // kaskade, yaitu saat kamera melompat lebih jauh daripada lebar
            // clipmap-nya. Membiarkannya tumbuh sendiri berarti buffer dibuat
            // ulang pada frame lompatan itu, tepat pada frame yang paling
            // tidak punya waktu luang.
            for (InstanceSlot& slot : slots_) {
                if (!slot.sdfStaging.Create(device_, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                            sdfClipmap_.StagingBytes())) {
                    return false;
                }
            }
        }
        // Piramida depth dibuat sebelum descriptor ditulis, alasan yang sama
        // dengan clipmap: descriptor yang menunjuk tekstur pengganti tidak
        // menghasilkan galat apa pun, hanya penelusuran yang membaca peta
        // bayangan alih-alih depth buffer.
        if (hiz_.Create(device_, shaderDirectory_)) {
            hiz_.Adopt(target_.AllocatedWidth(), target_.AllocatedHeight(), target_.DepthView(),
                       target_.Sampler());
            hiz_.AdoptLayouts();
        }
        // Post-process dibuat sebelum pipeline adegan mana pun dipakai: seluruh
        // pass adegan sekarang menggambar ke gambar HDR miliknya, bukan ke target
        // yang disampel UI.
        if (post_.Create(device_, shaderDirectory_, target_.ColorFormat())) {
            post_.Adopt(target_.AllocatedWidth(), target_.AllocatedHeight());
            post_.AdoptLayouts();
        }
        if (sky_.Create(device_, shaderDirectory_, PostProcess::kSceneFormat)) {
            sky_.AdoptLayouts();
            sky_.AdoptDepth(target_.DepthView(), target_.Sampler());
            // Sesudah AdoptDepth, karena set descriptor awan menunjuk depth
            // buffer dan menulisnya sekali di sini lebih murah daripada
            // menuliskannya lagi tiap frame.
            sky_.CreateClouds(shaderDirectory_, PostProcess::kSceneFormat);
        }
        CreateRadianceCache();
        if (probes_.Create(device_, shaderDirectory_, shadowSetLayout_)) {
            probes_.Adopt(target_.AllocatedWidth(), target_.AllocatedHeight(), kNormalFormat);
            probes_.AdoptLayouts();
        }
        if (!WriteShadowDescriptors()) {
            return false;
        }
        AdoptTargetLayout();
        AdoptShadowLayout();
        AdoptAtlasLayout();
        RefreshTextureHandle();
        SIM_INFO("Render", "VulkanRenderer ready ({}x{}, frame graph, reversed-Z)", target_.Width(),
                 target_.Height());
        return true;
    }

    /// Memindahkan target warna ke layout yang dijanjikannya, sekali, saat ia
    /// baru dibuat.
    ///
    /// **Ditemukan oleh validation layer, bukan dengan membaca kode.** Graph
    /// menyatakan target warna dimulai dalam keadaan `Present` — yaitu
    /// SHADER_READ_ONLY, karena ImGui membacanya sebagai tekstur. Itu benar
    /// untuk setiap frame kecuali yang pertama: image yang baru dibuat berada di
    /// UNDEFINED, dan kalau ImGui sempat membacanya sebelum frame pertama
    /// selesai — mis. karena panel Viewport tertutup jendela lain dan
    /// `Render()` tidak pernah dipanggil — ia membaca image dalam layout yang
    /// tidak sah. Memindahkannya di sini membuat janji graph benar sejak detik
    /// pertama, alih-alih benar mulai frame kedua.
    void AdoptTargetLayout() {
        VkCommandBuffer cmd = device_.BeginOneShot();
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = target_.ColorImage();
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
        barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
        device_.EndOneShot(cmd);
    }

    void Resize(uint32_t width, uint32_t height) override {
        if (width == 0 || height == 0 || !target_.Resize(width, height)) {
            return;
        }
        // Resize membangun image baru, jadi janji layoutnya harus dibuat ulang.
        AdoptTargetLayout();
        RefreshTextureHandle();
        // Piramidanya menunjuk image depth yang baru saja dibuat ulang, jadi
        // descriptor yang menyebut yang lama harus ditulis ulang. Aman
        // dilakukan di sini: `RenderTarget::Resize` menunggu device idle sebelum
        // mengalokasi ulang, jadi tidak ada frame yang masih memakainya.
        const bool hizChanged = hiz_.Adopt(target_.AllocatedWidth(), target_.AllocatedHeight(),
                                           target_.DepthView(), target_.Sampler());
        const bool probesChanged =
            probes_.Adopt(target_.AllocatedWidth(), target_.AllocatedHeight(), kNormalFormat);
        const bool postChanged =
            post_.Adopt(target_.AllocatedWidth(), target_.AllocatedHeight());
        if (postChanged) {
            post_.AdoptLayouts();
        }
        // Warna target juga berpindah image saat dialokasi ulang, dan binding 12
        // menunjuknya — jadi descriptor GI ditulis ulang walaupun probe sendiri
        // tidak berubah ukuran.
        if (hizChanged) {
            hiz_.AdoptLayouts();
        }
        if (probesChanged) {
            probes_.AdoptLayouts();
        }
        if (hizChanged || probesChanged || postChanged) {
            UpdateGiDescriptors();
        }
        // Kabut membaca depth buffer yang sama, dan image-nya juga baru.
        sky_.AdoptDepth(target_.DepthView(), target_.Sampler());
    }

    void Render(const ViewportDesc& desc, const ViewportScene& scene) override {
        if (!target_.IsValid()) {
            return;
        }
        const float aspect =
            static_cast<float>(target_.Width()) / static_cast<float>(target_.Height());
        // Reversed-Z: near di 1, far di 0. Konsekuensinya depth di-clear ke 0 dan
        // ujinya GREATER — ketiganya harus berpindah bersama, dan memisahkannya
        // menghasilkan layar yang kosong tanpa satu pun pesan galat.
        const Mat4 projection =
            PerspectiveReversedZ(desc.camera.fovYRadians, aspect, desc.camera.nearZ,
                                 desc.camera.farZ);
        const Mat4 viewProj = projection * desc.camera.View();

        Gather(desc, scene, viewProj);

        // Cascade dihitung dari kamera yang sama dengan yang dipakai menggambar.
        // Menghitungnya dari kamera frame sebelumnya menghemat satu ketergantungan
        // dan menukarnya dengan bayangan yang tertinggal satu frame — terlihat
        // sebagai bayangan yang "berenang" saat kamera bergerak cepat.
        // Directional pertama dari scene menjadi matahari; `desc.sunDirection`
        // adalah nilai mundurnya. Cascade hanya ada satu himpunan, jadi
        // directional kedua dan seterusnya diabaikan — dan mengabaikannya
        // diam-diam lebih baik daripada menjumlahkan arah, yang menghasilkan
        // bayangan yang tidak cocok dengan lampu mana pun.
        // Backend GI diselesaikan tiap frame dari kemampuan perangkat dan
        // permintaan pengguna. Murah, dan menyimpannya berarti pertanyaan
        // "yang mana yang berlaku sekarang" setiap kali preferensinya berubah.
        TraceBackendCaps caps;
        caps.rayQuery = device_.SupportsRayQuery();
        giBackend_ = SelectTraceBackend(caps, desc.gi.backend);

        sunDirection_ = desc.sunDirection;
        sunRadiance_ = desc.sunRadiance;
        sunCastsShadows_ = desc.castShadows;
        for (const LightInstance& light : scene.lights) {
            if (light.kind == LightKind::Directional) {
                sunDirection_ = light.direction;
                // Warna dikali intensitas — keduanya sampai ke shader sekarang.
                // Sebelumnya hanya arahnya yang dipakai, jadi menyunting warna
                // matahari di Inspector tidak mengubah apa pun: antarmuka yang
                // berbohong, dan itu lebih buruk daripada tombol yang belum ada.
                sunRadiance_ = light.color * light.intensity;
                sunCastsShadows_ = desc.castShadows && light.castShadows;
                break;
            }
        }
        CascadeSettings cascadeSettings;
        cascadeSettings.resolution = kShadowResolution;
        cascades_ = ComputeCascades(desc.camera, aspect, sunDirection_, cascadeSettings);

        InstanceSlot& slot = slots_[slotIndex_];
        device_.WaitTransient(slot.submitId);
        slot.submitId = 0;
        const std::size_t instanceCount = opaque_.size() + transparent_.size();
        bool slotReady = instanceCount == 0;
        if (instanceCount > 0) {
            upload_.clear();
            upload_.insert(upload_.end(), opaque_.begin(), opaque_.end());
            upload_.insert(upload_.end(), transparent_.begin(), transparent_.end());
            slotReady = slot.buffer.Reserve(sizeof(BoxInstance) * upload_.size()) &&
                        slot.buffer.Write(upload_.data(), sizeof(BoxInstance) * upload_.size());
        }

        lineVertices_.clear();
        for (const LineSegment& line : scene.lines) {
            lineVertices_.push_back({line.a, line.color});
            lineVertices_.push_back({line.b, line.color});
        }
        bool linesReady = false;
        if (!lineVertices_.empty()) {
            linesReady =
                slot.lineBuffer.Reserve(sizeof(LineVertex) * lineVertices_.size()) &&
                slot.lineBuffer.Write(lineVertices_.data(),
                                      sizeof(LineVertex) * lineVertices_.size());
        }

        // Clipmap SDF diperbarui hanya saat GI menyala. Membangunnya terus-
        // menerus untuk fitur yang dimatikan adalah biaya yang tidak ada yang
        // memintanya — dan biaya itu, di komposit CPU, bukan biaya yang kecil.
        giEnabled_ = desc.gi.enabled;
        // Piramida dibangun hanya kalau ada yang membacanya. Membangunnya saat
        // lapis screen-space dimatikan adalah 0,1 ms untuk tekstur yang tidak
        // dibaca satu sinar pun — dan mematikan lapis itu justru dilakukan untuk
        // mengukur harganya, jadi biaya yang tertinggal akan mengaburkan
        // pengukuran itu sendiri.
        screenTraceEnabled_ = desc.gi.enabled && desc.gi.screenTrace;

        // **Riwayat tidak lagi dibuang saat kamera bergerak.** Sampai M4 ia
        // dibuang seluruhnya, karena riwayatnya terikat ke piksel dan piksel
        // yang sama menunjuk permukaan yang berbeda. Reproyeksi mengikatnya ke
        // dunia; validasi bidang dan normal yang menolaknya kalau titik yang
        // ditemukan ternyata milik permukaan lain. Yang tersisa hanya satu
        // keadaan yang benar-benar menuntut pembuangan: frame pertama, saat
        // isinya belum pernah ditulis sama sekali.
        probeGrid_.Configure(target_.Width(), target_.Height(), ProbeGridSettings{});
        // **Diambil sebelum ditimpa.** Bentuk pertama saya menyalinnya sesudah
        // `lastViewProj_` diperbarui, jadi yang dikirim ke shader adalah matriks
        // frame ini — dan reproyeksi menjadi pemetaan identitas yang tidak
        // pernah salah dan tidak pernah berguna.
        previousViewProj_ = probeFrame_ == 0 ? viewProj : lastViewProj_;
        lastViewProj_ = viewProj;
        probeReset_ = probeFrame_ == 0;
        ++probeFrame_;
        ++cacheFrame_;
        sdfDebugEnabled_ = desc.gi.enabled && sdfClipmap_.IsValid() &&
                           desc.gi.debugView != GiDebugView::Off;
        // Langkah waktu diukur di sini, bukan diterima dari pemanggil.
        // `IViewportRenderer::Render` tidak punya parameter waktu, dan
        // menambahkannya berarti setiap pemanggil harus tahu bahwa eksposur
        // beradaptasi. Dijepit ke 0,25 detik: satu hentakan panjang — memuat
        // level, membangun pipeline — akan menjadi satu langkah adaptasi raksasa,
        // dan yang terlihat adalah kilatan terang tepat sesudah level terbuka.
        {
            const auto now = std::chrono::steady_clock::now();
            deltaSeconds_ =
                hasLastFrameTime_
                    ? std::min(std::chrono::duration<float>(now - lastFrameTime_).count(), 0.25f)
                    : 0.0f;
            lastFrameTime_ = now;
            hasLastFrameTime_ = true;
            cloudTimeSeconds_ += deltaSeconds_;
        }
        sdfVoxelsWritten_ = 0;
        sdfUpdateMs_ = 0.0f;
        if (desc.gi.enabled && sdfClipmap_.IsValid()) {
            const auto started = std::chrono::steady_clock::now();
            sdfVoxelsWritten_ =
                sdfClipmap_.Update(desc.camera.position, scene.meshes, slot.sdfStaging);
            sdfUpdateMs_ = std::chrono::duration<float, std::milli>(
                               std::chrono::steady_clock::now() - started)
                               .count();
        }

        // Peta HDR dimuat sebelum graph dibangun, karena graph memutuskan ada
        // atau tidaknya pass langit dari berhasil-tidaknya pemuatan ini.
        // `SetHdri` sendiri tidak melakukan apa-apa bila jalurnya tidak berubah.
        if (desc.skySource == SkySource::HdrMap) {
            sky_.SetHdri(std::filesystem::path(desc.hdriPath));
        }

        UpdateClusters(desc, scene, aspect, slot);
        UpdateShadowUniforms(desc, viewProj, slot);
        BuildGraph(desc);

        VkCommandBuffer cmd = device_.BeginTransient();
        profiler_.BeginFrame(cmd);
        // Sebelum pass mana pun, supaya kaskade yang dibaca `gi-sdf-debug` dan
        // nanti pass GI adalah kaskade posisi kamera frame ini — bukan posisi
        // frame sebelumnya.
        sdfClipmap_.RecordUploads(cmd);
        executor_.Clear();
        executor_.Bind(colorId_, BoundImage{target_.ColorImage(), target_.ColorView(),
                                            VK_IMAGE_ASPECT_COLOR_BIT});
        executor_.Bind(sceneId_, BoundImage{post_.SceneImage(), post_.SceneView(),
                                            VK_IMAGE_ASPECT_COLOR_BIT});
        executor_.Bind(depthId_, BoundImage{target_.DepthImage(), target_.DepthView(),
                                            VK_IMAGE_ASPECT_DEPTH_BIT});
        executor_.Bind(shadowId_, BoundImage{shadow_.image, shadow_.arrayView,
                                             VK_IMAGE_ASPECT_DEPTH_BIT});
        executor_.Bind(atlasId_, BoundImage{atlas_.image, atlas_.view,
                                            VK_IMAGE_ASPECT_DEPTH_BIT});

        const auto opaqueCount = static_cast<uint32_t>(opaque_.size());
        const auto transparentCount = static_cast<uint32_t>(transparent_.size());
        const BoxPush push{viewProj};

        const Mat4 invViewProj = glm::inverse(viewProj);
        // Ukurannya diambil dari graph, bukan dihitung tangan. Bentuk pertama
        // saya memakai larik tetap delapan, dan pass kesembilan — `hiz-build` —
        // menulis di luarnya: **stack corruption, bukan galat.** Angka yang
        // harus diperbarui setiap kali sebuah pass ditambahkan adalah angka yang
        // suatu saat lupa diperbarui.
        recorders_.assign(static_cast<std::size_t>(graph_.PassCount()), {});
        std::span<FrameGraphExecutor::Recorder> recorders(recorders_);
        recorders[shadowPassId_] = [&](VkCommandBuffer command) {
            RecordShadowPass(command, slot, casterCount_);
        };
        recorders[atlasPassId_] = [&](VkCommandBuffer command) {
            RecordAtlasPass(command, slot, casterCount_);
        };
        if (skyId_ != kInvalidPass) {
            recorders[skyId_] = [&](VkCommandBuffer command) {
                if (desc.skySource == SkySource::HdrMap) {
                    // DONT_CARE, alasan yang sama dengan langit atmosferik:
                    // petanya menutupi setiap piksel.
                    BeginRendering(command, desc, /*clearColor=*/false, /*loadDepth=*/false,
                                   /*writeColor=*/true, /*useDepth=*/false);
                    sky_.RecordHdriDraw(command, invViewProj, desc.hdriIntensity,
                                        desc.hdriRotation);
                    vkCmdEndRendering(command);
                    return;
                }
                sky_.RecordLuts(command, sunDirection_, desc.cameraHeightKm);
                // DONT_CARE, bukan CLEAR: langit menutupi setiap piksel, jadi
                // membersihkannya lebih dulu berarti menulis seluruh gambar dua
                // kali untuk hasil yang sama.
                BeginRendering(command, desc, /*clearColor=*/false, /*loadDepth=*/false,
                               /*writeColor=*/true, /*useDepth=*/false);
                sky_.RecordDraw(command, invViewProj, desc.camera.position,
                                desc.cameraHeightKm, sunDirection_, sunRadiance_,
                                desc.skyIntensity);
                vkCmdEndRendering(command);
            };
        }
        recorders[gridId_] = [&](VkCommandBuffer command) {
            // Grid membersihkan warna dan menjadi latar. Ia tidak menyentuh depth
            // sama sekali, jadi apa pun yang digambar sesudahnya menutupinya tanpa
            // uji apa pun.
            BeginRendering(command, desc, /*clearColor=*/skyId_ == kInvalidPass,
                           /*loadDepth=*/false, /*writeColor=*/true, /*useDepth=*/false);
            if (desc.showGrid && gridPipeline_ != VK_NULL_HANDLE) {
                GridPush push{};
                push.invViewProj = invViewProj;
                push.cameraPos = Vec4(desc.camera.position, 1.0f);
                // w = 1: reversed-Z, bidang dekat ada di depth 1.
                push.params = Vec4(desc.gridCellSize, desc.gridFadeDistance, 1.0f, 1.0f);
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, gridPipeline_);
                vkCmdPushConstants(command, gridLayout_,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(GridPush), &push);
                vkCmdDraw(command, 3, 1, 0, 0);
            }
            vkCmdEndRendering(command);
        };
        recorders[prepassId_] = [&](VkCommandBuffer command) {
            probes_.RecordNormalBegin(command);
            BeginPrepassRendering(command, desc);
            if (slotReady && opaqueCount > 0) {
                DrawInstances(command, prepassPipeline_, push, slot, 0, opaqueCount);
            }
            vkCmdEndRendering(command);
            probes_.RecordNormalEnd(command);
        };
        recorders[opaqueId_] = [&](VkCommandBuffer command) {
            BeginRendering(command, desc, /*clearColor=*/false, /*loadDepth=*/true,
                           /*writeColor=*/true);
            if (slotReady && opaqueCount > 0) {
                DrawInstances(command, opaquePipeline_, push, slot, 0, opaqueCount);
            }
            vkCmdEndRendering(command);
        };
        recorders[transparentId_] = [&](VkCommandBuffer command) {
            BeginRendering(command, desc, /*clearColor=*/false, /*loadDepth=*/true,
                           /*writeColor=*/true);
            if (slotReady && transparentCount > 0) {
                DrawInstances(command, transparentPipeline_, push, slot, opaqueCount,
                              transparentCount);
            }
            vkCmdEndRendering(command);
        };
        if (hizPassId_ != kInvalidPass) {
            recorders[hizPassId_] = [&](VkCommandBuffer command) {
                hiz_.Record(command, target_.Width(), target_.Height());
            };
        }
        if (probePassId_ != kInvalidPass) {
            recorders[probePassId_] = [&](VkCommandBuffer command) {
                probes_.Record(command, slot.shadowSet, probeGrid_, probeFrame_,
                               sdfClipmap_.Volume().Clipmap().MaxRange(), probeReset_);
            };
        }
        if (giDebugId_ != kInvalidPass) {
            recorders[giDebugId_] = [&](VkCommandBuffer command) {
                BeginRendering(command, desc, /*clearColor=*/false, /*loadDepth=*/false,
                               /*writeColor=*/true, /*useDepth=*/false);
                if (sdfDebugPipeline_ != VK_NULL_HANDLE) {
                    SdfDebugPush push;
                    push.invViewProj = invViewProj;
                    // w = 1: reversed-Z, bidang dekat ada di depth 1.
                    push.cameraPosition = Vec4(desc.camera.position, 1.0f);
                    push.params = Vec4(
                        static_cast<float>(desc.gi.debugView),
                        sdfClipmap_.Volume().Clipmap().MaxRange(),
                        static_cast<float>(probeGrid_.Settings().tileSize), 0.0f);
                    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      sdfDebugPipeline_);
                    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            sdfDebugLayout_, 0, 1, &slot.shadowSet, 0, nullptr);
                    vkCmdPushConstants(command, sdfDebugLayout_,
                                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                       0, sizeof(SdfDebugPush), &push);
                    vkCmdDraw(command, 3, 1, 0, 0);
                }
                vkCmdEndRendering(command);
            };
        }
        recorders[linesId_] = [&](VkCommandBuffer command) {
            BeginRendering(command, desc, /*clearColor=*/false, /*loadDepth=*/true,
                           /*writeColor=*/true);
            if (linesReady && linePipeline_ != VK_NULL_HANDLE) {
                LinePush linePush{viewProj};
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, linePipeline_);
                vkCmdPushConstants(command, lineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                                   sizeof(LinePush), &linePush);
                const VkDeviceSize offset = 0;
                const VkBuffer handle = slot.lineBuffer.Handle();
                vkCmdBindVertexBuffers(command, 0, 1, &handle, &offset);
                vkCmdDraw(command, static_cast<uint32_t>(lineVertices_.size()), 1, 0, 0);
            }
            vkCmdEndRendering(command);
        };

        if (cloudId_ != kInvalidPass) {
            recorders[cloudId_] = [&](VkCommandBuffer command) {
                BeginRendering(command, desc, /*clearColor=*/false, /*loadDepth=*/false,
                               /*writeColor=*/true, /*useDepth=*/false);
                sky_.RecordClouds(command, invViewProj, desc.camera.position,
                                  desc.cameraHeightKm, sunDirection_, sunRadiance_,
                                  desc.skyIntensity, desc.clouds, cloudTimeSeconds_);
                vkCmdEndRendering(command);
            };
        }

        if (aerialId_ != kInvalidPass) {
            // Jangkauan LUT mengikuti bidang jauh kamera. **Bukan 4 km per slice
            // seperti acuannya:** angka itu masuk akal untuk adegan seluas
            // planet, dan untuk adegan sepanjang dua kilometer ia menaruh seluruh
            // yang terlihat di dalam slice pertama — kabut lalu menjadi satu
            // nilai tetap yang tidak berubah terhadap jarak sama sekali, yang
            // tampak seperti kabut yang "tidak bekerja" alih-alih seperti LUT
            // yang salah skala.
            const float aerialRangeKm = std::max(desc.camera.farZ * 0.001f, 0.001f);
            recorders[aerialId_] = [&, aerialRangeKm](VkCommandBuffer command) {
                sky_.RecordAerialLut(command, invViewProj, desc.cameraHeightKm, sunDirection_,
                                     aerialRangeKm, desc.aerialHaze);
                BeginRendering(command, desc, /*clearColor=*/false, /*loadDepth=*/false,
                               /*writeColor=*/true, /*useDepth=*/false);
                sky_.RecordAerialApply(command, invViewProj, desc.camera.position, aerialRangeKm,
                                       desc.skyIntensity);
                vkCmdEndRendering(command);
            };
        }

        if (bloomId_ != kInvalidPass) {
            recorders[bloomId_] = [&](VkCommandBuffer command) {
                if (desc.post.enabled && desc.post.bloom.enabled) {
                    post_.RecordBloom(command, target_.Width(), target_.Height(),
                                      desc.post.bloom);
                }
            };
        }
        if (meterId_ != kInvalidPass) {
            recorders[meterId_] = [&](VkCommandBuffer command) {
                post_.RecordMeter(command, target_.Width(), target_.Height(), desc.post,
                                  deltaSeconds_);
            };
        }
        if (tonemapId_ != kInvalidPass) {
            recorders[tonemapId_] = [&](VkCommandBuffer command) {
                BeginDisplayRendering(command);
                post_.RecordResolve(command, desc.post.enabled, desc.post.bloom);
                vkCmdEndRendering(command);
            };
        }

        if (!executor_.Execute(compiled_, cmd, recorders, &profiler_)) {
            SIM_ERROR("Render", "frame graph execution failed: {}", compiled_.error);
        }
        profiler_.EndFrame(cmd);
        slot.submitId = device_.SubmitTransient(cmd);
        slotIndex_ = (slotIndex_ + 1) % slots_.size();
        drawnOpaque_ = opaqueCount;
        drawnTransparent_ = transparentCount;
    }

    TextureHandle ColorTarget() const override { return textureHandle_; }
    Vec2 ColorTargetUvMax() const override { return {target_.UvMaxU(), target_.UvMaxV()}; }
    uint32_t Width() const override { return target_.Width(); }
    uint32_t Height() const override { return target_.Height(); }
    const char* Name() const override { return "VulkanRenderer"; }

    TraceBackendSelection GiBackend() const override { return giBackend_; }
    float SdfUpdateMilliseconds() const override { return sdfUpdateMs_; }
    uint64_t SdfVoxelsWritten() const override { return sdfVoxelsWritten_; }

    std::span<const PassTiming> PassTimings() const override {
        timings_.clear();
        for (const rhi::GpuProfiler::Scope& scope : profiler_.Results()) {
            timings_.push_back(PassTiming{scope.name, static_cast<float>(scope.milliseconds)});
        }
        return timings_;
    }

private:
    struct InstanceSlot {
        rhi::DynamicBuffer buffer;
        rhi::DynamicBuffer lineBuffer;
        // Satu uniform buffer dan satu descriptor set per slot. Berbagi satu
        // set untuk seluruh frame in-flight berarti menulis ulang isinya
        // sementara frame sebelumnya masih membacanya — kerusakan yang muncul
        // sebagai bayangan yang sesekali melompat satu frame, bukan sebagai
        // pesan galat.
        rhi::DynamicBuffer shadowUniform;
        rhi::DynamicBuffer lightBuffer;
        rhi::DynamicBuffer clusterRangeBuffer;
        rhi::DynamicBuffer clusterIndexBuffer;
        rhi::DynamicBuffer shadowFaceBuffer;
        // Staging clipmap SDF. Per slot karena isinya masih dibaca GPU sampai
        // submit slot ini selesai; satu buffer bersama akan ditimpa frame
        // berikutnya di tengah salinan frame ini.
        rhi::DynamicBuffer sdfStaging;
        VkDescriptorSet shadowSet = VK_NULL_HANDLE;
        uint64_t submitId = 0;
    };

    /// Menyusun graph frame ini.
    ///
    /// Dibangun ulang tiap frame walaupun bentuknya tetap. Menyimpannya akan
    /// menghemat beberapa mikrodetik dan menukar itu dengan pertanyaan "graph
    /// mana yang berlaku sekarang" setiap kali sebuah pass menjadi bersyarat —
    /// dan pass bersyarat justru alasan graph ini ada.
    void BuildGraph(const ViewportDesc& desc) {
        graph_.Clear();
        colorId_ = graph_.Import("viewport-color", Access::Present);
        // Gambar HDR yang ditulis seluruh pass adegan. ShaderRead sebagai
        // keadaan awal: frame ini menimpanya sementara pass tone mapping frame
        // sebelumnya masih membacanya.
        sceneId_ = graph_.Import("scene-color", Access::ShaderRead);
        depthId_ = graph_.Import("viewport-depth", Access::None);
        // ShaderRead, bukan None: frame ini menulis ulang peta yang masih
        // dibaca fragment shader frame sebelumnya, dan `None` berarti "tidak
        // ada yang perlu ditunggu".
        shadowId_ = graph_.Import("shadow-cascades", Access::ShaderRead);
        atlasId_ = graph_.Import("shadow-atlas", Access::ShaderRead);

        shadowPassId_ = graph_.AddPass("shadow-cascades");
        graph_.Write(shadowPassId_, shadowId_, Access::DepthWrite);

        atlasPassId_ = graph_.AddPass("shadow-atlas");
        graph_.Write(atlasPassId_, atlasId_, Access::DepthWrite);

        // Langit lebih dulu, dan ia yang mengisi setiap piksel. Grid lalu tidak
        // perlu membersihkan warna — dua clear pada gambar yang sama dalam satu
        // frame adalah pekerjaan yang salah satunya pasti terbuang.
        skyId_ = kInvalidPass;
        const bool hdriSky = desc.skySource == SkySource::HdrMap && sky_.HdriIsValid();
        if (desc.skyEnabled && sky_.IsValid() &&
            (desc.skySource == SkySource::Atmosphere || hdriSky)) {
            skyId_ = graph_.AddPass("sky");
            graph_.Write(skyId_, sceneId_, Access::ColorWrite);
            // LUT-nya diurus `SkyAtmosphere` sendiri, bukan dilacak graph.
            graph_.SetSideEffect(skyId_);
        }

        gridId_ = graph_.AddPass("grid");
        graph_.Write(gridId_, sceneId_, Access::ColorWrite);

        prepassId_ = graph_.AddPass("depth-prepass");
        graph_.Write(prepassId_, depthId_, Access::DepthWrite);

        // Piramida dibangun dari depth prepass, bukan dari depth akhir. Yang
        // ditelusuri lapis screen-space adalah permukaan buram; yang tembus
        // pandang tidak menghalangi cahaya dan tidak menulis depth yang berarti
        // untuknya. Pass ini juga bersyarat: tanpa GI tidak ada yang membacanya.
        hizPassId_ = kInvalidPass;
        if (screenTraceEnabled_ && hiz_.IsValid()) {
            hizPassId_ = graph_.AddPass("hiz-build");
            graph_.Read(hizPassId_, depthId_, Access::ShaderRead);
            // **Efek samping, karena keluarannya bukan resource graph.** Piramida
            // mengurus perpindahan layout tiap mip-nya sendiri — graph melacak
            // resource sebagai satu kesatuan, sementara pembangunan piramida
            // membaca satu mip sambil menulis mip berikutnya. Tanpa penanda ini
            // graph menyimpulkan pass ini tidak menghasilkan apa pun dan
            // membuangnya: penelusuran lalu membaca piramida frame sebelumnya,
            // tanpa satu pun galat.
            graph_.SetSideEffect(hizPassId_);
        }

        opaqueId_ = graph_.AddPass("forward-opaque");
        graph_.Read(opaqueId_, depthId_, Access::DepthWrite);
        graph_.Read(opaqueId_, shadowId_, Access::ShaderRead);
        graph_.Read(opaqueId_, atlasId_, Access::ShaderRead);
        graph_.Write(opaqueId_, sceneId_, Access::ColorWrite);

        transparentId_ = graph_.AddPass("forward-transparent");
        graph_.Read(transparentId_, depthId_, Access::DepthWrite);
        graph_.Read(transparentId_, shadowId_, Access::ShaderRead);
        graph_.Read(transparentId_, atlasId_, Access::ShaderRead);
        graph_.Write(transparentId_, sceneId_, Access::ColorWrite);

        linesId_ = graph_.AddPass("lines");
        graph_.Read(linesId_, depthId_, Access::DepthWrite);
        graph_.Write(linesId_, sceneId_, Access::ColorWrite);

        // Pass probe berjalan **sesudah** forward-opaque, dan itu bukan urutan
        // yang bebas dipilih: satu-satunya sumber radiansi yang dimiliki M3
        // adalah buffer warna yang sudah dinaungi dan disinari pass itu. Sinar
        // yang mengenai lewat lapis layar membacanya; yang mengenai lewat SDF
        // belum punya warna sampai hash grid M4.
        probePassId_ = kInvalidPass;
        if (giEnabled_ && probes_.IsValid() && hiz_.IsValid()) {
            probePassId_ = graph_.AddPass("gi-probe-trace");
            graph_.Read(probePassId_, sceneId_, Access::ShaderRead);
            graph_.Read(probePassId_, depthId_, Access::ShaderRead);
            // Efek samping: keluarannya tekstur SH yang layout-nya diurus
            // `ProbeField` sendiri, bukan resource yang dilacak graph.
            graph_.SetSideEffect(probePassId_);
        }

        // Pass bersyarat: hanya ada saat debug view menyala. Graph yang
        // dibangun ulang tiap frame membuat ini sekadar sebuah `if` — dan pass
        // bersyarat justru alasan graph ini ada.
        giDebugId_ = kInvalidPass;
        if (sdfDebugEnabled_) {
            giDebugId_ = graph_.AddPass("gi-sdf-debug");
            graph_.Write(giDebugId_, sceneId_, Access::ColorWrite);
        }

        // Awan volumetrik, sesudah seluruh geometri: raymarch-nya berhenti di
        // permukaan terdekat, dan permukaan itu baru diketahui setelah depth
        // selesai ditulis. Sebelum kabut, karena awan berada di dalam atmosfer
        // yang sama — udara di antara kamera dan awan adalah udara yang sama
        // yang membiru di kejauhan.
        cloudId_ = kInvalidPass;
        if (desc.skyEnabled && desc.skySource == SkySource::Atmosphere &&
            desc.clouds.enabled && sky_.CloudsAreValid()) {
            cloudId_ = graph_.AddPass("clouds");
            graph_.Read(cloudId_, depthId_, Access::ShaderRead);
            graph_.Write(cloudId_, sceneId_, Access::ColorWrite);
        }

        // Kabut atmosferik, sesudah seluruh geometri dan sebelum bloom.
        //
        // **Urutannya terhadap bloom bukan selera.** Kabut adalah bagian dari
        // adegan: sorotan terang yang berada di balik lima kilometer udara sudah
        // diredam sebelum ia berpendar. Bloom yang berjalan lebih dulu akan
        // memendarkan kecerahan yang sesungguhnya tidak pernah sampai ke kamera,
        // dan yang terlihat adalah pendaran yang menembus kabut.
        aerialId_ = kInvalidPass;
        if (desc.skyEnabled && desc.skySource == SkySource::Atmosphere &&
            desc.aerialPerspective && sky_.AerialIsValid()) {
            aerialId_ = graph_.AddPass("aerial");
            graph_.Read(aerialId_, depthId_, Access::ShaderRead);
            graph_.Write(aerialId_, sceneId_, Access::ColorWrite);
            // LUT 3D-nya diurus `SkyAtmosphere` sendiri, bukan dilacak graph.
            graph_.SetSideEffect(aerialId_);
        }

        // Pengukuran luminansi dan eksposur. Sesudah seluruh pass adegan, karena
        // yang diukur adalah adegan yang sudah jadi — mengukurnya lebih awal
        // berarti mengukur gambar yang setengah tergambar, dan yang terlihat
        // adalah eksposur yang berkedip mengikuti jumlah objek di layar.
        meterId_ = kInvalidPass;
        tonemapId_ = kInvalidPass;
        bloomId_ = kInvalidPass;
        if (post_.IsValid()) {
            bloomId_ = graph_.AddPass("bloom");
            graph_.Read(bloomId_, sceneId_, Access::ShaderRead);
            // Efek samping, alasan yang sama dengan piramida depth: rantai
            // mip-nya diurus `PostProcess` sendiri, bukan dilacak graph.
            graph_.SetSideEffect(bloomId_);

            meterId_ = graph_.AddPass("post-meter");
            graph_.Read(meterId_, sceneId_, Access::ShaderRead);
            // Efek samping: keluarannya rantai luminansi dan texel eksposur, yang
            // layout-nya diurus `PostProcess` sendiri. Tanpa penanda ini graph
            // menyimpulkan pass ini tidak menghasilkan apa pun dan membuangnya —
            // eksposur lalu membeku pada nilai frame pertama, tanpa satu pun galat.
            graph_.SetSideEffect(meterId_);

            tonemapId_ = graph_.AddPass("tonemap");
            graph_.Read(tonemapId_, sceneId_, Access::ShaderRead);
            graph_.Write(tonemapId_, colorId_, Access::ColorWrite);
        }

        graph_.SetOutput(colorId_, Access::Present);
        graph_.SetOutput(sceneId_, Access::ShaderRead);
        // Peta bayangan harus kembali ke keadaan awalnya, karena itulah keadaan
        // yang diandaikan impor frame berikutnya.
        graph_.SetOutput(shadowId_, Access::ShaderRead);
        graph_.SetOutput(atlasId_, Access::ShaderRead);
        compiled_ = graph_.Compile();
    }

    /// Menyaring instance yang terlihat, memisahkan tembus pandang, lalu
    /// mengurutkannya dari belakang ke depan.
    void Gather(const ViewportDesc& desc, const ViewportScene& scene, const Mat4& viewProj) {
        opaque_.clear();
        transparent_.clear();
        casterCount_ = 0;
        const Frustum frustum(viewProj);
        const Vec3 eye = desc.camera.position;

        sorted_.clear();

        for (const MeshInstance& mesh : scene.meshes) {
            const Aabb local{mesh.boundsMin, mesh.boundsMax};
            const Aabb world = TransformAabb(local, mesh.transform);
            if (!frustum.Intersects(world)) {
                continue;
            }
            // Kotak batas dipetakan ke kubus satuan: geser ke pusatnya lalu skala
            // ke ukurannya. Begitu mesh sungguhan masuk di E8.4, langkah ini
            // hilang — transform instance-nya dipakai apa adanya.
            const Vec3 centre = (mesh.boundsMin + mesh.boundsMax) * 0.5f;
            const Vec3 size = glm::max(mesh.boundsMax - mesh.boundsMin, Vec3(1e-4f));
            Mat4 model = mesh.transform;
            model = glm::translate(model, centre);
            model = glm::scale(model, size);

            const Vec4 color = mesh.selected ? kSelectedColor : mesh.color;
            if (color.a >= 0.999f) {
                // **Yang menjatuhkan bayangan diletakkan lebih dulu.** Pass
                // bayangan lalu tinggal menggambar awalan daftarnya, tanpa
                // atribut tambahan dan tanpa cabang di shader. Trik yang sama
                // dengan pemisahan buram/tembus pandang di bawah, dan urutan di
                // antara sesama buram memang tidak berarti apa-apa — semuanya
                // diuji depth.
                if (mesh.castShadows) {
                    opaque_.insert(opaque_.begin() + static_cast<std::ptrdiff_t>(casterCount_),
                                   MakeInstance(model, color, mesh.receiveShadows));
                    ++casterCount_;
                } else {
                    opaque_.push_back(MakeInstance(model, color, mesh.receiveShadows));
                }
                continue;
            }
            const float distance = glm::length(world.Centre() - eye);
            // Tembus pandang tidak pernah menjatuhkan bayangan — kaca yang
            // menghitamkan lantai di bawahnya adalah kesalahan yang lebih
            // mencolok daripada kaca yang tidak berbayang sama sekali.
            sorted_.push_back(
                SortedEntry{distance, MakeInstance(model, color, mesh.receiveShadows)});
        }

        // Belakang ke depan. Alpha blending tidak komutatif: dua kaca yang
        // dicampur dengan urutan terbalik menghasilkan warna yang berbeda, dan
        // "berbeda" di sini berarti kaca yang lebih jauh terlihat di depan yang
        // lebih dekat.
        std::sort(sorted_.begin(), sorted_.end(),
                  [](const SortedEntry& a, const SortedEntry& b) { return a.distance > b.distance; });
        transparent_.reserve(sorted_.size());
        for (const SortedEntry& entry : sorted_) {
            transparent_.push_back(entry.instance);
        }
    }

    /// `useDepth` false berarti depth tidak dipasang sama sekali.
    ///
    /// Bukan sekadar penghematan: pass yang memasang depth tapi tidak
    /// menyatakannya di graph adalah pass yang barrier-nya tidak pernah
    /// disusun — dan lampiran depth selalu menyentuh image itu, walaupun
    /// shadernya tidak. Apa yang dipasang di sini harus sama persis dengan apa
    /// yang dideklarasikan `BuildGraph`.
    void BeginRendering(VkCommandBuffer cmd, const ViewportDesc& desc, bool clearColor,
                        bool loadDepth, bool writeColor, bool useDepth = true) {
        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        // **Gambar HDR, bukan target yang disampel UI.** Seluruh pass adegan
        // menulis radiance apa adanya ke sini; yang memetakannya ke layar adalah
        // pass `tonemap` di ujung graph.
        color.imageView = post_.SceneView();
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {
            {desc.clearColor.x, desc.clearColor.y, desc.clearColor.z, desc.clearColor.w}};

        VkRenderingAttachmentInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth.imageView = target_.DepthView();
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = loadDepth ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        // Pass grid tidak memakai depth sama sekali; membiarkannya meng-clear di
        // sana berarti prepass menemukan depth yang sudah bersih dan tetap benar,
        // tapi juga berarti dua clear pada image yang sama dalam satu frame.
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        // Nol, bukan satu: pada reversed-Z yang terjauh adalah nol.
        depth.clearValue.depthStencil = {0.0f, 0};

        VkRenderingInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        info.renderArea.extent = {target_.Width(), target_.Height()};
        info.layerCount = 1;
        info.colorAttachmentCount = writeColor || clearColor ? 1u : 0u;
        info.pColorAttachments = writeColor || clearColor ? &color : nullptr;
        info.pDepthAttachment = useDepth ? &depth : nullptr;
        vkCmdBeginRendering(cmd, &info);

        const VkViewport viewport{0.0f,
                                  0.0f,
                                  static_cast<float>(target_.Width()),
                                  static_cast<float>(target_.Height()),
                                  0.0f,
                                  1.0f};
        const VkRect2D scissor{{0, 0}, {target_.Width(), target_.Height()}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
    }

    /// Lampiran untuk pass tone mapping: target 8-bit yang disampel UI.
    ///
    /// Terpisah dari `BeginRendering` karena gambarnya berbeda dan formatnya
    /// berbeda — dan pipeline yang formatnya tidak cocok dengan lampiran yang
    /// dipasang adalah ketidakcocokan yang hanya dilaporkan validation layer.
    void BeginDisplayRendering(VkCommandBuffer cmd) {
        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = target_.ColorView();
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        info.renderArea.extent = {target_.Width(), target_.Height()};
        info.layerCount = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &color;
        vkCmdBeginRendering(cmd, &info);

        const VkViewport viewport{0.0f,
                                  0.0f,
                                  static_cast<float>(target_.Width()),
                                  static_cast<float>(target_.Height()),
                                  0.0f,
                                  1.0f};
        const VkRect2D scissor{{0, 0}, {target_.Width(), target_.Height()}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
    }

    /// Depth prepass: depth target plus lampiran normal G-buffer.
    ///
    /// Lampirannya bukan warna viewport, jadi ia tidak bisa memakai
    /// `BeginRendering` yang dipakai pass lain — formatnya berbeda, dan
    /// pipeline yang formatnya tidak cocok dengan lampiran yang dipasang adalah
    /// ketidakcocokan yang hanya dilaporkan validation layer.
    void BeginPrepassRendering(VkCommandBuffer cmd, const ViewportDesc& desc) {
        VkRenderingAttachmentInfo normal{};
        normal.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        normal.imageView = probes_.NormalView();
        normal.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        // CLEAR: piksel yang tidak tertutupi geometri apa pun harus punya normal
        // yang jelas-jelas bukan normal — nol panjang, yang ditolak pemakainya —
        // bukan sisa frame sebelumnya.
        normal.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        normal.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        normal.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

        VkRenderingAttachmentInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth.imageView = target_.DepthView();
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        // Nol, bukan satu: pada reversed-Z yang terjauh adalah nol.
        depth.clearValue.depthStencil = {0.0f, 0};

        VkRenderingInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        info.renderArea.extent = {target_.Width(), target_.Height()};
        info.layerCount = 1;
        info.colorAttachmentCount = probes_.IsValid() ? 1u : 0u;
        info.pColorAttachments = probes_.IsValid() ? &normal : nullptr;
        info.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &info);

        const VkViewport viewport{0.0f,
                                  0.0f,
                                  static_cast<float>(target_.Width()),
                                  static_cast<float>(target_.Height()),
                                  0.0f,
                                  1.0f};
        const VkRect2D scissor{{0, 0}, {target_.Width(), target_.Height()}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        (void)desc;
    }

    void DrawInstances(VkCommandBuffer cmd, VkPipeline pipeline, const BoxPush& push,
                       InstanceSlot& slot, uint32_t first, uint32_t count) {
        // Descriptor set diikat untuk setiap pipeline forward, termasuk prepass.
        // Prepass tidak membacanya, tapi layout-nya mendeklarasikannya — dan
        // set yang dideklarasikan tapi tidak terikat adalah pelanggaran meski
        // tidak ada yang membacanya.
        if (pipeline == VK_NULL_HANDLE || count == 0) {
            return;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                &slot.shadowSet, 0, nullptr);
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(BoxPush),
                           &push);
        const std::array<VkBuffer, 2> buffers{cubeBuffer_.Handle(), slot.buffer.Handle()};
        const std::array<VkDeviceSize, 2> offsets{0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, buffers.data(), offsets.data());
        vkCmdDraw(cmd, cubeVertexCount_, count, 0, first);
    }

    bool CreateCube() {
        const std::vector<BoxVertex> vertices = BuildUnitCube();
        cubeVertexCount_ = static_cast<uint32_t>(vertices.size());
        if (!cubeBuffer_.Create(device_, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                sizeof(BoxVertex) * vertices.size())) {
            return false;
        }
        return cubeBuffer_.Write(vertices.data(), sizeof(BoxVertex) * vertices.size());
    }

    /// Pipeline grid dan garis, dipinjam apa adanya dari shader yang sudah ada.
    ///
    /// Keduanya sengaja dibawa dari StubRenderer alih-alih ditinggalkan: grid
    /// dan garis bantu adalah cara utama orang membaca skala dan orientasi di
    /// viewport, dan renderer baru yang menghapusnya bukan kemajuan melainkan
    /// kemunduran yang kebetulan lebih benar secara teknis.
    bool CreateOverlayPipelines(const std::filesystem::path& shaderDirectory) {
        VkShaderModule gridVertex =
            CreateShaderModule(device_.Handle(), shaderDirectory / "grid.vert.spv");
        VkShaderModule gridFragment =
            CreateShaderModule(device_.Handle(), shaderDirectory / "grid.frag.spv");
        VkShaderModule lineVertex =
            CreateShaderModule(device_.Handle(), shaderDirectory / "line.vert.spv");
        VkShaderModule lineFragment =
            CreateShaderModule(device_.Handle(), shaderDirectory / "line.frag.spv");
        if (gridVertex == VK_NULL_HANDLE || gridFragment == VK_NULL_HANDLE ||
            lineVertex == VK_NULL_HANDLE || lineFragment == VK_NULL_HANDLE) {
            return false;
        }

        VkPushConstantRange gridRange{};
        gridRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        gridRange.size = sizeof(GridPush);
        VkPipelineLayoutCreateInfo gridLayoutInfo{};
        gridLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        gridLayoutInfo.pushConstantRangeCount = 1;
        gridLayoutInfo.pPushConstantRanges = &gridRange;
        SIM_VK_CHECK(
            vkCreatePipelineLayout(device_.Handle(), &gridLayoutInfo, nullptr, &gridLayout_));

        VkPushConstantRange lineRange{};
        lineRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        lineRange.size = sizeof(LinePush);
        VkPipelineLayoutCreateInfo lineLayoutInfo{};
        lineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        lineLayoutInfo.pushConstantRangeCount = 1;
        lineLayoutInfo.pPushConstantRanges = &lineRange;
        SIM_VK_CHECK(
            vkCreatePipelineLayout(device_.Handle(), &lineLayoutInfo, nullptr, &lineLayout_));

        gridPipeline_ = BuildOverlayPipeline(gridVertex, gridFragment, gridLayout_,
                                             VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                             /*useDepth=*/false, /*vertexInput=*/false);
        linePipeline_ = BuildOverlayPipeline(lineVertex, lineFragment, lineLayout_,
                                             VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                             /*useDepth=*/true, /*vertexInput=*/true);

        for (VkShaderModule module : {gridVertex, gridFragment, lineVertex, lineFragment}) {
            vkDestroyShaderModule(device_.Handle(), module, nullptr);
        }
        return gridPipeline_ != VK_NULL_HANDLE && linePipeline_ != VK_NULL_HANDLE;
    }

    VkPipeline BuildOverlayPipeline(VkShaderModule vertex, VkShaderModule fragment,
                                    VkPipelineLayout layout, VkPrimitiveTopology topology,
                                    bool useDepth, bool vertexInput) {
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        stages[1].pName = "main";

        const VkVertexInputBindingDescription binding{0, sizeof(LineVertex),
                                                      VK_VERTEX_INPUT_RATE_VERTEX};
        const std::array<VkVertexInputAttributeDescription, 2> attributes{
            VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                              offsetof(LineVertex, position)},
            VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                                              offsetof(LineVertex, color)},
        };
        VkPipelineVertexInputStateCreateInfo vertexInputState{};
        vertexInputState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        if (vertexInput) {
            vertexInputState.vertexBindingDescriptionCount = 1;
            vertexInputState.pVertexBindingDescriptions = &binding;
            vertexInputState.vertexAttributeDescriptionCount =
                static_cast<uint32_t>(attributes.size());
            vertexInputState.pVertexAttributeDescriptions = attributes.data();
        }

        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = topology;

        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = useDepth ? VK_TRUE : VK_FALSE;
        depthStencil.depthWriteEnable = VK_FALSE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

        VkPipelineColorBlendAttachmentState blendState{};
        blendState.blendEnable = VK_TRUE;
        blendState.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendState.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendState.colorBlendOp = VK_BLEND_OP_ADD;
        blendState.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendState.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendState.alphaBlendOp = VK_BLEND_OP_ADD;
        blendState.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &blendState;

        const std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT,
                                                          VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();

        const VkFormat colorFormat = PostProcess::kSceneFormat;
        VkPipelineRenderingCreateInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &colorFormat;
        rendering.depthAttachmentFormat =
            useDepth ? target_.DepthFormat() : VK_FORMAT_UNDEFINED;

        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.pNext = &rendering;
        info.stageCount = static_cast<uint32_t>(stages.size());
        info.pStages = stages.data();
        info.pVertexInputState = &vertexInputState;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depthStencil;
        info.pColorBlendState = &colorBlend;
        info.pDynamicState = &dynamic;
        info.layout = layout;

        VkPipeline pipeline = VK_NULL_HANDLE;
        SIM_VK_CHECK(vkCreateGraphicsPipelines(device_.Handle(), VK_NULL_HANDLE, 1, &info, nullptr,
                                               &pipeline));
        return pipeline;
    }

    bool CreatePipelines(const std::filesystem::path& shaderDirectory) {
        VkShaderModule vertex = CreateShaderModule(device_.Handle(), shaderDirectory / "box.vert.spv");
        VkShaderModule fragment =
            CreateShaderModule(device_.Handle(), shaderDirectory / "box.frag.spv");
        if (vertex == VK_NULL_HANDLE || fragment == VK_NULL_HANDLE) {
            return false;
        }

        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        range.size = sizeof(BoxPush);
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &range;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &shadowSetLayout_;
        SIM_VK_CHECK(
            vkCreatePipelineLayout(device_.Handle(), &layoutInfo, nullptr, &pipelineLayout_));

        // Pass bayangan memakai layout sendiri: ia tidak membaca peta bayangan,
        // dan mendeklarasikan descriptor set yang tidak pernah diikat berarti
        // setiap draw-nya melanggar aturan validasi.
        VkPipelineLayoutCreateInfo shadowLayoutInfo{};
        shadowLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        shadowLayoutInfo.pushConstantRangeCount = 1;
        shadowLayoutInfo.pPushConstantRanges = &range;
        SIM_VK_CHECK(
            vkCreatePipelineLayout(device_.Handle(), &shadowLayoutInfo, nullptr, &shadowLayout_));

        VkShaderModule sdfVertex =
            CreateShaderModule(device_.Handle(), shaderDirectory / "sdf_debug.vert.spv");
        VkShaderModule sdfFragment =
            CreateShaderModule(device_.Handle(), shaderDirectory / "sdf_debug.frag.spv");
        if (sdfVertex != VK_NULL_HANDLE && sdfFragment != VK_NULL_HANDLE) {
            VkPushConstantRange sdfRange{};
            sdfRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            sdfRange.size = sizeof(SdfDebugPush);
            VkPipelineLayoutCreateInfo sdfLayoutInfo{};
            sdfLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            sdfLayoutInfo.pushConstantRangeCount = 1;
            sdfLayoutInfo.pPushConstantRanges = &sdfRange;
            sdfLayoutInfo.setLayoutCount = 1;
            sdfLayoutInfo.pSetLayouts = &shadowSetLayout_;
            SIM_VK_CHECK(vkCreatePipelineLayout(device_.Handle(), &sdfLayoutInfo, nullptr,
                                                &sdfDebugLayout_));
            // Tanpa vertex input dan tanpa depth: ia menimpa seluruh layar dan
            // memang dimaksudkan menutupi apa pun yang sudah tergambar.
            sdfDebugPipeline_ = BuildOverlayPipeline(sdfVertex, sdfFragment, sdfDebugLayout_,
                                                     VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                                     /*useDepth=*/false, /*vertexInput=*/false);
            vkDestroyShaderModule(device_.Handle(), sdfVertex, nullptr);
            vkDestroyShaderModule(device_.Handle(), sdfFragment, nullptr);
        }

        VkShaderModule shadowVertex =
            CreateShaderModule(device_.Handle(), shaderDirectory / "shadow.vert.spv");
        if (shadowVertex == VK_NULL_HANDLE) {
            vkDestroyShaderModule(device_.Handle(), vertex, nullptr);
            vkDestroyShaderModule(device_.Handle(), fragment, nullptr);
            return false;
        }
        // Tanpa tahap fragment sama sekali — jalur depth-only. Sama alasannya
        // dengan prepass: shader fragment yang menulis ke lampiran warna yang
        // tidak dipasang adalah peringatan validasi berulang, dan menghapusnya
        // juga jalur yang lebih cepat.
        // Atribut 0 dan 2..5: posisi dan keempat kolom matriks. Normal, warna,
        // dan bendera tidak dibaca pass bayangan.
        shadowPipeline_ =
            BuildPipeline(shadowVertex, VK_NULL_HANDLE, /*depthWrite=*/true,
                          VK_COMPARE_OP_LESS_OR_EQUAL, /*blend=*/false, /*colorWrite=*/false,
                          shadowLayout_, kShadowFormat, /*colorAttachment=*/VK_FORMAT_UNDEFINED,
                          /*attributeMask=*/0b0011'1101u);
        vkDestroyShaderModule(device_.Handle(), shadowVertex, nullptr);

        // Tiga pipeline dari satu pasang shader. Yang berbeda hanya keadaan
        // depth dan blending — dan itu memang perbedaan antara prepass, opaque,
        // dan transparan.
        // Prepass memakai shader fragmennya sendiri, bukan shader forward:
        // yang ditulisnya hanya normal, dan menjalankan seluruh pencahayaan
        // lalu membuangnya adalah persis pekerjaan yang prepass ada untuk
        // menghindarinya.
        VkShaderModule prepassVertex =
            CreateShaderModule(device_.Handle(), shaderDirectory / "prepass.vert.spv");
        VkShaderModule prepassFragment =
            CreateShaderModule(device_.Handle(), shaderDirectory / "prepass.frag.spv");
        // Atribut 0..5: posisi, normal, dan keempat kolom matriks. Warna dan
        // bendera tidak dibacanya.
        prepassPipeline_ = BuildPipeline(prepassVertex, prepassFragment, /*depthWrite=*/true,
                                         VK_COMPARE_OP_GREATER, /*blend=*/false,
                                         /*colorWrite=*/true, /*layout=*/VK_NULL_HANDLE,
                                         /*depthFormat=*/VK_FORMAT_UNDEFINED, kNormalFormat,
                                         /*attributeMask=*/0b0011'1111u);
        for (VkShaderModule module : {prepassVertex, prepassFragment}) {
            if (module != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device_.Handle(), module, nullptr);
            }
        }
        // Uji EQUAL, bukan GREATER: depth-nya sudah diisi prepass, jadi hanya
        // fragmen yang benar-benar terlihat yang boleh menjalankan shader.
        // Itulah gunanya prepass — bukan menghemat depth test, melainkan
        // menghemat shading yang akan ditimpa.
        opaquePipeline_ = BuildPipeline(vertex, fragment, /*depthWrite=*/false,
                                        VK_COMPARE_OP_EQUAL, /*blend=*/false, /*colorWrite=*/true);
        // Transparan diuji terhadap depth opaque tapi tidak menulisinya: dua
        // permukaan tembus pandang harus sama-sama terlihat, dan yang di depan
        // tidak boleh menghapus yang di belakang.
        transparentPipeline_ = BuildPipeline(vertex, fragment, /*depthWrite=*/false,
                                             VK_COMPARE_OP_GREATER, /*blend=*/true,
                                             /*colorWrite=*/true);

        vkDestroyShaderModule(device_.Handle(), vertex, nullptr);
        vkDestroyShaderModule(device_.Handle(), fragment, nullptr);
        return prepassPipeline_ != VK_NULL_HANDLE && opaquePipeline_ != VK_NULL_HANDLE &&
               transparentPipeline_ != VK_NULL_HANDLE && shadowPipeline_ != VK_NULL_HANDLE;
    }

    /// `layout` dan `depthFormat` kosong berarti memakai milik pass forward.
    /// Pass bayangan menggambar ke image lain dengan format lain, jadi ia harus
    /// menyebutkan keduanya — pipeline yang formatnya tidak cocok dengan
    /// lampiran yang dipasang adalah ketidakcocokan yang hanya dilaporkan
    /// validation layer.
    VkPipeline BuildPipeline(VkShaderModule vertex, VkShaderModule fragment, bool depthWrite,
                             VkCompareOp depthCompare, bool blend, bool colorWrite,
                             VkPipelineLayout layout = VK_NULL_HANDLE,
                             VkFormat depthFormat = VK_FORMAT_UNDEFINED,
                             VkFormat colorAttachment = VK_FORMAT_UNDEFINED,
                             uint32_t attributeMask = 0xFFu) {
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        stages[1].pName = "main";
        // **Prepass jalan tanpa fragment shader sama sekali.**
        //
        // Bukan sekadar meredam peringatan validation bahwa `outColor` ditulis
        // ke lampiran yang tidak ada: itulah bentuk depth prepass yang benar.
        // Tanpa fragment shader, kartu grafis boleh memakai jalur depth-only
        // yang jauh lebih cepat — dan menjalankan shading yang hasilnya memang
        // dibuang adalah persis pekerjaan yang prepass ada untuk menghindarinya.
        const uint32_t stageCount = colorWrite ? 2u : 1u;

        const std::array<VkVertexInputBindingDescription, 2> bindings{
            VkVertexInputBindingDescription{0, sizeof(BoxVertex), VK_VERTEX_INPUT_RATE_VERTEX},
            VkVertexInputBindingDescription{1, sizeof(BoxInstance),
                                            VK_VERTEX_INPUT_RATE_INSTANCE},
        };
        const std::array<VkVertexInputAttributeDescription, 8> attributes{
            VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                              offsetof(BoxVertex, position)},
            VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                              offsetof(BoxVertex, normal)},
            VkVertexInputAttributeDescription{2, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                                              offsetof(BoxInstance, row0)},
            VkVertexInputAttributeDescription{3, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                                              offsetof(BoxInstance, row1)},
            VkVertexInputAttributeDescription{4, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                                              offsetof(BoxInstance, row2)},
            VkVertexInputAttributeDescription{5, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                                              offsetof(BoxInstance, row3)},
            VkVertexInputAttributeDescription{6, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                                              offsetof(BoxInstance, color)},
            VkVertexInputAttributeDescription{7, 1, VK_FORMAT_R32_UINT,
                                              offsetof(BoxInstance, flags)},
        };
        // Tiap pipeline menyebutkan atribut mana yang benar-benar dibacanya.
        // Buffer-nya sama dan stride-nya sama; yang berbeda hanya atribut yang
        // diambil.
        //
        // **Menyebutkannya jadi wajib setelah shader-nya pindah ke Slang.**
        // glslang mempertahankan input yang dideklarasikan walaupun tidak
        // dipakai, jadi selama shader-nya GLSL semua delapan atribut "terpakai".
        // Slang membuang yang tidak terpakai dari antarmuka entry point, dan
        // pipeline yang mendeklarasikan atribut yang tidak dikonsumsi shader-nya
        // adalah peringatan validasi di setiap pembuatan pipeline. Menyaringnya
        // di sini bukan sekadar meredam peringatan: atribut yang tidak diambil
        // memang tidak perlu diambil.
        std::array<VkVertexInputAttributeDescription, 8> used{};
        uint32_t usedCount = 0;
        for (const VkVertexInputAttributeDescription& attribute : attributes) {
            if ((attributeMask & (1u << attribute.location)) != 0u) {
                used[usedCount++] = attribute;
            }
        }

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
        vertexInput.pVertexBindingDescriptions = bindings.data();
        vertexInput.vertexAttributeDescriptionCount = usedCount;
        vertexInput.pVertexAttributeDescriptions = used.data();

        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = blend ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        // **COUNTER_CLOCKWISE, walaupun proyeksinya membalik Y.**
        //
        // Ini salah pada percobaan pertama, dan salahnya terlihat sebagai kotak
        // yang memperlihatkan sisi dalamnya. Alasannya: arah muka dinilai di
        // ruang framebuffer, sesudah transformasi viewport — dan sumbu Y
        // framebuffer Vulkan memang sudah menghadap ke bawah. Pembalikan Y di
        // matriks proyeksi justru meniadakan pembalikan itu, jadi geometri yang
        // ditulis berlawanan arah jarum jam tetap berlawanan arah jarum jam di
        // sana. Dua pembalikan yang saling meniadakan mudah dihitung sebagai
        // satu.
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = depthWrite ? VK_TRUE : VK_FALSE;
        depthStencil.depthCompareOp = depthCompare;

        VkPipelineColorBlendAttachmentState blendState{};
        blendState.colorWriteMask = colorWrite ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT)
                                               : 0;
        blendState.blendEnable = blend ? VK_TRUE : VK_FALSE;
        blendState.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendState.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendState.colorBlendOp = VK_BLEND_OP_ADD;
        blendState.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendState.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendState.alphaBlendOp = VK_BLEND_OP_ADD;

        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = colorWrite ? 1u : 0u;
        colorBlend.pAttachments = colorWrite ? &blendState : nullptr;

        const std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT,
                                                          VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();

        // **Prepass tidak punya lampiran warna sama sekali**, dan pipeline-nya
        // harus menyatakan hal yang sama. Menyatakan satu lampiran lalu
        // menjalankannya di pass yang tidak memasang apa pun adalah
        // ketidakcocokan yang hanya dilaporkan validation layer — tanpa layer,
        // yang terlihat adalah prepass yang kadang jalan dan kadang tidak,
        // bergantung driver. Ditemukan lewat validation, bukan dengan membaca
        // kode.
        const VkFormat colorFormat = colorAttachment != VK_FORMAT_UNDEFINED
                                         ? colorAttachment
                                         : PostProcess::kSceneFormat;
        VkPipelineRenderingCreateInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.colorAttachmentCount = colorWrite ? 1u : 0u;
        rendering.pColorAttachmentFormats = colorWrite ? &colorFormat : nullptr;
        rendering.depthAttachmentFormat =
            depthFormat != VK_FORMAT_UNDEFINED ? depthFormat : target_.DepthFormat();

        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.pNext = &rendering;
        info.stageCount = stageCount;
        info.pStages = stages.data();
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depthStencil;
        info.pColorBlendState = &colorBlend;
        info.pDynamicState = &dynamic;
        info.layout = layout != VK_NULL_HANDLE ? layout : pipelineLayout_;

        VkPipeline pipeline = VK_NULL_HANDLE;
        SIM_VK_CHECK(vkCreateGraphicsPipelines(device_.Handle(), VK_NULL_HANDLE, 1, &info, nullptr,
                                               &pipeline));
        return pipeline;
    }

    // --- bayangan -----------------------------------------------------------

    bool CreateShadowMap() {
        shadow_.resolution = kShadowResolution;
        shadow_.layers = static_cast<uint32_t>(kMaxCascades);

        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = kShadowFormat;
        imageInfo.extent = {shadow_.resolution, shadow_.resolution, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = shadow_.layers;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage =
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vmaCreateImage(device_.Allocator(), &imageInfo, &allocation, &shadow_.image,
                           &shadow_.allocation, nullptr) != VK_SUCCESS) {
            SIM_ERROR("Render", "cannot allocate the cascade shadow map");
            return false;
        }

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = shadow_.image;
        viewInfo.format = kShadowFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.levelCount = 1;

        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        viewInfo.subresourceRange.layerCount = shadow_.layers;
        SIM_VK_CHECK(
            vkCreateImageView(device_.Handle(), &viewInfo, nullptr, &shadow_.arrayView));

        // Satu view per lapis untuk menggambar ke dalamnya. Dynamic rendering
        // memasang sebuah view, bukan sebuah lapis, jadi menggambar ke cascade
        // tertentu menuntut view yang memang hanya berisi cascade itu.
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.subresourceRange.layerCount = 1;
        for (uint32_t layer = 0; layer < shadow_.layers; ++layer) {
            viewInfo.subresourceRange.baseArrayLayer = layer;
            SIM_VK_CHECK(vkCreateImageView(device_.Handle(), &viewInfo, nullptr,
                                           &shadow_.layerViews[layer]));
        }

        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        // LINEAR bersama compareEnable adalah PCF perangkat keras: satu
        // pengambilan mengembalikan rata-rata empat perbandingan, bukan rata-rata
        // empat kedalaman. Merata-ratakan kedalaman lalu membandingkannya sekali
        // menghasilkan tepi bayangan yang salah di setiap permukaan miring.
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.compareEnable = VK_TRUE;
        // LESS_OR_EQUAL, bukan GREATER: proyeksi cascade-nya ortografik biasa,
        // bukan reversed-Z. Keduanya hidup berdampingan di renderer ini, dan
        // memakai perbandingan yang salah membalik bayangan menjadi sorotan.
        samplerInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        SIM_VK_CHECK(vkCreateSampler(device_.Handle(), &samplerInfo, nullptr, &shadow_.sampler));

        const std::array<VkDescriptorSetLayoutBinding, 21> bindings{
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{14, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{15, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{17, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{18, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{19, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{20, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        SIM_VK_CHECK(vkCreateDescriptorSetLayout(device_.Handle(), &layoutInfo, nullptr,
                                                 &shadowSetLayout_));

        const std::array<VkDescriptorPoolSize, 3> sizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                 static_cast<uint32_t>(slots_.size())},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                 static_cast<uint32_t>(slots_.size()) * 15},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                 static_cast<uint32_t>(slots_.size()) * 5},
        };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = static_cast<uint32_t>(slots_.size());
        poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
        poolInfo.pPoolSizes = sizes.data();
        SIM_VK_CHECK(
            vkCreateDescriptorPool(device_.Handle(), &poolInfo, nullptr, &shadowPool_));
        return true;
    }

    bool CreateShadowAtlas() {
        atlasSettings_.resolution = kAtlasResolution;
        atlasSettings_.maxTile = 512;
        atlasSettings_.minTile = 128;

        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = kShadowFormat;
        imageInfo.extent = {kAtlasResolution, kAtlasResolution, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage =
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vmaCreateImage(device_.Allocator(), &imageInfo, &allocation, &atlas_.image,
                           &atlas_.allocation, nullptr) != VK_SUCCESS) {
            SIM_ERROR("Render", "cannot allocate the shadow atlas");
            return false;
        }

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = atlas_.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = kShadowFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        SIM_VK_CHECK(vkCreateImageView(device_.Handle(), &viewInfo, nullptr, &atlas_.view));

        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        // CLAMP_TO_EDGE, dan penjepitan di dalam ubin dilakukan shader. Sampler
        // hanya tahu batas atlas, bukan batas ubin — dan tepi ubin adalah milik
        // lampu lain, yang bocor masuk sebagai garis kalau tidak dijepit.
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.compareEnable = VK_TRUE;
        samplerInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        SIM_VK_CHECK(vkCreateSampler(device_.Handle(), &samplerInfo, nullptr, &atlas_.sampler));
        return true;
    }

    void DestroyShadowAtlas() {
        if (atlas_.sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device_.Handle(), atlas_.sampler, nullptr);
            atlas_.sampler = VK_NULL_HANDLE;
        }
        if (atlas_.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_.Handle(), atlas_.view, nullptr);
            atlas_.view = VK_NULL_HANDLE;
        }
        if (atlas_.image != VK_NULL_HANDLE) {
            vmaDestroyImage(device_.Allocator(), atlas_.image, atlas_.allocation);
            atlas_.image = VK_NULL_HANDLE;
            atlas_.allocation = VK_NULL_HANDLE;
        }
    }

    void RecordAtlasPass(VkCommandBuffer cmd, InstanceSlot& slot, uint32_t casterCount) {
        VkRenderingAttachmentInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth.imageView = atlas_.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = {1.0f, 0};

        VkRenderingInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        info.renderArea.extent = {kAtlasResolution, kAtlasResolution};
        info.layerCount = 1;
        info.pDepthAttachment = &depth;
        // Satu BeginRendering untuk seluruh atlas, lalu viewport dan scissor
        // dipindah per ubin. Memulai rendering per ubin berarti satu clear per
        // ubin pada image yang sama — dan clear yang kedua menghapus yang
        // pertama kalau areanya kebetulan bertemu.
        vkCmdBeginRendering(cmd, &info);

        if (shadowPipeline_ != VK_NULL_HANDLE && casterCount > 0 && slot.buffer.IsValid()) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
            const std::array<VkBuffer, 2> buffers{cubeBuffer_.Handle(), slot.buffer.Handle()};
            const std::array<VkDeviceSize, 2> offsets{0, 0};
            vkCmdBindVertexBuffers(cmd, 0, 2, buffers.data(), offsets.data());

            for (const ShadowAtlasEntry& entry : atlasAllocation_.entries) {
                const VkViewport viewport{static_cast<float>(entry.x),
                                          static_cast<float>(entry.y),
                                          static_cast<float>(entry.size),
                                          static_cast<float>(entry.size),
                                          0.0f,
                                          1.0f};
                const VkRect2D scissor{{static_cast<int32_t>(entry.x),
                                        static_cast<int32_t>(entry.y)},
                                       {entry.size, entry.size}};
                vkCmdSetViewport(cmd, 0, 1, &viewport);
                vkCmdSetScissor(cmd, 0, 1, &scissor);
                const BoxPush push{entry.viewProjection};
                vkCmdPushConstants(cmd, shadowLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                                   sizeof(BoxPush), &push);
                vkCmdDraw(cmd, cubeVertexCount_, casterCount, 0, 0);
            }
        }
        vkCmdEndRendering(cmd);
    }

    bool WriteShadowDescriptors() {
        std::vector<VkDescriptorSetLayout> layouts(slots_.size(), shadowSetLayout_);
        VkDescriptorSetAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocateInfo.descriptorPool = shadowPool_;
        allocateInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
        allocateInfo.pSetLayouts = layouts.data();

        std::vector<VkDescriptorSet> sets(slots_.size());
        if (vkAllocateDescriptorSets(device_.Handle(), &allocateInfo, sets.data()) != VK_SUCCESS) {
            SIM_ERROR("Render", "cannot allocate shadow descriptor sets");
            return false;
        }

        // Empat entri buffer per slot, dan vektornya dipesan penuh lebih dulu.
        // `pBufferInfo` menyimpan pointer, jadi vektor yang tumbuh sambil diisi
        // akan membuat entri yang sudah dicatat menunjuk memori yang sudah
        // dibebaskan — kerusakan yang tidak muncul sebagai galat validasi.
        std::vector<VkDescriptorBufferInfo> buffers(slots_.size() * 5);
        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(slots_.size() * 17);
        // SHADER_READ_ONLY, bukan DEPTH_READ_ONLY. Keduanya sah untuk mengambil
        // sampel dari image depth, tapi yang berlaku adalah yang disimpulkan
        // frame graph dari `Access::ShaderRead` — dan descriptor yang menyebut
        // layout lain adalah pelanggaran di setiap draw. DEPTH_READ_ONLY
        // gunanya untuk depth yang dipasang sebagai lampiran sekaligus
        // disampel, dan di sini tidak begitu.
        const VkDescriptorImageInfo image{shadow_.sampler, shadow_.arrayView,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo atlasImage{atlas_.sampler, atlas_.view,
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        // Kaskade SDF. Kalau clipmap gagal dibuat, ketiganya memakai peta
        // bayangan sebagai pengganti — descriptor yang dibiarkan kosong adalah
        // pelanggaran di setiap draw, bahkan pada pass yang tidak membacanya.
        const VkDescriptorImageInfo hizImage = HizDescriptorImage();
        const std::array<VkDescriptorImageInfo, 9> probeImages = ProbeDescriptorImages();
        // Cache radiansi dipakai bersama seluruh slot: ia riwayat lintas frame,
        // bukan data per frame. Kalau gagal dibuat, descriptor-nya menunjuk
        // buffer lampu — descriptor yang dibiarkan kosong adalah pelanggaran di
        // setiap draw, bahkan pada pass yang tidak membacanya.
        const VkDescriptorBufferInfo cacheBufferInfo{
            cache_.buffer != VK_NULL_HANDLE ? cache_.buffer : slots_[0].lightBuffer.Handle(), 0,
            VK_WHOLE_SIZE};
        std::array<VkDescriptorImageInfo, 3> sdfImages{};
        for (uint32_t cascade = 0; cascade < 3; ++cascade) {
            const bool ready = sdfClipmap_.IsValid() && cascade < sdfClipmap_.CascadeCount();
            sdfImages[cascade] = {ready ? sdfClipmap_.Texture(cascade).Sampler() : shadow_.sampler,
                                  ready ? sdfClipmap_.Texture(cascade).View() : shadow_.arrayView,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }

        for (std::size_t i = 0; i < slots_.size(); ++i) {
            slots_[i].shadowSet = sets[i];
            const std::size_t base = i * 5;
            buffers[base] = {slots_[i].shadowUniform.Handle(), 0, sizeof(ShadowUniforms)};
            buffers[base + 1] = {slots_[i].lightBuffer.Handle(), 0, VK_WHOLE_SIZE};
            buffers[base + 2] = {slots_[i].clusterRangeBuffer.Handle(), 0, VK_WHOLE_SIZE};
            buffers[base + 3] = {slots_[i].clusterIndexBuffer.Handle(), 0, VK_WHOLE_SIZE};
            buffers[base + 4] = {slots_[i].shadowFaceBuffer.Handle(), 0, VK_WHOLE_SIZE};

            VkWriteDescriptorSet uniform{};
            uniform.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            uniform.dstSet = sets[i];
            uniform.dstBinding = 0;
            uniform.descriptorCount = 1;
            uniform.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            uniform.pBufferInfo = &buffers[base];
            writes.push_back(uniform);

            VkWriteDescriptorSet sampled = uniform;
            sampled.dstBinding = 1;
            sampled.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            sampled.pBufferInfo = nullptr;
            sampled.pImageInfo = &image;
            writes.push_back(sampled);

            for (uint32_t binding = 2; binding <= 5; ++binding) {
                VkWriteDescriptorSet storage = uniform;
                storage.dstBinding = binding;
                storage.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                storage.pBufferInfo = &buffers[base + binding - 1];
                writes.push_back(storage);
            }

            VkWriteDescriptorSet atlas = sampled;
            atlas.dstBinding = 6;
            atlas.pImageInfo = &atlasImage;
            writes.push_back(atlas);

            for (uint32_t cascade = 0; cascade < 3; ++cascade) {
                VkWriteDescriptorSet volume = sampled;
                volume.dstBinding = 7 + cascade;
                volume.pImageInfo = &sdfImages[cascade];
                writes.push_back(volume);
            }

            VkWriteDescriptorSet pyramid = sampled;
            pyramid.dstBinding = 10;
            pyramid.pImageInfo = &hizImage;
            writes.push_back(pyramid);

            // Sembilan image menempati binding 11..15 dan 17..20: 16 dipakai
            // buffer cache radiansi, dan menyisipkannya di tengah deretan itu
            // adalah cara paling murah membuat seluruh binding sesudahnya
            // bergeser satu tanpa satu pun galat.
            static constexpr std::array<uint32_t, 9> kProbeBindings{11, 12, 13, 14, 15,
                                                                    17, 18, 19, 20};
            for (uint32_t offset = 0; offset < probeImages.size(); ++offset) {
                VkWriteDescriptorSet probe = sampled;
                probe.dstBinding = kProbeBindings[offset];
                probe.pImageInfo = &probeImages[offset];
                writes.push_back(probe);
            }

            VkWriteDescriptorSet cache = uniform;
            cache.dstBinding = 16;
            cache.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            cache.pBufferInfo = &cacheBufferInfo;
            writes.push_back(cache);
        }
        vkUpdateDescriptorSets(device_.Handle(), static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
        return true;
    }

    /// Menulis ulang binding piramida saja.
    ///
    /// **Bukan `WriteShadowDescriptors` lagi.** Fungsi itu mengalokasi set baru
    /// dari pool yang hanya cukup untuk satu putaran, jadi memanggilnya kedua
    /// kali kehabisan pool — kegagalan yang muncul sebagai layar yang berhenti
    /// diperbarui setelah panel diseret, bukan sebagai galat di tempat yang
    /// benar.
    void UpdateHizDescriptors() {
        const VkDescriptorImageInfo image = HizDescriptorImage();
        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(slots_.size());
        for (const InstanceSlot& slot : slots_) {
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = slot.shadowSet;
            write.dstBinding = 10;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &image;
            writes.push_back(write);
        }
        vkUpdateDescriptorSets(device_.Handle(), static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }

    /// Normal G-buffer, warna tersinari, dan tiga kanal SH probe — atau peta
    /// bayangan sebagai pengganti kalau probe gagal dibuat. Descriptor yang
    /// dibiarkan kosong adalah pelanggaran di setiap draw, bahkan pada pass yang
    /// tidak membacanya.
    std::array<VkDescriptorImageInfo, 9> ProbeDescriptorImages() const {
        const bool ready = probes_.IsValid() && post_.IsValid();
        const VkSampler sampler = ready ? probes_.Sampler() : shadow_.sampler;
        const VkImageView fallback = shadow_.arrayView;
        std::array<VkDescriptorImageInfo, 9> images{};
        images[0] = {sampler, ready ? probes_.NormalView() : fallback,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        // **Radiansi HDR, bukan target tampilan.** Sinar yang mengenai lewat
        // lapis layar membaca warna yang sudah dinaungi — dan sejak pass tone
        // mapping ada, warna di target tampilan sudah dipetakan dan di-encode
        // sRGB. Memantulkannya berarti memantulkan gambar layar, bukan cahaya.
        images[1] = {sampler, ready ? post_.SceneView() : fallback,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        for (uint32_t channel = 0; channel < ProbeField::kShChannels; ++channel) {
            images[2 + channel] = {sampler, ready ? probes_.ShView(channel) : fallback,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            images[5 + channel] = {sampler, ready ? probes_.HistoryShView(channel) : fallback,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }
        images[8] = {sampler, ready ? probes_.HistorySurfaceView() : fallback,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        return images;
    }

    /// Cache radiansi: satu buffer device-local, dibersihkan sekali.
    ///
    /// **Dibersihkan dengan `vkCmdFillBuffer`, bukan diunggah dari CPU.**
    /// Kapasitas 2²⁰ entri berarti 32 MB; mengunggahnya lewat staging berarti
    /// menyalin 32 MB nol melewati PCIe untuk sesuatu yang bisa dituliskan
    /// perangkat sendiri dalam sekali perintah.
    void CreateRadianceCache() {
        cacheSettings_ = RadianceCacheSettings{};
        // Harus sama persis dengan `GiCacheEntry` di Shaders/gi_cache.slang.
        constexpr VkDeviceSize kEntryBytes = 32;
        cache_.bytes = static_cast<VkDeviceSize>(cacheSettings_.capacity) * kEntryBytes;

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = cache_.bytes;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateBuffer(device_.Allocator(), &bufferInfo, &allocation, &cache_.buffer,
                            &cache_.allocation, nullptr) != VK_SUCCESS) {
            SIM_WARN("Render", "radiance cache unavailable; SDF hits stay colourless");
            cache_.buffer = VK_NULL_HANDLE;
            cache_.bytes = 0;
            return;
        }

        VkCommandBuffer cmd = device_.BeginOneShot();
        vkCmdFillBuffer(cmd, cache_.buffer, 0, cache_.bytes, 0);
        device_.EndOneShot(cmd);
    }

    /// Menulis ulang binding piramida dan probe saja.
    void UpdateGiDescriptors() {
        const VkDescriptorImageInfo hiz = HizDescriptorImage();
        const std::array<VkDescriptorImageInfo, 9> probeImages = ProbeDescriptorImages();
        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(slots_.size() * 11);
        for (const InstanceSlot& slot : slots_) {
            static constexpr std::array<uint32_t, 9> kProbeBindings{11, 12, 13, 14, 15,
                                                                    17, 18, 19, 20};
            VkWriteDescriptorSet pyramid{};
            pyramid.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            pyramid.dstSet = slot.shadowSet;
            pyramid.dstBinding = 10;
            pyramid.descriptorCount = 1;
            pyramid.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            pyramid.pImageInfo = &hiz;
            writes.push_back(pyramid);
            for (uint32_t offset = 0; offset < probeImages.size(); ++offset) {
                VkWriteDescriptorSet write = pyramid;
                write.dstBinding = kProbeBindings[offset];
                write.pImageInfo = &probeImages[offset];
                writes.push_back(write);
            }
        }
        vkUpdateDescriptorSets(device_.Handle(), static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }

    /// Piramida depth, atau peta bayangan sebagai pengganti kalau ia gagal
    /// dibuat. Descriptor yang dibiarkan kosong adalah pelanggaran di setiap
    /// draw, bahkan pada pass yang tidak membacanya.
    VkDescriptorImageInfo HizDescriptorImage() const {
        const bool ready = hiz_.IsValid();
        return {ready ? hiz_.Sampler() : shadow_.sampler,
                ready ? hiz_.View() : shadow_.arrayView,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }

    /// Peta bayangan diimpor graph sebagai `ShaderRead`, sama seperti target
    /// warna diimpor sebagai `Present`.
    ///
    /// **Bukan `None`.** `None` berarti "tidak ada yang perlu ditunggu", dan itu
    /// tidak benar di sini: frame berikutnya menulis ulang peta yang masih
    /// dibaca fragment shader frame sebelumnya. Menyatakannya `ShaderRead`
    /// membuat graph memancarkan barrier yang menunggu pembacaan itu selesai —
    /// dan konsekuensinya keadaan awalnya harus benar sejak frame pertama, sama
    /// seperti target warna.
    void AdoptShadowLayout() {
        VkCommandBuffer cmd = device_.BeginOneShot();
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = shadow_.image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
        barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
        device_.EndOneShot(cmd);
    }

    void AdoptAtlasLayout() {
        VkCommandBuffer cmd = device_.BeginOneShot();
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = atlas_.image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
        barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
        device_.EndOneShot(cmd);
    }

    void DestroyShadowMap() {
        if (shadowPool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_.Handle(), shadowPool_, nullptr);
            shadowPool_ = VK_NULL_HANDLE;
        }
        if (shadowSetLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_.Handle(), shadowSetLayout_, nullptr);
            shadowSetLayout_ = VK_NULL_HANDLE;
        }
        if (shadow_.sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device_.Handle(), shadow_.sampler, nullptr);
            shadow_.sampler = VK_NULL_HANDLE;
        }
        for (VkImageView& view : shadow_.layerViews) {
            if (view != VK_NULL_HANDLE) {
                vkDestroyImageView(device_.Handle(), view, nullptr);
                view = VK_NULL_HANDLE;
            }
        }
        if (shadow_.arrayView != VK_NULL_HANDLE) {
            vkDestroyImageView(device_.Handle(), shadow_.arrayView, nullptr);
            shadow_.arrayView = VK_NULL_HANDLE;
        }
        if (shadow_.image != VK_NULL_HANDLE) {
            vmaDestroyImage(device_.Allocator(), shadow_.image, shadow_.allocation);
            shadow_.image = VK_NULL_HANDLE;
            shadow_.allocation = VK_NULL_HANDLE;
        }
    }

    /// Menyaring lampu punctual ke cluster lalu mengunggahnya.
    ///
    /// **Directional tidak ikut.** Ia mengenai setiap cluster, jadi
    /// memasukkannya hanya menambah satu entri ke setiap daftar — dan ia sudah
    /// ditangani terpisah bersama cascade bayangannya.
    void UpdateClusters(const ViewportDesc& desc, const ViewportScene& scene, float aspect,
                        InstanceSlot& slot) {
        gpuLights_.clear();
        clusterLights_.clear();
        for (uint32_t sceneIndex = 0; sceneIndex < scene.lights.size(); ++sceneIndex) {
            const LightInstance& light = scene.lights[sceneIndex];
            if (light.kind == LightKind::Directional) {
                continue;
            }
            if (gpuLights_.size() >= kMaxClusterLights) {
                break;
            }
            const float range = std::max(light.range, 1e-3f);
            // Jari-jari sumber dijaga di atas batas bawah. Nol berarti sumber
            // yang benar-benar sebuah titik, dan kuadrat terbalik di sana tidak
            // punya nilai berhingga sama sekali — bukan angka besar, melainkan
            // pembagian dengan nol.
            const float radius = std::max(light.sourceRadius, kMinSourceRadius);

            GpuLight gpu;
            gpu.positionInvRangeSq = Vec4(light.position, 1.0f / (range * range));
            gpu.directionCosOuter = Vec4(glm::normalize(light.direction), light.cosOuter);
            // Eksposur yang sama dengan matahari. Dua jalur cahaya pada skala
            // berbeda adalah ketidakcocokan yang paling sulit dilacak: setiap
            // lampu terlihat masuk akal sendiri-sendiri.
            gpu.colorCosInner =
                // Radiance apa adanya. **Pengali eksposur yang dulu ada di sini
                // sudah hilang bersama `ViewportDesc::exposure`:** ia penambal
                // untuk tidak adanya operator nada, dan operatornya sekarang ada
                // di ujung graph. Lampu yang tetap dikalikan di sini akan
                // dikalikan dua kali.
                Vec4(light.color * light.intensity, light.cosInner);
            // Indeks entri atlas dan bias normalnya. Biasnya dalam satuan
            // dunia dan dihitung di sini karena ia bergantung pada ukuran ubin
            // yang baru diputuskan pengalokasi — ubin yang lebih kecil menuntut
            // pergeseran yang lebih besar, dan ukuran ubin berubah setiap kali
            // lampunya mendekat.
            const int32_t first = sceneIndex < atlasAllocation_.firstEntry.size()
                                      ? atlasAllocation_.firstEntry[sceneIndex]
                                      : -1;
            float normalBias = 0.0f;
            if (first >= 0) {
                const uint32_t tile =
                    std::max(atlasAllocation_.entries[static_cast<size_t>(first)].size, 1u);
                normalBias = light.range / static_cast<float>(tile) * kAtlasNormalBiasTexels;
            }
            gpu.kind = Vec4(light.kind == LightKind::Spot ? 1.0f : 0.0f, radius * radius,
                            static_cast<float>(first), normalBias);
            gpuLights_.push_back(gpu);

            ClusterLight entry;
            entry.type = light.kind == LightKind::Spot ? ClusterLightType::Spot
                                                       : ClusterLightType::Point;
            entry.position = light.position;
            entry.direction = glm::normalize(light.direction);
            entry.range = light.range;
            entry.cosOuterAngle = light.cosOuter;
            clusterLights_.push_back(entry);
        }

        // Atlas dialokasikan dari daftar lampu yang sama, sebelum entri GPU
        // ditulis: `kind.z` tiap lampu menunjuk entri pertamanya, dan nomor itu
        // baru ada setelah pembagian atlasnya selesai.
        atlasAllocation_ = AllocateShadowAtlas(scene.lights, desc.camera.position,
                                               atlasSettings_);
        if (atlasAllocation_.dropped > 0) {
            SIM_WARN("Render", "{} shadow-casting lights did not fit the atlas",
                     atlasAllocation_.dropped);
        }

        gpuFaces_.clear();
        gpuFaces_.reserve(atlasAllocation_.entries.size());
        const auto atlasExtent = static_cast<float>(atlasSettings_.resolution);
        for (const ShadowAtlasEntry& entry : atlasAllocation_.entries) {
            GpuShadowFace face;
            face.viewProjection = entry.viewProjection;
            face.tile = Vec4(static_cast<float>(entry.x) / atlasExtent,
                             static_cast<float>(entry.y) / atlasExtent,
                             static_cast<float>(entry.size) / atlasExtent,
                             static_cast<float>(entry.size) / atlasExtent);
            gpuFaces_.push_back(face);
        }
        if (gpuFaces_.empty()) {
            gpuFaces_.push_back(GpuShadowFace{});
        }
        const VkDeviceSize faceBytes = sizeof(GpuShadowFace) * gpuFaces_.size();
        if (slot.shadowFaceBuffer.Reserve(faceBytes)) {
            slot.shadowFaceBuffer.Write(gpuFaces_.data(), faceBytes);
        }

        clusterGrid_.Build(clusterSettings_, desc.camera.fovYRadians, aspect, desc.camera.nearZ,
                           std::min(desc.camera.farZ, kClusterFar));
        clusterAssignment_ =
            AssignLights(clusterGrid_, desc.camera.View(), clusterLights_, clusterSettings_);
        if (clusterAssignment_.overflowed > 0) {
            // Dilaporkan, tidak didiamkan. Pemotongan yang diam-diam terlihat
            // sebagai lampu yang hilang di sudut tertentu saja, dan tidak ada
            // yang akan menghubungkannya dengan batas per-cluster.
            SIM_WARN("Render", "{} clusters exceeded {} lights and were truncated",
                     clusterAssignment_.overflowed, clusterSettings_.maxLightsPerCluster);
        }

        // Buffer kosong tidak sah, jadi keduanya selalu berisi minimal satu
        // entri boneka. Cluster yang jumlahnya nol tidak pernah membacanya.
        if (gpuLights_.empty()) {
            gpuLights_.push_back(GpuLight{});
        }
        if (clusterAssignment_.indices.empty()) {
            clusterAssignment_.indices.push_back(0);
        }

        const VkDeviceSize lightBytes = sizeof(GpuLight) * gpuLights_.size();
        const VkDeviceSize rangeBytes =
            sizeof(ClusterAssignment::Range) * clusterAssignment_.ranges.size();
        const VkDeviceSize indexBytes = sizeof(uint32_t) * clusterAssignment_.indices.size();
        if (slot.lightBuffer.Reserve(lightBytes) && slot.clusterRangeBuffer.Reserve(rangeBytes) &&
            slot.clusterIndexBuffer.Reserve(indexBytes)) {
            slot.lightBuffer.Write(gpuLights_.data(), lightBytes);
            slot.clusterRangeBuffer.Write(clusterAssignment_.ranges.data(), rangeBytes);
            slot.clusterIndexBuffer.Write(clusterAssignment_.indices.data(), indexBytes);
        }
    }

    void UpdateShadowUniforms(const ViewportDesc& desc, const Mat4& viewProj,
                              InstanceSlot& slot) {
        ShadowUniforms uniforms;
        for (int i = 0; i < cascades_.count; ++i) {
            const Cascade& cascade = cascades_.cascades[static_cast<size_t>(i)];
            uniforms.cascadeViewProj[static_cast<size_t>(i)] = cascade.viewProjection;
            uniforms.cascadeSplitFar[i] = cascade.splitFar;
            uniforms.cascadeBlendBegin[i] = cascade.blendBegin;
            uniforms.cascadeTexelSize[i] = cascade.texelWorldSize;
        }
        // Cascade yang tidak terpakai diberi batas tak terhingga, bukan nol.
        // Nol membuat `chooseCascade` menganggap setiap kedalaman melewatinya
        // dan jatuh ke cascade yang matriksnya identitas.
        for (int i = cascades_.count; i < kMaxCascades; ++i) {
            uniforms.cascadeSplitFar[i] = std::numeric_limits<float>::max();
            uniforms.cascadeBlendBegin[i] = std::numeric_limits<float>::max();
            uniforms.cascadeTexelSize[i] = 1.0f;
        }

        const Vec3 sun = glm::length(sunDirection_) > 1e-6f ? glm::normalize(sunDirection_)
                                                            : Vec3(0.0f, 1.0f, 0.0f);
        uniforms.lightDirection = Vec4(sun, static_cast<float>(cascades_.count));
        uniforms.cameraPosition =
            Vec4(desc.camera.position, sunCastsShadows_ && cascades_.count > 0 ? 1.0f : 0.0f);
        uniforms.sunRadiance = Vec4(sunRadiance_, 0.0f);
        uniforms.cameraForward = Vec4(desc.camera.Forward(), kNormalBiasTexels);
        uniforms.clusterCounts = Vec4(static_cast<float>(clusterGrid_.TilesX()),
                                      static_cast<float>(clusterGrid_.TilesY()),
                                      static_cast<float>(clusterGrid_.Slices()),
                                      static_cast<float>(gpuLights_.size()));
        // Skala dan bias irisan datang dari CPU apa adanya, bukan diturunkan
        // ulang di shader dari near dan far. Dua rumus yang setara secara
        // matematis tapi ditulis berbeda berselisih satu irisan di tepinya.
        const Vec2 first = clusterGrid_.SliceBounds(0);
        const Vec2 last = clusterGrid_.SliceBounds(clusterGrid_.Slices() - 1);
        const float logRatio = std::log(last.y / first.x);
        const float scale = static_cast<float>(clusterGrid_.Slices()) / logRatio;
        uniforms.clusterDepth = Vec4(scale, -scale * std::log(first.x), first.x, last.y);
        uniforms.viewportSize = Vec4(static_cast<float>(target_.Width()),
                                     static_cast<float>(target_.Height()), 0.0f, 0.0f);
        // Parameter clipmap. Titik asalnya berubah tiap kali kamera menggeser
        // sebuah kaskade, jadi ia diunggah tiap frame bersama sisanya alih-alih
        // disimpan — dan satu frame yang memakai titik asal lama membaca voxel
        // milik posisi yang sudah ditinggalkan.
        const SdfClipmap& clipmap = sdfClipmap_.Volume().Clipmap();
        for (uint32_t cascade = 0; cascade < kMaxSdfCascades; ++cascade) {
            if (cascade < clipmap.CascadeCount()) {
                uniforms.sdfOrigin[cascade] =
                    Vec4(clipmap.WorldOrigin(cascade), clipmap.VoxelSize(cascade));
            }
        }
        uniforms.sdfParams = Vec4(static_cast<float>(clipmap.Settings().resolution),
                                  static_cast<float>(clipmap.CascadeCount()),
                                  clipmap.Settings().bandVoxels, kSdfMaxSteps);
        uniforms.viewProj = viewProj;
        uniforms.invViewProj = glm::inverse(viewProj);
        // Nol tingkat berarti lapis screen-space mati, dan itulah keadaan yang
        // benar saat piramidanya gagal dibuat: penelusur lalu langsung memakai
        // SDF alih-alih membaca tekstur yang tidak ada isinya.
        const float hizLevels =
            hiz_.IsValid() && desc.gi.screenTrace
                ? static_cast<float>(
                      DepthPyramid::LevelsFor(target_.Width(), target_.Height()))
                : 0.0f;
        uniforms.screenTrace = Vec4(kScreenThickness, kScreenOriginBias, kScreenMaxSteps,
                                    hizLevels);
        // Kapasitas nol berarti cache mati, dan itu keadaan yang benar saat
        // buffernya gagal dibuat: sinar SDF lalu kembali dibuang seperti di M3
        // alih-alih membaca memori yang tidak ada isinya.
        const float capacity =
            cache_.buffer != VK_NULL_HANDLE ? static_cast<float>(cacheSettings_.capacity) : 0.0f;
        uniforms.cacheParams = Vec4(capacity, cacheSettings_.cellSize,
                                    cacheSettings_.lodDistance,
                                    static_cast<float>(cacheSettings_.maxProbe));
        uniforms.cacheDecay = Vec4(static_cast<float>(cacheSettings_.accumulationFrames),
                                   static_cast<float>(cacheSettings_.staleFrames),
                                   static_cast<float>(cacheFrame_), 0.0f);
        uniforms.previousViewProj = previousViewProj_;
        uniforms.denoise =
            Vec4(static_cast<float>(probeGrid_.Settings().accumulationFrames),
                 kHistoryPlaneDistance, kHistoryNormalCosine, kHistoryClampScale);
        // **Baru berlaku sesudah probe punya isi.** Frame pertama menulis SH
        // yang belum pernah dipadu dengan apa pun, dan memakainya sebagai
        // cahaya tak-langsung membuat satu frame gelap gulita setiap kali GI
        // dinyalakan.
        const bool giReady = desc.gi.enabled && probes_.IsValid() && probeFrame_ > 1;
        uniforms.giParams = Vec4(giReady ? 1.0f : 0.0f,
                                 static_cast<float>(probeGrid_.Settings().tileSize), 0.0f, 0.0f);
        slot.shadowUniform.Write(&uniforms, sizeof(uniforms));
    }

    void RecordShadowPass(VkCommandBuffer cmd, InstanceSlot& slot, uint32_t casterCount) {
        if (shadowPipeline_ == VK_NULL_HANDLE) {
            return;
        }
        for (int i = 0; i < cascades_.count; ++i) {
            VkRenderingAttachmentInfo depth{};
            depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depth.imageView = shadow_.layerViews[static_cast<size_t>(i)];
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            // Satu, bukan nol. Cascade memakai ortografik biasa — yang terjauh
            // adalah satu di sana, kebalikan dari target viewport yang
            // reversed-Z. Keduanya hidup berdampingan di renderer ini.
            depth.clearValue.depthStencil = {1.0f, 0};

            VkRenderingInfo info{};
            info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            info.renderArea.extent = {shadow_.resolution, shadow_.resolution};
            info.layerCount = 1;
            info.pDepthAttachment = &depth;
            vkCmdBeginRendering(cmd, &info);

            const VkViewport viewport{0.0f,
                                      0.0f,
                                      static_cast<float>(shadow_.resolution),
                                      static_cast<float>(shadow_.resolution),
                                      0.0f,
                                      1.0f};
            const VkRect2D scissor{{0, 0}, {shadow_.resolution, shadow_.resolution}};
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);

            // Awalan daftar buram, yaitu yang benar-benar menjatuhkan bayangan.
            if (casterCount > 0 && slot.buffer.IsValid()) {
                const Cascade& cascade = cascades_.cascades[static_cast<size_t>(i)];
                const BoxPush push{cascade.viewProjection};
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
                vkCmdPushConstants(cmd, shadowLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                                   sizeof(BoxPush), &push);
                const std::array<VkBuffer, 2> buffers{cubeBuffer_.Handle(), slot.buffer.Handle()};
                const std::array<VkDeviceSize, 2> offsets{0, 0};
                vkCmdBindVertexBuffers(cmd, 0, 2, buffers.data(), offsets.data());
                vkCmdDraw(cmd, cubeVertexCount_, casterCount, 0, 0);
            }
            vkCmdEndRendering(cmd);
        }
    }

    void RefreshTextureHandle() {
        if (textureHandle_ != kInvalidTexture) {
            textures_.Release(textureHandle_);
        }
        textureHandle_ = textures_.Acquire(target_.ColorView(), target_.Sampler());
    }

    void Shutdown() {
        device_.WaitIdle();
        if (textureHandle_ != kInvalidTexture) {
            textures_.Release(textureHandle_);
            textureHandle_ = kInvalidTexture;
        }
        for (InstanceSlot& slot : slots_) {
            slot.buffer.Destroy();
            slot.lineBuffer.Destroy();
            slot.shadowUniform.Destroy();
            slot.lightBuffer.Destroy();
            slot.clusterRangeBuffer.Destroy();
            slot.clusterIndexBuffer.Destroy();
            slot.shadowFaceBuffer.Destroy();
            slot.sdfStaging.Destroy();
        }
        hiz_.Destroy();
        post_.Destroy();
        sky_.Destroy();
        probes_.Destroy();
        if (cache_.buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(device_.Allocator(), cache_.buffer, cache_.allocation);
            cache_.buffer = VK_NULL_HANDLE;
        }
        cubeBuffer_.Destroy();
        for (VkPipeline* pipeline :
             {&prepassPipeline_, &opaquePipeline_, &transparentPipeline_, &gridPipeline_,
              &linePipeline_, &shadowPipeline_, &sdfDebugPipeline_}) {
            if (*pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_.Handle(), *pipeline, nullptr);
                *pipeline = VK_NULL_HANDLE;
            }
        }
        DestroyShadowMap();
        DestroyShadowAtlas();
        for (VkPipelineLayout* layout :
             {&pipelineLayout_, &gridLayout_, &lineLayout_, &shadowLayout_, &sdfDebugLayout_}) {
            if (*layout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device_.Handle(), *layout, nullptr);
                *layout = VK_NULL_HANDLE;
            }
        }
        target_.Destroy();
    }

    rhi::Device& device_;
    rhi::ITextureRegistry& textures_;
    rhi::RenderTarget target_;
    rhi::GpuProfiler profiler_;
    /// Disusun ulang saat diminta, bukan disimpan tiap frame — panel Statistics
    /// biasanya tertutup, dan menyusun daftar yang tidak ada yang membacanya
    /// adalah pekerjaan yang dibuang setiap frame.
    mutable std::vector<PassTiming> timings_;
    TraceBackendSelection giBackend_;
    SdfClipmapResource sdfClipmap_;
    DepthPyramid hiz_;
    PostProcess post_;
    SkyAtmosphere sky_;
    /// Langkah waktu frame ini, dipakai adaptasi eksposur.
    float deltaSeconds_ = 0.0f;
    /// Jam angin awan. Berakumulasi dari langkah waktu, bukan dibaca dari jam
    /// dinding: awan yang bergerak mengikuti jam dinding akan meloncat setiap
    /// kali frame tersendat, dan yang meloncat pada lapisan sebesar itu terlihat
    /// sebagai seluruh langit yang bergeser.
    float cloudTimeSeconds_ = 0.0f;
    std::chrono::steady_clock::time_point lastFrameTime_{};
    bool hasLastFrameTime_ = false;
    /// Cache radiansi hash grid. Riwayat lintas frame, jadi ia bukan per slot —
    /// dan device-local, bukan host-visible: ia dibaca dan ditulis GPU tiap
    /// frame, dan memori host-visible akan menyeretnya lewat PCIe.
    struct RadianceCacheBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkDeviceSize bytes = 0;
    };
    RadianceCacheBuffer cache_;
    RadianceCacheSettings cacheSettings_;

    ProbeField probes_;
    ProbeGrid probeGrid_;
    uint32_t probeFrame_ = 0;
    Mat4 lastViewProj_{0.0f};
    bool probeReset_ = true;
    /// Nomor frame cache. Naik terus, tidak di-reset saat kamera bergerak: entri
    /// cache terikat ke dunia, bukan ke piksel.
    uint32_t cacheFrame_ = 0;
    /// Dunia → clip frame sebelumnya, dipakai reproyeksi riwayat probe.
    Mat4 previousViewProj_{1.0f};
    std::filesystem::path shaderDirectory_;
    uint64_t sdfVoxelsWritten_ = 0;
    float sdfUpdateMs_ = 0.0f;
    bool sdfDebugEnabled_ = false;
    PassId giDebugId_ = kInvalidPass;
    PassId hizPassId_ = kInvalidPass;
    PassId probePassId_ = kInvalidPass;
    bool giEnabled_ = false;
    bool screenTraceEnabled_ = false;
    TextureHandle textureHandle_ = kInvalidTexture;

    FrameGraph graph_;
    CompiledGraph compiled_;
    FrameGraphExecutor executor_;
    ResourceId colorId_ = kInvalidResource;
    ResourceId sceneId_ = kInvalidResource;
    ResourceId depthId_ = kInvalidResource;
    PassId skyId_ = kInvalidPass;
    PassId cloudId_ = kInvalidPass;
    PassId aerialId_ = kInvalidPass;
    PassId bloomId_ = kInvalidPass;
    PassId meterId_ = kInvalidPass;
    PassId tonemapId_ = kInvalidPass;
    PassId gridId_ = kInvalidPass;
    PassId prepassId_ = kInvalidPass;
    PassId linesId_ = kInvalidPass;
    PassId opaqueId_ = kInvalidPass;
    PassId transparentId_ = kInvalidPass;

    VkPipelineLayout gridLayout_ = VK_NULL_HANDLE;
    VkPipeline gridPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout lineLayout_ = VK_NULL_HANDLE;
    VkPipeline linePipeline_ = VK_NULL_HANDLE;
    std::vector<LineVertex> lineVertices_;
    /// Dipakai ulang tiap frame; ukurannya mengikuti jumlah pass graph.
    std::vector<FrameGraphExecutor::Recorder> recorders_;

    static constexpr std::size_t kMaxClusterLights = 256;
    /// Jari-jari sumber terkecil yang masih menghasilkan angka berhingga, meter.
    /// Sama dengan konstanta yang dulu tertanam di shader sebagai `1e-4` —
    /// akarnya tepat satu sentimeter.
    static constexpr float kMinSourceRadius = 0.01f;
    /// Cluster berhenti di jarak ini, bukan di `farZ` kamera. Irisan
    /// eksponensial sampai dua kilometer membuat irisan pertama setipis
    /// sentimeter, dan lampu punctual memang tidak relevan di kejauhan.
    static constexpr float kClusterFar = 300.0f;

    Vec3 sunDirection_{0.0f, 1.0f, 0.0f};
    Vec3 sunRadiance_{0.75f};
    bool sunCastsShadows_ = true;
    ClusterGridSettings clusterSettings_;
    ClusterGrid clusterGrid_;
    ClusterAssignment clusterAssignment_;
    std::vector<GpuLight> gpuLights_;
    std::vector<ClusterLight> clusterLights_;

    /// Atlas point/spot: satu bidang depth, ubinnya beragam ukuran.
    struct ShadowAtlasImage {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
    };
    ShadowAtlasImage atlas_;
    ShadowAtlasSettings atlasSettings_;
    ShadowAtlasResult atlasAllocation_;
    std::vector<GpuShadowFace> gpuFaces_;
    /// Anggaran langkah sphere tracing. Angka yang paling sering disetel saat
    /// menyeimbangkan kualitas dan biaya, jadi ia disebut sekali di sini.
    static constexpr float kSdfMaxSteps = 48.0f;
    /// Anggaran langkah lapis screen-space. Rencana GI menyebut 16, dan angka
    /// itulah yang membuat fallback ke SDF bukan kemewahan melainkan keharusan.
    static constexpr float kScreenMaxSteps = 16.0f;
    /// Ketebalan yang diandaikan untuk permukaan di depth buffer, meter.
    static constexpr float kScreenThickness = 0.5f;
    /// Dorongan awal sinar, meter — supaya permukaan asalnya sendiri tidak
    /// menghalangi sinarnya.
    static constexpr float kScreenOriginBias = 0.02f;
    /// Normal disandikan oktahedral ke dua kanal 16-bit.
    static constexpr VkFormat kNormalFormat = VK_FORMAT_R16G16_SFLOAT;
    /// Riwayat yang lebih jauh dari ini di depan bidang piksel ditolak, meter.
    static constexpr float kHistoryPlaneDistance = 0.15f;
    /// Kesamaan normal minimum sebelum riwayat ditolak.
    static constexpr float kHistoryNormalCosine = 0.7f;
    /// Seberapa longgar riwayat dijepit ke sekitar sampel frame ini. Terlalu
    /// ketat menghapus riwayat yang masih sah dan mengembalikan deraunya;
    /// terlalu longgar tidak menjepit apa pun dan ghosting-nya kembali.
    static constexpr float kHistoryClampScale = 4.0f;

    ResourceId atlasId_ = kInvalidResource;
    PassId atlasPassId_ = kInvalidPass;

    ShadowMap shadow_;
    VkDescriptorSetLayout shadowSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool shadowPool_ = VK_NULL_HANDLE;
    VkPipelineLayout shadowLayout_ = VK_NULL_HANDLE;
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout sdfDebugLayout_ = VK_NULL_HANDLE;
    VkPipeline sdfDebugPipeline_ = VK_NULL_HANDLE;
    ResourceId shadowId_ = kInvalidResource;
    PassId shadowPassId_ = kInvalidPass;
    CascadeSet cascades_;

    rhi::DynamicBuffer cubeBuffer_;
    uint32_t cubeVertexCount_ = 0;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline prepassPipeline_ = VK_NULL_HANDLE;
    VkPipeline opaquePipeline_ = VK_NULL_HANDLE;
    VkPipeline transparentPipeline_ = VK_NULL_HANDLE;

    std::array<InstanceSlot, 3> slots_;
    std::size_t slotIndex_ = 0;
    std::vector<BoxInstance> opaque_;
    std::vector<BoxInstance> transparent_;
    std::vector<BoxInstance> upload_;
    struct SortedEntry {
        float distance = 0.0f;
        BoxInstance instance;
    };
    std::vector<SortedEntry> sorted_;
    /// Banyaknya entri di awal `opaque_` yang menjatuhkan bayangan.
    uint32_t casterCount_ = 0;
    uint32_t drawnOpaque_ = 0;
    uint32_t drawnTransparent_ = 0;
};

}  // namespace

std::unique_ptr<IViewportRenderer> CreateVulkanRenderer(rhi::Device& device,
                                                        rhi::ITextureRegistry& textures,
                                                        const StubRendererDesc& desc) {
    auto renderer = std::make_unique<VulkanRenderer>(device, textures);
    if (!renderer->Initialize(desc)) {
        return nullptr;
    }
    return renderer;
}

}  // namespace sim::render
