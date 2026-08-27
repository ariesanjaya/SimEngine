#pragma once

#include "Sim/Reflect/TypeRegistry.h"
#include "Sim/Scene/World.h"

#include <filesystem>
#include <string>
#include <vector>

namespace sim::scene {

/// Versi skema berkas `.simlevel` yang ditulis sekarang.
///
/// Riwayat:
///   1 — rotasi disimpan sebagai sudut Euler derajat [x, y, z]
///   2 — rotasi disimpan sebagai quaternion [w, x, y, z]
///   3 — rujukan mesh/material berupa GUID aset, bukan nama berkas
///   4 — blok `"world"`: bagaimana level disinari (docs/PLAN-IBL.md B0)
///
/// Naikkan setiap kali bentuk data berubah, dan tambahkan langkahnya di
/// Migrate(). Berkas lama harus tetap bisa dibuka: level adalah hasil kerja
/// pengguna, bukan artefak build.
///
/// **Versi 4 tidak punya langkah migrasi, dan itu bukan kelalaian.** Ia hanya
/// menambah satu blok baru; tidak ada satu pun byte versi 3 yang berubah
/// artinya. Berkas tanpa blok itu berarti bawaan `WorldSettings` — pola
/// `root.value(...)` yang sudah dipakai `LoadProject` — dan bawaan itu dipilih
/// supaya sama persis dengan perilaku sebelum blok ini ada. Versinya tetap naik
/// karena berkas yang **ditulis** sekarang memuat sesuatu yang tidak dipahami
/// editor lama, dan itulah yang dijawab pemeriksaan "ditulis editor yang lebih
/// baru" di bawah.
inline constexpr int kLevelSchemaVersion = 4;

struct LevelIoResult {
    bool ok = false;
    std::string error;
    /// Versi skema berkas sebelum migrasi. Sama dengan kLevelSchemaVersion bila
    /// tidak ada migrasi yang berjalan.
    int sourceVersion = kLevelSchemaVersion;
    bool migrated = false;
    std::size_t entityCount = 0;
};

/// Menulis seluruh dunia ke teks JSON.
///
/// Keluarannya deterministik: entity ditulis menurut penelusuran hierarki dari
/// akar, dan komponen menurut urutan pendaftarannya. Dua kali menyimpan dunia
/// yang sama menghasilkan byte yang sama persis — itu yang membuat berkas level
/// ramah diff dan ramah git.
std::string SaveLevelToString(const World& world);
LevelIoResult SaveLevelToFile(const World& world, const std::filesystem::path& path);

/// Mengganti isi `world` dengan isi berkas. Menjalankan migrasi bila perlu.
LevelIoResult LoadLevelFromString(World& world, const std::string& text);
LevelIoResult LoadLevelFromFile(World& world, const std::filesystem::path& path);

/// Menyimpan satu entity beserta seluruh keturunannya sebagai teks level.
///
/// Dipakai undo penghapusan dan prefab: keduanya butuh membangun ulang sub-pohon
/// persis seperti semula, lengkap dengan GUID-nya, bukan salinan yang mirip.
std::string SaveSubtreeToString(const World& world, Entity root);

/// Membangun ulang sub-pohon hasil SaveSubtreeToString di bawah induk tertentu.
/// `parentGuid` tidak valid berarti dipasang sebagai akar.
bool RestoreSubtree(World& world, const std::string& text, Uuid parentGuid);

/// Mengganti seluruh GUID di dalam teks sub-pohon dengan yang baru.
///
/// Dipakai duplikasi dan instansiasi prefab: keduanya butuh salinan yang bisa
/// hidup berdampingan dengan aslinya, dan GUID adalah satu-satunya pembeda yang
/// bertahan lintas-sesi. Penggantiannya konsisten dalam satu teks sehingga
/// hubungan induk-anak di dalamnya tetap utuh; rujukan induk yang menunjuk ke
/// luar teks dibuang, karena penempatan akarnya ditentukan pemanggil.
///
/// Mengembalikan teks kosong bila `text` bukan sub-pohon yang sah.
std::string RemapGuids(const std::string& text, std::string* outRootGuid = nullptr);

/// Menyalin satu komponen ke/dari teks JSON.
///
/// Dipakai undo generik di editor: menyimpan cuplikan sebelum dan sesudah
/// sebuah suntingan tanpa perlu satu pun command khusus per tipe komponen.
/// Menyalin byte mentah tidak bisa dipakai karena komponen memuat std::string
/// dan std::vector.
std::string SerializeComponent(const reflect::TypeDesc& type, const void* component);
void DeserializeComponent(const reflect::TypeDesc& type, void* component,
                          const std::string& text);

/// Nama field yang berbeda antara dua cuplikan komponen.
///
/// Dipakai penyuntingan banyak objek sekaligus: Inspector perlu tahu field mana
/// yang baru saja diubah pengguna agar hanya field itu yang diteruskan ke objek
/// lain — menyalin seluruh komponen akan menyeragamkan nilai yang tidak pernah
/// disentuh.
std::vector<std::string> DiffComponentFields(const std::string& first, const std::string& second);

/// Menyalin sebagian field dari `source` ke atas `target`.
std::string MergeComponentFields(const std::string& target, const std::string& source,
                                 const std::vector<std::string>& fields);

/// True bila `text` masuk akal sebagai cuplikan komponen bertipe `type`.
///
/// Dipakai menu Paste untuk menonaktifkan dirinya ketika papan klip berisi teks
/// lain. Mencoba lalu gagal diam-diam lebih membingungkan daripada menu yang
/// jujur terlihat tidak tersedia.
bool IsComponentSnapshot(const reflect::TypeDesc& type, const std::string& text);

}  // namespace sim::scene
