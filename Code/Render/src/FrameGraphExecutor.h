#pragma once

#include "Sim/RHI/GpuProfiler.h"
#include "Sim/RHI/Vulkan.h"
#include "Sim/Render/FrameGraph.h"

#include <functional>
#include <span>
#include <vector>

namespace sim::render {

/// Image sungguhan di balik sebuah resource graph.
struct BoundImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
};

/// Buffer sungguhan di balik sebuah resource graph.
///
/// **Terpisah dari `BoundImage` karena barrier-nya jenis yang berbeda, bukan
/// karena kerapian.** Buffer tidak punya layout: yang berpindah hanyalah
/// ketampakan tulisannya, jadi `VkBufferMemoryBarrier2` menyebut stage dan
/// akses tanpa `oldLayout`/`newLayout`. Memaksa keduanya lewat satu jalur
/// berarti buffer ikut membawa layout yang tidak berarti apa-apa — dan layout
/// yang tidak berarti apa-apa adalah layout yang suatu saat dipakai.
struct BoundBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = VK_WHOLE_SIZE;
};

/// Menjalankan hasil kompilasi frame graph sebagai perintah Vulkan.
///
/// **Eksekutor tidak tahu apa pun tentang pass yang dijalankannya.** Ia hanya
/// memasang barrier yang sudah disimpulkan graph lalu memanggil kembali
/// perekamnya. Itu yang membuat menambah pass tidak menyentuh eksekutor sama
/// sekali — dan yang membuat `FrameGraph` bisa tinggal di header yang bebas
/// Vulkan, karena satu-satunya yang tahu Vulkan adalah berkas ini.
///
/// Barrier memakai `synchronization2`. Bukan demi kebaruan: `VkImageMemoryBarrier2`
/// memisahkan stage sumber dari stage tujuan per-barrier, sedangkan barrier lama
/// memakai satu pasang stage untuk seluruh kelompok — jadi satu tekstur yang
/// pindah dari tulis-warna ke baca-shader memaksa *semua* barrier di
/// panggilan yang sama menunggu di stage yang sama.
class FrameGraphExecutor {
public:
    /// Perekam sebuah pass. Indeksnya `PassId` — pass yang dibuang graph tidak
    /// pernah dipanggil, jadi perekam boleh mengandaikan resource-nya siap.
    using Recorder = std::function<void(VkCommandBuffer)>;

    /// Memasangkan sebuah resource graph ke image sungguhan. Wajib untuk setiap
    /// resource yang benar-benar dipakai pass yang hidup.
    void Bind(ResourceId resource, const BoundImage& image);
    /// Sama, untuk resource yang isinya buffer.
    ///
    /// Belum ada pass yang memakainya: pemakai pertamanya adalah penetapan lampu
    /// ke cluster di G4, yang menulis hasilnya ke storage buffer. Ada sejak
    /// sekarang dengan alasan yang sama seperti antrean compute di G2 — jalur
    /// yang ditambahkan bersama pemakai pertamanya adalah jalur yang dirancang
    /// untuk satu pemakai.
    void Bind(ResourceId resource, const BoundBuffer& buffer);
    void Clear();

    /// Menjalankan graph. `recorders` diindeks `PassId`; entri kosong berarti
    /// pass yang tidak menggambar apa pun.
    ///
    /// Mengembalikan false kalau ada resource yang dipakai tapi tidak dipasangkan
    /// — dilaporkan, bukan dilewati diam-diam: pass yang menggambar ke image
    /// kosong menghasilkan layar hitam tanpa satu pun pesan validasi.
    /// `profiler` boleh null. Bila ada, tiap pass dibungkus lingkup ukur yang
    /// namanya diambil dari graph — jadi menambah pass otomatis menambah
    /// barisnya di tabel waktu, tanpa satu pun daftar yang harus dipelihara.
    bool Execute(const CompiledGraph& compiled, VkCommandBuffer cmd,
                 std::span<const Recorder> recorders, rhi::GpuProfiler* profiler = nullptr);

    /// Keadaan layout terakhir sebuah resource sesudah `Execute`. Dipakai
    /// pemanggil yang harus mengembalikan image impor ke keadaan semula.
    VkImageLayout LayoutOf(ResourceId resource) const;

private:
    struct Stage {
        VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
        VkAccessFlags2 access = VK_ACCESS_2_NONE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    static Stage Translate(Access access);

    /// Memastikan ketiga larik di bawah muat untuk `resource`.
    void Reserve(ResourceId resource);

    std::vector<BoundImage> bound_;
    std::vector<BoundBuffer> buffers_;
    std::vector<VkImageLayout> layout_;
};

}  // namespace sim::render
