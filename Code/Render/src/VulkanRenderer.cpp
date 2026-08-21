#include "Sim/Render/RendererFactory.h"

#include "ClusterAssign.h"
#include "ComputeGradient.h"
#include "DepthPyramid.h"
#include "DrawCull.h"

#include <format>
#include "PostProcess.h"
#include "SkyAtmosphere.h"
#include "VolumePass.h"
#include "FrameGraphExecutor.h"
#include "ProbeField.h"
#include "SdfClipmapResource.h"
#include "Sim/Assets/MeshData.h"
#include "Sim/Core/Assert.h"
#include "Sim/Core/Log.h"
#include "Sim/RHI/Buffer.h"
#include "Sim/RHI/Device.h"
#include "Sim/RHI/GpuProfiler.h"
#include "Sim/RHI/RenderTarget.h"
#include "PresentSource.h"
#include "Sim/RHI/TextureRegistry.h"
#include "Sim/Render/FrameGraph.h"
#include "Sim/Render/DrawRun.h"
#include "Sim/Render/Frustum.h"
#include "Sim/Render/LightCluster.h"
#include "Sim/Render/RadianceCache.h"
#include "Sim/Render/ShadowAtlas.h"
#include "Sim/Render/TraceBackend.h"
#include "Sim/Render/ShadowCascades.h"

#include <algorithm>
#include <chrono>
#include <tuple>
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

/// Tahap yang boleh membaca `BoxPush`.
///
/// **Fragment ikut, dan ia wajib ikut di ketiga layout sekaligus.** Dua pipeline
/// layout bersifat *compatible* — yang membuat set 0..1 bertahan saat pipeline
/// material diikat di tengah gelung ruas — hanya kalau push constant range-nya
/// sama persis, `stageFlags` termasuk. Menambahkannya di layout forward saja
/// membuat pass bayangan diam-diam melepas set yang sudah diikat.
constexpr VkShaderStageFlags kBoxPushStages =
    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

/// Slot material di larik bindless.
///
/// **Disebut angka, bukan dibiarkan tumbuh** — aturan yang sama dengan
/// `kMaxMaterialSets` di jalur mundur. Larik yang dibuat ulang saat penuh
/// berarti descriptor set yang ditulis ulang sementara frame sebelumnya masih
/// membacanya, dan itu kerusakan yang muncul beberapa detik sesudah sebabnya.
constexpr uint32_t kMaxBindlessMaterials = 1024;

/// Membulatkan ke kelipatan 16, penjajaran minimum sebuah constant buffer.
constexpr std::size_t AlignUp16(std::size_t bytes) {
    return (bytes + 15) & ~static_cast<std::size_t>(15);
}

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
    /// x 1 kalau iradiansi GI berlaku, y ukuran ubin probe dalam piksel,
    /// z banyaknya grid medan jarak yang punya entri frame ini, w 1 saat uji
    /// tungku.
    Vec4 giParams{0.0f};
    /// Langit untuk GI: x pengali radiansi, y ketinggian kamera (km),
    /// zw ukuran LUT sky-view. **Pengali nol berarti tidak ada atmosfer** —
    /// langit mati atau HDRI — dan penelusur GI kembali ke gradien cadangannya.
    Vec4 skyParams{0.0f};
    /// Penutup rekursi untuk permukaan yang belum dikenal cache radiansi:
    /// xyz albedo yang ditebak, w 1 kalau tebakan itu dipakai.
    Vec4 giBounce{0.0f};
};
// 7 mat4 + 22 vec4. Angkanya ditulis eksplisit supaya menambah medan tanpa
// memperbarui shader-nya menjadi galat kompilasi, bukan bayangan yang bergeser.
static_assert(sizeof(ShadowUniforms) == 7 * 64 + 22 * 16,
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

/// **Tata letaknya harus sama persis dengan `assets::MeshVertex`.** Selama
/// keduanya cocok, kubus bawaan dan mesh yang diimpor memakai pipeline yang
/// sama — dan pipeline kedua yang hanya berbeda pada offset atribut adalah
/// pipeline yang suatu saat berbeda pada hal lain tanpa ada yang menyadarinya.
///
/// `uv` belum dibaca shader mana pun; ia ada karena mesh yang diimpor membawanya
/// dan membuang atribut yang sudah ada di berkasnya berarti mengimpornya lagi
/// begitu material bertekstur mendarat.
/// Nilai satuan warna simpul: putih pekat.
constexpr Vec4 kWhite{1.0f, 1.0f, 1.0f, 1.0f};

struct BoxVertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
    /// Tangent ruang dunia, arah tangan di `w`. Lihat `assets::MeshVertex`.
    Vec4 tangent;
    /// Warna per simpul, dikalikan dengan warna ruas/instance. Putih adalah
    /// nilai satuannya, jadi mesh yang tidak mengisinya tergambar persis seperti
    /// sebelum atribut ini ada.
    Vec4 color;
};

static_assert(sizeof(BoxVertex) == sizeof(assets::MeshVertex),
              "BoxVertex dan assets::MeshVertex harus setata letak — keduanya "
              "memakai pipeline yang sama, dan selisih satu byte menggeser "
              "seluruh mesh alih-alih menghasilkan galat");
static_assert(offsetof(BoxVertex, normal) == offsetof(assets::MeshVertex, normal),
              "offset normal harus sama");
static_assert(offsetof(BoxVertex, uv) == offsetof(assets::MeshVertex, uv),
              "offset uv harus sama");
static_assert(offsetof(BoxVertex, tangent) == offsetof(assets::MeshVertex, tangent),
              "offset tangent harus sama");

// `assets::SkinInfluence` diunggah apa adanya sebagai buffer skin, dan
// `VkVertexInputAttributeDescription` di bawah menyebut formatnya secara
// terpisah. Keduanya harus sepakat: indeks yang terbaca sebagai bobot tidak
// menghasilkan galat apa pun — hanya kulit yang mengikuti bone yang salah.
static_assert(sizeof(assets::SkinInfluence) == 24,
              "SkinInfluence harus 24 byte: empat uint16 lalu empat float");
static_assert(offsetof(assets::SkinInfluence, bones) == 0, "bones harus di offset 0");
static_assert(offsetof(assets::SkinInfluence, weights) == 8, "weights harus di offset 8");

/// Satu instance kotak. Tata letaknya harus sama persis dengan atribut instance
/// di Shaders/box.vert.
struct BoxInstance {
    /// **Transform-nya tidak di sini melainkan di storage buffer.** Ia pindah
    /// supaya pipeline material — yang memang membacanya dari buffer — bisa
    /// menggantikan `box.frag` tanpa mengubah cara instance dikumpulkan; yang
    /// ikut hilang 64 byte per instance dan empat lokasi atribut yang harus
    /// disediakan setiap pipeline yang tidak memakainya.
    Vec4 color;
    /// Bit 0: menerima bayangan. Sebuah bitmask, bukan float bernilai 0/1 —
    /// bendera per-instance berikutnya tinggal mengambil bit berikutnya alih-alih
    /// menuntut atribut vertex baru.
    uint32_t flags;
    /// Indeks matriks pertama instance ini di dalam buffer palet kulit.
    ///
    /// **Medan sendiri, bukan bit-bit sisa `flags`.** Ia bukan bendera: ia
    /// dijumlahkan dengan indeks bone di shader, dan angka yang harus dibongkar
    /// dari sebuah bitmask lebih dulu adalah angka yang suatu saat dibongkar
    /// dengan pergeseran yang salah — tanpa satu pun galat, hanya karakter yang
    /// memakai pose karakter lain.
    uint32_t skinBase;
    /// Slot material dan slot tekstur ruas ini di larik bindless.
    ///
    /// **Di sini, bukan di push constant, dan itu yang mengembalikan
    /// instancing.** Sampai G6 keduanya dikirim per panggilan gambar bersama
    /// warna ruas, jadi dua entity bermesh sama dengan material berbeda adalah
    /// dua draw. Karena `partColorFirst` milik tiap entity sendiri, kenyataannya
    /// lebih buruk: **setiap** entity menjadi satu draw, dan adegan tiga ribu
    /// prop dari empat mesh menghasilkan tiga ribu panggilan.
    ///
    /// Yang membuatnya bisa pindah adalah G5: sebelum material terindeks,
    /// "material ruas ini" bukan sebuah nomor melainkan sebuah descriptor set.
    uint32_t materialSlot;
    uint32_t textureSlot;
};

/// Satu panggilan gambar: sebuah mesh dan ruas instance yang memakainya.
///
/// **Ruas, bukan daftar per instance.** Seluruh instance yang memakai mesh yang
/// sama digambar dengan satu `vkCmdDrawIndexed` ber-`instanceCount` — bentuk
/// yang sama dengan sebelum importir mesh ada, hanya kini ada lebih dari satu
/// geometri untuk dikelompokkan.
/// Bit 0 dari `BoxInstance::flags`. Harus sama dengan `kReceiveShadows` di
/// Shaders/box.frag.
constexpr uint32_t kInstanceReceiveShadows = 1u;
/// Bit 1 dulu berarti "warna instance mengalahkan warna material ruas", dipakai
/// sorotan seleksi. **Ia hilang di G6**: warna ruas diselesaikan CPU sebelum
/// diunggah, jadi yang sampai ke shader satu warna dan tidak ada lagi dua warna
/// yang harus dipilih di sana. Nomor bitnya sengaja tidak dipakai ulang —
/// bendera yang berpindah arti adalah bendera yang salah terbaca oleh kode yang
/// belum ikut berubah.

/// Dua varian dari satu pasang shader: tanpa kulit dan dengan kulit.
///
/// **Dua pipeline, bukan satu dengan cabang runtime.** Yang membedakannya bukan
/// hanya konstanta spesialisasinya melainkan juga stride binding skin — nol pada
/// yang statis — dan stride adalah bagian dari pipeline, bukan sesuatu yang bisa
/// diganti per draw.
constexpr std::size_t kPipelineVariants = 2;
/// Indeks 0 tanpa kulit, indeks 1 dengan kulit.
using PipelineVariants = std::array<VkPipeline, kPipelineVariants>;

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
        // Tangent kubus sudah diketahui per muka, jadi ia ditulis langsung
        // alih-alih diturunkan dari UV: kubus ini tidak pernah lewat importir.
        const Vec4 tangent(t, 1.0f);
        vertices.push_back({a0, n, Vec2(0.0f, 0.0f), tangent, kWhite});
        vertices.push_back({a1, n, Vec2(1.0f, 0.0f), tangent, kWhite});
        vertices.push_back({a2, n, Vec2(1.0f, 1.0f), tangent, kWhite});
        vertices.push_back({a0, n, Vec2(0.0f, 0.0f), tangent, kWhite});
        vertices.push_back({a2, n, Vec2(1.0f, 1.0f), tangent, kWhite});
        vertices.push_back({a3, n, Vec2(0.0f, 1.0f), tangent, kWhite});
    }
    return vertices;
}

BoxInstance MakeInstance(const Vec4& color, bool receiveShadows, uint32_t skinBase,
                         uint32_t materialSlot, uint32_t textureSlot) {
    BoxInstance instance;
    instance.color = color;
    instance.flags = receiveShadows ? kInstanceReceiveShadows : 0u;
    instance.skinBase = skinBase;
    instance.materialSlot = materialSlot;
    instance.textureSlot = textureSlot;
    return instance;
}

/// Bentuk matriks model seperti yang dibaca shader dari storage buffer.
///
/// **Apa adanya, tanpa transpose — dan itu diperiksa, bukan diasumsikan.**
/// Godaannya besar untuk menambahkan transpose di sini: Slang mendekorasi
/// `float4x4` di dalam StructuredBuffer dengan `RowMajor`, sementara `glm::mat4`
/// menyimpan kolom demi kolom, dan kedua kata itu terbaca bertentangan.
///
/// Yang menyelesaikannya adalah bukti yang sudah ada di repo ini: `BoxPush`
/// mengunggah `viewProj` sebagai `glm::mat4` mentah, slangc mendekorasinya
/// **dengan cara yang sama persis** (`OpMemberDecorate ... RowMajor`,
/// MatrixStride 16), `mul` pada keduanya menjadi `OpVectorTimesMatrix` yang
/// sama, dan jalur itu sudah menggambar dengan benar sejak E8.1. Perlakuan yang
/// identik menuntut unggahan yang identik.
///
/// Transpose yang salah di sini tidak menghasilkan galat apa pun — hanya setiap
/// objek di tempat yang salah dengan orientasi yang salah.
Mat4 InstanceTransform(const Mat4& model) {
    return model;
}

/// Pengukur waktu CPU yang menuliskan hasilnya saat ia keluar dari lingkup.
///
/// **Bukan profiler, dan tidak berniat menjadi profiler.** Tidak bersarang,
/// tidak berpohon, dan namanya ditulis tangan di beberapa tempat saja. Itu
/// cukup untuk pertanyaan yang sedang ditanyakan G0 — tahap CPU mana yang mahal
/// sebelum ada yang dipindahkan ke GPU — dan alat yang lebih besar daripada
/// pertanyaannya adalah alat yang harus dirawat sebelum ada yang memakainya.
///
/// Namanya `string_view` ke literal, sama dengan `PassTiming` di sisi GPU:
/// menyalin string per lingkup per frame adalah alokasi yang diperkenalkan oleh
/// alat ukur ke dalam hal yang sedang diukurnya.
class CpuScope {
public:
    CpuScope(std::vector<PassTiming>& into, std::string_view name)
        : into_(into), name_(name), start_(std::chrono::steady_clock::now()) {}

    ~CpuScope() {
        const double milliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_)
                .count();
        into_.push_back(PassTiming{name_, static_cast<float>(milliseconds)});
    }

    CpuScope(const CpuScope&) = delete;
    CpuScope& operator=(const CpuScope&) = delete;

private:
    std::vector<PassTiming>& into_;
    std::string_view name_;
    std::chrono::steady_clock::time_point start_;
};

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
class VulkanRenderer final : public IViewportRenderer, public IPresentSource {
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
        SelectMaterialBinding(desc.materialBinding);
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
                                              sizeof(GpuShadowFace) * 64) ||
                !slot.skinBuffer.Create(device_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        sizeof(Mat4) * 256) ||
                !slot.instanceBuffer.Create(device_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                            sizeof(Mat4) * 256)) {
                return false;
            }
        }
        // Pengaruh skin tiruan untuk jalur tak-ber-kulit.
        //
        // **Dipasang lewat binding ber-stride nol, jadi satu elemen melayani
        // seluruh vertex mesh apa pun.** Slang mengunci daftar atribut entry
        // point sebelum `kSkinned` dinilai, jadi pipeline statis pun harus
        // menyediakan kedua atribut skin; menyediakannya dengan buffer sepanjang
        // mesh berarti 24 byte per vertex nol yang tidak pernah dibaca siapa pun.
        {
            const assets::SkinInfluence empty{};
            if (!dummySkin_.Create(device_, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, sizeof(empty)) ||
                !dummySkin_.Write(&empty, sizeof(empty))) {
                return false;
            }
        }
        // Profiler boleh gagal dibuat; renderer tetap jalan tanpa tabel waktu.
        profiler_.Create(device_);

        // 64³ dan bukan 128³ yang diminta rencana. Batasnya dulu komposit CPU:
        // medan jaraknya dievaluasi per voxel, dan 128³ berarti delapan kali
        // pekerjaan itu. **Batas itu sudah hilang di G4** — kompositnya
        // sekarang dispatch, dan `cpu-sdf` tinggal 0,079 ms melawan anggaran
        // 0,4 ms. Angkanya dibiarkan 64³ di sini karena menaikkannya adalah
        // keputusan kualitas GI yang harus dibuka dengan pengukurannya sendiri,
        // bukan efek samping milestone yang membuka jalannya.
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
            // Komposit clipmap di compute. Gagal menyiapkannya bukan kegagalan
            // renderer: jalur CPU-nya masih ada dan masih benar, hanya jauh
            // lebih mahal.
            if (!sdfClipmap_.CreateGpuFill(shaderDirectory_)) {
                SIM_WARN("Render", "GPU SDF composite unavailable; staying on the CPU path");
            }
        }
        static_assert(std::tuple_size_v<decltype(slots_)> == SdfClipmapResource::kSlots,
                      "SdfClipmapResource::kSlots harus sama dengan banyaknya slot frame");
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
        // Format target tampilan, bukan format gambar HDR: pemeriksa jalur
        // compute menggambar **sesudah** tone mapping, supaya yang terlihat
        // adalah warna yang ditulis dispatch dan bukan warna itu setelah
        // dipetakan operator nada.
        // Penetapan cluster di GPU. Gagal membuatnya bukan kegagalan renderer:
        // jalur CPU-nya masih ada dan masih benar.
        // Culling GPU. **Gagal dibuat bukan kegagalan fatal**: jalur CPU tetap
        // ada, dan ia memang harus tetap ada — lihat catatan di `DrawCull`.
        if (device_.Capabilities().multiDrawIndirect) {
            if (!drawCull_.Create(device_, shaderDirectory_) ||
                !occlusion_.Create(device_, shaderDirectory_, DepthReduce::Farthest)) {
                SIM_WARN("Render", "GPU draw culling unavailable; culling stays on the CPU");
                drawCull_.Destroy();
                occlusion_.Destroy();
            } else {
                // Piramidanya diadopsi di sini, sesudah keduanya ada — bukan di
                // sebelah `hiz_`, yang dibuat lebih dulu. Descriptor culling
                // menunjuk piramida ini, dan yang belum ditulis membuat dispatch
                // tidak pernah berjalan: buffer perintahnya lalu berisi sampah,
                // dan yang terlihat adalah adegan yang kosong sama sekali.
                occlusion_.Adopt(target_.AllocatedWidth(), target_.AllocatedHeight(),
                                 target_.DepthView(), target_.Sampler());
                occlusion_.AdoptLayouts();
                drawCull_.AdoptPyramid(occlusion_.View(), occlusion_.Sampler(),
                                       target_.DepthView(), target_.Sampler());
            }
        } else {
            SIM_INFO("Render",
                     "this GPU has no multi-draw indirect; culling stays on the CPU");
        }
        if (clusterAssign_.Create(device_, shaderDirectory_)) {
            clusterGrid_.Build(clusterSettings_, 1.0f, 1.0f, 0.1f, kClusterFar);
            clusterAssign_.Adopt(clusterGrid_.ClusterCount(),
                                 clusterSettings_.maxLightsPerCluster);
        } else {
            SIM_WARN("Render", "GPU cluster assignment unavailable; staying on the CPU path");
        }
        if (gradient_.Create(device_, shaderDirectory_, target_.ColorFormat())) {
            gradient_.Adopt(target_.AllocatedWidth(), target_.AllocatedHeight());
            gradient_.AdoptLayout(device_);
        } else {
            // Dikatakan sekarang, bukan dibiarkan sebagai sakelar yang tidak
            // melakukan apa-apa. Yang gagal di sini adalah alat yang gunanya
            // justru menjelaskan kegagalan, dan alat semacam itu yang diam
            // adalah yang paling menyesatkan: sakelarnya menyala, layarnya
            // tidak berubah, dan tidak ada satu pun petunjuk kenapa.
            SIM_WARN("Render",
                     "compute path unavailable; the gradient debug view will do nothing");
        }
        if (sky_.Create(device_, shaderDirectory_, PostProcess::kSceneFormat)) {
            sky_.AdoptLayouts();
            sky_.AdoptDepth(target_.DepthView(), target_.Sampler());
            // Sesudah AdoptDepth, karena set descriptor awan menunjuk depth
            // buffer dan menulisnya sekali di sini lebih murah daripada
            // menuliskannya lagi tiap frame.
            sky_.CreateClouds(shaderDirectory_, PostProcess::kSceneFormat);
        }
        // Pass volume. Pipeline-nya dibuat sekali; volumenya sendiri diunggah
        // belakangan, ketika ada `ViewportDesc::volume` yang membawanya.
        volumePass_.Create(device_, shaderDirectory_, PostProcess::kSceneFormat);
        CreateRadianceCache();
        if (probes_.Create(device_, shaderDirectory_, shadowSetLayout_)) {
            probes_.Adopt(target_.AllocatedWidth(), target_.AllocatedHeight(), kNormalFormat);
            probes_.AdoptLayouts();
        }
        if (!WriteShadowDescriptors()) {
            return false;
        }
        WriteSkinDescriptors();
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
        if (occlusion_.IsValid() &&
            occlusion_.Adopt(target_.AllocatedWidth(), target_.AllocatedHeight(),
                             target_.DepthView(), target_.Sampler())) {
            occlusion_.AdoptLayouts();
            drawCull_.AdoptPyramid(occlusion_.View(), occlusion_.Sampler(),
                                   target_.DepthView(), target_.Sampler());
        }
        const bool probesChanged =
            probes_.Adopt(target_.AllocatedWidth(), target_.AllocatedHeight(), kNormalFormat);
        const bool postChanged =
            post_.Adopt(target_.AllocatedWidth(), target_.AllocatedHeight());
        if (postChanged) {
            post_.AdoptLayouts();
        }
        if (gradient_.Adopt(target_.AllocatedWidth(), target_.AllocatedHeight())) {
            gradient_.AdoptLayout(device_);
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
        // **Dikosongkan di sini, bukan di akhir frame sebelumnya.** Frame yang
        // keluar lebih awal karena targetnya belum sah tidak boleh menghapus
        // angka terakhir yang benar — tabel yang berkedip kosong setiap kali
        // panel diubah ukurannya tidak bisa dibaca siapa pun.
        cpuTimings_.clear();
        // **Dinolkan sebelum perekaman, bukan bersama angka yang lain.** Sisa
        // isi `stats_` diisi di ujung `Render`, sesudah frame graph selesai
        // merekam; kedua hitungan ini justru dijumlahkan *selama* perekaman itu,
        // jadi menolkannya di sana berarti menolkan hasilnya sendiri.
        stats_.descriptorSetBinds = 0;
        stats_.drawCalls = 0;
        const CpuScope totalScope(cpuTimings_, "cpu-total");
        const float aspect =
            static_cast<float>(target_.Width()) / static_cast<float>(target_.Height());
        // Reversed-Z: near di 1, far di 0. Konsekuensinya depth di-clear ke 0 dan
        // ujinya GREATER — ketiganya harus berpindah bersama, dan memisahkannya
        // menghasilkan layar yang kosong tanpa satu pun pesan galat.
        const Mat4 projection =
            PerspectiveReversedZ(desc.camera.fovYRadians, aspect, desc.camera.nearZ,
                                 desc.camera.farZ);
        const Mat4 viewProj = projection * desc.camera.View();

        {
            const CpuScope scope(cpuTimings_, "cpu-gather");
            Gather(desc, scene, viewProj);
        }

        // **Culling pindah ke GPU, dan CPU berhenti memutuskan apa yang
        // digambar.** Yang diunggah adalah kotak tiap permukaan dan rentang
        // indeksnya; yang kembali sebuah buffer perintah gambar. Jalur CPU tetap
        // ada di sebelahnya — ia jalur mundur untuk perangkat tanpa indirect
        // draw, dan ia pembanding: dua implementasi yang harus sepakat adalah
        // cara termurah menemukan yang mana yang salah.
        gpuCullActive_ = desc.gpuCull && drawCull_.IsValid() && !cullSurfaces_.empty();
        gpuOcclusionActive_ = gpuCullActive_ && desc.gpuOcclusion && occlusion_.IsValid();
        cullDebugActive_ = gpuOcclusionActive_ && desc.cullDebug;
        cullLimit_ = desc.cullLimit;
        cullFirst_ = desc.cullFirst;
        asyncComputeActive_ = desc.asyncCompute && device_.Capabilities().asyncCompute &&
                              device_.Capabilities().timelineSemaphore;
        // **Tiap frame, dan tanpa syarat.** Ia berhenti sendiri kalau tidak ada
        // yang berubah, dan itu yang membuatnya murah — sementara menggantungkan
        // penulisan descriptor pada sebuah syarat adalah cara paling mudah
        // membuatnya tidak pernah ditulis sama sekali.
        //
        // **Pernah begitu, dan akibatnya diam:** binding piramida baru sah
        // sesudah `occlusion_` ada, yang terjadi belakangan daripada
        // `DrawCull::Create`. Penulisan descriptor yang menunggunya lalu tidak
        // pernah kejadian pada jalur yang occlusion-nya mati — dan dispatch
        // membaca descriptor tak terdefinisi, yaitu perintah gambar berisi
        // sampah. Validation layer melaporkannya dengan jelas; yang tidak
        // melaporkannya adalah gambarnya, yang cuma sedikit berbeda.
        if (gpuCullActive_) {
            drawCull_.AdoptPyramid(occlusion_.View(), occlusion_.Sampler(),
                                   target_.DepthView(), target_.Sampler());
        }
        lastCullSlot_ = static_cast<uint32_t>(slotIndex_);
        lastViewProjection_ = viewProj;
        if (gpuCullActive_) {
            const CpuScope scope(cpuTimings_, "cpu-draw-cull");
            gpuCullActive_ = drawCull_.Upload(
                static_cast<uint32_t>(slotIndex_), Frustum(viewProj), viewProj, target_.Width(),
                target_.Height(), DepthPyramid::LevelsFor(target_.Width(), target_.Height()),
                cullBounds_, cullSurfaces_);
        }

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
            transformUpload_.clear();
            transformUpload_.insert(transformUpload_.end(), opaqueTransforms_.begin(),
                                    opaqueTransforms_.end());
            transformUpload_.insert(transformUpload_.end(), transparentTransforms_.begin(),
                                    transparentTransforms_.end());
            const VkDeviceSize transformBytes = sizeof(Mat4) * transformUpload_.size();
            const uint64_t beforeTransforms = slot.instanceBuffer.Generation();
            slotReady =
                slot.buffer.Reserve(sizeof(BoxInstance) * upload_.size()) &&
                slot.buffer.Write(upload_.data(), sizeof(BoxInstance) * upload_.size()) &&
                slot.instanceBuffer.Reserve(transformBytes) &&
                slot.instanceBuffer.Write(transformUpload_.data(), transformBytes);
            if (slot.instanceBuffer.Generation() != beforeTransforms) {
                // **Generasi, bukan handle.** Buffer yang dibuat ulang lazimnya
                // mendapat `VkBuffer` dengan angka yang sama persis dengan yang
                // baru saja dimusnahkan, dan penjaga yang membandingkan handle
                // lalu tidak melihat apa-apa — descriptor dibiarkan menunjuk
                // memori yang sudah dibebaskan. Lihat catatan di
                // `DynamicBuffer::Generation`.
                WriteSkinDescriptor(slot);
            }
        }

        // **Paletnya diunggah utuh, bukan per instance yang lolos culling.**
        // Indeks yang dipegang tiap instance menunjuk ke larik milik pemanggil,
        // jadi memadatkannya berarti menomori ulang seluruhnya — satu lintasan
        // tambahan untuk menghemat penyalinan yang sudah satu `memcpy`. Karakter
        // yang keluar frustum tetap membayar paletnya; puluhan kilobyte per frame
        // adalah harga yang jauh lebih murah daripada pemetaan ulang yang salah.
        if (!scene.skinMatrices.empty()) {
            const VkDeviceSize bytes = sizeof(Mat4) * scene.skinMatrices.size();
            const uint64_t before = slot.skinBuffer.Generation();
            if (!slot.skinBuffer.Reserve(bytes) ||
                !slot.skinBuffer.Write(scene.skinMatrices.data(), bytes)) {
                SIM_WARN("Render", "cannot upload {} skin matrices", scene.skinMatrices.size());
            } else if (slot.skinBuffer.Generation() != before) {
                // Buffer yang tumbuh adalah `VkBuffer` yang lain. Descriptor yang
                // masih menunjuk yang lama membaca memori yang sudah dibebaskan.
                // Hanya set slot ini yang ditulis ulang — slot lain punya buffer
                // sendiri, dan bisa saja masih dibaca GPU.
                WriteSkinDescriptor(slot);
            }
        }

        partColors_.assign(scene.partColors.begin(), scene.partColors.end());
        partTextures_.assign(scene.partTextures.begin(), scene.partTextures.end());
        partMaterials_.assign(scene.partMaterials.begin(), scene.partMaterials.end());

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
        //
        // Kecuali bila pemanggil memaksakan angkanya lewat
        // `ViewportDesc::fixedDeltaSeconds`. Yang melakukannya hanya mode ukur,
        // dan yang dibelinya adalah frame yang bisa diulang: dua jalan dengan
        // binary yang sama menghasilkan gambar yang sama persis, sehingga
        // perbedaan piksel benar-benar berarti perubahan kode.
        {
            if (desc.fixedDeltaSeconds >= 0.0f) {
                deltaSeconds_ = std::min(desc.fixedDeltaSeconds, 0.25f);
            } else {
                const auto now = std::chrono::steady_clock::now();
                deltaSeconds_ =
                    hasLastFrameTime_
                        ? std::min(std::chrono::duration<float>(now - lastFrameTime_).count(),
                                   0.25f)
                        : 0.0f;
                lastFrameTime_ = now;
                hasLastFrameTime_ = true;
            }
            cloudTimeSeconds_ += deltaSeconds_;
        }
        sdfVoxelsWritten_ = 0;
        sdfUpdateMs_ = 0.0f;
        if (desc.gi.enabled && sdfClipmap_.IsValid()) {
            // Dua pengukur untuk satu blok, dan keduanya punya pembacanya
            // sendiri: `sdfUpdateMs_` sudah dipakai panel GI terhadap anggaran
            // 0,4 ms rencana GI, sementara lingkup CPU membuatnya muncul di
            // tabel yang sama dengan tahap lain — tanpa itu, tahap termahal
            // frame ini tidak ada di tabel mana pun.
            sdfClipmap_.SetGpuFill(desc.gpuSdf);
            const CpuScope scope(cpuTimings_, "cpu-sdf");
            const auto started = std::chrono::steady_clock::now();
            // Medan jarak per instance, disusun dari peta per mesh. Larik ini
            // sejajar `scene.meshes`, dan nullptr berarti mesh itu belum dibake.
            meshFieldPointers_.clear();
            meshFieldPointers_.reserve(scene.meshes.size());
            for (const MeshInstance& instance : scene.meshes) {
                const auto index = static_cast<std::size_t>(instance.mesh);
                meshFieldPointers_.push_back(
                    index < meshFields_.size() ? meshFields_[index].get() : nullptr);
            }
            sdfVoxelsWritten_ =
                sdfClipmap_.Update(desc.camera.position, scene.meshes, meshFieldPointers_,
                                   slot.sdfStaging, static_cast<uint32_t>(slotIndex_));
            sdfUpdateMs_ = std::chrono::duration<float, std::milli>(
                               std::chrono::steady_clock::now() - started)
                               .count();
            // **Menunggu device idle, sekali per grid baru.** Descriptor set
            // frame ini bisa saja masih dipegang command buffer yang belum
            // selesai, dan menulisinya adalah pelanggaran. Yang dibayar satu
            // hentakan pada frame sebuah mesh selesai dibake — dan mesh itu
            // baru saja menghabiskan tujuh detik di baker.
            if (sdfClipmap_.GridGeneration() != sdfGridGeneration_) {
                sdfGridGeneration_ = sdfClipmap_.GridGeneration();
                device_.WaitIdle();
                WriteGiGridDescriptors();
            }
        }

        // Peta HDR dimuat sebelum graph dibangun, karena graph memutuskan ada
        // atau tidaknya pass langit dari berhasil-tidaknya pemuatan ini.
        // `SetHdri` sendiri tidak melakukan apa-apa bila jalurnya tidak berubah.
        if (desc.skySource == SkySource::HdrMap) {
            sky_.SetHdri(std::filesystem::path(desc.hdriPath));
        }

        // Volume diunggah hanya ketika revisinya berubah. Ia berharga puluhan
        // megabyte dan menyubmit sendiri lalu menunggu queue idle — jalur yang
        // benar untuk sekali muat dan salah untuk apa pun yang berulang tiap
        // frame. Membandingkan pointer saja tidak cukup: volume kedua bisa
        // mendarat di alamat yang baru saja dibebaskan volume pertama.
        if (volumePass_.IsValid()) {
            if (desc.volume.grid == nullptr) {
                if (volumeUploaded_) {
                    volumePass_.ClearVolume();
                    volumeUploaded_ = false;
                }
            } else if (!volumeUploaded_ || desc.volume.revision != uploadedVolumeRevision_) {
                volumeUploaded_ = volumePass_.SetVolume(*desc.volume.grid,
                                                        VolumeTextureFormat::R8Unorm);
                uploadedVolumeRevision_ = desc.volume.revision;
            }
        }

        // **Berpindah jalur menuntut device menganggur.** Yang berubah bukan
        // sebuah angka melainkan buffer yang ditunjuk descriptor set, dan
        // menulis ulang descriptor yang masih dipakai command buffer yang belum
        // selesai adalah perilaku tak terdefinisi. Harganya satu hentakan pada
        // frame tempat sakelarnya ditekan — dan sakelar itu ditekan seseorang
        // yang sedang membandingkan dua jalur, bukan enam puluh kali sedetik.
        const bool wantGpuClusters = desc.gpuClusters && clusterAssign_.IsValid();
        if (wantGpuClusters != gpuClustersActive_) {
            device_.WaitIdle();
            gpuClustersActive_ = wantGpuClusters;
            UpdateClusterDescriptors();
            SIM_INFO("Render", "cluster assignment now runs on the {}",
                     gpuClustersActive_ ? "GPU" : "CPU");
        }

        {
            const CpuScope scope(cpuTimings_, "cpu-clusters");
            UpdateClusters(desc, scene, aspect, slot);
        }
        UpdateShadowUniforms(desc, viewProj, slot);
        BuildGraph(desc);

        // Perekaman dan submit dihitung satu lingkup. Memisahkan keduanya
        // terdengar lebih rinci dan tidak: `vkQueueSubmit` adalah biaya CPU yang
        // besarnya ditentukan oleh berapa banyak yang direkam sebelumnya, jadi
        // dua angka yang bergerak bersama hanya menambah satu baris tabel.
        const CpuScope recordScope(cpuTimings_, "cpu-record");
        VkCommandBuffer cmd = device_.BeginTransient();
        profiler_.BeginFrame(cmd);
        // Sebelum pass mana pun, supaya kaskade yang dibaca `gi-sdf-debug` dan
        // nanti pass GI adalah kaskade posisi kamera frame ini — bukan posisi
        // frame sebelumnya.
        // Dibungkus lingkup ukur sendiri, di luar frame graph. Ia bukan pass
        // graph — graph melacak resource sebagai satu kesatuan, sementara ini
        // menulis potongan toroidal sebuah tekstur yang layout-nya diurus
        // `SdfClipmapResource` — tetapi biayanya harus tetap terlihat. Sejak
        // kompositnya pindah ke compute (G4), yang direkam di sini bukan lagi
        // salinan melainkan pekerjaan sungguhan, dan pekerjaan yang tidak
        // muncul di tabel mana pun adalah pekerjaan yang dianggap gratis.
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
                // **Prepass memakai perintah fase frustum, forward memakai fase
                // occlusion.** Prepass yang ikut menyaring occlusion akan
                // menyaring dirinya sendiri: piramidanya dibangun dari depth
                // yang baru saja ditulis prepass itu.
                DrawInstances(command, prepassPipelines_, push, slot, MainViewRuns(), 0,
                              /*materialVariant=*/-1,
                              gpuCullActive_
                                  ? drawCull_.CommandBuffer(static_cast<uint32_t>(slotIndex_))
                                  : VK_NULL_HANDLE,
                              /*skipMasked=*/true);
            }
            vkCmdEndRendering(command);
            probes_.RecordNormalEnd(command);
        };
        recorders[opaqueId_] = [&](VkCommandBuffer command) {
            BeginRendering(command, desc, /*clearColor=*/false, /*loadDepth=*/true,
                           /*writeColor=*/true);
            if (slotReady && opaqueCount > 0) {
                DrawInstances(command, opaquePipelines_, push, slot, MainViewRuns(), 0,
                              /*materialVariant=*/0, OpaqueCommandBuffer());
            }
            vkCmdEndRendering(command);
        };
        recorders[transparentId_] = [&](VkCommandBuffer command) {
            BeginRendering(command, desc, /*clearColor=*/false, /*loadDepth=*/true,
                           /*writeColor=*/true);
            if (slotReady && transparentCount > 0) {
                DrawInstances(command, transparentPipelines_, push, slot, transparentRuns_,
                              opaqueCount, /*materialVariant=*/1);
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
                const SdfClipmap& clipmap = sdfClipmap_.Volume().Clipmap();
                // Diikat ke voxel kaskade terhalus, bukan ke satu angka meter:
                // yang harus dilewati bias ini adalah ambang berhenti sphere
                // tracing, dan ambang itu sendiri setengah voxel.
                // **Dalam voxel, bukan dalam meter.** Ambang "mengenai" sphere
                // tracing setengah voxel *kaskade tempat titiknya berada*, dan
                // kaskade terkasar bervoxel 1,6 m — enam belas kali kaskade
                // terhalus. Bias yang dihitung sekali dari kaskade nol karena
                // itu terlalu kecil di mana-mana kecuali di dekat kamera, dan
                // probe yang biasnya terlalu kecil terjepit di ambang: medan
                // tidak punya jawaban untuk satu pun sinarnya. Yang mengalikan
                // dengan ukuran voxel setempat adalah shader-nya.
                const float normalBias = probeGrid_.Settings().normalBiasVoxels;
                probes_.Record(command, slot.shadowSet, probeGrid_, probeFrame_,
                               clipmap.MaxRange(), normalBias, probeReset_);
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
                    BindSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, sdfDebugLayout_, 0, 1,
                             &slot.shadowSet, 0, nullptr);
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

        if (volumeId_ != kInvalidPass) {
            recorders[volumeId_] = [&](VkCommandBuffer command) {
                BeginRendering(command, desc, /*clearColor=*/false, /*loadDepth=*/false,
                               /*writeColor=*/true, /*useDepth=*/false);
                volumePass_.RecordDraw(command, invViewProj, desc.camera.position, desc.volume);
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

        if (drawCullPassId_ != kInvalidPass) {
            const auto cullSlot = static_cast<uint32_t>(slotIndex_);
            executor_.Bind(drawCommandId_,
                           BoundBuffer{drawCull_.CommandBuffer(cullSlot), 0, VK_WHOLE_SIZE});
            recorders[drawCullPassId_] = [this, cullSlot](VkCommandBuffer command) {
                drawCull_.Record(command, cullSlot, DrawCull::Phase::Frustum, false, cullLimit_,
                                 cullFirst_);
            };
            if (drawCullLatePassId_ != kInvalidPass) {
                executor_.Bind(visibleCommandId_,
                               BoundBuffer{drawCull_.VisibleCommandBuffer(cullSlot), 0,
                                           VK_WHOLE_SIZE});
                recorders[occlusionPyramidPassId_] = [this](VkCommandBuffer command) {
                    occlusion_.Record(command, target_.Width(), target_.Height());
                };
                recorders[drawCullLatePassId_] = [this, cullSlot](VkCommandBuffer command) {
                    drawCull_.Record(command, cullSlot, DrawCull::Phase::Occlusion,
                                     cullDebugActive_, cullLimit_, cullFirst_);
                };
            }
        }

        if (sdfPassId_ != kInvalidPass) {
            for (uint32_t cascade = 0; cascade < kMaxSdfCascades; ++cascade) {
                if (sdfCascadeId_[cascade] != kInvalidResource) {
                    executor_.Bind(sdfCascadeId_[cascade],
                                   BoundImage{sdfClipmap_.Texture(cascade).Image(), VK_NULL_HANDLE,
                                              VK_IMAGE_ASPECT_COLOR_BIT});
                }
            }
            recorders[sdfPassId_] = [this](VkCommandBuffer command) {
                sdfClipmap_.RecordUploads(command);
            };
        }

        if (clusterPassId_ != kInvalidPass) {
            const auto clusterSlot = static_cast<uint32_t>(slotIndex_);
            executor_.Bind(clusterRangeId_,
                           BoundBuffer{clusterAssign_.RangeBuffer(clusterSlot), 0, VK_WHOLE_SIZE});
            executor_.Bind(clusterIndexId_,
                           BoundBuffer{clusterAssign_.IndexBuffer(clusterSlot), 0, VK_WHOLE_SIZE});
            recorders[clusterPassId_] = [this, clusterSlot](VkCommandBuffer command) {
                clusterAssign_.Record(command, clusterSlot);
            };
        }

        if (gradientFillId_ != kInvalidPass) {
            // Tanpa view: eksekutor hanya memasang barrier, dan barrier image
            // menyebut image-nya, bukan view-nya. Yang membutuhkan view adalah
            // descriptor, dan itu urusan `ComputeGradient` sendiri.
            executor_.Bind(gradientId_, BoundImage{gradient_.Image(), VK_NULL_HANDLE,
                                                   VK_IMAGE_ASPECT_COLOR_BIT});
            recorders[gradientFillId_] = [&](VkCommandBuffer command) {
                gradient_.RecordFill(command, target_.Width(), target_.Height());
            };
            recorders[gradientBlitId_] = [&](VkCommandBuffer command) {
                BeginDisplayRendering(command);
                gradient_.RecordBlit(command, target_.Width(), target_.Height());
                vkCmdEndRendering(command);
            };
        }

        // **Command buffer per segmen, dan yang pertama adalah `cmd` sendiri.**
        // Prolog frame — reset query dan `sdf-fill` — sudah terekam di sana, dan
        // ia harus berjalan di antrean grafis; jadi ia batch pertama, dan segmen
        // graph mendapat command buffer masing-masing sesudahnya.
        segmentBuffers_.clear();
        submitBatches_.clear();
        rhi::SubmitBatch prologue;
        prologue.commandBuffer = cmd;
        submitBatches_.push_back(prologue);
        for (const Segment& segment : compiled_.segments) {
            const bool async = segment.queue == Queue::AsyncCompute;
            VkCommandBuffer into = async ? device_.BeginTransientCompute(cmd)
                                         : device_.BeginTransientGraphics(cmd);
            SIM_VERIFY(into != VK_NULL_HANDLE, "a frame graph segment has no command buffer");
            segmentBuffers_.push_back(into);
            rhi::SubmitBatch batch;
            batch.commandBuffer = into;
            batch.queue = async ? rhi::QueueKind::AsyncCompute : rhi::QueueKind::Graphics;
            batch.wait = segment.wait;
            batch.waitQueue = segment.waitQueue == Queue::AsyncCompute
                                  ? rhi::QueueKind::AsyncCompute
                                  : rhi::QueueKind::Graphics;
            // Penungguan berlaku sedekat mungkin dengan pemakainya. Segmen yang
            // menunggu selalu dimulai oleh pass yang mengambil kepemilikan, dan
            // pengambilan itu barrier pertamanya — jadi menahan seluruh perintah
            // batch ini sampai sinyalnya datang tidak menahan apa pun yang bisa
            // jalan lebih dulu; yang bisa jalan lebih dulu ada di segmen
            // sebelumnya.
            batch.waitStages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            batch.signal = segment.signal;
            submitBatches_.push_back(batch);
        }
        if (!executor_.Execute(compiled_, segmentBuffers_, recorders,
                               device_.GraphicsQueueFamily(), device_.ComputeQueueFamily(),
                               &profiler_)) {
            SIM_ERROR("Render", "frame graph execution failed: {}", compiled_.error);
        }
        // Query frame ditutup di command buffer terakhir yang benar-benar
        // dipakai graph — bukan di `cmd`, yang sudah lewat.
        VkCommandBuffer last = segmentBuffers_.empty() ? cmd : segmentBuffers_.back();
        profiler_.EndFrame(last);
        slot.submitId = device_.SubmitTransientBatches(cmd, submitBatches_);
        slotIndex_ = (slotIndex_ + 1) % slots_.size();
        drawnOpaque_ = opaqueCount;
        drawnTransparent_ = transparentCount;

        stats_.opaqueInstances = opaqueCount;
        stats_.opaqueDrawn = 0;
        for (const DrawRun& run : visibleOpaqueRuns_) {
            stats_.opaqueDrawn += run.count;
        }
        stats_.transparentDrawn = transparentCount;
        stats_.shadowCasters = casterCount_;
        stats_.shadowFaces = static_cast<uint32_t>(atlasAllocation_.entries.size());
        stats_.shadowLightsDropped = atlasAllocation_.dropped;
    }

    bool CaptureSdf(std::vector<uint8_t>& out, std::string& error) override {
        if (!sdfClipmap_.IsValid()) {
            error = "the SDF clipmap is not available";
            return false;
        }
        // Menunggu seluruh frame selesai lebih dulu: yang dibaca adalah isi
        // tekstur sesudah pembaruan terakhir, bukan isi yang sedang ditulis.
        device_.WaitIdle();
        out.clear();
        std::vector<uint8_t> cascade;
        for (uint32_t at = 0; at < sdfClipmap_.CascadeCount(); ++at) {
            if (!sdfClipmap_.Texture(at).Readback(cascade)) {
                error = "cannot read back SDF cascade " + std::to_string(at);
                return false;
            }
            out.insert(out.end(), cascade.begin(), cascade.end());
        }
        return true;
    }

    MeshAsset AcquireMeshData(std::string_view key, const assets::MeshData& data,
                              uint64_t version) override {
        MeshAsset asset;
        if (key.empty() || !data.IsValid()) {
            return asset;
        }
        const std::string name(key);
        const auto found = meshDataVersion_.find(name);
        if (found != meshDataVersion_.end() && found->second.version == version) {
            const GpuMesh& mesh = *meshes_[static_cast<std::size_t>(found->second.handle)];
            return MeshAsset{found->second.handle,
                             mesh.boundsMin,
                             mesh.boundsMax,
                             true,
                             static_cast<uint32_t>(mesh.parts.size()),
                             mesh.boneCount,
                             mesh.triangleCount,
                             mesh.vertexCount};
        }

        // Versinya berubah: yang lama sudah bukan bentuk yang sama lagi. Ia
        // dibiarkan tetap ada alih-alih dilepas di sini — frame yang sedang
        // digambar masih boleh memakainya, dan melepasnya sekarang berarti GPU
        // membaca memori yang sudah kembali ke alokator.
        const MeshHandle handle = UploadMesh(data);
        if (handle == kUnitCubeMesh) {
            SIM_ERROR("Render", "cannot upload generated mesh {}", name);
            return asset;
        }
        meshDataVersion_[name] = GeneratedMesh{handle, version};

        const GpuMesh& mesh = *meshes_[static_cast<std::size_t>(handle)];
        return MeshAsset{handle,
                         mesh.boundsMin,
                         mesh.boundsMax,
                         true,
                         static_cast<uint32_t>(mesh.parts.size()),
                         mesh.boneCount,
                         mesh.triangleCount,
                         mesh.vertexCount};
    }

    /// Memuat sebuah `.ktx2` yang sudah di-bake.
    ///
    /// **Hanya `.ktx2`, dan itu satu-satunya jalur tekstur material yang ada.**
    /// Berkas sumber tidak pernah sampai ke sini: yang membangunnya adalah
    /// `assets::TextureBakery`, di `TaskPool`, dan renderer hanya menerima
    /// hasilnya. Batas itu ditegakkan uji yang menyisir berkas ini mencari
    /// `imageio::` — jalur kedua yang terlanjur ada tidak akan pernah dihapus.
    TextureHandle AcquireTexture(std::string_view path) override {
        if (path.empty() || materialSetLayout_ == VK_NULL_HANDLE) {
            return kInvalidTexture;
        }
        const std::string key(path);
        const auto found = textureByPath_.find(key);
        if (found != textureByPath_.end()) {
            return found->second;
        }

        rhi::Ktx2Texture image;
        const rhi::Ktx2Result read = rhi::ReadKtx2(std::filesystem::path(key), image);
        if (!read.ok) {
            // Dicatat gagal supaya tidak dibaca ulang tiap frame — aturan yang
            // sama dengan `AcquireMesh`.
            SIM_WARN("Render", "cannot load texture {}: {}", key, read.error);
            textureByPath_.emplace(key, kInvalidTexture);
            return kInvalidTexture;
        }

        auto entry = std::make_unique<GpuTexture>();
        if (!entry->texture.CreateFromKtx2(device_, image)) {
            SIM_WARN("Render", "cannot upload texture {}", key);
            textureByPath_.emplace(key, kInvalidTexture);
            return kInvalidTexture;
        }
        if (!bindless_) {
            entry->set = AllocateMaterialSet(entry->texture);
            if (entry->set == VK_NULL_HANDLE) {
                textureByPath_.emplace(key, kInvalidTexture);
                return kInvalidTexture;
            }
        }

        // Handle-nya indeks + 1: nol tetap berarti "tidak ada", sehingga
        // pemanggil tidak perlu membedakan "belum diminta" dari "gagal". Nomor
        // itu **sekaligus** slot bindless-nya, dan slot nol putih 1x1 — jadi
        // `kInvalidTexture` menunjuk nilai satuan perkalian tanpa dipetakan.
        const TextureHandle handle = static_cast<TextureHandle>(materialTextures_.size() + 1);
        if (bindless_ && handle >= bindlessCapacity_) {
            // Batasnya disebut, bukan dilampaui diam-diam: menulis melewati
            // ujung larik adalah descriptor tak sah yang muncul sebagai tekstur
            // acak pada benda lain, jauh dari tekstur yang menyebabkannya.
            SIM_WARN("Render",
                     "the bindless texture array is full ({} slots); {} falls back to white",
                     bindlessCapacity_, key);
            entry->texture.Destroy();
            textureByPath_.emplace(key, kInvalidTexture);
            return kInvalidTexture;
        }

        textureBytes_ += entry->texture.GpuBytes();
        if (bindless_) {
            WriteBindlessTexture(static_cast<uint32_t>(handle), entry->texture);
        }
        materialTextures_.push_back(std::move(entry));
        textureByPath_.emplace(key, handle);
        SIM_INFO("Render", "texture ready: {} ({}x{}, {} level, format {}, {} KB)", key,
                 image.width, image.height, image.levels.size(), image.format,
                 materialTextures_.back()->texture.GpuBytes() / 1024);
        return handle;
    }

    TextureHandle PendingTexture() const override { return pendingHandle_; }

    uint64_t TextureBytes() const override { return textureBytes_; }


    /// Material yang pipeline-nya sudah dibangun. Indeks + 1 menjadi handle-nya.
    struct GpuMaterial {
        /// Ketiganya kosong pada jalur bindless: di sana tidak ada satu pun
        /// objek descriptor milik sebuah material sendiri.
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        /// Layout bersama pada jalur bindless, milik sendiri pada jalur mundur.
        /// `DestroyMaterial` membedakannya dengan membandingkannya.
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        /// Nomor material ini di larik bindless. Nol pada jalur mundur.
        uint32_t slot = 0;
        /// Bertopeng: shader-nya membuang fragmen di bawah ambang opasitas.
        /// Ruasnya dilewati prepass dan pass bayangan — lihat `DrawRuns`.
        bool masked = false;
        /// Dipadu: ruasnya pindah ke daftar tersortir, dan dengan itu keluar
        /// sendirinya dari prepass maupun pass bayangan — keduanya menggambar
        /// daftar buram. Lihat `Gather`.
        bool blended = false;
        rhi::DynamicBuffer parameters;
        /// [transparan][ber-kulit].
        std::array<PipelineVariants, 2> pipelines{};
    };

    /// Membangun pipeline sebuah material, atau mengembalikan yang sudah ada.
    ///
    /// **Set 0 dan set 1 dipakai bersama pipeline kotak, dan itu yang membuat
    /// pergantian pipeline per ruas tidak merusak apa pun.** Dua pipeline layout
    /// yang set 0..1-nya objek yang sama persis dan push constant range-nya sama
    /// bersifat *compatible* untuk kedua set itu, jadi set yang sudah diikat
    /// sebelum ruas pertama tidak ikut terganggu saat pipeline material diikat
    /// di tengah jalan.
    MaterialHandle AcquireMaterial(std::string_view key,
                                   const MaterialProgram& program) override {
        if (key.empty() || program.fragmentSpirv.empty() || boxVertexModule_ == VK_NULL_HANDLE) {
            return kInvalidMaterial;
        }
        if (program.bindless != bindless_) {
            // **Ditolak, bukan dibangun.** Kedua jalur menghasilkan modul yang
            // sama sahnya dan sama bentuk entry point-nya; yang berbeda hanya
            // descriptor set layout yang diharapkannya. Pipeline-nya akan
            // terbangun tanpa satu pun keluhan, lalu menyampel descriptor yang
            // tidak pernah ditulis siapa pun.
            SIM_WARN("Render", "material {} was compiled for the {} path", key,
                     program.bindless ? "bindless" : "per-part set");
            return kInvalidMaterial;
        }
        const std::string cacheKey(key);
        if (const auto found = materialByKey_.find(cacheKey); found != materialByKey_.end()) {
            return found->second;
        }
        // Kegagalan ikut diingat, aturan yang sama dengan `AcquireMesh` dan
        // `AcquireTexture`: material yang shader-nya tidak bisa dibangun tidak
        // boleh dicoba enam puluh kali per detik.
        materialByKey_.emplace(cacheKey, kInvalidMaterial);

        auto material = std::make_unique<GpuMaterial>();
        const uint32_t textureCount = static_cast<uint32_t>(program.textures.size());

        if (bindless_) {
            // **Tidak ada satu pun objek descriptor milik material ini.** Set 2
            // sudah berupa larik bersama yang diikat sekali per pass, dan
            // layout-nya sudah ikut ke dalam `pipelineLayout_` — jadi pipeline
            // material memakainya kembali apa adanya, bukan membangun kembarnya.
            //
            // Itu pula yang membuat pergantian pipeline per ruas tidak lagi
            // menyeret pergantian layout: ketiga set-nya benda yang sama persis.
            if (materials_.size() >= kMaxBindlessMaterials) {
                SIM_WARN("Render", "the bindless material array is full ({} slots); {} falls back",
                         kMaxBindlessMaterials, cacheKey);
                return kInvalidMaterial;
            }
            material->pipelineLayout = pipelineLayout_;
            material->slot = static_cast<uint32_t>(materials_.size());
        } else {
            // Binding 0 blok parameter, lalu tekstur dan sampler berselang mulai
            // 1 — konvensi kompiler graph, dan yang menuliskannya di sisi shader
            // adalah kompiler itu sendiri.
            std::vector<VkDescriptorSetLayoutBinding> bindings;
            bindings.push_back({0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
            for (uint32_t i = 0; i < textureCount; ++i) {
                bindings.push_back({1 + i * 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                    VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
                bindings.push_back({2 + i * 2, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                                    VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
            }
            VkDescriptorSetLayoutCreateInfo layoutInfo{};
            layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
            layoutInfo.pBindings = bindings.data();
            if (vkCreateDescriptorSetLayout(device_.Handle(), &layoutInfo, nullptr,
                                            &material->setLayout) != VK_SUCCESS) {
                SIM_WARN("Render", "cannot create descriptor layout for material {}", cacheKey);
                return kInvalidMaterial;
            }

            VkPushConstantRange range{};
            range.stageFlags = kBoxPushStages;
            range.size = sizeof(BoxPush);
            const std::array<VkDescriptorSetLayout, 3> sets{shadowSetLayout_, skinSetLayout_,
                                                            material->setLayout};
            VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
            pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pipelineLayoutInfo.pushConstantRangeCount = 1;
            pipelineLayoutInfo.pPushConstantRanges = &range;
            pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(sets.size());
            pipelineLayoutInfo.pSetLayouts = sets.data();
            if (vkCreatePipelineLayout(device_.Handle(), &pipelineLayoutInfo, nullptr,
                                       &material->pipelineLayout) != VK_SUCCESS) {
                SIM_WARN("Render", "cannot create pipeline layout for material {}", cacheKey);
                DestroyMaterial(*material);
                return kInvalidMaterial;
            }
        }

        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = program.fragmentSpirv.size() * sizeof(uint32_t);
        moduleInfo.pCode = program.fragmentSpirv.data();
        VkShaderModule fragment = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device_.Handle(), &moduleInfo, nullptr, &fragment) !=
            VK_SUCCESS) {
            SIM_WARN("Render", "material {} has SPIR-V the driver rejected", cacheKey);
            DestroyMaterial(*material);
            return kInvalidMaterial;
        }

        material->masked = program.masked;
        material->blended = program.blended;
        // **Yang bertopeng diuji `GREATER_OR_EQUAL`, bukan `EQUAL`.** Ruasnya
        // tidak ikut prepass — fragmen yang dibuang tidak boleh meninggalkan
        // kedalamannya di sana — jadi kedalaman yang tertulis di buffer adalah
        // milik permukaan **di belakangnya**. `EQUAL` karena itu tidak akan
        // pernah lolos. Sama-dalam ikut diterima karena decal memang sebidang
        // dengan dindingnya: reversed-Z membuat "lebih dekat" berarti lebih
        // besar, jadi yang di depan tetap lolos dan yang di belakang tetap
        // gagal.
        const VkCompareOp opaqueDepth =
            material->masked ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_EQUAL;
        for (std::size_t skinned = 0; skinned < kPipelineVariants; ++skinned) {
            // Keadaan depth dan blending-nya sama persis dengan pipeline kotak
            // yang digantikannya — kalau tidak, ruas bermaterial akan diuji
            // terhadap depth dengan aturan yang berbeda dari tetangganya.
            material->pipelines[0][skinned] =
                BuildPipeline(boxVertexModule_, fragment, /*depthWrite=*/false,
                              opaqueDepth, /*blend=*/false, /*colorWrite=*/true,
                              /*skinned=*/skinned != 0, material->pipelineLayout);
            material->pipelines[1][skinned] =
                BuildPipeline(boxVertexModule_, fragment, /*depthWrite=*/false,
                              VK_COMPARE_OP_GREATER, /*blend=*/true, /*colorWrite=*/true,
                              /*skinned=*/skinned != 0, material->pipelineLayout);
        }
        vkDestroyShaderModule(device_.Handle(), fragment, nullptr);
        for (const PipelineVariants& variants : material->pipelines) {
            for (VkPipeline pipeline : variants) {
                if (pipeline == VK_NULL_HANDLE) {
                    SIM_WARN("Render", "cannot build pipeline for material {}", cacheKey);
                    DestroyMaterial(*material);
                    return kInvalidMaterial;
                }
            }
        }

        // Blok parameter. **Minimal satu byte**: buffer berukuran nol tidak sah,
        // dan material tanpa satu pun parameter tetap mendeklarasikan
        // `cbuffer`-nya.
        //
        // **Pada jalur bindless ia membawa satu hal lagi: nomor slot tiap
        // teksturnya.** Tabel itu ditempel sesudah blok parameter yang sudah
        // dibulatkan ke 16, dan di sisi shader ia dideklarasikan `uint4` —
        // std140 menjajarkan `uint4` ke 16, jadi tabelnya jatuh tepat di ujung
        // blok. Dideklarasikan `uint` satu-satu, ia justru akan mengisi celah
        // sisipan di dalamnya: sebuah `float3` di akhir blok berakhir di offset
        // +12, dan `uint` berjajar 4.
        const std::size_t slotBytes = bindless_ ? 16 * ((textureCount + 3) / 4) : 0;
        const std::size_t blockBytes = AlignUp16(program.parameters.size());
        const VkDeviceSize parameterBytes =
            std::max<VkDeviceSize>(blockBytes + slotBytes, 16);
        if (!material->parameters.Create(device_, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                         parameterBytes)) {
            DestroyMaterial(*material);
            return kInvalidMaterial;
        }
        std::vector<uint8_t> padded(static_cast<std::size_t>(parameterBytes), 0);
        std::copy(program.parameters.begin(), program.parameters.end(), padded.begin());
        if (bindless_) {
            for (uint32_t i = 0; i < textureCount; ++i) {
                // Slot tekstur = handle-nya, dan nol putih 1x1. Sama persis
                // dengan jalur mundur di bawah, yang memilih `fallbackTexture_`
                // untuk slot yang kosong.
                const TextureHandle texture = program.textures[i];
                const uint32_t slot = texture != kInvalidTexture &&
                                              texture <= materialTextures_.size()
                                          ? static_cast<uint32_t>(texture)
                                          : 0u;
                std::memcpy(padded.data() + blockBytes + i * sizeof(uint32_t), &slot,
                            sizeof(slot));
            }
        }
        material->parameters.Write(padded.data(), parameterBytes);

        if (bindless_) {
            WriteBindlessMaterial(material->slot, material->parameters);
            materials_.push_back(std::move(material));
            const MaterialHandle handle = static_cast<MaterialHandle>(materials_.size());
            materialByKey_[cacheKey] = handle;
            SIM_INFO("Render", "material ready: {} ({} texture, bindless slot {})", cacheKey,
                     textureCount, handle - 1);
            return handle;
        }

        std::vector<VkDescriptorPoolSize> poolSizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
        if (textureCount > 0) {
            poolSizes.push_back({VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, textureCount});
            poolSizes.push_back({VK_DESCRIPTOR_TYPE_SAMPLER, textureCount});
        }
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        if (vkCreateDescriptorPool(device_.Handle(), &poolInfo, nullptr, &material->pool) !=
            VK_SUCCESS) {
            DestroyMaterial(*material);
            return kInvalidMaterial;
        }
        VkDescriptorSetAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocateInfo.descriptorPool = material->pool;
        allocateInfo.descriptorSetCount = 1;
        allocateInfo.pSetLayouts = &material->setLayout;
        if (vkAllocateDescriptorSets(device_.Handle(), &allocateInfo, &material->set) !=
            VK_SUCCESS) {
            DestroyMaterial(*material);
            return kInvalidMaterial;
        }

        const VkDescriptorBufferInfo params{material->parameters.Handle(), 0, VK_WHOLE_SIZE};
        std::vector<VkDescriptorImageInfo> images(textureCount);
        std::vector<VkDescriptorImageInfo> samplers(textureCount);
        std::vector<VkWriteDescriptorSet> writes;
        writes.push_back({});
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = material->set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &params;
        for (uint32_t i = 0; i < textureCount; ++i) {
            // Slot yang kosong mendapat tekstur putih 1x1 — nilai satuan
            // perkalian, dan yang membuat material yang teksturnya belum
            // dipasang tetap tergambar alih-alih menampilkan descriptor tak sah.
            const rhi::Texture2D* texture = &fallbackTexture_;
            const TextureHandle handle = program.textures[i];
            if (handle != kInvalidTexture && handle <= materialTextures_.size()) {
                texture = &materialTextures_[static_cast<std::size_t>(handle) - 1]->texture;
            }
            images[i].imageView = texture->View();
            images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            samplers[i].sampler = texture->Sampler();

            VkWriteDescriptorSet image{};
            image.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            image.dstSet = material->set;
            image.dstBinding = 1 + i * 2;
            image.descriptorCount = 1;
            image.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            image.pImageInfo = &images[i];
            writes.push_back(image);

            VkWriteDescriptorSet sampler{};
            sampler.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            sampler.dstSet = material->set;
            sampler.dstBinding = 2 + i * 2;
            sampler.descriptorCount = 1;
            sampler.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            sampler.pImageInfo = &samplers[i];
            writes.push_back(sampler);
        }
        vkUpdateDescriptorSets(device_.Handle(), static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);

        materials_.push_back(std::move(material));
        const MaterialHandle handle = static_cast<MaterialHandle>(materials_.size());
        materialByKey_[cacheKey] = handle;
        SIM_INFO("Render", "material ready: {} ({} texture)", cacheKey, textureCount);
        return handle;
    }

    void DestroyMaterial(GpuMaterial& material) {
        for (PipelineVariants& variants : material.pipelines) {
            for (VkPipeline& pipeline : variants) {
                if (pipeline != VK_NULL_HANDLE) {
                    vkDestroyPipeline(device_.Handle(), pipeline, nullptr);
                    pipeline = VK_NULL_HANDLE;
                }
            }
        }
        material.parameters.Destroy();
        if (material.pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_.Handle(), material.pool, nullptr);
            material.pool = VK_NULL_HANDLE;
        }
        // Layout bersama tidak ikut dihancurkan bersama salah satu pemakainya.
        if (material.pipelineLayout != VK_NULL_HANDLE &&
            material.pipelineLayout != pipelineLayout_) {
            vkDestroyPipelineLayout(device_.Handle(), material.pipelineLayout, nullptr);
            material.pipelineLayout = VK_NULL_HANDLE;
        }
        if (material.setLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_.Handle(), material.setLayout, nullptr);
            material.setLayout = VK_NULL_HANDLE;
        }
    }

    MeshAsset AcquireMesh(std::string_view path) override {
        MeshAsset asset;
        if (path.empty()) {
            return asset;
        }
        const std::string key(path);
        const auto found = meshByPath_.find(key);
        if (found != meshByPath_.end()) {
            // Kubus satuan di sini berarti "sudah pernah dicoba dan gagal".
            // Mencobanya lagi berarti mengurai berkas rusak enam puluh kali per
            // detik sambil membanjiri log dengan pesan yang sama.
            if (found->second == kUnitCubeMesh) {
                return asset;
            }
            const GpuMesh& mesh = *meshes_[static_cast<std::size_t>(found->second)];
            return MeshAsset{found->second,
                             mesh.boundsMin,
                             mesh.boundsMax,
                             true,
                             static_cast<uint32_t>(mesh.parts.size()),
                             mesh.boneCount,
                             mesh.triangleCount,
                             mesh.vertexCount};
        }

        std::string error;
        const assets::MeshData data = assets::LoadMesh(std::filesystem::path(key), error);
        if (!data.IsValid()) {
            SIM_WARN("Render", "cannot load mesh {}: {}", key, error);
            meshByPath_.emplace(key, kUnitCubeMesh);
            return asset;
        }
        const MeshHandle handle = UploadMesh(data);
        meshByPath_.emplace(key, handle);
        if (handle == kUnitCubeMesh) {
            SIM_ERROR("Render", "cannot upload mesh {}", key);
            return asset;
        }
        const GpuMesh& uploaded = *meshes_[static_cast<std::size_t>(handle)];
        SIM_INFO("Render", "mesh ready: {} ({} tris, {} verts, {} bones)", key,
                 data.TriangleCount(), data.vertices.size(), uploaded.boneCount);
        return MeshAsset{handle,
                         data.boundsMin,
                         data.boundsMax,
                         true,
                         static_cast<uint32_t>(uploaded.parts.size()),
                         uploaded.boneCount,
                         uploaded.triangleCount,
                         uploaded.vertexCount};
    }

    bool CapturePixels(std::vector<uint8_t>& outRgba, uint32_t& outWidth, uint32_t& outHeight,
                       std::string& error) override {
        return target_.ReadPixels(outRgba, outWidth, outHeight, error);
    }

    // --- IPresentSource: dipakai `Presenter` untuk menyalinnya ke layar ---
    VkImageView PresentView() const override { return target_.ColorView(); }
    VkSampler PresentSampler() const override { return target_.Sampler(); }
    float PresentUvMaxU() const override { return target_.UvMaxU(); }
    float PresentUvMaxV() const override { return target_.UvMaxV(); }

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
            timings_.push_back(PassTiming{scope.name, static_cast<float>(scope.milliseconds),
                                          scope.primitives});
        }
        return timings_;
    }

    uint64_t TimingSerial() const override { return profiler_.ResultsSerial(); }

    float FrameGpuMilliseconds() const override { return profiler_.FrameMilliseconds(); }

    std::span<const PassTiming> CpuTimings() const override { return cpuTimings_; }

    RenderStats Stats() const override { return stats_; }

    bool UsesBindlessMaterials() const override { return bindless_; }

    /// **Menunggu GPU diam lebih dulu.** Angka yang dibaca sementara dispatch
    /// yang menulisnya masih berjalan adalah angka setengah frame ini dan
    /// setengah frame lalu — yaitu tepat jenis bukti yang menyesatkan.
    bool CaptureDepth(std::vector<float>& out, uint32_t& outWidth, uint32_t& outHeight,
                      std::string& error) override {
        return target_.ReadDepth(out, outWidth, outHeight, executor_.LayoutOf(depthId_), error);
    }

    bool CaptureCullDebug(std::string& out, std::string& error) override {
        if (!gpuCullActive_ || !drawCull_.IsValid()) {
            error = "GPU culling is off";
            return false;
        }
        if (!cullDebugActive_) {
            error = "cull debug was not on when the last frame was drawn";
            return false;
        }
        device_.WaitIdle();
        std::vector<DrawCull::GpuCullDebug> entries;
        drawCull_.ReadDebug(lastCullSlot_, entries);
        if (entries.empty()) {
            error = "no cull data was written";
            return false;
        }
        // Matriksnya ikut supaya yang memeriksa bisa menghitung ulang petak dan
        // kedalamannya sendiri, dari kotak yang sama — dua implementasi yang
        // harus sepakat, aturan yang sama dengan yang lain.
        out = "# viewProj kolom demi kolom\n";
        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row) {
                out += std::format("{}{:.9g}", (column == 0 && row == 0) ? "# " : " ",
                                   lastViewProjection_[column][row]);
            }
        }
        out += "\n# index centre.xyz extent.xyz uvMin.xy uvMax.xy nearest farthest level visible "
               "depth surfaceId translate.xyz indexCount firstIndex\n";
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const DrawCull::GpuCullDebug& entry = entries[i];
            out += std::format(
                "{} {:.6g} {:.6g} {:.6g} {:.6g} {:.6g} {:.6g} {:.6g} {:.6g} {:.6g} {:.6g} "
                "{:.9g} {:.9g} {:g} {:g} {:.9g} {:.9g} {:.6g} {:.6g} {:.6g} {} {}\n",
                i, entry.centre.x, entry.centre.y, entry.centre.z, entry.extent.x, entry.extent.y,
                entry.extent.z, entry.rect.x, entry.rect.y, entry.rect.z, entry.rect.w,
                entry.result.x, entry.result.y, entry.result.z, entry.result.w, entry.centre.w,
                entry.extent.w,
                i < opaqueTransforms_.size() ? opaqueTransforms_[i][3][0] : 0.0f,
                i < opaqueTransforms_.size() ? opaqueTransforms_[i][3][1] : 0.0f,
                i < opaqueTransforms_.size() ? opaqueTransforms_[i][3][2] : 0.0f,
                i < cullSurfaces_.size() ? cullSurfaces_[i].indexCount : 0u,
                i < cullSurfaces_.size() ? cullSurfaces_[i].firstIndex : 0u);
        }
        return true;
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
        /// Palet kulit seluruh instance ber-skin frame ini. Per slot karena
        /// alasan yang sama dengan yang lain: menulisinya sementara frame
        /// sebelumnya masih membacanya adalah pose yang sesekali melompat satu
        /// frame, bukan sebuah galat.
        rhi::DynamicBuffer skinBuffer;
        /// Transform dunia tiap instance frame ini, sejajar dengan aliran
        /// atribut instance. Per slot dengan alasan yang sama seperti palet
        /// kulit.
        rhi::DynamicBuffer instanceBuffer;
        VkDescriptorSet shadowSet = VK_NULL_HANDLE;
        VkDescriptorSet skinSet = VK_NULL_HANDLE;
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

        // **Pengisian kaskade SDF, dan sejak G7 ia pass graph.** Sebelumnya ia
        // direkam langsung ke command buffer frame, di luar graph, dengan
        // perpindahan layoutnya diurus `SdfClipmapResource` sendiri. Itu tidak
        // bisa dipertahankan begitu ia boleh berjalan di antrean lain:
        // perpindahan kepemilikan keluarga antrean berbentuk sepasang, dan
        // sepasang hanya bisa disusun oleh yang melihat kedua sisinya.
        //
        // Dideklarasikan hanya kalau ada yang mau ditulis. Pass tanpa isi tetap
        // membawa perpindahan layout keempat kaskadenya, dan itu biaya yang
        // dibayar untuk tidak menulis apa pun.
        sdfPassId_ = kInvalidPass;
        sdfCascadeId_.fill(kInvalidResource);
        if (sdfClipmap_.IsValid() && sdfClipmap_.HasPendingUploads()) {
            const Access fillAccess =
                sdfClipmap_.GpuFillActive() ? Access::ShaderWrite : Access::TransferWrite;
            sdfPassId_ = graph_.AddPass("sdf-fill");
            for (uint32_t cascade = 0; cascade < kMaxSdfCascades; ++cascade) {
                if (!sdfClipmap_.Touches(cascade) || !sdfClipmap_.Texture(cascade).IsValid()) {
                    continue;
                }
                sdfCascadeId_[cascade] = graph_.Import(
                    "sdf-cascade-" + std::to_string(cascade), Access::ShaderRead);
                graph_.Write(sdfPassId_, sdfCascadeId_[cascade], fillAccess);
                // Dikembalikan ke ShaderRead sesudah frame: itulah keadaan yang
                // diandaikan penelusuran GI, dan keadaan awal yang
                // dideklarasikan frame berikutnya.
                graph_.SetOutput(sdfCascadeId_[cascade], Access::ShaderRead);
            }
            graph_.SetSideEffect(sdfPassId_);
            // **Hanya kalau ada yang membacanya frame ini.** Tanpa pembaca,
            // kepemilikannya harus dikembalikan ke antrean grafis oleh barrier
            // penutup — dan barrier penutup tidak mengenal antrean. Tanpa
            // pembaca ia juga tidak membeli apa pun.
            if (asyncComputeActive_ && sdfClipmap_.GpuFillActive() && giEnabled_ &&
                probes_.IsValid() && hiz_.IsValid()) {
                graph_.SetQueue(sdfPassId_, Queue::AsyncCompute);
            }
        }

        // Penetapan lampu ke cluster, paling awal: yang membacanya adalah pass
        // forward, dan hasilnya tidak bergantung pada apa pun yang digambar.
        // **Ini pemakai pertama barrier buffer di eksekutor** — yang ditulis
        // dispatch adalah storage buffer, bukan lampiran, dan buffer tidak
        // punya layout untuk dipindahkan.
        clusterRangeId_ = kInvalidResource;
        clusterIndexId_ = kInvalidResource;
        clusterPassId_ = kInvalidPass;
        if (gpuClustersActive_ && clusterAssign_.IsValid()) {
            // ShaderRead sebagai keadaan awal: itulah keadaan yang ditinggalkan
            // pass forward frame sebelumnya atas slot yang sama.
            clusterRangeId_ = graph_.Import("cluster-ranges", Access::ShaderRead);
            clusterIndexId_ = graph_.Import("cluster-indices", Access::ShaderRead);
            clusterPassId_ = graph_.AddPass("cluster-assign");
            graph_.Write(clusterPassId_, clusterRangeId_, Access::ShaderWrite);
            graph_.Write(clusterPassId_, clusterIndexId_, Access::ShaderWrite);
            // **Pemakai pertama antrean compute terpisah (G7).** Ia tidak
            // menghalangi apa pun sampai pass forward membacanya, dan yang ada
            // di antaranya — bayangan, langit, prepass — tidak menyentuh satu
            // pun keluarannya.
            if (asyncComputeActive_) {
                graph_.SetQueue(clusterPassId_, Queue::AsyncCompute);
            }
        }

        // **Culling lebih dulu daripada apa pun yang menggambar geometri.**
        // Yang dihasilkannya dibaca tahap `DRAW_INDIRECT`, yaitu sebelum tahap
        // vertex — jadi barrier-nya harus sudah selesai sebelum draw pertama,
        // bukan sebelum shader pertama.
        drawCommandId_ = kInvalidResource;
        drawCullPassId_ = kInvalidPass;
        visibleCommandId_ = kInvalidResource;
        occlusionPyramidPassId_ = kInvalidPass;
        drawCullLatePassId_ = kInvalidPass;
        if (gpuCullActive_) {
            drawCommandId_ = graph_.Import("draw-commands", Access::IndirectRead);
            visibleCommandId_ = graph_.Import("visible-commands", Access::IndirectRead);
            drawCullPassId_ = graph_.AddPass("draw-cull");
            graph_.Write(drawCullPassId_, drawCommandId_, Access::ShaderWrite);
        }

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
        if (drawCullPassId_ != kInvalidPass) {
            graph_.Read(prepassId_, drawCommandId_, Access::IndirectRead);
        }

        // **Piramida occlusion dibangun dari depth frame ini sendiri, bukan
        // dari frame lalu.** Itu yang membuat tidak ada benda yang bisa berkedip
        // masuk satu frame terlambat: yang diuji fase kedua adalah kedalaman
        // yang benar-benar tergambar, bukan tebakan.
        if (gpuOcclusionActive_) {
            occlusionPyramidPassId_ = graph_.AddPass("occlusion-pyramid");
            graph_.Read(occlusionPyramidPassId_, depthId_, Access::ShaderRead);
            // Efek samping, alasan yang sama dengan `hiz-build`: keluarannya
            // bukan resource graph — piramida mengurus perpindahan layout tiap
            // mip-nya sendiri.
            graph_.SetSideEffect(occlusionPyramidPassId_);

            drawCullLatePassId_ = graph_.AddPass("draw-cull-late");
            graph_.Write(drawCullLatePassId_, visibleCommandId_, Access::ShaderWrite);
            // Depth-nya ikut dibaca — untuk dibandingkan dengan piramidanya.
            graph_.Read(drawCullLatePassId_, depthId_, Access::ShaderRead);
        }

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
        if (clusterPassId_ != kInvalidPass) {
            graph_.Read(opaqueId_, clusterRangeId_, Access::ShaderRead);
            graph_.Read(opaqueId_, clusterIndexId_, Access::ShaderRead);
        }
        if (drawCullLatePassId_ != kInvalidPass) {
            graph_.Read(opaqueId_, visibleCommandId_, Access::IndirectRead);
        }
        graph_.Read(opaqueId_, depthId_, Access::DepthWrite);
        graph_.Read(opaqueId_, shadowId_, Access::ShaderRead);
        graph_.Read(opaqueId_, atlasId_, Access::ShaderRead);
        graph_.Write(opaqueId_, sceneId_, Access::ColorWrite);

        transparentId_ = graph_.AddPass("forward-transparent");
        if (clusterPassId_ != kInvalidPass) {
            graph_.Read(transparentId_, clusterRangeId_, Access::ShaderRead);
            graph_.Read(transparentId_, clusterIndexId_, Access::ShaderRead);
        }
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
            // Kaskade SDF yang ditulis frame ini. Inilah yang menempatkan
            // penungguan lintas-antrean tepat di sini dan bukan lebih awal.
            for (const ResourceId cascade : sdfCascadeId_) {
                if (cascade != kInvalidResource) {
                    graph_.Read(probePassId_, cascade, Access::ShaderRead);
                }
            }
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

        // Volume `.vdb`, sesudah seluruh geometri dengan alasan yang sama seperti
        // awan: raymarch-nya berhenti di permukaan terdekat, dan permukaan itu
        // baru diketahui setelah depth selesai ditulis. Sebelum kabut aerial,
        // karena asapnya berada **di dalam** udara yang membiru di kejauhan —
        // bukan di depannya.
        volumeId_ = kInvalidPass;
        if (desc.volume.grid != nullptr && volumePass_.IsValid() && volumePass_.HasVolume()) {
            volumeId_ = graph_.AddPass("volume");
            graph_.Read(volumeId_, depthId_, Access::ShaderRead);
            graph_.Write(volumeId_, sceneId_, Access::ColorWrite);
            // Teksturnya diurus `VolumePass` sendiri, bukan dilacak graph.
            graph_.SetSideEffect(volumeId_);
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

        // --- Pemeriksa jalur compute (G3) ---
        //
        // Paling akhir, dan menulis ke target tampilan langsung. Yang diperiksa
        // gambar ini adalah dispatch-nya sendiri, jadi apa pun yang berdiri di
        // antara storage image dan layar — tone mapping, bloom, eksposur —
        // hanya menambah hal yang bisa disalahkan ketika hasilnya tidak muncul.
        //
        // **Dua pass, dan yang penting justru barrier di antaranya.** Yang
        // pertama menulis storage image dalam layout `GENERAL`, yang kedua
        // membacanya sebagai tekstur; perpindahan itulah yang dituntut setiap
        // pemakai compute berikutnya, dan graph menyimpulkannya dari dua baris
        // deklarasi di bawah tanpa satu pun barrier yang ditulis tangan.
        gradientId_ = kInvalidResource;
        gradientFillId_ = kInvalidPass;
        gradientBlitId_ = kInvalidPass;
        if (desc.computeGradient && gradient_.IsValid()) {
            // `ShaderRead` sebagai keadaan awal, bukan `None`: itulah keadaan
            // yang ditinggalkan pass blit pada frame sebelumnya. `None` berarti
            // "tidak ada yang perlu ditunggu", dan yang tidak ditunggu di sini
            // adalah pembacaan frame sebelumnya atas image yang sedang ditimpa.
            gradientId_ = graph_.Import("compute-gradient", Access::ShaderRead);
            gradientFillId_ = graph_.AddPass("compute-gradient");
            graph_.Write(gradientFillId_, gradientId_, Access::ShaderWrite);

            gradientBlitId_ = graph_.AddPass("compute-gradient-blit");
            graph_.Read(gradientBlitId_, gradientId_, Access::ShaderRead);
            graph_.Write(gradientBlitId_, colorId_, Access::ColorWrite);
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
    /// Menyusun daftar permukaan yang bisa digambar frame ini.
    ///
    /// **Satuannya permukaan, bukan entity, dan itu berubah di G6.** Sebuah
    /// permukaan adalah sepasang (entity, ruas mesh): ia yang punya satu warna,
    /// satu material, dan satu tekstur. Sampai G6 ketiganya dikirim lewat push
    /// constant per panggilan gambar, jadi setiap entity — bahkan yang bermesh
    /// sama — menjadi panggilannya sendiri. Sejak ketiganya menjadi data
    /// instance, seluruh permukaan yang sepakat soal mesh, ruas, material, dan
    /// pipeline digambar sekali.
    void Gather(const ViewportDesc& desc, const ViewportScene& scene, const Mat4& viewProj) {
        opaque_.clear();
        transparent_.clear();
        opaqueTransforms_.clear();
        transparentTransforms_.clear();
        opaqueRuns_.clear();
        casterRuns_.clear();
        transparentRuns_.clear();
        visibleOpaqueRuns_.clear();
        instanceBounds_.clear();
        instanceVisible_.clear();
        gathered_.clear();
        sorted_.clear();
        casterCount_ = 0;
        const Frustum frustum(viewProj);
        const Vec3 eye = desc.camera.position;

        for (const MeshInstance& mesh : scene.meshes) {
            const Aabb local{mesh.boundsMin, mesh.boundsMax};
            const Aabb world = TransformAabb(local, mesh.transform);
            const bool cameraVisible = frustum.Intersects(world);
            // **Yang di luar pandangan tetap masuk daftar bila ia menjatuhkan
            // bayangan.** Membuangnya di sini — yang berlaku sejak E8.1 —
            // berarti bayangan sebuah benda ikut hilang begitu bendanya sendiri
            // keluar layar: pohon di sebelah kiri layar berhenti membayangi
            // jalan di tengahnya, dan yang terlihat bukan bayangan yang
            // menghilang melainkan bayangan yang "berkedip saat menengok".
            //
            // Yang menggantikan penyaringan ini bukan ketiadaan penyaringan
            // melainkan penyaringan yang lebih tepat, satu per pass: pandangan
            // utama menyaring terhadap frustum kamera, tiap muka bayangan
            // terhadap volumenya sendiri. Lihat `SplitRuns`.
            if (!cameraVisible && !mesh.castShadows) {
                continue;
            }
            // Mesh yang belum ada geometrinya tidak menghasilkan satu pun
            // permukaan. Diperiksa di sini, bukan saat menggambar: daftar ruas
            // yang memuat mesh yang tidak bisa digambar adalah daftar yang
            // panjangnya berbohong.
            const GpuMesh* geometry = mesh.mesh < meshes_.size()
                                          ? meshes_[static_cast<std::size_t>(mesh.mesh)].get()
                                          : nullptr;
            if (geometry == nullptr || geometry->indexCount == 0 || geometry->parts.empty()) {
                continue;
            }

            // **Dua jalur, dan perbedaannya bukan optimisasi.** Kubus satuan
            // harus dipetakan ke kotak batasnya — geser ke pusat lalu skala ke
            // ukurannya — karena vertexnya membentang -0,5..0,5 apa pun batasnya.
            // Mesh yang diimpor vertexnya **sudah** berada di ruang lokal yang
            // sama dengan batasnya, jadi pemetaan yang sama akan menskalakannya
            // dua kali: sebuah shader ball setinggi 2,7 m menjadi setinggi 7,2 m,
            // dan tidak ada satu pun galat yang menyertainya.
            Mat4 model = mesh.transform;
            if (mesh.mesh == kUnitCubeMesh) {
                const Vec3 centre = (mesh.boundsMin + mesh.boundsMax) * 0.5f;
                const Vec3 size = glm::max(mesh.boundsMax - mesh.boundsMin, Vec3(1e-4f));
                model = glm::translate(model, centre);
                model = glm::scale(model, size);
            }

            const bool skinned = IsSkinnable(mesh, scene.skinMatrices.size());
            // Ruas warna yang benar-benar ada di larik pemanggil. Yang melewati
            // ujungnya diabaikan seluruhnya, bukan dipotong: ruas separuh berarti
            // sebagian material entity ini diambil dari entity lain.
            uint32_t colorFirst = 0;
            uint32_t colorCount = 0;
            if (mesh.partCount > 0 && static_cast<std::size_t>(mesh.partFirst) + mesh.partCount <=
                                          scene.partColors.size()) {
                colorFirst = mesh.partFirst;
                colorCount = mesh.partCount;
            }
            const Vec4 instanceColor = mesh.selected ? kSelectedColor : mesh.color;
            // **Buram atau tembus pandang diputuskan warna entity, bukan warna
            // ruas.** Aturan yang sama dengan sebelum G6, dan mempertahankannya
            // disengaja: alpha ruas tetap sampai ke target seperti dulu, dan
            // memindahkan keputusannya ke per-ruas akan memindahkan sebagian
            // benda ke pass lain tanpa ada yang memintanya.
            const bool opaqueInstance = instanceColor.a >= 0.999f;
            // Tembus pandang tidak pernah menjatuhkan bayangan — kaca yang
            // menghitamkan lantai di bawahnya adalah kesalahan yang lebih
            // mencolok daripada kaca yang tidak berbayang sama sekali. Jadi yang
            // di luar pandangan memang tidak punya alasan ikut.
            if (!opaqueInstance && !cameraVisible) {
                continue;
            }
            const float distance = glm::length(world.Centre() - eye);
            const Mat4 transform = InstanceTransform(model);

            for (std::size_t partIndex = 0; partIndex < geometry->parts.size(); ++partIndex) {
                const GpuMesh::Part& part = geometry->parts[partIndex];
                if (part.indexCount == 0) {
                    continue;
                }
                const std::size_t slot = colorFirst + partIndex;
                const bool hasSlot = partIndex < colorCount;

                // **Yang ditetapkan editor menang atas yang tertulis di berkas
                // mesh.** Berkasnya adalah bagaimana model itu diekspor; slot
                // material entity adalah keputusan orang yang menyusun adegan.
                // Sorotan seleksi menang atas keduanya: objek terpilih harus
                // berubah warna seluruhnya.
                Vec4 color = instanceColor;
                if (!mesh.selected) {
                    if (part.baseColor.a > 0.0f) {
                        color = part.baseColor;
                    }
                    if (hasSlot && slot < partColors_.size() && partColors_[slot].a > 0.0f) {
                        color = partColors_[slot];
                    }
                }

                MaterialHandle material = kInvalidMaterial;
                if (hasSlot && slot < partMaterials_.size()) {
                    const MaterialHandle handle = partMaterials_[slot];
                    if (handle != kInvalidMaterial && handle <= materials_.size()) {
                        material = handle;
                    }
                }
                // Tekstur jalur mundur hanya berlaku untuk ruas yang **tidak**
                // punya material: material membawa teksturnya sendiri.
                TextureHandle texture = kInvalidTexture;
                if (material == kInvalidMaterial && hasSlot && slot < partTextures_.size()) {
                    const TextureHandle handle = partTextures_[slot];
                    if (handle != kInvalidTexture && handle <= materialTextures_.size()) {
                        texture = handle;
                    }
                }

                // **Dipadu diputuskan per ruas, tembus pandang per instance —
                // dan keduanya bermuara ke daftar yang sama.** Alfa warna
                // instance milik pemanggil; `alphaMode: blend` milik
                // materialnya. Sebuah dinding buram yang salah satu ruasnya
                // decal kotoran adalah instance buram dengan satu ruas yang
                // harus dipadu, dan memutuskannya per instance akan memindahkan
                // seluruh dindingnya ke jalur tersortir.
                const bool materialBlended =
                    material != kInvalidMaterial && material <= materials_.size() &&
                    materials_[static_cast<std::size_t>(material) - 1]->blended;
                const uint32_t materialSlot =
                    material == kInvalidMaterial
                        ? 0u
                        : materials_[static_cast<std::size_t>(material) - 1]->slot;
                const BoxInstance instance =
                    MakeInstance(color, mesh.receiveShadows, skinned ? mesh.skinFirst : 0u,
                                 materialSlot, static_cast<uint32_t>(texture));

                SurfaceEntry entry{mesh.mesh,
                                   static_cast<uint32_t>(partIndex),
                                   material,
                                   texture,
                                   skinned,
                                   instance,
                                   transform,
                                   mesh.castShadows,
                                   world,
                                   cameraVisible,
                                   distance};
                if (opaqueInstance && !materialBlended) {
                    gathered_.push_back(entry);
                } else if (cameraVisible) {
                    // **Yang di luar pandangan dijatuhkan, bukan disortir.**
                    // Daftar tersortir digambar seluruhnya tanpa penyaringan
                    // lagi — pass transparan tidak punya prepass yang menyaring
                    // untuknya — dan ia juga tidak menjatuhkan bayangan, jadi
                    // tidak ada alasan tersisa untuk membawanya. Aturan yang
                    // sama sudah berlaku untuk instance tembus pandang, satu
                    // tingkat di atas.
                    sorted_.push_back(entry);
                }
            }
        }

        // **Yang menjatuhkan bayangan lebih dulu, lalu dikelompokkan.** Kunci
        // pertama menjaga sifat lama: pass bayangan menggambar awalan daftarnya
        // tanpa atribut tambahan dan tanpa cabang di shader.
        //
        // `stable_sort`, bukan `sort` — urutan di antara permukaan yang seluruh
        // kuncinya sama memang tidak berarti apa-apa, tapi urutan yang
        // berubah-ubah tiap frame membuat setiap perbandingan gambar menjadi
        // tidak bisa dipakai.
        std::stable_sort(gathered_.begin(), gathered_.end(),
                         [](const SurfaceEntry& a, const SurfaceEntry& b) {
                             if (a.caster != b.caster) {
                                 return a.caster;
                             }
                             // **Kunci kedua: yang terlihat kamera lebih dulu.**
                             // Sejak caster di luar pandangan ikut didaftar,
                             // mencampurnya di antara yang terlihat memecah ruas
                             // pandangan utama menjadi potongan-potongan pendek —
                             // dan yang dibayar adalah draw call tambahan di
                             // prepass dan forward, yaitu dua pass yang justru
                             // tidak punya urusan dengan bayangan. Dengan kunci
                             // ini, yang terlihat menjadi satu awalan bersambung
                             // di dalam ruas caster, dan pass bayangan tidak
                             // kehilangan apa pun: ia toh menyaring ulang per
                             // muka.
                             if (a.cameraVisible != b.cameraVisible) {
                                 return static_cast<int>(a.cameraVisible) >
                                        static_cast<int>(b.cameraVisible);
                             }
                             if (a.mesh != b.mesh) {
                                 return a.mesh < b.mesh;
                             }
                             // Yang berkulit dan yang tidak memakai pipeline yang
                             // berbeda, jadi mencampurnya di dalam satu ruas
                             // berarti sebagian permukaan digambar lewat pipeline
                             // yang bukan miliknya.
                             if (a.skinned != b.skinned) {
                                 return static_cast<int>(a.skinned) > static_cast<int>(b.skinned);
                             }
                             if (a.part != b.part) {
                                 return a.part < b.part;
                             }
                             // Material menentukan pipeline, tekstur menentukan
                             // descriptor set jalur mundur. Keduanya kunci ruas.
                             if (a.material != b.material) {
                                 return a.material < b.material;
                             }
                             return a.texture < b.texture;
                         });

        opaque_.reserve(gathered_.size());
        opaqueTransforms_.reserve(gathered_.size());
        instanceBounds_.reserve(gathered_.size());
        instanceVisible_.reserve(gathered_.size());
        cullBounds_.clear();
        cullSurfaces_.clear();
        cullBounds_.reserve(gathered_.size());
        cullSurfaces_.reserve(gathered_.size());
        for (const SurfaceEntry& entry : gathered_) {
            if (entry.caster) {
                ++casterCount_;
            }
            AppendRun(opaqueRuns_, entry, static_cast<uint32_t>(opaque_.size()));
            opaque_.push_back(entry.instance);
            opaqueTransforms_.push_back(entry.transform);
            instanceBounds_.push_back(entry.bounds);
            instanceVisible_.push_back(entry.cameraVisible ? 1u : 0u);
            // Bahan untuk pass culling. **Pusat dan setengah-lebar dihitung di
            // sini, sekali**, bukan di shader dari min/max: `Frustum::Intersects`
            // memakai keduanya, dan dua pembagian yang seharusnya menghasilkan
            // angka yang sama adalah dua angka yang suatu saat berbeda satu ULP.
            cullBounds_.push_back(DrawCull::GpuBounds{Vec4(entry.bounds.Centre(), 0.0f),
                                                      Vec4(entry.bounds.Extent(), 0.0f)});
            const GpuMesh::Part& part =
                meshes_[static_cast<std::size_t>(entry.mesh)]->parts[entry.part];
            cullSurfaces_.push_back(DrawCull::GpuSurface{part.indexCount, part.firstIndex});
        }

        // Ruas untuk pandangan utama. Dipecah, bukan disaring saat menggambar:
        // yang tidak terlihat kamera tetap ada di dalam buffer karena bayangannya
        // dibutuhkan, dan menggambarnya di prepass maupun forward berarti
        // membayar shading untuk piksel yang tidak ada.
        const std::vector<uint8_t>& visible = instanceVisible_;
        SplitRuns(opaqueRuns_, [&visible](uint32_t index) { return visible[index] != 0; },
                  visibleOpaqueRuns_);
        // Ruas bayangan adalah awalan daftar yang sama, dipotong di
        // `casterCount_`. Dipotong, bukan dibangun ulang: dua daftar yang harus
        // sepakat adalah dua daftar yang suatu saat tidak sepakat.
        for (const DrawRun& run : opaqueRuns_) {
            if (run.first >= casterCount_) {
                break;
            }
            DrawRun caster = run;
            caster.count = std::min(run.count, casterCount_ - run.first);
            casterRuns_.push_back(caster);
        }

        // Belakang ke depan. Alpha blending tidak komutatif: dua kaca yang
        // dicampur dengan urutan terbalik menghasilkan warna yang berbeda, dan
        // "berbeda" di sini berarti kaca yang lebih jauh terlihat di depan yang
        // lebih dekat.
        //
        // **Urutan menang atas pengelompokan di sini**, kebalikan dari yang
        // buram. Mengelompokkan tembus pandang per mesh berarti menggambarnya di
        // luar urutan jaraknya, dan yang didapat — beberapa draw call lebih
        // sedikit — jauh lebih murah daripada yang hilang.
        std::stable_sort(sorted_.begin(), sorted_.end(),
                         [](const SurfaceEntry& a, const SurfaceEntry& b) {
                             return a.distance > b.distance;
                         });
        transparent_.reserve(sorted_.size());
        transparentTransforms_.reserve(sorted_.size());
        for (const SurfaceEntry& entry : sorted_) {
            AppendRun(transparentRuns_, entry, static_cast<uint32_t>(transparent_.size()));
            transparent_.push_back(entry.instance);
            transparentTransforms_.push_back(entry.transform);
        }
    }

    /// Apakah instance ini benar-benar bisa digambar lewat jalur berkulit.
    ///
    /// Tiga syarat, dan ketiganya bisa gagal sendiri-sendiri: mesh-nya harus
    /// punya bobot skin di GPU, pemanggilnya harus memasok pose, dan pose itu
    /// harus sepanjang rangkanya serta berada di dalam larik yang dikirimkannya.
    /// **Yang gagal digambar sebagai mesh statis, bukan dilewati.** Vertexnya
    /// sudah berada di bind pose, jadi yang terlihat adalah karakter yang berdiri
    /// diam — bukan karakter yang lenyap, dan bukan pula shader yang membaca
    /// matriks milik instance lain.
    bool IsSkinnable(const MeshInstance& instance, std::size_t paletteSize) const {
        if (instance.skinCount == 0 || instance.mesh >= meshes_.size()) {
            return false;
        }
        const GpuMesh& mesh = *meshes_[static_cast<std::size_t>(instance.mesh)];
        if (mesh.boneCount == 0 || instance.skinCount != mesh.boneCount) {
            return false;
        }
        return static_cast<std::size_t>(instance.skinFirst) + instance.skinCount <= paletteSize;
    }

    /// Menambah instance ke ruas terakhir bila mesh dan jalurnya sama, atau
    /// membuka ruas baru.
    /// Sepasang (entity, ruas mesh) sebelum diurutkan menjadi ruas draw.
    ///
    /// **Satu bentuk untuk buram dan tembus pandang.** Sampai G6 keduanya punya
    /// struct sendiri yang isinya nyaris sama; satu-satunya yang benar-benar
    /// hanya milik tembus pandang adalah jaraknya, dan sebuah medan yang tidak
    /// dibaca lebih murah daripada dua struct yang harus dijaga sepakat.
    struct SurfaceEntry {
        MeshHandle mesh = kUnitCubeMesh;
        uint32_t part = 0;
        MaterialHandle material = kInvalidMaterial;
        TextureHandle texture = kInvalidTexture;
        bool skinned = false;
        BoxInstance instance;
        Mat4 transform{1.0f};
        bool caster = false;
        /// Kotak dunia entity ini, disimpan supaya tiap pass bayangan bisa
        /// mengujinya terhadap volumenya sendiri. Tanpa ini setiap pass harus
        /// mentransformasikan ulang kotak lokalnya — 32 muka atlas dikali
        /// ribuan permukaan berarti puluhan ribu transformasi kotak per frame
        /// untuk jawaban yang tidak berubah di antara keduanya.
        Aabb bounds;
        /// Terlihat kamera. **Bukan syarat untuk ikut didaftar**: caster yang
        /// berada di luar pandangan tetap harus menjatuhkan bayangan ke dalamnya.
        bool cameraVisible = true;
        /// Jarak ke kamera. Dibaca hanya jalur tembus pandang.
        float distance = 0.0f;
    };
    /// Buffer perintah pass forward.
    ///
    /// **Dua buffer, dan yang membedakannya siapa yang menyaring.** Tanpa
    /// occlusion culling, forward menggambar himpunan yang sama dengan prepass —
    /// yaitu isi frustum. Dengan occlusion, ia menggambar yang lolos uji
    /// terhadap piramida depth yang baru saja dibangun dari prepass itu.
    VkBuffer OpaqueCommandBuffer() const {
        if (!gpuCullActive_) {
            return VK_NULL_HANDLE;
        }
        const auto slot = static_cast<uint32_t>(slotIndex_);
        return gpuOcclusionActive_ ? drawCull_.VisibleCommandBuffer(slot)
                                   : drawCull_.CommandBuffer(slot);
    }

    /// Ruas yang digambar pandangan utama — prepass dan forward buram.
    ///
    /// **Dua daftar, dan yang membedakan siapa yang menyaring.** Jalur CPU
    /// memakai daftar yang sudah dipecah `SplitRuns` sehingga hanya memuat yang
    /// terlihat; jalur GPU memakai daftar utuh, karena yang menyaring adalah
    /// `instanceCount` di dalam perintah gambarnya. Daftar utuh juga lebih
    /// pendek: ia tidak terpecah oleh batas terlihat/tidak.
    std::span<const DrawRun> MainViewRuns() const {
        return gpuCullActive_ ? std::span<const DrawRun>(opaqueRuns_)
                              : std::span<const DrawRun>(visibleOpaqueRuns_);
    }

    /// Menyambung sebuah permukaan ke ruas terakhir, atau membuka ruas baru.
    ///
    /// **Kuncinya adalah segala sesuatu yang tidak bisa berubah di tengah satu
    /// panggilan gambar**: mesh dan ruasnya menentukan buffer dan rentang
    /// indeksnya, material menentukan pipeline-nya, tekstur menentukan
    /// descriptor set jalur mundur, dan kulit menentukan keduanya sekaligus.
    /// Warna, slot material, dan slot tekstur **tidak** ada di sini — ketiganya
    /// data instance sejak G6, dan itulah yang membuat ribuan entity menjadi
    /// puluhan panggilan.
    static void AppendRun(std::vector<DrawRun>& runs, const SurfaceEntry& entry, uint32_t index) {
        if (!runs.empty() && runs.back().mesh == entry.mesh && runs.back().part == entry.part &&
            runs.back().material == entry.material && runs.back().texture == entry.texture &&
            runs.back().skinned == entry.skinned) {
            ++runs.back().count;
            return;
        }
        runs.push_back(DrawRun{entry.mesh, entry.part, entry.material, entry.texture,
                               entry.skinned, index, 1});
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

    /// Memutuskan jalur material, dan **menyebutkan alasannya**.
    ///
    /// Alasannya ikut karena jalur yang dipilih diam-diam adalah jalur yang
    /// tidak ada yang tahu sedang berjalan — persis alasan pemilih backend GI
    /// menuliskan alasannya. Di sini akibatnya lebih tajam: pada mesin yang
    /// mampu bindless, jalur mundur berhenti dijalankan siapa pun pada hari ini
    /// mendarat, sementara justru jalur itu yang harus tetap bekerja di GPU yang
    /// tidak mampu.
    void SelectMaterialBinding(MaterialBindingPreference preference) {
        const rhi::DeviceCapabilities& caps = device_.Capabilities();
        if (!caps.descriptorIndexing) {
            SIM_INFO("Render",
                     "material binding: per-part sets (this GPU has no descriptor indexing)");
            return;
        }
        if (preference == MaterialBindingPreference::ForceClassic) {
            SIM_INFO("Render", "material binding: per-part sets (forced)");
            return;
        }
        bindless_ = true;
        bindlessCapacity_ = caps.bindlessTextureCapacity;
        SIM_INFO("Render", "material binding: bindless ({} texture slots, {} material slots)",
                 bindlessCapacity_, kMaxBindlessMaterials);
    }

    /// `vkCmdBindDescriptorSets` yang ikut terhitung.
    ///
    /// **Setiap pengikatan lewat sini, termasuk yang bukan milik material.**
    /// Angka yang hanya menghitung sebagian adalah angka yang menjawab
    /// pertanyaan lain daripada yang ditanyakan kriteria selesai G5 — dan yang
    /// membacanya tidak punya cara mengetahuinya.
    void BindSets(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint, VkPipelineLayout layout,
                  uint32_t firstSet, uint32_t count, const VkDescriptorSet* sets,
                  uint32_t dynamicCount, const uint32_t* dynamicOffsets) {
        vkCmdBindDescriptorSets(cmd, bindPoint, layout, firstSet, count, sets, dynamicCount,
                                dynamicOffsets);
        ++stats_.descriptorSetBinds;
    }

    void DrawInstances(VkCommandBuffer cmd, const PipelineVariants& pipelines, const BoxPush& push,
                       InstanceSlot& slot, std::span<const DrawRun> runs, uint32_t instanceBase,
                       int materialVariant = -1, VkBuffer indirectCommands = VK_NULL_HANDLE,
                       bool skipMasked = false) {
        // Descriptor set diikat untuk setiap pipeline forward, termasuk prepass.
        // Prepass tidak membacanya, tapi layout-nya mendeklarasikannya — dan
        // set yang dideklarasikan tapi tidak terikat adalah pelanggaran meski
        // tidak ada yang membacanya.
        if (pipelines[0] == VK_NULL_HANDLE || runs.empty()) {
            return;
        }
        // Set 1 palet kulit, diikat untuk kedua varian. **Termasuk yang statis**:
        // yang menentukan sebuah descriptor "terpakai" adalah modul SPIR-V, dan
        // di sana cabang `kSkinned` masih ada — spesialisasi baru menilainya
        // sesudah itu.
        //
        // **Set 2 ikut di sini pada jalur bindless, dan hanya di sini.** Itulah
        // seluruh isi G5: satu ikatan di awal pass menggantikan satu ikatan per
        // ruas. Pada jalur mundur ia tetap diikat di dalam gelung, karena di
        // sana setiap material adalah set yang berbeda.
        const std::array<VkDescriptorSet, 3> sets{slot.shadowSet, slot.skinSet, bindlessSet_};
        BindSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, bindless_ ? 3u : 2u,
                 sets.data(), 0, nullptr);
        vkCmdPushConstants(cmd, pipelineLayout_, kBoxPushStages, 0, sizeof(BoxPush), &push);
        DrawRuns(cmd, slot, runs, instanceBase, pipelines, pipelineLayout_,
                 /*bindsMaterial=*/true, materialVariant, indirectCommands, skipMasked);
    }

    /// Mengikat geometri tiap ruas lalu menggambarnya. Dipakai bersama pass
    /// forward dan pass bayangan — keduanya menggambar geometri yang sama
    /// dengan pipeline yang berbeda, dan menyalin pengikatannya ke dua tempat
    /// adalah cara termudah membuat keduanya suatu saat berbeda.
    ///
    /// **Pipeline diikat di sini, bukan oleh pemanggilnya.** Sejak jalur berkulit
    /// ada, dua ruas berturut-turut bisa menuntut pipeline yang berbeda — dan
    /// pemanggil yang mengikatnya sekali di awal akan menggambar sebagian ruas
    /// lewat pipeline yang bukan miliknya, tanpa satu pun galat.
    /// `bindsMaterial` false untuk pass bayangan. **Bukan optimisasi.**
    /// `shadowLayout_` hanya mendeklarasikan satu set, jadi mengikat set material
    /// di nomor dua di sana adalah `firstSet + descriptorSetCount >
    /// setLayoutCount` — perilaku tak terdefinisi yang di mesin ini berakhir
    /// sebagai segfault beberapa detik sesudah adegan yang menjatuhkan bayangan
    /// selesai dimuat, jauh dari sebabnya.
    /// `skipMasked` melewatkan ruas yang materialnya bertopeng. **Dipakai
    /// prepass dan pass bayangan, dan keduanya karena alasan yang sama:** yang
    /// menentukan bentuk material bertopeng adalah fragmen yang dibuangnya, dan
    /// kedua pass itu tidak menjalankan shader materialnya sama sekali. Yang
    /// ikut serta di sana akan menulis kedalaman — dan menjatuhkan bayangan —
    /// berbentuk kuad penuh, termasuk di bagian yang seharusnya tidak ada.
    void DrawRuns(VkCommandBuffer cmd, InstanceSlot& slot, std::span<const DrawRun> runs,
                  uint32_t instanceBase, const PipelineVariants& pipelines,
                  VkPipelineLayout layout, bool bindsMaterial, int materialVariant = -1,
                  VkBuffer indirectCommands = VK_NULL_HANDLE, bool skipMasked = false) {
        VkPipeline bound = VK_NULL_HANDLE;
        // **Bukan sebuah handle sentinel**: nol adalah kubus satuan, yaitu mesh
        // yang sah dan yang paling sering muncul. Bendera terpisah karena itu,
        // bukan nilai ajaib.
        bool boundGeometry = false;
        MeshHandle boundMesh = kUnitCubeMesh;
        bool boundSkinned = false;
        for (const DrawRun& run : runs) {
            if (run.count == 0 || run.mesh >= meshes_.size()) {
                continue;
            }
            if (skipMasked && run.material != kInvalidMaterial &&
                run.material <= materials_.size() &&
                materials_[static_cast<std::size_t>(run.material) - 1]->masked) {
                continue;
            }
            const GpuMesh& mesh = *meshes_[static_cast<std::size_t>(run.mesh)];
            if (run.part >= mesh.parts.size()) {
                continue;
            }
            const GpuMesh::Part& part = mesh.parts[static_cast<std::size_t>(run.part)];
            if (part.indexCount == 0) {
                continue;
            }
            // Ruas berkulit yang mesh-nya ternyata tidak punya bobot digambar
            // statis, bukan dilewati: `Gather` sudah menyaringnya, dan yang di
            // sini adalah jaring pengaman supaya buffer skin yang tidak ada tidak
            // pernah bisa terikat.
            const bool skinned = run.skinned && mesh.skin.IsValid();
            const VkPipeline fallbackPipeline = pipelines[skinned ? 1u : 0u];
            if (fallbackPipeline == VK_NULL_HANDLE) {
                continue;
            }

            // **Material ruas ini kalau ada, jalur mundur `box.frag` kalau
            // tidak.** Keduanya hidup berdampingan dengan sengaja: ruas yang
            // materialnya belum dikompilasi, gagal dikompilasi, atau memang
            // tidak punya material tetap tergambar — dan viewport tidak pernah
            // kosong hanya karena satu material bermasalah.
            const GpuMaterial* material = nullptr;
            if (materialVariant >= 0 && run.material != kInvalidMaterial &&
                run.material <= materials_.size()) {
                material = materials_[static_cast<std::size_t>(run.material) - 1].get();
            }

            const VkPipelineLayout partLayout =
                material != nullptr ? material->pipelineLayout : layout;
            const VkPipeline partPipeline =
                material != nullptr
                    ? material->pipelines[static_cast<std::size_t>(materialVariant)]
                                         [skinned ? 1u : 0u]
                    : fallbackPipeline;
            if (partPipeline == VK_NULL_HANDLE) {
                continue;
            }

            // **Geometri diikat hanya saat mesh-nya benar-benar berganti.**
            // Sejak ruas mesh menjadi kunci ruas draw, dua ruas berturut-turut
            // dari mesh yang sama adalah hal yang biasa — dan mengikat ulang
            // vertex buffer yang sama persis untuk masing-masingnya adalah
            // pekerjaan CPU yang dibayar ribuan kali per frame.
            if (!boundGeometry || run.mesh != boundMesh || skinned != boundSkinned) {
                const std::array<VkBuffer, 3> buffers{
                    mesh.vertices.Handle(), slot.buffer.Handle(),
                    skinned ? mesh.skin.Handle() : dummySkin_.Handle()};
                const std::array<VkDeviceSize, 3> offsets{0, 0, 0};
                vkCmdBindVertexBuffers(cmd, 0, 3, buffers.data(), offsets.data());
                vkCmdBindIndexBuffer(cmd, mesh.indices.Handle(), 0, VK_INDEX_TYPE_UINT32);
                boundGeometry = true;
                boundMesh = run.mesh;
                boundSkinned = skinned;
            }

            // **Pipeline diikat di dalam gelung, bukan di luarnya.** Dua ruas
            // berturut-turut bisa memakai material yang berbeda, dan yang
            // mengikatnya sekali di awal akan menggambar sebagiannya lewat
            // pipeline yang bukan miliknya — tanpa satu pun galat.
            if (partPipeline != bound) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, partPipeline);
                bound = partPipeline;
            }

            if (!bindless_) {
                VkDescriptorSet materialSet = VK_NULL_HANDLE;
                if (material != nullptr) {
                    materialSet = material->set;
                } else if (bindsMaterial) {
                    materialSet =
                        run.texture == kInvalidTexture
                            ? fallbackSet_
                            : materialTextures_[static_cast<std::size_t>(run.texture) - 1]->set;
                }
                if (materialSet != VK_NULL_HANDLE) {
                    BindSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, partLayout, 2, 1, &materialSet,
                             0, nullptr);
                }
            }

            // **Basis instance tidak dikirim lagi, ia sudah ada di draw-nya.**
            // `firstInstance` adalah `SV_StartInstanceLocation` yang dibaca tahap
            // vertex; mengirimnya lagi lewat push constant berarti dua salinan
            // dari satu angka — dan indirect draw menutup pilihan itu
            // seluruhnya, karena perintahnya dibangkitkan GPU.
            if (indirectCommands != VK_NULL_HANDLE) {
                // Satu panggilan untuk seluruh permukaan ruas ini, berapa pun
                // banyaknya. Yang tersaring ada di sana juga, dengan
                // `instanceCount` nol — lihat catatan di `DrawCull`.
                vkCmdDrawIndexedIndirect(
                    cmd, indirectCommands,
                    static_cast<VkDeviceSize>(run.first) * sizeof(VkDrawIndexedIndirectCommand),
                    run.count, sizeof(VkDrawIndexedIndirectCommand));
            } else {
                // **`firstIndex` lalu `vertexOffset`, dan urutan itu sempat
                // tertukar.** `SubMesh::firstIndex` adalah offset ke dalam
                // buffer indeks; menyerahkannya sebagai `vertexOffset` berarti
                // ruas kedua sebuah mesh membaca indeks milik ruas pertama lalu
                // menggeser vertexnya sejauh itu. Mesh berruas satu — yaitu
                // hampir semuanya — punya `firstIndex` nol, jadi keduanya
                // menghasilkan gambar yang sama dan tidak ada yang menemukannya.
                // Yang menemukannya jalur indirect, yang menuliskan kedua medan
                // itu dengan namanya masing-masing.
                vkCmdDrawIndexed(cmd, part.indexCount, run.count,
                                 /*firstIndex=*/part.firstIndex, /*vertexOffset=*/0,
                                 instanceBase + run.first);
            }
            ++stats_.drawCalls;
        }
    }

    /// Mengunggah sebuah mesh dan mengembalikan handle-nya, atau nol bila gagal.
    void SetMeshDistanceField(MeshHandle mesh, std::shared_ptr<const SdfGrid> grid) override {
        const auto index = static_cast<std::size_t>(mesh);
        if (index >= meshes_.size()) {
            return;
        }
        if (meshFields_.size() <= index) {
            meshFields_.resize(index + 1);
        }
        if (meshFields_[index] == grid) {
            return;
        }
        meshFields_[index] = std::move(grid);
        if (meshFields_[index] != nullptr) {
            SIM_INFO("Render", "mesh {} now has a baked distance field ({}x{}x{} at {:.3f} m)",
                     index, meshFields_[index]->sizeX, meshFields_[index]->sizeY,
                     meshFields_[index]->sizeZ, meshFields_[index]->voxelSize);
        }
    }

    MeshHandle UploadMesh(const assets::MeshData& data) {
        auto mesh = std::make_unique<GpuMesh>();
        const VkDeviceSize vertexBytes = sizeof(assets::MeshVertex) * data.vertices.size();
        const VkDeviceSize indexBytes = sizeof(uint32_t) * data.indices.size();
        if (!mesh->vertices.Create(device_, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertexBytes) ||
            !mesh->vertices.Write(data.vertices.data(), vertexBytes) ||
            !mesh->indices.Create(device_, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indexBytes) ||
            !mesh->indices.Write(data.indices.data(), indexBytes)) {
            return kUnitCubeMesh;
        }
        // **Rangka yang lebih besar daripada yang bisa diindeks atribut skin
        // ditolak seluruhnya, bukan dipotong.** Indeks bone-nya 16 bit, jadi rig
        // di atas itu akan membungkus diam-diam ke bone yang salah — kulit yang
        // mengikuti tulang acak, tanpa satu pun galat.
        if (data.IsSkinned() && data.skeleton.bones.size() <= 0xFFFFu) {
            const VkDeviceSize skinBytes = sizeof(assets::SkinInfluence) * data.influences.size();
            if (mesh->skin.Create(device_, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, skinBytes) &&
                mesh->skin.Write(data.influences.data(), skinBytes)) {
                mesh->boneCount = static_cast<uint32_t>(data.skeleton.bones.size());
            } else {
                SIM_WARN("Render", "cannot upload skin weights; the mesh stays at its bind pose");
                mesh->skin.Destroy();
            }
        } else if (data.IsSkinned()) {
            SIM_WARN("Render", "skeleton has {} bones, above the 65535 an index can name",
                     data.skeleton.bones.size());
        }
        mesh->indexCount = static_cast<uint32_t>(data.indices.size());
        mesh->triangleCount = static_cast<uint32_t>(data.TriangleCount());
        mesh->vertexCount = static_cast<uint32_t>(data.vertices.size());
        for (const assets::SubMesh& part : data.parts) {
            GpuMesh::Part gpuPart;
            gpuPart.firstIndex = part.firstIndex;
            gpuPart.indexCount = part.indexCount;
            if (part.material >= 0 &&
                static_cast<std::size_t>(part.material) < data.materials.size()) {
                const assets::MeshMaterial& material =
                    data.materials[static_cast<std::size_t>(part.material)];
                gpuPart.baseColor = Vec4(material.baseColor, 1.0f);
            }
            mesh->parts.push_back(gpuPart);
        }
        if (mesh->parts.empty()) {
            mesh->parts.push_back(GpuMesh::Part{0, mesh->indexCount, Vec4(0.0f)});
        }
        mesh->boundsMin = data.boundsMin;
        mesh->boundsMax = data.boundsMax;
        // **Kotak batas yang berbohong tidak menghasilkan satu pun galat.** Ia
        // dipakai frustum culling, occlusion culling, dan lintasan kamera alat
        // ukur; simpul yang menonjol keluar darinya berarti benda yang dibuang
        // padahal terlihat, atau — lebih buruk — tergambar di tempat yang tidak
        // diperhitungkan siapa pun. Diperiksa sekali saat unggah, bukan
        // dipercaya.
        if (!data.vertices.empty()) {
            Vec3 low = data.vertices[0].position;
            Vec3 high = low;
            for (const assets::MeshVertex& vertex : data.vertices) {
                low = glm::min(low, vertex.position);
                high = glm::max(high, vertex.position);
            }
            const Vec3 slack = glm::max(data.boundsMax - data.boundsMin, Vec3(1.0f)) * 1e-3f;
            if (glm::any(glm::lessThan(low, data.boundsMin - slack)) ||
                glm::any(glm::greaterThan(high, data.boundsMax + slack))) {
                SIM_WARN("Render",
                         "a mesh has vertices outside the bounds it reports: "
                         "[{:.3f} {:.3f} {:.3f}]..[{:.3f} {:.3f} {:.3f}] vs "
                         "[{:.3f} {:.3f} {:.3f}]..[{:.3f} {:.3f} {:.3f}]",
                         low.x, low.y, low.z, high.x, high.y, high.z, data.boundsMin.x,
                         data.boundsMin.y, data.boundsMin.z, data.boundsMax.x, data.boundsMax.y,
                         data.boundsMax.z);
            }
        }
        // **Indeks yang menunjuk ke luar buffer simpul tidak menghasilkan satu
        // pun galat.** Vulkan menyebutnya perilaku tak terdefinisi, dan yang
        // keluar dari driver ini adalah simpul berisi sampah. Diperiksa sekali
        // saat unggah, alasan yang sama persis dengan pemeriksaan kotak batas
        // di atas.
        {
            uint32_t highest = 0;
            for (uint32_t value : data.indices) {
                highest = std::max(highest, value);
            }
            if (!data.indices.empty() &&
                static_cast<std::size_t>(highest) >= data.vertices.size()) {
                SIM_WARN("Render",
                         "a mesh has indices past its vertex buffer: highest {} of {} vertices",
                         highest, data.vertices.size());
            }
            for (std::size_t at = 0; at < mesh->parts.size(); ++at) {
                const GpuMesh::Part& part = mesh->parts[at];
                if (static_cast<std::size_t>(part.firstIndex) + part.indexCount >
                    data.indices.size()) {
                    SIM_WARN("Render",
                             "mesh part {} runs past the index buffer: [{}, {}) of {} indices", at,
                             part.firstIndex, part.firstIndex + part.indexCount,
                             data.indices.size());
                }
            }
        }
        meshes_.push_back(std::move(mesh));
        return static_cast<MeshHandle>(meshes_.size() - 1);
    }

    /// Kubus satuan menjadi mesh nol.
    ///
    /// **Diberi buffer indeks walaupun tidak butuh**, supaya seluruh jalur
    /// gambar memakai `vkCmdDrawIndexed` tanpa kecuali. Satu cabang "mesh ini
    /// berindeks, yang itu tidak" akan berlipat di setiap pass yang menggambar
    /// geometri — dan pass bayangan, prepass, forward buram, dan forward tembus
    /// pandang semuanya menggambar geometri yang sama.
    bool CreateCube() {
        const std::vector<BoxVertex> vertices = BuildUnitCube();
        assets::MeshData cube;
        cube.vertices.resize(vertices.size());
        std::memcpy(cube.vertices.data(), vertices.data(), sizeof(BoxVertex) * vertices.size());
        cube.indices.resize(vertices.size());
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            cube.indices[i] = static_cast<uint32_t>(i);
        }
        cube.ComputeBounds();
        meshes_.clear();
        return UploadMesh(cube) == kUnitCubeMesh && !meshes_.empty();
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
        SIM_VK_CHECK(vkCreateGraphicsPipelines(device_.Handle(), device_.PipelineCache(), 1, &info, nullptr,
                                               &pipeline));
        return pipeline;
    }

    bool CreatePipelines(const std::filesystem::path& shaderDirectory) {
        VkShaderModule vertex = CreateShaderModule(device_.Handle(), shaderDirectory / "box.vert.spv");
        // Dua modul, bukan satu modul dengan konstanta spesialisasi: yang
        // berbeda antara keduanya bukan sebuah nilai melainkan bentuk descriptor
        // set layout-nya, dan itu sudah ditetapkan sebelum spesialisasi dinilai.
        VkShaderModule fragment = CreateShaderModule(
            device_.Handle(),
            shaderDirectory / (bindless_ ? "box_bindless.frag.spv" : "box.frag.spv"));
        if (vertex == VK_NULL_HANDLE || fragment == VK_NULL_HANDLE) {
            return false;
        }

        VkPushConstantRange range{};
        range.stageFlags = kBoxPushStages;
        range.size = sizeof(BoxPush);
        // Set 0 keadaan bayangan per-frame, set 1 palet kulit. Nomornya harus
        // sama dengan `SKIN_SET` di box.vert dan prepass.vert.
        const std::array<VkDescriptorSetLayout, 3> forwardSets{
            shadowSetLayout_, skinSetLayout_,
            bindless_ ? bindlessSetLayout_ : materialSetLayout_};
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &range;
        layoutInfo.setLayoutCount = static_cast<uint32_t>(forwardSets.size());
        layoutInfo.pSetLayouts = forwardSets.data();
        SIM_VK_CHECK(
            vkCreatePipelineLayout(device_.Handle(), &layoutInfo, nullptr, &pipelineLayout_));

        // Pass bayangan memakai layout sendiri: ia tidak membaca peta bayangan,
        // dan mendeklarasikan descriptor set yang tidak pernah diikat berarti
        // setiap draw-nya melanggar aturan validasi. Yang tersisa untuknya hanya
        // palet kulit — dan karena itu palet jatuh ke set 0 di sini, sama dengan
        // `SKIN_SET` di shadow.vert.
        VkPipelineLayoutCreateInfo shadowLayoutInfo{};
        shadowLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        shadowLayoutInfo.pushConstantRangeCount = 1;
        shadowLayoutInfo.pPushConstantRanges = &range;
        shadowLayoutInfo.setLayoutCount = 1;
        shadowLayoutInfo.pSetLayouts = &skinSetLayout_;
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
        // Atribut 0 dan 8..10: posisi dan ketiga atribut skin. Normal, warna,
        // dan bendera tidak dibaca pass bayangan; transform instance tidak lagi
        // menjadi atribut sama sekali.
        //
        // **Atribut skin ikut walaupun `kSkinned` mati.** Slang mengunci daftar
        // antarmuka entry point sebelum konstanta spesialisasi dinilai, jadi
        // ketiganya tetap ada di kedua varian — dan pipeline yang tidak
        // menyediakan atribut yang dikonsumsi shader-nya adalah galat validasi
        // di setiap draw.
        for (std::size_t skinned = 0; skinned < kPipelineVariants; ++skinned) {
            shadowPipelines_[skinned] = BuildPipeline(
                shadowVertex, VK_NULL_HANDLE, /*depthWrite=*/true, VK_COMPARE_OP_LESS_OR_EQUAL,
                /*blend=*/false, /*colorWrite=*/false, /*skinned=*/skinned != 0, shadowLayout_,
                kShadowFormat, /*colorAttachment=*/VK_FORMAT_UNDEFINED,
                /*attributeMask=*/0b0111'0000'0001u);
        }
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
        // Atribut 0, 1, dan 8..10: posisi, normal, dan skin. Warna dan bendera
        // tidak dibacanya.
        for (std::size_t skinned = 0; skinned < kPipelineVariants; ++skinned) {
            prepassPipelines_[skinned] =
                BuildPipeline(prepassVertex, prepassFragment, /*depthWrite=*/true,
                              VK_COMPARE_OP_GREATER, /*blend=*/false, /*colorWrite=*/true,
                              /*skinned=*/skinned != 0, /*layout=*/VK_NULL_HANDLE,
                              /*depthFormat=*/VK_FORMAT_UNDEFINED, kNormalFormat,
                              /*attributeMask=*/0b0111'0000'0011u);
            // Uji EQUAL, bukan GREATER: depth-nya sudah diisi prepass, jadi hanya
            // fragmen yang benar-benar terlihat yang boleh menjalankan shader.
            // Itulah gunanya prepass — bukan menghemat depth test, melainkan
            // menghemat shading yang akan ditimpa.
            //
            // **Prepass dan forward karena itu wajib menghitung posisi yang sama
            // persis**, termasuk kulitnya: prepass yang menaruh vertex di tempat
            // berbeda satu ULP saja membuat uji EQUAL gagal, dan yang terlihat
            // adalah karakter yang lenyap seluruhnya dari pass forward.
            opaquePipelines_[skinned] =
                BuildPipeline(vertex, fragment, /*depthWrite=*/false, VK_COMPARE_OP_EQUAL,
                              /*blend=*/false, /*colorWrite=*/true, /*skinned=*/skinned != 0);
            // Transparan diuji terhadap depth opaque tapi tidak menulisinya: dua
            // permukaan tembus pandang harus sama-sama terlihat, dan yang di depan
            // tidak boleh menghapus yang di belakang.
            transparentPipelines_[skinned] =
                BuildPipeline(vertex, fragment, /*depthWrite=*/false, VK_COMPARE_OP_GREATER,
                              /*blend=*/true, /*colorWrite=*/true, /*skinned=*/skinned != 0);
        }
        for (VkShaderModule module : {prepassVertex, prepassFragment}) {
            if (module != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device_.Handle(), module, nullptr);
            }
        }

        // **`box.vert` tidak dilepas.** Pipeline material memakainya kembali
        // sebagai tahap vertexnya — yang berganti hanya shader fragmen — dan
        // membangunnya ulang dari berkas tiap kali sebuah material dikompilasi
        // berarti membaca `.spv` yang sama puluhan kali per sesi.
        boxVertexModule_ = vertex;
        vkDestroyShaderModule(device_.Handle(), fragment, nullptr);
        for (std::size_t skinned = 0; skinned < kPipelineVariants; ++skinned) {
            if (prepassPipelines_[skinned] == VK_NULL_HANDLE ||
                opaquePipelines_[skinned] == VK_NULL_HANDLE ||
                transparentPipelines_[skinned] == VK_NULL_HANDLE ||
                shadowPipelines_[skinned] == VK_NULL_HANDLE) {
                return false;
            }
        }
        return true;
    }

    /// `layout` dan `depthFormat` kosong berarti memakai milik pass forward.
    /// Pass bayangan menggambar ke image lain dengan format lain, jadi ia harus
    /// menyebutkan keduanya — pipeline yang formatnya tidak cocok dengan
    /// lampiran yang dipasang adalah ketidakcocokan yang hanya dilaporkan
    /// validation layer.
    VkPipeline BuildPipeline(VkShaderModule vertex, VkShaderModule fragment, bool depthWrite,
                             VkCompareOp depthCompare, bool blend, bool colorWrite,
                             bool skinned = false, VkPipelineLayout layout = VK_NULL_HANDLE,
                             VkFormat depthFormat = VK_FORMAT_UNDEFINED,
                             VkFormat colorAttachment = VK_FORMAT_UNDEFINED,
                             uint32_t attributeMask = 0xFFC3u) {
        // `kSkinned`, konstanta spesialisasi 0 di `Shaders/skin_common.slang`.
        // Ukurannya empat byte walaupun tipenya bool: Vulkan menyatakan
        // konstanta spesialisasi boolean berukuran `VkBool32`, dan ukuran yang
        // salah membuat nilainya diam-diam tidak berlaku.
        const VkBool32 skinnedValue = skinned ? VK_TRUE : VK_FALSE;
        const VkSpecializationMapEntry entry{0, 0, sizeof(VkBool32)};
        VkSpecializationInfo specialization{};
        specialization.mapEntryCount = 1;
        specialization.pMapEntries = &entry;
        specialization.dataSize = sizeof(skinnedValue);
        specialization.pData = &skinnedValue;

        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex;
        stages[0].pName = "main";
        stages[0].pSpecializationInfo = &specialization;
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

        // Binding 2 adalah pengaruh skin. **Stride-nya nol pada varian statis**,
        // dan itu yang membuat satu elemen tiruan melayani mesh apa pun: setiap
        // vertex membaca offset yang sama, dan `kSkinned` yang mati membuang
        // hasilnya. Buffer sepanjang mesh untuk data yang tidak pernah dibaca
        // adalah 24 byte per vertex yang dibayar seluruh adegan statis.
        const std::array<VkVertexInputBindingDescription, 3> bindings{
            VkVertexInputBindingDescription{0, sizeof(BoxVertex), VK_VERTEX_INPUT_RATE_VERTEX},
            VkVertexInputBindingDescription{1, sizeof(BoxInstance),
                                            VK_VERTEX_INPUT_RATE_INSTANCE},
            VkVertexInputBindingDescription{
                2, skinned ? static_cast<uint32_t>(sizeof(assets::SkinInfluence)) : 0u,
                VK_VERTEX_INPUT_RATE_VERTEX},
        };
        const std::array<VkVertexInputAttributeDescription, 12> attributes{
            VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                              offsetof(BoxVertex, position)},
            VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                              offsetof(BoxVertex, normal)},
            VkVertexInputAttributeDescription{6, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                                              offsetof(BoxInstance, color)},
            VkVertexInputAttributeDescription{7, 1, VK_FORMAT_R32_UINT,
                                              offsetof(BoxInstance, flags)},
            VkVertexInputAttributeDescription{8, 2, VK_FORMAT_R16G16B16A16_UINT,
                                              offsetof(assets::SkinInfluence, bones)},
            VkVertexInputAttributeDescription{9, 2, VK_FORMAT_R32G32B32A32_SFLOAT,
                                              offsetof(assets::SkinInfluence, weights)},
            VkVertexInputAttributeDescription{10, 1, VK_FORMAT_R32_UINT,
                                              offsetof(BoxInstance, skinBase)},
            // Warna simpul, di binding 0 bersama posisi dan normal — bukan
            // binding sendiri. Binding terpisah menuntut stride yang berbeda
            // untuk mesh yang punya dan yang tidak, dan stride adalah sifat
            // pipeline: ia akan melipatgandakan varian pipeline yang sudah ada
            // demi menghemat memori GPU yang tetap dibayar karena strukturnya
            // seragam.
            VkVertexInputAttributeDescription{11, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                                              offsetof(BoxVertex, color)},
            // UV, juga di binding 0: ia sudah terunggah di dalam `MeshVertex`
            // sejak mesh sungguhan masuk, dan yang belum ada selama ini hanya
            // baris ini.
            VkVertexInputAttributeDescription{12, 0, VK_FORMAT_R32G32_SFLOAT,
                                              offsetof(BoxVertex, uv)},
            // Tangent, untuk normal map. Lokasi 13 dan bukan mengisi lubang
            // 2..5 yang ditinggalkan transform instance: lubang itu memang
            // dibiarkan supaya tidak ada satu pun atribut yang bergeser.
            VkVertexInputAttributeDescription{13, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                                              offsetof(BoxVertex, tangent)},
            // Slot material dan tekstur, di binding 1 bersama warna dan bendera:
            // keduanya milik permukaan, bukan milik simpul. Lihat catatannya di
            // `BoxInstance`.
            VkVertexInputAttributeDescription{14, 1, VK_FORMAT_R32_UINT,
                                              offsetof(BoxInstance, materialSlot)},
            VkVertexInputAttributeDescription{15, 1, VK_FORMAT_R32_UINT,
                                              offsetof(BoxInstance, textureSlot)},
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
        std::array<VkVertexInputAttributeDescription, 12> used{};
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
        SIM_VK_CHECK(vkCreateGraphicsPipelines(device_.Handle(), device_.PipelineCache(), 1, &info, nullptr,
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

        const std::array<VkDescriptorSetLayoutBinding, 24> bindings{
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
            VkDescriptorSetLayoutBinding{21, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{22, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{23, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                         SdfClipmapResource::kMaxGrids,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        SIM_VK_CHECK(vkCreateDescriptorSetLayout(device_.Handle(), &layoutInfo, nullptr,
                                                 &shadowSetLayout_));

        const std::array<VkDescriptorPoolSize, 4> sizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                 static_cast<uint32_t>(slots_.size())},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                 static_cast<uint32_t>(slots_.size()) * 16},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                 static_cast<uint32_t>(slots_.size()) * 6},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                 static_cast<uint32_t>(slots_.size()) *
                                     SdfClipmapResource::kMaxGrids},
        };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = static_cast<uint32_t>(slots_.size());
        poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
        poolInfo.pPoolSizes = sizes.data();
        SIM_VK_CHECK(
            vkCreateDescriptorPool(device_.Handle(), &poolInfo, nullptr, &shadowPool_));
        return CreateSkinDescriptors() && CreateMaterialDescriptors() &&
               CreateBindlessDescriptors();
    }

    /// Set descriptor material: parameter, tekstur, sampler — **nomor binding
    /// yang sama persis dengan yang dihasilkan kompiler graph material.**
    ///
    /// Binding 0 (parameter) dideklarasikan walau `box.frag` belum membacanya,
    /// dan diisi buffer kecil bersama. Itu yang membuat janji "pipeline material
    /// mengganti shader-nya, bukan pipa-nya" bisa ditepati: layout-nya sudah
    /// berbentuk final, dan yang berubah nanti hanya siapa yang membacanya.
    bool CreateMaterialDescriptors() {
        const std::array<VkDescriptorSetLayoutBinding, 3> bindings{
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        SIM_VK_CHECK(vkCreateDescriptorSetLayout(device_.Handle(), &layoutInfo, nullptr,
                                                 &materialSetLayout_));

        // Satu set per tekstur, ditambah satu untuk yang mundur. Batasnya
        // disebut angka, bukan dibiarkan tumbuh: pool yang kehabisan
        // mengembalikan galat alokasi di tengah frame, dan yang terlihat adalah
        // tekstur yang hilang secara acak.
        constexpr uint32_t kMaxMaterialSets = 1024;
        const std::array<VkDescriptorPoolSize, 3> sizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxMaterialSets},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxMaterialSets},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, kMaxMaterialSets},
        };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = kMaxMaterialSets;
        poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
        poolInfo.pPoolSizes = sizes.data();
        SIM_VK_CHECK(
            vkCreateDescriptorPool(device_.Handle(), &poolInfo, nullptr, &materialPool_));

        // Buffer parameter bersama. Isinya tidak pernah dibaca hari ini, tetapi
        // binding yang dideklarasikan dan tidak pernah ditulis adalah descriptor
        // tak sah pada setiap pengikatan.
        if (!materialParams_.Create(device_, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 64)) {
            SIM_ERROR("Render", "cannot create material parameter buffer");
            return false;
        }
        const std::array<float, 16> zeros{};
        materialParams_.Write(zeros.data(), sizeof(zeros));

        // Tekstur mundur putih 1x1. Putih adalah nilai satuan perkalian, jadi
        // ruas tanpa tekstur tergambar persis seperti sebelum jalur ini ada.
        const uint32_t white = 0xFFFFFFFFu;
        if (!fallbackTexture_.CreateFromRgba(device_, 1, 1, &white)) {
            SIM_ERROR("Render", "cannot create fallback texture");
            return false;
        }
        if (!bindless_) {
            fallbackSet_ = AllocateMaterialSet(fallbackTexture_);
            if (fallbackSet_ == VK_NULL_HANDLE) {
                return false;
            }
        }

        // **Magenta, dan bukan putih maupun hitam.** Ini yang tergambar untuk
        // ruas yang *punya* tekstur tetapi hasil bake-nya belum ada, dan
        // keduanya berbeda arti: putih berarti "memang tidak bertekstur", hitam
        // terlihat seperti bayangan atau material yang salah. Magenta tidak
        // pernah dikira apa pun selain "belum siap".
        //
        // Satu piksel, bukan papan catur: papan catur menuntut sampler
        // berulang, dan yang lewat `CreateFromRgba` justru clamp — yang terlihat
        // lalu bukan papan catur melainkan empat kuadran yang meregang.
        const uint32_t magenta = 0xFFFF00FFu;
        auto pending = std::make_unique<GpuTexture>();
        if (!pending->texture.CreateFromRgba(device_, 1, 1, &magenta)) {
            SIM_ERROR("Render", "cannot create pending texture");
            return false;
        }
        if (!bindless_) {
            pending->set = AllocateMaterialSet(pending->texture);
            if (pending->set == VK_NULL_HANDLE) {
                return false;
            }
        }
        materialTextures_.push_back(std::move(pending));
        // Ia menempati slot pertama supaya handle-nya seperti tekstur lain, dan
        // jalur gambarnya tidak perlu mengenal satu pun kasus khusus.
        pendingHandle_ = static_cast<TextureHandle>(materialTextures_.size());
        return true;
    }

    /// Satu set descriptor yang menunjuk sebuah tekstur.
    VkDescriptorSet AllocateMaterialSet(const rhi::Texture2D& texture) {
        VkDescriptorSetAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocateInfo.descriptorPool = materialPool_;
        allocateInfo.descriptorSetCount = 1;
        allocateInfo.pSetLayouts = &materialSetLayout_;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(device_.Handle(), &allocateInfo, &set) != VK_SUCCESS) {
            SIM_WARN("Render", "material descriptor pool is full; texture falls back to white");
            return VK_NULL_HANDLE;
        }

        const VkDescriptorBufferInfo params{materialParams_.Handle(), 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo image{};
        image.imageView = texture.View();
        image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo sampler{};
        sampler.sampler = texture.Sampler();

        std::array<VkWriteDescriptorSet, 3> writes{};
        for (VkWriteDescriptorSet& write : writes) {
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set;
            write.descriptorCount = 1;
        }
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &params;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[1].pImageInfo = &image;
        writes[2].dstBinding = 2;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[2].pImageInfo = &sampler;
        vkUpdateDescriptorSets(device_.Handle(), static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
        return set;
    }

    /// Larik descriptor bindless: satu set untuk seluruh material dan seluruh
    /// tekstur adegan, diikat sekali per pass.
    ///
    /// **Nomor binding-nya sama persis dengan set material jalur mundur** —
    /// 0 parameter, 1 tekstur, 2 sampler — dan yang berbeda hanya larik atau
    /// bukan larik. Itu yang membuat `box.frag` dan `box_bindless.frag` berbeda
    /// satu baris, bukan satu berkas.
    ///
    /// `UPDATE_AFTER_BIND` wajib, dan alasannya masa hidup: tekstur baru selesai
    /// di-bake di tengah adegan yang sedang berjalan, dan slotnya ditulis
    /// sementara dua frame sebelumnya masih terbang. Tanpa medan itu, penulisan
    /// tersebut adalah pelanggaran — yang muncul sebagai kerusakan acak, bukan
    /// sebagai galat.
    ///
    /// `PARTIALLY_BOUND` juga wajib, dan alasannya berbeda: larik dialokasikan
    /// sepenuh kapasitasnya sejak awal, sementara yang terisi hanya sebanyak
    /// tekstur yang sudah dimuat. Tanpa medan itu seluruh slot harus sah pada
    /// setiap draw, termasuk yang tidak pernah dibaca siapa pun.
    bool CreateBindlessDescriptors() {
        if (!bindless_) {
            return true;
        }
        const std::array<VkDescriptorSetLayoutBinding, 3> bindings{
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                         kMaxBindlessMaterials, VK_SHADER_STAGE_FRAGMENT_BIT,
                                         nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, bindlessCapacity_,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_SAMPLER, bindlessCapacity_,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        };
        constexpr VkDescriptorBindingFlags kFlags =
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        const std::array<VkDescriptorBindingFlags, 3> bindingFlags{kFlags, kFlags, kFlags};
        VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
        flagsInfo.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        flagsInfo.bindingCount = static_cast<uint32_t>(bindingFlags.size());
        flagsInfo.pBindingFlags = bindingFlags.data();

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.pNext = &flagsInfo;
        layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(device_.Handle(), &layoutInfo, nullptr,
                                        &bindlessSetLayout_) != VK_SUCCESS) {
            SIM_ERROR("Render", "cannot create bindless descriptor layout");
            return false;
        }

        const std::array<VkDescriptorPoolSize, 3> sizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxBindlessMaterials},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, bindlessCapacity_},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, bindlessCapacity_},
        };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
        poolInfo.pPoolSizes = sizes.data();
        if (vkCreateDescriptorPool(device_.Handle(), &poolInfo, nullptr, &bindlessPool_) !=
            VK_SUCCESS) {
            SIM_ERROR("Render", "cannot create bindless descriptor pool");
            return false;
        }

        VkDescriptorSetAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocateInfo.descriptorPool = bindlessPool_;
        allocateInfo.descriptorSetCount = 1;
        allocateInfo.pSetLayouts = &bindlessSetLayout_;
        if (vkAllocateDescriptorSets(device_.Handle(), &allocateInfo, &bindlessSet_) !=
            VK_SUCCESS) {
            SIM_ERROR("Render", "cannot allocate the bindless descriptor set");
            return false;
        }

        // Slot nol putih 1x1. **Ditulis sekarang, bukan saat ruas pertama tanpa
        // tekstur digambar**: ia yang membuat "tidak punya tekstur" tidak
        // menuntut satu pun cabang, dan cabang yang tidak ada tidak bisa lupa
        // dijalankan.
        WriteBindlessTexture(0, fallbackTexture_);
        for (std::size_t i = 0; i < materialTextures_.size(); ++i) {
            WriteBindlessTexture(static_cast<uint32_t>(i + 1), materialTextures_[i]->texture);
        }
        return true;
    }

    /// Menaruh sebuah tekstur di slot bindless-nya. Slot = handle tekstur, dan
    /// nol adalah putih 1x1 — jadi `kInvalidTexture` menunjuk tempat yang benar
    /// tanpa dipetakan.
    void WriteBindlessTexture(uint32_t slot, const rhi::Texture2D& texture) {
        if (bindlessSet_ == VK_NULL_HANDLE || slot >= bindlessCapacity_) {
            return;
        }
        VkDescriptorImageInfo image{};
        image.imageView = texture.View();
        image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo sampler{};
        sampler.sampler = texture.Sampler();

        std::array<VkWriteDescriptorSet, 2> writes{};
        for (VkWriteDescriptorSet& write : writes) {
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = bindlessSet_;
            write.dstArrayElement = slot;
            write.descriptorCount = 1;
        }
        writes[0].dstBinding = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[0].pImageInfo = &image;
        writes[1].dstBinding = 2;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[1].pImageInfo = &sampler;
        vkUpdateDescriptorSets(device_.Handle(), static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }

    /// Menaruh blok parameter sebuah material di slot bindless-nya.
    void WriteBindlessMaterial(uint32_t slot, const rhi::DynamicBuffer& parameters) {
        if (bindlessSet_ == VK_NULL_HANDLE || slot >= kMaxBindlessMaterials) {
            return;
        }
        const VkDescriptorBufferInfo info{parameters.Handle(), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = bindlessSet_;
        write.dstBinding = 0;
        write.dstArrayElement = slot;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &info;
        vkUpdateDescriptorSets(device_.Handle(), 1, &write, 0, nullptr);
    }

    /// Set descriptor palet kulit: satu storage buffer, tahap vertex.
    ///
    /// **Set tersendiri, bukan binding ke-22 di set bayangan.** Pass bayangan
    /// juga menggambar kulit, dan set bayangan berisi peta bayangan yang sedang
    /// ia tulis — mengikatnya di sana berarti sebuah image dipakai sebagai
    /// lampiran dan sebagai descriptor pada draw yang sama. Set kecil ini bisa
    /// diikat kedua pass tanpa membawa apa pun yang bukan miliknya.
    bool CreateSkinDescriptors() {
        // Binding 0 palet kulit, binding 1 transform instance. **Keduanya di set
        // yang sama, dan itu ditentukan pass bayangan**: ia hanya mengikat satu
        // set, jadi apa pun yang dibutuhkan tahap vertexnya harus ada di sana
        // bersama-sama.
        const std::array<VkDescriptorSetLayoutBinding, 2> bindings{
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_VERTEX_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        SIM_VK_CHECK(
            vkCreateDescriptorSetLayout(device_.Handle(), &layoutInfo, nullptr, &skinSetLayout_));

        // Dua storage buffer per set, jadi dua kali jumlah slot.
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                        static_cast<uint32_t>(slots_.size()) * 2};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = static_cast<uint32_t>(slots_.size());
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &size;
        SIM_VK_CHECK(vkCreateDescriptorPool(device_.Handle(), &poolInfo, nullptr, &skinPool_));

        std::vector<VkDescriptorSetLayout> layouts(slots_.size(), skinSetLayout_);
        VkDescriptorSetAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocateInfo.descriptorPool = skinPool_;
        allocateInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
        allocateInfo.pSetLayouts = layouts.data();
        std::vector<VkDescriptorSet> sets(slots_.size());
        if (vkAllocateDescriptorSets(device_.Handle(), &allocateInfo, sets.data()) != VK_SUCCESS) {
            SIM_ERROR("Render", "cannot allocate skin descriptor sets");
            return false;
        }
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            slots_[i].skinSet = sets[i];
        }
        // Isinya ditulis `WriteSkinDescriptors` sesudah buffer slot dibuat.
        // Layout-nya dibutuhkan lebih awal daripada itu — `CreatePipelines`
        // memakainya — jadi keduanya memang terjadi pada saat yang berbeda.
        return true;
    }

    /// Menunjuk ulang tiap set ke buffer palet slot-nya.
    ///
    /// **Dipanggil ulang setiap kali buffer-nya tumbuh.** `DynamicBuffer::Reserve`
    /// membuat `VkBuffer` baru saat kapasitasnya kurang, dan descriptor yang
    /// masih menunjuk buffer lama adalah descriptor yang menunjuk memori yang
    /// sudah dibebaskan — kerusakan yang muncul sebagai pose acak pada frame
    /// tempat sebuah karakter kedua masuk ke adegan, bukan sebagai galat.
    void WriteSkinDescriptor(const InstanceSlot& slot) {
        const std::array<VkDescriptorBufferInfo, 2> buffers{
            VkDescriptorBufferInfo{slot.skinBuffer.Handle(), 0, VK_WHOLE_SIZE},
            VkDescriptorBufferInfo{slot.instanceBuffer.Handle(), 0, VK_WHOLE_SIZE}};
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (std::size_t i = 0; i < writes.size(); ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = slot.skinSet;
            writes[i].dstBinding = static_cast<uint32_t>(i);
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &buffers[i];
        }
        vkUpdateDescriptorSets(device_.Handle(), static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }

    /// Seluruh slot sekaligus. **Hanya saat start.** Di tengah frame yang boleh
    /// ditulis ulang hanyalah set milik slot yang sudah ditunggu selesai; slot
    /// lain bisa saja masih dibaca GPU, dan menulisi descriptor yang sedang
    /// dipakai adalah kerusakan yang muncul di frame yang salah.
    void WriteSkinDescriptors() {
        for (const InstanceSlot& slot : slots_) {
            WriteSkinDescriptor(slot);
        }
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

        if (shadowPipelines_[0] != VK_NULL_HANDLE && casterCount > 0 && slot.buffer.IsValid()) {
            // Pengikatan geometri dan pipeline pindah ke dalam `DrawRuns`: sejak
            // ada lebih dari satu mesh, buffer yang diikat berbeda per ruas —
            // dan sejak ada jalur berkulit, pipeline-nya juga.
            BindSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowLayout_, 0, 1, &slot.skinSet,
                     0, nullptr);

            for (const ShadowAtlasEntry& entry : atlasAllocation_.entries) {
                // **Tiap muka menyaring terhadap volumenya sendiri.** Sebuah
                // lampu berjangkauan 16 meter tidak bisa dibayangi oleh benda
                // yang berada 60 meter darinya, dan menggambarnya ke dalam
                // ubinnya adalah pekerjaan yang seluruh hasilnya dibuang oleh
                // clip. Sampai G1 daftar yang digambar ke setiap muka adalah
                // daftar caster utuh — dikali sampai 32 muka.
                //
                // Frustum diturunkan dari matriks muka itu apa adanya. Untuk
                // lampu punctual itu tepat dan tidak perlu diperlebar: apa pun
                // yang bayangannya jatuh di dalam muka ini pasti berada di
                // antara lampu dan bidang jauhnya, yaitu di dalam frustumnya.
                // (Kaskade directional berbeda, dan perbedaannya ditangani
                // `casterPullback`. Lihat `RecordShadowPass`.)
                const Frustum faceFrustum(entry.viewProjection);
                const std::vector<Aabb>& bounds = instanceBounds_;
                SplitRuns(casterRuns_,
                          [&faceFrustum, &bounds](uint32_t index) {
                              return faceFrustum.Intersects(bounds[index]);
                          },
                          shadowRuns_);
                if (shadowRuns_.empty()) {
                    // Ubin yang tidak berisi apa pun tetap benar: ia sudah
                    // di-clear oleh `loadOp` satu kali untuk seluruh atlas.
                    continue;
                }
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
                vkCmdPushConstants(cmd, shadowLayout_, kBoxPushStages, 0,
                                   sizeof(BoxPush), &push);
                DrawRuns(cmd, slot, shadowRuns_, 0, shadowPipelines_, shadowLayout_,
                         /*bindsMaterial=*/false, /*materialVariant=*/-1,
                         /*indirectCommands=*/VK_NULL_HANDLE, /*skipMasked=*/true);
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
        writes.reserve(slots_.size() * 18);
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
        // LUT sky-view, atau piramida HiZ sebagai pengganti kalau atmosfernya
        // gagal dibuat — alasan yang sama dengan kaskade SDF di atas: descriptor
        // yang dibiarkan kosong adalah pelanggaran di setiap draw, bahkan pada
        // pass yang tidak membacanya. Yang menjaga ia tidak terbaca adalah
        // `skyParams.x` yang nol, bukan descriptor ini. **Penggantinya harus
        // image 2D biasa**, bukan peta bayangan: yang itu view array, dan
        // sebuah `Sampler2D` yang dipasangi view array adalah pelanggaran
        // walaupun tidak ada satu pun shader yang membacanya.
        const VkDescriptorImageInfo skyImage =
            sky_.IsValid() ? VkDescriptorImageInfo{sky_.SkyViewSampler(), sky_.SkyViewImage(),
                                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
                           : hizImage;
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
            // Jalur GPU menulis ke buffer device-local miliknya sendiri; jalur
            // CPU tetap memakai buffer host-visible yang dipetakan. Yang
            // membedakan keduanya hanya dua baris ini — sisi pembacanya,
            // `cluster_common.slang`, tidak tahu jalur mana yang mengisinya.
            const bool gpu = gpuClustersActive_ && clusterAssign_.IsValid();
            buffers[base + 2] = {gpu ? clusterAssign_.RangeBuffer(static_cast<uint32_t>(i))
                                     : slots_[i].clusterRangeBuffer.Handle(),
                                 0, VK_WHOLE_SIZE};
            buffers[base + 3] = {gpu ? clusterAssign_.IndexBuffer(static_cast<uint32_t>(i))
                                     : slots_[i].clusterIndexBuffer.Handle(),
                                 0, VK_WHOLE_SIZE};
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

            VkWriteDescriptorSet skyView = sampled;
            skyView.dstBinding = 21;
            skyView.pImageInfo = &skyImage;
            writes.push_back(skyView);
        }
        vkUpdateDescriptorSets(device_.Handle(), static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
        WriteGiGridDescriptors();
        return true;
    }

    /// Menulis ulang binding 22 dan 23: entri grid dan tekstur albedonya.
    ///
    /// **Terpisah dari `WriteShadowDescriptors`, karena waktunya berbeda.**
    /// Yang itu berjalan saat start, ketika belum ada satu pun command buffer;
    /// yang ini berjalan saat sebuah mesh selesai dibake, yaitu di tengah
    /// frame yang berjalan — dan menulisi descriptor set yang sedang dipakai
    /// adalah pelanggaran. Pemanggil yang menunggu device idle lebih dulu.
    void WriteGiGridDescriptors() {
        std::array<VkDescriptorImageInfo, SdfClipmapResource::kMaxGrids> images{};
        for (uint32_t at = 0; at < SdfClipmapResource::kMaxGrids; ++at) {
            const VkImageView view = sdfClipmap_.GridAlbedoView(at);
            // Slot tanpa albedo tetap harus punya descriptor yang sah:
            // descriptor kosong adalah pelanggaran pada setiap draw, termasuk
            // draw yang tidak membacanya. Penggantinya kaskade SDF — satu-
            // satunya tekstur 3D yang pasti ada — dan yang menjaganya tidak
            // pernah terbaca adalah jumlah grid di `giGridCount`.
            images[at] = {VK_NULL_HANDLE,
                          view != VK_NULL_HANDLE ? view : sdfClipmap_.Texture(0).View(),
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }
        std::vector<VkDescriptorBufferInfo> buffers(slots_.size());
        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(slots_.size() * 2);
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            const VkBuffer grid = sdfClipmap_.GridBuffer(static_cast<uint32_t>(i));
            buffers[i] = {grid != VK_NULL_HANDLE ? grid : slots_[i].lightBuffer.Handle(), 0,
                          VK_WHOLE_SIZE};
            VkWriteDescriptorSet entries{};
            entries.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            entries.dstSet = slots_[i].shadowSet;
            entries.dstBinding = 22;
            entries.descriptorCount = 1;
            entries.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            entries.pBufferInfo = &buffers[i];
            writes.push_back(entries);

            VkWriteDescriptorSet albedo = entries;
            albedo.dstBinding = 23;
            albedo.descriptorCount = SdfClipmapResource::kMaxGrids;
            albedo.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            albedo.pBufferInfo = nullptr;
            albedo.pImageInfo = images.data();
            writes.push_back(albedo);
        }
        vkUpdateDescriptorSets(device_.Handle(), static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }

    /// Menulis ulang binding rentang dan indeks cluster saja.
    ///
    /// Alasannya sama dengan `UpdateHizDescriptors`: `WriteShadowDescriptors`
    /// mengalokasi set baru dari pool yang hanya cukup untuk satu putaran, jadi
    /// memanggilnya kedua kali kehabisan pool — dan kegagalannya muncul sebagai
    /// pass forward yang membaca daftar lampu milik jalur yang sudah tidak
    /// dipakai, bukan sebagai galat di tempat yang benar.
    void UpdateClusterDescriptors() {
        const bool gpu = gpuClustersActive_ && clusterAssign_.IsValid();
        std::vector<VkDescriptorBufferInfo> buffers(slots_.size() * 2);
        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(slots_.size() * 2);
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            const auto index = static_cast<uint32_t>(i);
            buffers[i * 2] = {gpu ? clusterAssign_.RangeBuffer(index)
                                  : slots_[i].clusterRangeBuffer.Handle(),
                              0, VK_WHOLE_SIZE};
            buffers[i * 2 + 1] = {gpu ? clusterAssign_.IndexBuffer(index)
                                      : slots_[i].clusterIndexBuffer.Handle(),
                                  0, VK_WHOLE_SIZE};
            for (uint32_t at = 0; at < 2; ++at) {
                VkWriteDescriptorSet write{};
                write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet = slots_[i].shadowSet;
                write.dstBinding = 3 + at;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo = &buffers[i * 2 + at];
                writes.push_back(write);
            }
        }
        vkUpdateDescriptorSets(device_.Handle(), static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
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
        if (skinPool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_.Handle(), skinPool_, nullptr);
            skinPool_ = VK_NULL_HANDLE;
        }
        textureBytes_ = 0;
        pendingHandle_ = kInvalidTexture;
        for (std::unique_ptr<GpuMaterial>& material : materials_) {
            DestroyMaterial(*material);
        }
        materials_.clear();
        materialByKey_.clear();
        if (boxVertexModule_ != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device_.Handle(), boxVertexModule_, nullptr);
            boxVertexModule_ = VK_NULL_HANDLE;
        }
        for (std::unique_ptr<GpuTexture>& texture : materialTextures_) {
            texture->texture.Destroy();
        }
        materialTextures_.clear();
        textureByPath_.clear();
        fallbackTexture_.Destroy();
        materialParams_.Destroy();
        if (materialPool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_.Handle(), materialPool_, nullptr);
            materialPool_ = VK_NULL_HANDLE;
        }
        if (materialSetLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_.Handle(), materialSetLayout_, nullptr);
            materialSetLayout_ = VK_NULL_HANDLE;
        }
        if (bindlessPool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_.Handle(), bindlessPool_, nullptr);
            bindlessPool_ = VK_NULL_HANDLE;
            bindlessSet_ = VK_NULL_HANDLE;
        }
        if (bindlessSetLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_.Handle(), bindlessSetLayout_, nullptr);
            bindlessSetLayout_ = VK_NULL_HANDLE;
        }
        if (skinSetLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_.Handle(), skinSetLayout_, nullptr);
            skinSetLayout_ = VK_NULL_HANDLE;
        }
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
        {
            const CpuScope scope(cpuTimings_, "cpu-shadow-atlas");
            atlasAllocation_ = AllocateShadowAtlas(scene.lights, desc.camera.position,
                                                   atlasSettings_);
        }
        // **Diperingatkan saat angkanya berubah, bukan tiap frame.** Enam puluh
        // baris identik per detik bukan peringatan melainkan derau, dan derau
        // yang menenggelamkan peringatan lain adalah cara terbaik membuat log
        // berhenti dibaca. Angka yang sebenarnya hidup di `Stats()`, tempat ia
        // bisa dilihat kapan saja tanpa ada yang perlu menggulung log.
        if (atlasAllocation_.dropped != reportedShadowDrop_) {
            reportedShadowDrop_ = atlasAllocation_.dropped;
            if (atlasAllocation_.dropped > 0) {
                SIM_WARN("Render", "{} shadow-casting lights did not fit the atlas",
                         atlasAllocation_.dropped);
            } else {
                SIM_INFO("Render", "every shadow-casting light fits the atlas again");
            }
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
        // **Satu dari dua jalur, dan yang tidak dipakai tidak dibayar.** Yang di
        // GPU menetapkan 3.456 cluster dalam satu dispatch; yang di CPU
        // mengerjakannya satu per satu dan tetap ada sebagai jalur mundur dan
        // sebagai pembanding — dua implementasi yang harus sepakat adalah cara
        // termurah menemukan yang mana yang salah.
        uint32_t overflowed = 0;
        if (gpuClustersActive_) {
            const CpuScope scope(cpuTimings_, "cpu-cluster-assign");
            // Angkanya dari jalan terakhir slot ini, yang fence-nya sudah
            // ditunggu — dibaca sebelum `Upload` mengosongkannya kembali.
            overflowed = clusterAssign_.Overflowed(static_cast<uint32_t>(slotIndex_));
            clusterAssign_.Upload(static_cast<uint32_t>(slotIndex_), clusterGrid_,
                                  desc.camera.View(), clusterLights_,
                                  clusterSettings_.maxLightsPerCluster);
        } else {
            const CpuScope scope(cpuTimings_, "cpu-cluster-assign");
            clusterAssignment_ =
                AssignLights(clusterGrid_, desc.camera.View(), clusterLights_, clusterSettings_);
            overflowed = clusterAssignment_.overflowed;
        }
        // **Diperingatkan saat angkanya berubah, bukan tiap frame.** Alasannya
        // sama dengan lampu yang tidak muat atlas: enam puluh baris identik per
        // detik bukan peringatan melainkan derau.
        if (overflowed != reportedClusterOverflow_) {
            reportedClusterOverflow_ = overflowed;
            if (overflowed > 0) {
                SIM_WARN("Render", "{} clusters exceeded {} lights and were truncated",
                         overflowed, clusterSettings_.maxLightsPerCluster);
            }
        }

        // Buffer kosong tidak sah, jadi keduanya selalu berisi minimal satu
        // entri boneka. Cluster yang jumlahnya nol tidak pernah membacanya.
        if (gpuLights_.empty()) {
            gpuLights_.push_back(GpuLight{});
        }
        if (clusterAssignment_.indices.empty()) {
            clusterAssignment_.indices.push_back(0);
        }
        if (clusterAssignment_.ranges.empty()) {
            clusterAssignment_.ranges.push_back(ClusterAssignment::Range{});
        }

        const VkDeviceSize lightBytes = sizeof(GpuLight) * gpuLights_.size();
        const VkDeviceSize rangeBytes =
            sizeof(ClusterAssignment::Range) * clusterAssignment_.ranges.size();
        const VkDeviceSize indexBytes = sizeof(uint32_t) * clusterAssignment_.indices.size();
        const CpuScope uploadScope(cpuTimings_, "cpu-cluster-upload");
        // Buffer lampu diunggah pada kedua jalur — yang dibacanya fragment
        // shader, bukan penetapan. Rentang dan indeks hanya pada jalur CPU:
        // pada jalur GPU keduanya device-local dan ditulis dispatch.
        if (slot.lightBuffer.Reserve(lightBytes)) {
            slot.lightBuffer.Write(gpuLights_.data(), lightBytes);
        }
        if (!gpuClustersActive_ && slot.clusterRangeBuffer.Reserve(rangeBytes) &&
            slot.clusterIndexBuffer.Reserve(indexBytes)) {
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
                                 static_cast<float>(probeGrid_.Settings().tileSize),
                                 static_cast<float>(sdfClipmap_.ActiveGridCount()),
                                 desc.gi.furnaceTest ? 1.0f : 0.0f);
        // **Syarat yang sama persis dengan syarat pass langit didaftarkan.**
        // LUT sky-view hanya diperbarui di dalam pass itu; menyuruh GI
        // membacanya saat pass-nya tidak ada berarti menerangi adegan dengan
        // matahari di posisi frame kapan pun terakhir kali langit digambar.
        const bool atmosphere = desc.skyEnabled && sky_.IsValid() &&
                                desc.skySource != SkySource::HdrMap;
        uniforms.skyParams =
            Vec4(atmosphere ? desc.skyIntensity : 0.0f, desc.cameraHeightKm,
                 static_cast<float>(SkyAtmosphere::kSkyViewWidth),
                 static_cast<float>(SkyAtmosphere::kSkyViewHeight));
        uniforms.giBounce = Vec4(Vec3(kBounceAlbedo), 1.0f);
        slot.shadowUniform.Write(&uniforms, sizeof(uniforms));
    }

    void RecordShadowPass(VkCommandBuffer cmd, InstanceSlot& slot, uint32_t casterCount) {
        if (shadowPipelines_[0] == VK_NULL_HANDLE) {
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
                // **Tiap kaskade menyaring terhadap ortografiknya sendiri.**
                // Kaskade nol mencakup beberapa meter pertama; menggambar
                // seluruh caster sejauh batas pandang ke dalamnya berarti
                // membayar tiga kali untuk satu peta yang hampir seluruhnya
                // kosong.
                //
                // Menyaring dengan frustum kaskade ini **tepat, bukan terlalu
                // ketat**, dan alasannya `CascadeSettings::casterPullback`:
                // bidang dekat cahaya sudah ditarik mundur 200 meter justru
                // supaya caster yang berada di antara matahari dan irisannya
                // ikut termuat. Volume yang diuji karena itu sudah volume yang
                // benar; tanpa tarikan mundur itu, uji ini akan memotong pohon
                // dan bangunan yang bayangannya paling diperhatikan.
                const Frustum cascadeFrustum(cascade.viewProjection);
                const std::vector<Aabb>& bounds = instanceBounds_;
                SplitRuns(casterRuns_,
                          [&cascadeFrustum, &bounds](uint32_t index) {
                              return cascadeFrustum.Intersects(bounds[index]);
                          },
                          shadowRuns_);
                const BoxPush push{cascade.viewProjection};
                if (!shadowRuns_.empty()) {
                    BindSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowLayout_, 0, 1,
                             &slot.skinSet, 0, nullptr);
                    vkCmdPushConstants(cmd, shadowLayout_, kBoxPushStages, 0,
                                       sizeof(BoxPush), &push);
                    DrawRuns(cmd, slot, shadowRuns_, 0, shadowPipelines_, shadowLayout_,
                             /*bindsMaterial=*/false, /*materialVariant=*/-1,
                             /*indirectCommands=*/VK_NULL_HANDLE, /*skipMasked=*/true);
                }
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
            slot.skinBuffer.Destroy();
            slot.instanceBuffer.Destroy();
        }
        dummySkin_.Destroy();
        // **Ditambahkan di G4, dan ia menutup kebocoran yang sudah ada
        // sebelumnya.** Kaskade SDF tidak pernah ikut dibongkar di sini; yang
        // dilaporkan `vkDestroyDevice` sebagai tujuh objek bocor sebagian
        // adalah ketiga teksturnya. Komposit compute menambah pipeline,
        // descriptor pool, dan buffer entrinya ke tumpukan yang sama — dan
        // tumpukan yang sudah bocor adalah tempat kebocoran baru bersembunyi.
        sdfClipmap_.Destroy();
        hiz_.Destroy();
        occlusion_.Destroy();
        post_.Destroy();
        sky_.Destroy();
        probes_.Destroy();
        if (cache_.buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(device_.Allocator(), cache_.buffer, cache_.allocation);
            cache_.buffer = VK_NULL_HANDLE;
        }
        meshes_.clear();
        meshByPath_.clear();
        for (PipelineVariants* variants : {&prepassPipelines_, &opaquePipelines_,
                                           &transparentPipelines_, &shadowPipelines_}) {
            for (VkPipeline& pipeline : *variants) {
                if (pipeline != VK_NULL_HANDLE) {
                    vkDestroyPipeline(device_.Handle(), pipeline, nullptr);
                    pipeline = VK_NULL_HANDLE;
                }
            }
        }
        for (VkPipeline* pipeline : {&gridPipeline_, &linePipeline_, &sdfDebugPipeline_}) {
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
    /// Tahap CPU frame terakhir. Tidak `mutable`: yang mengisinya adalah
    /// `Render`, bukan pembacanya.
    std::vector<PassTiming> cpuTimings_;
    TraceBackendSelection giBackend_;
    SdfClipmapResource sdfClipmap_;
    DepthPyramid hiz_;
    PostProcess post_;
    ComputeGradient gradient_;
    ClusterAssign clusterAssign_;
    /// Culling di GPU yang menghasilkan perintah gambar (G6). Lihat `DrawCull`.
    DrawCull drawCull_;
    /// Jalur GPU-driven yang benar-benar dipakai frame ini.
    bool gpuCullActive_ = false;
    /// Occlusion culling yang benar-benar dipakai frame ini. Lihat
    /// `ViewportDesc::gpuOcclusion` — bawaannya mati.
    bool gpuOcclusionActive_ = false;
    /// Menulis angka antara uji occlusion. Alat diagnostik; lihat
    /// `ViewportDesc::cullDebug`.
    bool cullDebugActive_ = false;
    /// Batas bisect uji occlusion; lihat `ViewportDesc::cullLimit`.
    uint32_t cullLimit_ = 0xffffffffu;
    /// Antrean compute terpisah dipakai frame ini; lihat
    /// `ViewportDesc::asyncCompute`.
    bool asyncComputeActive_ = false;
    uint32_t cullFirst_ = 0;
    /// Command buffer tiap segmen graph, dan batch submit-nya. Anggota, bukan
    /// lokal: keduanya dibangun ulang tiap frame dan tidak ada gunanya
    /// mengalokasi ulang vektornya setiap kali.
    std::vector<VkCommandBuffer> segmentBuffers_;
    std::vector<rhi::SubmitBatch> submitBatches_;
    /// Slot yang perintahnya terakhir disubmit, dan viewProj-nya. Dipakai
    /// pembaca angka antara supaya ia membaca frame yang benar.
    uint32_t lastCullSlot_ = 0;
    Mat4 lastViewProjection_{1.0f};
    /// Kotak dan rentang indeks tiap permukaan, dipakai ulang tiap frame.
    std::vector<DrawCull::GpuBounds> cullBounds_;
    std::vector<DrawCull::GpuSurface> cullSurfaces_;
    ResourceId drawCommandId_ = kInvalidResource;
    ResourceId visibleCommandId_ = kInvalidResource;
    PassId drawCullPassId_ = kInvalidPass;
    PassId occlusionPyramidPassId_ = kInvalidPass;
    PassId drawCullLatePassId_ = kInvalidPass;
    /// Piramida depth yang meringkas dengan **minimum** — permukaan terjauh.
    /// Terpisah dari `hiz_`, yang meringkas dengan maksimum untuk penelusuran
    /// GI. Lihat `DepthReduce`.
    DepthPyramid occlusion_;
    /// Jalur yang benar-benar dipakai frame ini. Berubahnya menuntut descriptor
    /// ditulis ulang, jadi ia disimpan alih-alih dibaca dari `desc` tiap kali.
    bool gpuClustersActive_ = false;
    SkyAtmosphere sky_;
    VolumePass volumePass_;
    /// Revisi volume yang sedang terunggah. Unggahannya berharga puluhan
    /// megabyte, jadi ia hanya diulang ketika angka ini tidak lagi cocok.
    uint64_t uploadedVolumeRevision_ = 0;
    bool volumeUploaded_ = false;
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
    PassId sdfPassId_ = kInvalidPass;
    std::array<ResourceId, kMaxSdfCascades> sdfCascadeId_{};
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
    PassId volumeId_ = kInvalidPass;
    PassId bloomId_ = kInvalidPass;
    PassId meterId_ = kInvalidPass;
    PassId tonemapId_ = kInvalidPass;
    PassId gridId_ = kInvalidPass;
    PassId prepassId_ = kInvalidPass;
    /// Pemeriksa jalur compute (G3). Ketiganya `kInvalid` kecuali saat debug
    /// view-nya menyala — pass yang tidak didaftarkan tidak membayar apa pun.
    ResourceId gradientId_ = kInvalidResource;
    PassId gradientFillId_ = kInvalidPass;
    PassId gradientBlitId_ = kInvalidPass;
    /// Penetapan lampu ke cluster di GPU (G4). `kInvalid` saat jalur CPU aktif.
    ResourceId clusterRangeId_ = kInvalidResource;
    ResourceId clusterIndexId_ = kInvalidResource;
    PassId clusterPassId_ = kInvalidPass;
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
    uint32_t reportedClusterOverflow_ = 0;
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
    /// Albedo yang ditebak untuk permukaan yang mengenai clipmap SDF tetapi
    /// belum pernah terlihat layar, jadi cache radiansi tidak mengenalnya.
    ///
    /// **Sebuah tebakan, dan disebut tebakan.** Clipmap hanya menyimpan jarak;
    /// warna permukaannya tidak ada di mana pun sampai ada volume albedo. Yang
    /// dibayar tanpa tebakan ini bukan gambar yang sedikit meleset melainkan
    /// serambi yang hitam pekat: di dalam ruang tertutup hampir setiap sinar
    /// probe mengenai batu yang tidak pernah masuk layar, dan sinar yang
    /// dibuang semuanya berarti iradiansi nol. 0,5 adalah albedo rata-rata
    /// bahan bangunan — plester, batu, kayu — dan salah 0,2 di sini
    /// menggeser kecerahan pantulan, bukan menghilangkannya.
    ///
    /// **Dipakai hanya untuk mesh yang albedonya belum dibake.** Yang sudah
    /// membawa warnanya sendiri dibaca dari grid; lihat `giAlbedoAt`. Angka ini
    /// tetap 0,5 walaupun 28 material Sponza yang diukur rata-rata 0,25 linear:
    /// yang dijawabnya bukan "berapa albedo batu" melainkan "berapa albedo
    /// sesuatu yang tidak diketahui", dan menyetelnya ke satu adegan yang
    /// kebetulan diukur adalah menyetelnya ke adegan yang salah.
    static constexpr float kBounceAlbedo = 0.5f;
    /// Anggaran langkah lapis screen-space. Rencana GI menyebut 16, dan angka
    /// itulah yang membuat fallback ke SDF bukan kemewahan melainkan keharusan.
    ///
    /// **Diuji menaikkannya, dan tidak dibayar.** Di Sponza 16 langkah menjawab
    /// 6% sinar probe; 64 menjawab 15–32%, tetapi gambarnya hampir tidak
    /// berubah — rata-rata 57,4 menjadi 58,4 — sementara `gi-probe-trace` naik
    /// 0,39 ms menjadi 0,93 ms. Sebabnya sinar tambahan itu menemukan permukaan
    /// yang warnanya sudah ikut biru langit, jadi yang ditukar hanya satu
    /// sumber biru dengan sumber biru yang lain. Yang akan membuat angka ini
    /// berarti adalah pantulan yang benar-benar membawa warna matahari.
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
    PipelineVariants shadowPipelines_{};
    /// Palet kulit: set descriptor tersendiri, dipakai pass forward (set 1) dan
    /// pass bayangan (set 0).
    VkDescriptorSetLayout skinSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool skinPool_ = VK_NULL_HANDLE;
    /// Satu elemen pengaruh skin nol, dipasang ber-stride nol oleh jalur statis.
    rhi::DynamicBuffer dummySkin_;
    VkPipelineLayout sdfDebugLayout_ = VK_NULL_HANDLE;
    VkPipeline sdfDebugPipeline_ = VK_NULL_HANDLE;
    ResourceId shadowId_ = kInvalidResource;
    PassId shadowPassId_ = kInvalidPass;
    CascadeSet cascades_;

    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    PipelineVariants prepassPipelines_{};
    PipelineVariants opaquePipelines_{};
    PipelineVariants transparentPipelines_{};

    std::array<InstanceSlot, 3> slots_;
    // Penetapan cluster di GPU memegang buffer keluarannya sendiri, satu per
    // slot. Selisih di antara kedua angka itu tidak menghasilkan galat apa pun,
    // hanya pembacaan di luar batas yang muncul sebagai device lost beberapa
    // frame kemudian.
    static_assert(std::tuple_size_v<decltype(slots_)> == ClusterAssign::kSlots,
                  "ClusterAssign::kSlots harus sama dengan banyaknya slot frame");
    std::size_t slotIndex_ = 0;
    std::vector<BoxInstance> opaque_;
    std::vector<BoxInstance> transparent_;
    std::vector<BoxInstance> upload_;
    /// Transform tiap instance, **sejajar indeks** dengan `opaque_`,
    /// `transparent_`, dan `upload_`.
    ///
    /// Larik terpisah, bukan medan di dalam `BoxInstance`: yang satu menjadi
    /// atribut vertex dan yang lain menjadi storage buffer, dan menggabungkannya
    /// berarti mengunggah 64 byte per instance ke aliran atribut yang tidak
    /// membacanya.
    std::vector<Mat4> opaqueTransforms_;
    std::vector<Mat4> transparentTransforms_;
    std::vector<Mat4> transformUpload_;
    /// Warna ruas frame ini, disalin dari `ViewportScene`. Disalin dan bukan
    /// disimpan sebagai span: span pemanggil hanya sah selama `Render()`,
    /// sementara perekaman command buffer membacanya di dalam recorder yang
    /// dijalankan frame graph.
    std::vector<Vec4> partColors_;
    std::vector<SurfaceEntry> sorted_;
    std::vector<SurfaceEntry> gathered_;
    /// Ruas draw: satu panggilan gambar per mesh yang berurutan.
    std::vector<DrawRun> opaqueRuns_;
    std::vector<DrawRun> casterRuns_;
    std::vector<DrawRun> transparentRuns_;
    /// Ruas buram yang benar-benar terlihat kamera — awalan `opaqueRuns_` yang
    /// sudah dipecah membuang instance yang hanya ada di daftar karena ia
    /// menjatuhkan bayangan.
    std::vector<DrawRun> visibleOpaqueRuns_;
    /// Kotak dunia dan keterlihatan tiap instance di `opaque_`, sejajar indeks.
    std::vector<Aabb> instanceBounds_;
    std::vector<uint8_t> instanceVisible_;
    /// Papan tulis untuk ruas per muka bayangan. Satu, dipakai ulang: 32 muka
    /// atlas dikali satu vektor baru per muka adalah 32 alokasi per frame untuk
    /// data yang umurnya satu panggilan gambar.
    std::vector<DrawRun> shadowRuns_;
    /// Banyaknya entri di awal `opaque_` yang menjatuhkan bayangan.
    uint32_t casterCount_ = 0;

    /// Geometri yang sudah ada di GPU. Indeks nol selalu kubus satuan, yaitu
    /// nilai mundur untuk setiap mesh renderer yang asetnya belum ada, gagal
    /// dimuat, atau memang belum ditetapkan.
    struct GpuMesh {
        rhi::DynamicBuffer vertices;
        rhi::DynamicBuffer indices;
        /// Disimpan supaya jalur cache-hit bisa menjawabnya tanpa memegang
        /// `MeshData` yang sudah dilepas sesudah diunggah.
        uint32_t triangleCount = 0;
        uint32_t vertexCount = 0;
        /// Pengaruh skin, sejajar dengan `vertices`. Tidak dibuat sama sekali
        /// untuk mesh tanpa rangka — 24 byte per vertex adalah harga yang tidak
        /// ada gunanya dibayar mesh statis, dan mesh statis adalah hampir
        /// seluruh isi adegan.
        rhi::DynamicBuffer skin;
        uint32_t indexCount = 0;
        /// Ruas per material, sesuai `assets::MeshData::parts`. Selalu berisi
        /// sedikitnya satu — jalur gambar karena itu tidak perlu membedakan
        /// "punya ruas" dan "tidak".
        struct Part {
            uint32_t firstIndex = 0;
            uint32_t indexCount = 0;
            /// Warna dasar material berkasnya. `w` nol berarti berkasnya tidak
            /// menyebut material untuk ruas ini, dan yang berlaku adalah warna
            /// instance — yaitu material yang ditetapkan editor.
            Vec4 baseColor{0.0f};
        };
        std::vector<Part> parts;
        /// Nol berarti mesh ini tidak punya rangka, jadi tidak pernah digambar
        /// lewat pipeline ber-kulit.
        uint32_t boneCount = 0;
        Vec3 boundsMin{-0.5f};
        Vec3 boundsMax{0.5f};
    };
    /// `unique_ptr`, karena `DynamicBuffer` tidak bisa dipindah — dan vektor yang
    /// tumbuh akan memindahkan isinya.
    std::vector<std::unique_ptr<GpuMesh>> meshes_;
    /// Medan jarak hasil bake, diindeks `MeshHandle`. Kosong berarti mesh itu
    /// memakai kotak batasnya di clipmap, seperti sebelum M1.
    std::vector<std::shared_ptr<const SdfGrid>> meshFields_;
    /// Larik sejajar `ViewportScene::meshes`, disusun ulang tiap frame supaya
    /// `SdfClipmapResource` tidak perlu mengenal `MeshHandle` sama sekali.
    std::vector<const SdfGrid*> meshFieldPointers_;
    /// Generasi grid clipmap yang descriptor-nya sudah ditulis. Lihat
    /// `WriteGiGridDescriptors`.
    uint64_t sdfGridGeneration_ = 0;
    /// Jalur → handle. Jalur yang gagal dimuat dipetakan ke kubus satuan supaya
    /// ia tidak dicoba lagi setiap frame.
    /// Tekstur yang sudah di GPU, beserta set descriptor-nya.
    struct GpuTexture {
        rhi::Texture2D texture;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    std::vector<std::unique_ptr<GpuTexture>> materialTextures_;
    std::unordered_map<std::string, TextureHandle> textureByPath_;
    /// Modul `box.vert`, dipegang supaya pipeline material bisa memakainya.
    VkShaderModule boxVertexModule_ = VK_NULL_HANDLE;
    std::vector<std::unique_ptr<GpuMaterial>> materials_;
    std::unordered_map<std::string, MaterialHandle> materialByKey_;
    std::vector<MaterialHandle> partMaterials_;
    /// Handle placeholder "sedang di-bake". Menempati slot pertama.
    TextureHandle pendingHandle_ = kInvalidTexture;
    /// Byte tekstur material yang sedang berada di GPU, dijumlahkan dari yang
    /// sungguh diunggah — bukan dari dimensi dikali tebakan bytes-per-texel.
    uint64_t textureBytes_ = 0;
    VkDescriptorSetLayout materialSetLayout_ = VK_NULL_HANDLE;

    /// Jalur material yang benar-benar dipakai. Lihat `SelectMaterialBinding`.
    bool bindless_ = false;
    uint32_t bindlessCapacity_ = 0;
    VkDescriptorSetLayout bindlessSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool bindlessPool_ = VK_NULL_HANDLE;
    /// Satu set untuk seluruh adegan, diikat sekali per pass.
    VkDescriptorSet bindlessSet_ = VK_NULL_HANDLE;
    VkDescriptorPool materialPool_ = VK_NULL_HANDLE;
    rhi::DynamicBuffer materialParams_;
    rhi::Texture2D fallbackTexture_;
    VkDescriptorSet fallbackSet_ = VK_NULL_HANDLE;
    std::vector<TextureHandle> partTextures_;

    std::unordered_map<std::string, MeshHandle> meshByPath_;
    /// Mesh yang datang sebagai data. Versinya disimpan supaya yang tidak
    /// berubah tidak diunggah ulang tiap frame.
    struct GeneratedMesh {
        MeshHandle handle = 0;
        uint64_t version = 0;
    };
    std::unordered_map<std::string, GeneratedMesh> meshDataVersion_;
    uint32_t drawnOpaque_ = 0;
    RenderStats stats_;
    /// Jumlah lampu terbuang yang terakhir dilaporkan ke log. Ada supaya
    /// laporannya hanya keluar saat angkanya berpindah.
    uint32_t reportedShadowDrop_ = 0;
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
