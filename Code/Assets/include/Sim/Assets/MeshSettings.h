#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sim::assets {

/// Penandaan per ruas mesh yang tidak bisa tinggal di dalam berkas mesh-nya.
///
/// **Dikunci nama material, bukan nomor ruas.** Nomor ruas bergeser begitu
/// seseorang mengekspor ulang model dengan material tambahan, dan penandaan yang
/// bergeser diam-diam memindahkan sifat "kain" ke bagian tubuh yang salah. Nama
/// material bertahan pada ekspor ulang, dan ia juga yang dilihat orang di DCC-nya.
struct MeshPartSettings {
    std::string material;
    /// Ruas ini kain: yang menggerakkannya kelak simulasi, bukan kulit rangka.
    ///
    /// **Baru penandaannya, belum ada yang menjalankannya.** Simulasi kain
    /// menunggu physics di E9; yang ada sekarang adalah tempat menyimpan
    /// keputusannya, supaya rig yang sudah disiapkan tidak perlu ditandai ulang
    /// nanti.
    bool cloth = false;
};

/// Pengaturan sebuah aset mesh, disimpan di sebelah berkasnya.
///
/// **Berkas tersendiri, bukan di dalam `.meta`.** `.meta` memuat identitas aset
/// dan tidak memuat apa pun selain itu; ia ditulis sekali saat aset ditemukan
/// dan tidak pernah disentuh lagi. Menumpangkan pengaturan yang berubah-ubah di
/// sana berarti setiap penyimpanan menulis ulang berkas yang memegang satu-
/// satunya hal yang tidak boleh hilang.
struct MeshSettings {
    std::vector<MeshPartSettings> parts;

    /// Benar bila ruas bermaterial itu ditandai kain. Nama yang tidak tercatat
    /// berarti bukan kain — bawaan yang benar untuk mesh yang belum disentuh.
    bool ClothFor(std::string_view material) const;
    /// Menandai sebuah material. Entri yang belum ada dibuat.
    void SetCloth(std::string_view material, bool cloth);
    /// Membuang entri yang tidak menandai apa pun, supaya berkasnya tidak tumbuh
    /// oleh baris yang seluruhnya bernilai bawaan.
    void Prune();
    bool Empty() const { return parts.empty(); }
};

/// Jalur berkas pengaturan untuk sebuah berkas mesh.
std::filesystem::path MeshSettingsPath(const std::filesystem::path& meshPath);

/// Memuat pengaturan. Berkas yang belum ada berarti pengaturan kosong, bukan
/// galat — itu keadaan setiap mesh yang baru diimpor.
bool LoadMeshSettings(MeshSettings& settings, const std::filesystem::path& meshPath);

/// Menyimpan pengaturan. Yang seluruhnya bernilai bawaan **menghapus** berkasnya
/// alih-alih menulis berkas kosong: berkas pengaturan yang tidak mengatur apa pun
/// hanya menambah satu berkas yang harus ikut kontrol versi.
bool SaveMeshSettings(const MeshSettings& settings, const std::filesystem::path& meshPath);

}  // namespace sim::assets
