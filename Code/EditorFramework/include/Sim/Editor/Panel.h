#pragma once

#include "Sim/Core/Uuid.h"

#include "Sim/Editor/EditorContext.h"

#include <string>
#include <utility>

namespace sim::editor {

/// Kategori untuk pengelompokan di menu Window.
enum class PanelCategory {
    Scene,
    Assets,
    Authoring,
    Debug,
};

/// Kelas dasar semua panel editor.
///
/// Sengaja tipis. Yang membuat menambah panel jadi murah bukan kelas dasar yang
/// pintar, melainkan PanelManager yang mengurus jendela, menu, dan penyimpanan
/// keadaan — sehingga panel cukup memikirkan isinya.
class Panel {
public:
    Panel(std::string id, std::string title, PanelCategory category)
        : id_(std::move(id)), title_(std::move(title)), category_(category) {}

    virtual ~Panel() = default;

    Panel(const Panel&) = delete;
    Panel& operator=(const Panel&) = delete;

    /// Isi panel. Dipanggil di antara Begin()/End() yang sudah diurus
    /// PanelManager, jadi implementasi tidak perlu memanggilnya sendiri.
    virtual void OnDraw(EditorContext& context) = 0;

    /// Dipanggil sekali per frame walau panel tertutup. Untuk pekerjaan yang
    /// harus tetap berjalan (mis. polling hasil job import).
    virtual void OnUpdate(EditorContext& /*context*/) {}

    /// Membuka sebuah aset di panel ini. Mengembalikan false bila panel ini
    /// tidak menangani jenis aset itu.
    ///
    /// **Panelnya yang memutuskan, bukan sebuah tabel tipe di tempat lain.**
    /// Yang tahu jenis berkas apa yang bisa dibuka sebuah editor hanyalah editor
    /// itu sendiri, dan tabel terpusat adalah tempat yang harus diingat setiap
    /// kali sebuah editor baru lahir — persis yang dihindari pendaftaran panel
    /// lewat SIM_REGISTER_PANEL.
    virtual bool OpenAsset(const Uuid& /*guid*/, EditorContext& /*context*/) { return false; }

    /// Flag ImGui tambahan untuk jendela panel ini (mis. MenuBar, NoScrollbar).
    virtual int WindowFlags() const { return 0; }

    /// Padding nol dipakai panel yang isinya satu gambar penuh (Viewport),
    /// supaya tidak ada bingkai abu-abu di sekelilingnya.
    virtual bool WantsZeroPadding() const { return false; }

    /// Panel terkunci tidak bisa dilepas dari dockspace, digeser, maupun
    /// ditutup — tombol X pada tabnya ikut hilang.
    ///
    /// Dipakai panel yang mahal kalau sampai memiliki jendela OS sendiri.
    /// Panel yang ditarik keluar dockspace menjadi viewport platform terpisah
    /// dengan swapchain dan present-nya sendiri; untuk panel berisi teks itu
    /// murah, tapi untuk Viewport 3D artinya satu render target penuh melewati
    /// jalur present tambahan setiap frame.
    ///
    /// Tombol tutup ikut dihilangkan karena panel yang tidak bisa dipindahkan
    /// tapi bisa hilang adalah kombinasi yang menyesatkan: pengguna kehilangan
    /// viewport tanpa cara mengembalikannya ke tempat semula.
    /// Ukurannya tetap bisa diubah lewat pemisah dockspace.
    virtual bool IsDockLocked() const { return false; }

    const std::string& Id() const { return id_; }
    const std::string& Title() const { return title_; }
    PanelCategory Category() const { return category_; }

    bool IsOpen() const { return open_; }
    void SetOpen(bool open) { open_ = open; }
    bool* OpenFlag() { return &open_; }

    /// True saat panel sedang jadi jendela yang difokuskan. Diisi PanelManager.
    ///
    /// **Ini hasil pengamatan, bukan perintah.** PanelManager menimpanya tiap
    /// frame dengan keadaan ImGui yang sebenarnya, jadi menyetelnya dari luar
    /// tidak memindahkan fokus ke mana-mana — nilainya hilang di frame
    /// berikutnya. Yang memindahkan fokus adalah `RequestFocus`.
    bool IsFocused() const { return focused_; }
    void SetFocused(bool focused) { focused_ = focused; }

    /// Meminta jendela panel ini dibawa ke depan sekali, di frame berikutnya.
    ///
    /// Perlu untuk panel yang di-dock jadi tab: membukanya saja (`SetOpen`)
    /// tidak terlihat kalau tabnya tertumpuk di belakang tab lain — orang klik
    /// ganda sebuah aset, dan layarnya tampak tidak berubah sama sekali.
    void RequestFocus() { focusRequested_ = true; }
    /// Mengambil sekaligus menghapus permintaannya. Dipanggil PanelManager.
    bool TakeFocusRequest() { return std::exchange(focusRequested_, false); }

private:
    std::string id_;
    std::string title_;
    PanelCategory category_;
    bool open_ = true;
    bool focused_ = false;
    bool focusRequested_ = false;
};

}  // namespace sim::editor
