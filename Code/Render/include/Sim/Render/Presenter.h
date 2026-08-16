#pragma once

#include <filesystem>
#include <memory>

// Hanya deklarasi maju, aturan yang sama dengan `RendererFactory.h`: header
// publik `Render` tidak boleh menyeret Vulkan ke siapa pun yang meng-include-nya.
namespace sim::rhi {
class Device;
class Swapchain;
}  // namespace sim::rhi

namespace sim::render {

class IViewportRenderer;

/// Memindahkan gambar yang sudah dirender ke layar.
///
/// **Sebuah pass, bukan sebuah blit.** Keduanya menyalin piksel, tapi yang satu
/// adalah tempat yang bisa ditumpangi dan yang lain bukan: tonemap akhir,
/// letterbox, dan overlay UI player semuanya berakhir di sini. Sebuah
/// `vkCmdBlitImage` harus dibongkar lagi begitu salah satunya dibutuhkan.
///
/// **Ada karena perender menggambar ke targetnya sendiri.** Itu keputusan
/// editor: viewport hidup di dalam sebuah panel, jadi hasilnya harus berupa
/// tekstur. Di editor yang memindahkannya ke layar adalah ImGui; player tidak
/// punya ImGui, dan ini yang menggantikannya.
///
/// Pipeline-nya dibangun terhadap render pass swapchain, jadi ia harus dibuat
/// ulang bila swapchain-nya dibangun ulang dengan format yang berbeda. Perubahan
/// ukuran saja tidak menuntut apa-apa — viewport dan scissor dinamis.
class Presenter {
public:
    Presenter();
    ~Presenter();

    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    bool Create(rhi::Device& device, rhi::Swapchain& swapchain,
                const std::filesystem::path& shaderDirectory);
    void Destroy();

    /// Menggambar keluaran `renderer` memenuhi layar.
    ///
    /// Dipanggil **di dalam** render pass swapchain yang sedang aktif — antara
    /// `Swapchain::BeginRenderPass` dan `EndRenderPass`.
    ///
    /// Mengembalikan false bila perender ini tidak bisa menyerahkan gambarnya.
    /// Bukan setiap perender bisa: perender uji tidak punya target sama sekali,
    /// dan yang tidak bisa harus mengatakannya alih-alih membiarkan layar hitam
    /// yang terbaca sebagai adegan kosong.
    bool Draw(IViewportRenderer& renderer, rhi::Swapchain& swapchain);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sim::render
