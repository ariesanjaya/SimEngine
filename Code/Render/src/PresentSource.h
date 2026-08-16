#pragma once

#include "Sim/RHI/Vulkan.h"

namespace sim::render {

/// Apa yang dibutuhkan `Presenter` dari sebuah perender untuk menyalinnya ke
/// layar.
///
/// **Internal `Sim::Render`, dan itu yang menjaga aturannya.** Header publik
/// modul ini tidak boleh menyebut satu pun tipe Vulkan; `IViewportRenderer`
/// karena itu hanya menyerahkan `TextureHandle`, yang di editor adalah descriptor
/// set ImGui dan tidak berarti apa-apa di luar sana. Antarmuka ini hidup di
/// `src/`, tempat Vulkan memang boleh, dan `Presenter` menemukannya lewat
/// `dynamic_cast`.
///
/// Perender yang tidak mengimplementasikannya tidak bisa dipresent — dan itu
/// jawaban yang jujur, bukan layar hitam.
class IPresentSource {
public:
    virtual ~IPresentSource() = default;

    virtual VkImageView PresentView() const = 0;
    virtual VkSampler PresentSampler() const = 0;

    /// Petak yang benar-benar terisi, sebagai pecahan alokasinya. Target render
    /// dialokasikan lebih besar daripada yang diminta; tanpa angka ini yang
    /// tersalin adalah sisa alokasi yang tidak pernah diisi.
    virtual float PresentUvMaxU() const = 0;
    virtual float PresentUvMaxV() const = 0;
};

}  // namespace sim::render
