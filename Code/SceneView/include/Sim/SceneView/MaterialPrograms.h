#pragma once

#include "Sim/Core/Uuid.h"
#include "Sim/Material/MaterialCompiler.h"
#include "Sim/Material/MaterialInstance.h"
#include "Sim/Material/MaterialParameterBlock.h"
#include "Sim/Material/ShaderCache.h"
#include "Sim/Render/Types.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sim {
class TaskPool;
}

namespace sim::assets {
class AssetDatabase;
}

namespace sim::render {
class IViewportRenderer;
}

/// Penjaga shader material untuk pass forward: dari GUID material ke pipeline
/// yang sudah dibangun renderer.
///
/// **Ada karena mengompilasi material adalah detik, bukan milidetik.** Satu
/// panggilan `slangc` per material, dan pekerjaan sebesar itu di dalam jalur
/// gambar berarti editor yang membeku tepat pada frame sebuah level dibuka —
/// yaitu frame yang paling terasa. Bentuknya meniru `assets::TextureBakery`, dan
/// karena alasan yang sama persis.
namespace sim::view {

enum class MaterialProgramState : uint8_t {
    /// Sedang dikompilasi, atau baru diantre. **Bukan galat** — yang menerimanya
    /// menggambar ruas itu lewat jalur mundur `box.frag` sampai ia berubah.
    Pending,
    Ready,
    /// Sudah dicoba dan gagal. Diingat supaya material rusak tidak dikompilasi
    /// ulang enam puluh kali per detik.
    Failed,
};

struct MaterialProgramRef {
    MaterialProgramState state = MaterialProgramState::Pending;
    /// Terisi hanya pada `Ready`.
    render::MaterialHandle handle = render::kInvalidMaterial;
};

struct ResolvedMaterialTexture {
    render::TextureHandle handle = render::kInvalidTexture;
    /// False selama hasil bake-nya belum ada.
    ///
    /// **Dibawa terpisah dari handle-nya, dan itu menentukan.** Handle yang
    /// belum siap tetap sebuah handle — placeholder magenta — dan material yang
    /// dibangun dengan handle itu akan memakainya **selamanya**: descriptor
    /// set-nya ditulis sekali, dan tidak ada yang meninjaunya lagi. Yang
    /// terlihat adalah objek magenta yang tidak pernah berubah, bukan objek
    /// yang sedang menunggu.
    bool ready = false;
};

/// Menyelesaikan sebuah aset gambar menjadi handle tekstur.
///
/// Diserahkan pemanggil karena jalur itu miliknya: ia yang memegang baker
/// tekstur, dan ia pula yang tahu placeholder apa yang berlaku sementara hasil
/// bake-nya belum ada.
using MaterialTextureResolver = std::function<ResolvedMaterialTexture(const Uuid&)>;

class MaterialPrograms {
public:
    /// `tasks` boleh null: tanpanya kompilasi dikerjakan di tempat, di thread
    /// yang memanggil. Itu jalur uji dan jalur headless — keduanya tidak punya
    /// frame yang bisa terlihat membeku.
    MaterialPrograms(std::filesystem::path shaderCacheDir, std::filesystem::path shaderDir,
                     TaskPool* tasks);

    MaterialPrograms(const MaterialPrograms&) = delete;
    MaterialPrograms& operator=(const MaterialPrograms&) = delete;

    /// Meminta pipeline sebuah material. **Tidak pernah memblokir** ketika ada
    /// `TaskPool`.
    ///
    /// Harus dipanggil dari main thread: langkah terakhirnya membuat objek
    /// Vulkan lewat `renderer`, dan itu bukan sesuatu yang boleh terjadi di
    /// worker.
    MaterialProgramRef Request(const assets::AssetDatabase& assets, const Uuid& guid,
                               render::IViewportRenderer& renderer,
                               const MaterialTextureResolver& resolveTexture);

    /// Melupakan sebuah material sehingga permintaan berikutnya
    /// mengompilasinya ulang. Dipanggil ketika berkasnya berubah.
    void Invalidate(const Uuid& guid);

    /// Berapa yang belum selesai. Dipakai status bar.
    std::size_t PendingCount() const;

    /// Berapa kali `slangc` benar-benar dijalankan sejak kelas ini dibuat —
    /// bukan berapa kali ia diminta.
    ///
    /// **Ada supaya "sekali per material" dibuktikan dengan hitungan.** Dua
    /// tugas untuk material yang sama menghasilkan berkas SPIR-V yang identik,
    /// jadi tidak ada satu pun akibat yang terlihat — yang terbuang hanya
    /// beberapa detik inti, diam-diam.
    std::size_t CompileCount() const { return compiles_.load(std::memory_order_relaxed); }

    /// False bila `slangc` atau berkas shader renderer tidak ada. Setiap ruas
    /// lalu digambar jalur mundur, dan itu dilaporkan sekali di log — bukan
    /// didiamkan, karena yang melihatnya akan mengira materialnya yang salah.
    bool Usable() const { return usable_; }

private:
    /// Apa yang dibutuhkan worker, sudah dikumpulkan di main thread.
    ///
    /// **Jalur berkas dan salinan instance, bukan pointer ke indeks aset.**
    /// `AssetDatabase` ditukar utuh saat pemindaian latar selesai, jadi pointer
    /// ke dalamnya hanya sah selama satu frame — sementara tugas ini hidup
    /// beberapa detik.
    struct Job {
        std::filesystem::path graphPath;
        material::MaterialInstance instance;
        bool hasInstance = false;
        /// Jalur yang diminta renderer, dibaca sekali di `Request`.
        ///
        /// **Ikut di dalam job, bukan disimpan sebagai keadaan kelas ini.**
        /// Worker mengompilasi di thread lain; renderer yang ditanya di
        /// tengah-tengahnya adalah renderer yang boleh saja sudah dihancurkan.
        bool bindless = false;
    };

    /// Hasil worker, menunggu diambil main thread.
    struct Compiled {
        std::vector<uint32_t> spirv;
        std::vector<uint8_t> parameters;
        /// Aset tekstur tiap slot, urutannya urutan deklarasi kompiler.
        std::vector<Uuid> textures;
        /// Jalur yang dipakai saat SPIR-V ini dihasilkan.
        bool bindless = false;
        /// Material bertopeng: shader-nya membuang fragmen di bawah ambang, dan
        /// renderer harus menggambarnya di luar prepass. Lihat
        /// `MaterialCompileResult::alphaTest`.
        bool masked = false;
        bool blended = false;
    };

    struct Entry {
        MaterialProgramState state = MaterialProgramState::Pending;
        render::MaterialHandle handle = render::kInvalidMaterial;
        bool compiled = false;
        Compiled result;
    };

    void Compile(Uuid guid, Job job);

    std::filesystem::path shaderCacheDir_;
    std::filesystem::path shaderDir_;
    TaskPool* tasks_ = nullptr;
    bool usable_ = false;
    std::atomic<std::size_t> compiles_{0};
    std::string prelude_;
    std::string frameDeclarations_;

    /// Menjaga `entries_`.
    mutable std::mutex mutex_;
    std::unordered_map<Uuid, Entry> entries_;

    /// Menjaga `cache_` sendiri. **Terpisah dari `mutex_`**: kompilasi memakan
    /// detik, dan menahan kunci entri selama itu membuat setiap `Request` di
    /// main thread ikut menunggunya — yaitu persis pembekuan yang kelas ini ada
    /// untuk menghindarinya.
    std::mutex cacheMutex_;
    material::ShaderCache cache_;
};

}  // namespace sim::view
