#pragma once

#include "Sim/Whitebox/WhiteboxMesh.h"

#include <filesystem>
#include <string>

/// Membaca dan menulis `.simwhitebox`.
///
/// **Yang disimpan adalah topologi, bukan segitiga hasilnya.** Yang tersimpan
/// harus yang bisa disunting lagi; segitiga adalah keluaran, dan menyimpannya
/// berarti blockout yang tidak bisa diubah sesudah disimpan — persis kebalikan
/// dari gunanya whitebox.
namespace sim::whitebox {

inline constexpr int kWhiteboxSchemaVersion = 1;

struct WhiteboxIoResult {
    bool ok = false;
    std::string error;
    int sourceVersion = kWhiteboxSchemaVersion;
};

/// Menulis whitebox menjadi teks.
///
/// **Keluarannya harus sama untuk masukan yang sama** — rusuk tersembunyi
/// diurutkan, dan material ditulis per face menurut urutan face. Berkas yang
/// isinya bergeser tanpa ada yang menyuntingnya menghasilkan diff palsu di
/// kontrol versi, dan diff palsu membuat yang sungguhan tidak terbaca.
std::string SaveToString(const WhiteboxMesh& box);

WhiteboxIoResult LoadFromString(WhiteboxMesh& box, const std::string& text);

WhiteboxIoResult SaveToFile(const WhiteboxMesh& box, const std::filesystem::path& path);
WhiteboxIoResult LoadFromFile(WhiteboxMesh& box, const std::filesystem::path& path);

}  // namespace sim::whitebox
