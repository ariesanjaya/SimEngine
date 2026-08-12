#pragma once

#include "Sim/Scene/Project.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sim::editor {

/// Sebuah project yang pernah dibuka.
///
/// Namanya ikut disimpan, tidak dibaca ulang dari `project.simproj` tiap kali
/// daftar digambar: berkasnya bisa berada di drive jaringan yang sedang mati,
/// dan daftar project yang menggantung karena satu entri tidak bisa dibaca
/// adalah daftar yang justru tidak bisa dipakai untuk membuka yang lain.
struct RecentProject {
    std::string name;
    /// Folder project — yang memuat `project.simproj`.
    std::filesystem::path path;
    /// Detik Unix saat terakhir dibuka. Nol berarti belum pernah tercatat.
    int64_t lastOpened = 0;

    /// Benar bila `project.simproj`-nya masih ada. Entri yang hilang tetap
    /// ditampilkan — tapi ditandai, bukan disembunyikan diam-diam: project yang
    /// lenyap dari daftar tanpa penjelasan terbaca sebagai editor yang lupa.
    bool Exists() const;
};

/// Daftar project yang pernah dibuka, beserta pembuatan dan pembukaannya.
///
/// **Tanpa satu pun panggilan ImGui.** Yang di sini adalah keputusan tentang
/// berkas dan folder, dan keputusan itu harus bisa diuji tanpa jendela — panel
/// yang menggambarnya menjadi lapisan tipis di atasnya.
class ProjectLibrary {
public:
    /// Membaca daftar dari sebuah berkas JSON. Berkas yang tidak ada berarti
    /// daftar kosong, bukan galat: itu keadaan pemakaian pertama.
    void Load(const std::filesystem::path& file);
    bool Save(const std::filesystem::path& file) const;

    /// Terurut dari yang paling baru dibuka.
    const std::vector<RecentProject>& Recent() const { return recent_; }

    /// Mencatat sebuah project sebagai yang terakhir dibuka. Entri berjalur sama
    /// dipindahkan ke depan alih-alih digandakan.
    void Remember(const scene::Project& project, int64_t nowUnix);
    /// Membuang sebuah entri. Yang dibuang hanya catatannya — berkas project-nya
    /// tidak disentuh.
    bool Forget(const std::filesystem::path& path);

    /// Membuat project baru di `parent/<nama folder>` beserta folder isinya.
    ///
    /// Gagal bila foldernya sudah ada, dan itu disengaja: menimpa folder yang
    /// sudah berisi adalah satu-satunya cara alat seperti ini bisa menghapus
    /// pekerjaan orang.
    bool Create(const std::filesystem::path& parent, std::string_view name,
                scene::Project& out, std::string& error) const;

    /// Membuka `project.simproj`. `path` boleh menunjuk berkasnya atau foldernya.
    bool Open(const std::filesystem::path& path, scene::Project& out, std::string& error) const;

    /// Nama folder yang sah dari sebuah nama project.
    ///
    /// Yang dibuang adalah pemisah jalur dan aksara yang ditolak sistem berkas
    /// Windows — bukan hanya yang ditolak Linux. Project dibuat di satu mesin dan
    /// dibuka di mesin lain, dan folder bernama `Level: 2` adalah folder yang
    /// tidak bisa di-checkout di sana sama sekali.
    static std::string SanitizeFolderName(std::string_view name);

    /// Berapa entri yang disimpan. Selebihnya dibuang saat menyimpan — daftar
    /// "terakhir dibuka" yang memuat ratusan entri bukan daftar lagi.
    static constexpr std::size_t kMaxRecent = 16;

private:
    std::vector<RecentProject> recent_;
};

}  // namespace sim::editor
