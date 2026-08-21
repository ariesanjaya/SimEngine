#pragma once

#include "Sim/RHI/Vulkan.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

struct SDL_Window;

namespace sim::rhi {

struct DeviceDesc {
    std::string applicationName = "SimEngine";
    /// Ekstensi instance yang diminta platform (dari SDL). Ekstensi debug
    /// ditambahkan sendiri oleh Device saat validasi aktif.
    std::vector<const char*> instanceExtensions;
    bool enableValidation = SIM_DEBUG != 0;

    /// Validasi sinkronisasi: apakah setiap barrier benar-benar menutup lubang
    /// yang seharusnya ia tutup.
    ///
    /// **Terpisah dari `enableValidation`, dan matinya secara bawaan bukan
    /// kelalaian.** Yang diperiksa lapisan ini adalah setiap pasangan
    /// baca/tulis atas setiap resource sepanjang command buffer, dan harganya
    /// beberapa kali lipat waktu frame — memakainya terus-menerus berarti
    /// editor yang tidak bisa dipakai. Ia dinyalakan pada satu jalan yang
    /// disengaja, sesudah pass baru ditambahkan, lalu dimatikan lagi.
    ///
    /// **Dan ia satu-satunya yang bisa menjawab pertanyaan G3.** Validasi biasa
    /// memeriksa layout: sebuah descriptor yang menjanjikan `GENERAL` atas
    /// image yang sedang `SHADER_READ_ONLY` dilaporkannya. Barrier yang
    /// *hilang* di antara dua dispatch tidak melanggar satu pun aturan layout —
    /// yang dilanggarnya adalah urutan — dan tanpa lapisan ini ia hanya terlihat
    /// sebagai hasil yang berubah-ubah antar-jalan di mesin orang lain.
    bool enableSyncValidation = false;

    /// Berkas tempat isi `VkPipelineCache` disimpan antar-jalan. Kosong berarti
    /// tidak disimpan sama sekali.
    ///
    /// **Cache yang hidup hanya selama satu proses hampir tidak ada gunanya.**
    /// Yang mahal bukan pipeline kedua yang bentuknya sama dengan yang pertama,
    /// melainkan pipeline pertama — dan itu dibayar ulang setiap kali program
    /// dijalankan. Yang terlihat adalah tersendat beberapa ratus milidetik saat
    /// material pertama muncul di viewport, dan besok ia terjadi lagi.
    std::filesystem::path pipelineCachePath;
};

/// Apa yang perangkat ini sanggup lakukan, dalam nama yang berarti bagi kode di
/// atasnya.
///
/// **Satu tempat, dan jawabannya bisa ditanyakan.** Sebelum ini setiap fitur
/// dinyalakan dengan menempelkan satu baris di tengah `CreateLogicalDevice`, dan
/// tidak ada satu pun yang bisa menjawab "apa yang sebenarnya aktif di mesin
/// ini" tanpa membaca kode. Empat milestone berikutnya masing-masing menambah
/// satu fitur; tanpa tempat yang jelas, keempatnya akan menaruhnya di empat
/// tempat berbeda.
///
/// **Didukung tidak sama dengan dipakai.** Sebagian medan di sini dilaporkan
/// jauh sebelum ada yang memakainya — justru itu gunanya: keputusan "jalur mana
/// yang akan ditempuh di mesin ini" bisa dilihat sebelum jalurnya ditulis.
struct DeviceCapabilities {
    bool vulkan13 = false;
    bool dynamicRendering = false;
    bool synchronization2 = false;
    bool blockCompression = false;
    /// Ray query untuk backend GI tier atas (M7 rencana GI). Dideteksi, belum
    /// diaktifkan.
    bool rayQuery = false;

    // --- Yang dituntut milestone di depan (docs/PLAN-GPU-OPTIM.md) ---
    /// Bindless, dan **sudah dinyalakan** — bukan sekadar didukung.
    ///
    /// Ia benar hanya kalau seluruh sub-fitur yang dituntut jalur bindless ikut
    /// ada: larik tak berbatas, indeks non-uniform, penulisan sesudah pengikatan,
    /// dan pengikatan sebagian. `descriptorIndexing` sendirian tidak menjamin
    /// satu pun di antaranya — ia hanya berarti ekstensinya dipromosikan ke
    /// inti, dan perangkat boleh menjawab "ya" untuknya sambil menolak
    /// keempatnya.
    bool descriptorIndexing = false;
    /// Banyaknya slot di larik tekstur bindless. Nol saat bindless mati.
    uint32_t bindlessTextureCapacity = 0;
    /// Dituntut G6 bersama indirect draw.
    bool bufferDeviceAddress = false;
    /// Sinkronisasi lintas-antrean G7. Semaphore biner memaksa satu penunggu per
    /// sinyal, dan ketergantungan yang berbentuk graf harus dipaksa jadi rantai.
    bool timelineSemaphore = false;
    /// Indirect draw dengan banyak perintah **dan** `firstInstance` bukan nol.
    /// Keduanya dituntut jalur GPU-driven, jadi keduanya dijawab satu medan:
    /// yang punya salah satunya saja tetap tidak bisa memakainya.
    bool multiDrawIndirect = false;
    bool drawIndirectCount = false;
    /// Setengah presisi di shader. Turing ke atas menjalankannya dua kali lipat,
    /// dan matematika GI adalah tempat yang paling diuntungkan.
    bool shaderFloat16 = false;
    /// Query statistik pipeline. Dipakai profiler untuk menghitung primitif per
    /// pass — satu-satunya angka yang bisa membuktikan culling bekerja.
    bool pipelineStatisticsQuery = false;
    /// Ada keluarga antrean compute yang **terpisah** dari antrean grafis.
    /// Prasyarat G7; antreannya sendiri sudah dibuat sejak sekarang.
    bool asyncCompute = false;
};

/// Instance + physical device + logical device + queue + allocator.
///
/// Refactor dari sdl3_vulkan.cpp menjadi objek dengan masa hidup jelas.
/// Perbedaan yang disengaja dari berkas acuan:
///   - memakai VK_EXT_debug_utils, bukan VK_EXT_debug_report yang sudah usang
///   - pemilihan GPU mengutamakan discrete, dan mencatat pilihannya ke log
///   - alokasi memori lewat VMA sejak awal, supaya E8 tidak perlu migrasi
/// Antrean tempat sebuah batch berjalan.
enum class QueueKind : uint8_t {
    Graphics,
    /// Keluarga compute yang **tidak** bisa menggambar. Keluarga yang punya
    /// kedua bit hampir selalu keluarga grafis itu sendiri, dan dua antrean di
    /// atasnya tidak membeli tumpang tindih apa pun.
    AsyncCompute,
};

/// Satu batch submit: sebuah command buffer utuh pada satu antrean.
///
/// **Utuh, dan itu bukan pilihan.** Penungguan semaphore berlaku di awal
/// submit, jadi pekerjaan yang boleh jalan sebelum penungguan harus berada di
/// command buffer yang berbeda. Itulah yang membuat "batch" di sini sama dengan
/// "command buffer", bukan potongan sebuah command buffer.
struct SubmitBatch {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    QueueKind queue = QueueKind::Graphics;
    /// Nilai timeline **relatif frame ini**, mulai 1. Nol berarti tidak
    /// menunggu apa pun.
    uint64_t wait = 0;
    /// Tahap tempat penungguan itu berlaku. Menyebut tahap yang sedekat mungkin
    /// dengan pemakai sebenarnya adalah seluruh gunanya: `TOP_OF_PIPE` menahan
    /// seluruh batch dan menghapus tumpang tindih yang baru saja dibeli.
    VkPipelineStageFlags2 waitStages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    /// Nilai timeline relatif yang di-signal batch ini. Nol berarti tidak.
    uint64_t signal = 0;
};

class Device {
public:
    Device() = default;
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&&) = delete;
    Device& operator=(Device&&) = delete;

    bool Create(const DeviceDesc& desc);
    void Destroy();

    /// Membuat surface untuk sebuah jendela SDL. Ada di sini, bukan di
    /// Platform, supaya VkInstance tidak perlu bocor ke luar RHI.
    VkSurfaceKHR CreateSurface(SDL_Window* window) const;
    void DestroySurface(VkSurfaceKHR surface) const;

    /// Command buffer sekali pakai untuk transfer/transisi layout saat setup.
    ///
    /// Menunggu queue idle sebelum kembali. Hanya untuk pekerjaan di luar loop
    /// frame (pembuatan aset, unggah tekstur awal) — memakainya per frame
    /// berarti satu stall CPU-GPU penuh setiap frame.
    VkCommandBuffer BeginOneShot() const;
    void EndOneShot(VkCommandBuffer commandBuffer) const;

    /// Command buffer kedua pada slot transient yang sama, di keluarga antrean
    /// compute.
    ///
    /// `VK_NULL_HANDLE` bila perangkat ini tidak punya keluarga compute
    /// terpisah atau tidak punya timeline semaphore — pemanggilnya lalu merekam
    /// semuanya ke command buffer grafis, dan itulah jalur satu-antrean yang
    /// wajib tetap ada sebagai pembanding.
    VkCommandBuffer BeginTransientCompute(VkCommandBuffer primary) const;

    /// Command buffer grafis tambahan pada slot yang sama.
    ///
    /// **Ada karena batch adalah command buffer utuh.** Sebuah submit menunggu
    /// semaphore di awalnya, jadi pekerjaan grafis yang boleh jalan *sebelum*
    /// penungguan itu harus berada di command buffer yang lain.
    VkCommandBuffer BeginTransientGraphics(VkCommandBuffer primary) const;

    /// Versi yang tidak memblokir, untuk pekerjaan per frame.
    ///
    /// Submit ditandai fence dan command buffer-nya dipakai ulang begitu fence
    /// tersebut selesai, sehingga CPU tidak pernah menunggu GPU. Urutan
    /// eksekusi antar-submit pada queue yang sama tetap terjaga, dan
    /// ketergantungan memorinya diurus barrier/subpass dependency di pass yang
    /// bersangkutan — jadi tidak ada yang hilang dengan menghapus penungguan.
    VkCommandBuffer BeginTransient() const;

    /// Mengembalikan nomor submit, bukan fence.
    ///
    /// Fence-nya milik Device dan didaur ulang: begitu sebuah slot selesai, ia
    /// di-reset dan dipakai submit berikutnya. Pemanggil yang menyimpan fence
    /// itu untuk ditunggu belakangan akan menunggu submit yang sama sekali
    /// berbeda — dan bila slot tersebut sedang direkam untuk frame ini,
    /// menunggunya berarti menunggu sesuatu yang belum di-submit sama sekali.
    /// Nomor submit tidak pernah dipakai ulang, jadi tidak bisa salah sasaran.
    uint64_t SubmitTransient(VkCommandBuffer commandBuffer) const;

    /// Submit beberapa batch sekaligus, dengan urutan antar-antrean dijaga
    /// timeline semaphore.
    ///
    /// **Nilai timeline-nya relatif frame ini**, mulai dari 1. Device yang
    /// menerjemahkannya ke nilai mutlak; pemanggil tidak boleh menyimpan angka
    /// mutlak, karena semaphore-nya satu dan dipakai seluruh frame.
    ///
    /// Batch terakhir tiap antrean yang dipakai mendapat fence-nya sendiri, dan
    /// slot ini baru bisa dipakai ulang setelah keduanya selesai. Tanpa itu,
    /// slot yang antrean grafisnya sudah selesai akan direkam ulang sementara
    /// antrean compute-nya masih membaca command buffer yang sama.
    uint64_t SubmitTransientBatches(VkCommandBuffer primary,
                                    std::span<const SubmitBatch> batches) const;

    /// Menunggu submit transient tertentu selesai.
    ///
    /// Langsung kembali bila submit itu memang sudah selesai — termasuk kalau
    /// slotnya sudah dipakai ulang, karena Device hanya memakai ulang slot yang
    /// fence-nya sudah di-signal. Dipakai pemanggil yang menulis ulang buffer
    /// host-visible: tanpa ini GPU bisa membaca data yang sedang ditimpa.
    void WaitTransient(uint64_t submitId) const;

    void WaitIdle() const;

    VkInstance Instance() const { return instance_; }
    VkPhysicalDevice PhysicalDevice() const { return physicalDevice_; }
    VkDevice Handle() const { return device_; }
    VkQueue GraphicsQueue() const { return graphicsQueue_; }
    uint32_t GraphicsQueueFamily() const { return graphicsQueueFamily_; }
    /// Antrean compute khusus, atau `VK_NULL_HANDLE` bila perangkat ini tidak
    /// punya keluarga antrean compute yang terpisah dari grafis.
    ///
    /// **Dibuat sekarang, dipakai G7.** Alasannya sama dengan `dynamicRendering`
    /// yang dinyalakan di E1 padahal baru dipakai di E8: pembuatan device adalah
    /// tempat yang tidak enak disentuh ulang, dan yang menyentuhnya belakangan
    /// harus menguji ulang setiap perangkat.
    VkQueue ComputeQueue() const { return computeQueue_; }
    uint32_t ComputeQueueFamily() const { return computeQueueFamily_; }
    VmaAllocator Allocator() const { return allocator_; }
    uint32_t ApiVersion() const { return apiVersion_; }

    /// Kemampuan perangkat ini, satu tempat untuk seluruh pertanyaan "boleh
    /// tidak jalur ini dipakai di sini".
    const DeviceCapabilities& Capabilities() const { return capabilities_; }

    /// Apakah perangkat ini **bisa** memakai ray query. Belum diaktifkan —
    /// aktivasinya bersama acceleration structure di M7 rencana GI.
    bool SupportsRayQuery() const { return supportsRayQuery_; }
    const std::string& DeviceName() const { return deviceName_; }
    VkPipelineCache PipelineCache() const { return pipelineCache_; }
    /// True bila dynamic rendering, synchronization2, dan kawan-kawannya
    /// tersedia. Dipakai E8 untuk memilih jalur render modern.
    bool SupportsVulkan13() const { return supportsVulkan13_; }

    /// Apakah perangkat ini bisa menggambar dari format blok BCn.
    ///
    /// **Ditanya, bukan diandaikan.** Yang membaca `.ktx2` ber-BC7 pada
    /// perangkat tanpa dukungan itu harus memilih jalur lain — bukan mengunggah
    /// dan berharap.
    bool SupportsBlockCompression() const { return supportsBlockCompression_; }

private:
    bool CreateInstance(const DeviceDesc& desc);
    bool SelectPhysicalDevice();
    bool CreateLogicalDevice();
    /// Membuat pipeline cache, diisi dari berkas bila isinya memang milik
    /// perangkat ini.
    void CreatePipelineCache();
    /// Menulis isi cache ke berkasnya. Dipanggil sekali, dari `Destroy`.
    void SavePipelineCache() const;

    /// Satu slot command buffer sekali pakai beserta fence penandanya.
    ///
    /// **Dua keluarga antrean, dua kolam, dua fence.** Command pool terikat pada
    /// keluarga antrean, jadi command buffer yang dijalankan antrean compute
    /// tidak bisa datang dari kolam grafis; dan fence menandai satu submit, jadi
    /// dua antrean menuntut dua penanda.
    struct TransientSubmit {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandPool computePool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> graphics;
        std::vector<VkCommandBuffer> compute;
        /// Berapa banyak yang sudah dibagikan frame ini.
        uint32_t graphicsUsed = 0;
        uint32_t computeUsed = 0;
        VkFence fence = VK_NULL_HANDLE;
        VkFence computeFence = VK_NULL_HANDLE;
        bool computePending = false;
        /// Nomor submit yang sedang ditandai fence ini, 0 bila slot menganggur.
        uint64_t submitId = 0;
        bool pending = false;
    };

    /// Command buffer berikutnya dari kolam `pool`, dialokasikan bila perlu.
    VkCommandBuffer NextTransientBuffer(std::vector<VkCommandBuffer>& pool, uint32_t& used,
                                        VkCommandPool from) const;
    TransientSubmit* FindTransient(VkCommandBuffer primary) const;

    TransientSubmit CreateTransient() const;

    // mutable: Begin/SubmitTransient dipanggil dari jalur render yang memegang
    // Device secara const, tapi keduanya memang mengubah kolam internal ini.
    mutable std::vector<TransientSubmit> transients_;
    mutable uint64_t nextSubmitId_ = 1;
    /// Satu untuk seluruh perangkat, menaik terus. Lihat `SubmitTransientBatches`.
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    mutable uint64_t timelineValue_ = 0;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily_ = UINT32_MAX;
    VkQueue computeQueue_ = VK_NULL_HANDLE;
    uint32_t computeQueueFamily_ = UINT32_MAX;
    DeviceCapabilities capabilities_;
    std::filesystem::path pipelineCachePath_;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkCommandPool oneShotPool_ = VK_NULL_HANDLE;
    VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
    uint32_t apiVersion_ = VK_API_VERSION_1_0;
    bool supportsRayQuery_ = false;
    /// True bila device dibuat tanpa ekstensi instance — yaitu tanpa jendela.
    bool headless_ = false;
    std::string deviceName_;
    bool validationEnabled_ = false;
    bool syncValidationEnabled_ = false;
    bool supportsVulkan13_ = false;
    bool supportsBlockCompression_ = false;
};

}  // namespace sim::rhi
