#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace sim::material {

/// Tahap shader yang dikompilasi. Menentukan entry point dan profil `slangc`.
enum class ShaderStage : uint8_t {
    Vertex,
    Fragment,
};

const char* ToString(ShaderStage stage);

/// Nilai spesialisasi yang membedakan varian sebuah material.
///
/// **Varian TIDAK ikut kunci cache, dan itu inti rancangannya.** Ketiganya
/// dipasang sebagai konstanta spesialisasi saat pipeline dibuat, bukan saat
/// shader dikompilasi — jadi satu modul SPIR-V melayani kedelapan kombinasinya.
/// Menjadikan varian bagian dari kunci akan melipatgandakan modul secara
/// kombinatorial: tiga sakelar menjadi delapan modul yang masing-masing harus
/// dikompilasi, disimpan, dan dibatalkan sendiri-sendiri — dan itu sebelum
/// varian keempat ditambahkan.
struct ShaderVariant {
    bool skinned = false;
    bool instanced = false;
    bool alphaTest = false;

    /// Nilai untuk `VkSpecializationInfo`, urutannya tetap dan menjadi
    /// `constant_id` 0..2. Urutan yang bergeser berarti sakelar yang tertukar
    /// tanpa satu pun pesan galat.
    std::array<uint32_t, 3> Constants() const;
    /// Ringkasan untuk log dan statistik.
    uint32_t Mask() const;
};

/// Permintaan kompilasi yang diteruskan ke kompilator.
struct CompileRequest {
    std::string source;
    ShaderStage stage = ShaderStage::Fragment;
    std::string entryPoint;
};

struct CompileOutput {
    bool ok = false;
    std::string error;
    /// SPIR-V sebagai kata 32-bit.
    std::vector<uint32_t> spirv;
};

/// Cache SPIR-V hasil kompilasi Slang, beralamat isi.
///
/// **Kuncinya isi, bukan nama material.** Dua material yang kebetulan
/// menghasilkan sumber yang sama persis berbagi satu entri, dan mengganti nama
/// sebuah material tidak membatalkan apa pun. Yang menentukan kunci: sumbernya,
/// tahap dan entry point-nya, serta **identitas kompilatornya**.
///
/// Identitas kompilator ikut karena tanpanya cache akan menyerahkan SPIR-V yang
/// dihasilkan `slangc` yang sudah tidak ada — dan bug itu muncul sebagai shader
/// yang jalan di satu mesin dan tidak di mesin lain, yaitu bentuk bug yang
/// paling mahal dicari.
///
/// **Entri diperiksa, bukan dipercaya.** Berkas yang terpotong — disk penuh,
/// proses dimatikan di tengah tulis — dibaca sebagai cache miss, bukan sebagai
/// SPIR-V yang sah. Penulisannya sendiri lewat berkas sementara lalu di-rename,
/// jadi keadaan setengah tertulis tidak pernah punya nama yang dicari siapa pun;
/// pemeriksaannya adalah sabuk pengaman untuk berkas yang datang dari versi lama
/// atau dari salinan yang rusak.
class ShaderCache {
public:
    /// Kompilator yang benar-benar dipanggil saat cache meleset. Bisa diganti —
    /// itu yang membuat cache-nya bisa diuji tanpa `slangc` terpasang.
    using Compiler = std::function<CompileOutput(const CompileRequest&)>;

    struct Stats {
        uint32_t hits = 0;
        uint32_t misses = 0;
        uint32_t compiles = 0;
        uint32_t rejected = 0;  ///< entri yang ada tapi tidak lolos pemeriksaan
    };

    /// `compilerIdentity` bebas bentuknya asal berubah saat kompilatornya
    /// berubah — versi `slangc` sudah cukup.
    ///
    /// `directory` kosong berarti tanpa lapisan disk: kompilasinya tetap jalan,
    /// hanya tidak ada yang tersimpan antar-jalan. Itu keadaan yang benar untuk
    /// proses headless yang tidak boleh menulis ke mana pun, dan lebih jujur
    /// daripada diam-diam menulis ke direktori kerja.
    void Configure(std::filesystem::path directory, std::string compilerIdentity);
    void SetCompiler(Compiler compiler);

    /// Kunci sebuah permintaan, dalam bentuk heksadesimal — sekaligus nama
    /// berkasnya.
    std::string KeyOf(const CompileRequest& request) const;

    /// Mengambil dari cache, atau mengompilasi lalu menyimpannya.
    CompileOutput Get(const CompileRequest& request);

    /// Membuang seluruh isi cache di disk.
    void Purge();

    const Stats& Statistics() const { return stats_; }
    void ResetStatistics() { stats_ = Stats{}; }

private:
    std::filesystem::path PathFor(const std::string& key) const;
    bool Load(const std::string& key, std::vector<uint32_t>& spirv);
    bool Store(const std::string& key, const std::vector<uint32_t>& spirv) const;

    std::filesystem::path directory_;
    std::string compilerIdentity_;
    Compiler compiler_;
    Stats stats_;
};

/// Apakah sebuah kata SPIR-V masuk akal sebagai modul.
///
/// Memeriksa angka ajaib dan panjang minimumnya. Bukan verifikasi penuh — itu
/// tugas validation layer — melainkan penjagaan supaya berkas yang terpotong
/// tidak sampai ke `vkCreateShaderModule`, tempat ia menjadi crash driver
/// alih-alih pesan yang bisa dibaca.
bool LooksLikeSpirv(const std::vector<uint32_t>& words);

/// Kompilator yang memanggil `slangc` sebagai proses.
///
/// `slangcPath` kosong berarti mencarinya di `VULKAN_SDK` lalu di `PATH`.
/// Mengembalikan kompilator yang selalu gagal dengan pesan yang jelas bila tidak
/// ditemukan — bukan yang diam-diam menghasilkan modul kosong.
ShaderCache::Compiler MakeSlangCompiler(std::filesystem::path slangcPath = {});

/// Versi `slangc` yang terpasang, untuk dipakai sebagai identitas kompilator.
/// Kosong bila tidak bisa dijalankan.
std::string SlangCompilerIdentity(const std::filesystem::path& slangcPath = {});

}  // namespace sim::material
