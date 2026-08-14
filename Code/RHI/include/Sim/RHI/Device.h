#pragma once

#include "Sim/RHI/Vulkan.h"

#include <cstdint>
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
};

/// Instance + physical device + logical device + queue + allocator.
///
/// Refactor dari sdl3_vulkan.cpp menjadi objek dengan masa hidup jelas.
/// Perbedaan yang disengaja dari berkas acuan:
///   - memakai VK_EXT_debug_utils, bukan VK_EXT_debug_report yang sudah usang
///   - pemilihan GPU mengutamakan discrete, dan mencatat pilihannya ke log
///   - alokasi memori lewat VMA sejak awal, supaya E8 tidak perlu migrasi
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
    VmaAllocator Allocator() const { return allocator_; }
    uint32_t ApiVersion() const { return apiVersion_; }

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

    /// Satu slot command buffer sekali pakai beserta fence penandanya.
    struct TransientSubmit {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        /// Nomor submit yang sedang ditandai fence ini, 0 bila slot menganggur.
        uint64_t submitId = 0;
        bool pending = false;
    };

    TransientSubmit CreateTransient() const;

    // mutable: Begin/SubmitTransient dipanggil dari jalur render yang memegang
    // Device secara const, tapi keduanya memang mengubah kolam internal ini.
    mutable std::vector<TransientSubmit> transients_;
    mutable uint64_t nextSubmitId_ = 1;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily_ = UINT32_MAX;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkCommandPool oneShotPool_ = VK_NULL_HANDLE;
    VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
    uint32_t apiVersion_ = VK_API_VERSION_1_0;
    bool supportsRayQuery_ = false;
    std::string deviceName_;
    bool validationEnabled_ = false;
    bool supportsVulkan13_ = false;
    bool supportsBlockCompression_ = false;
};

}  // namespace sim::rhi
