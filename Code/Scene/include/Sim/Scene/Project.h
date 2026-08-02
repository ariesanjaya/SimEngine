#pragma once

#include "Sim/Scene/World.h"

#include <filesystem>
#include <string>

namespace sim::scene {

/// Isi berkas `project.simproj`.
///
/// Project menyimpan lokasi, bukan data: aset dan level tetap berkas terpisah.
/// Dengan begitu satu project bisa dibuka dua orang tanpa berebut satu berkas
/// besar, dan diff-nya tetap terbaca.
struct Project {
    std::string name = "Untitled";
    /// Relatif terhadap folder project.
    std::string assetsPath = "Assets";
    std::string startupLevel;

    /// Folder tempat project.simproj berada. Tidak ikut ditulis ke berkas.
    std::filesystem::path root;

    std::filesystem::path AssetsDirectory() const { return root / assetsPath; }
    std::filesystem::path StartupLevelPath() const {
        return startupLevel.empty() ? std::filesystem::path{} : root / startupLevel;
    }
};

/// Membuka `project.simproj`. `path` boleh menunjuk berkasnya langsung atau
/// folder yang memuatnya.
bool LoadProject(Project& project, const std::filesystem::path& path, std::string& error);
bool SaveProject(const Project& project, const std::filesystem::path& path);

// --- prefab v0 --------------------------------------------------------------

/// Menyimpan sebuah entity beserta keturunannya sebagai `.simprefab`.
///
/// Formatnya sama persis dengan level: sebuah prefab memang hanya potongan
/// level. Menyamakan keduanya berarti satu jalur kode, satu jalur migrasi, dan
/// satu tempat yang bisa salah.
bool SavePrefab(const World& world, Entity root, const std::filesystem::path& path);

/// Menempatkan salinan prefab ke dalam dunia di bawah `parent`.
///
/// GUID dibuat baru untuk setiap entity — dua salinan prefab yang sama di satu
/// level harus bisa dibedakan, dan GUID adalah satu-satunya pembeda yang
/// bertahan lintas-sesi.
Entity InstantiatePrefab(World& world, const std::filesystem::path& path,
                         Entity parent = kNullEntity);

}  // namespace sim::scene
