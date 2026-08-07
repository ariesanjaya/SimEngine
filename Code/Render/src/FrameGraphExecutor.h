#pragma once

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
    void Clear();

    /// Menjalankan graph. `recorders` diindeks `PassId`; entri kosong berarti
    /// pass yang tidak menggambar apa pun.
    ///
    /// Mengembalikan false kalau ada resource yang dipakai tapi tidak dipasangkan
    /// — dilaporkan, bukan dilewati diam-diam: pass yang menggambar ke image
    /// kosong menghasilkan layar hitam tanpa satu pun pesan validasi.
    bool Execute(const CompiledGraph& compiled, VkCommandBuffer cmd,
                 std::span<const Recorder> recorders);

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

    std::vector<BoundImage> bound_;
    std::vector<VkImageLayout> layout_;
};

}  // namespace sim::render
