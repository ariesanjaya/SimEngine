#pragma once

#include "Sim/Assets/AssetTypes.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace sim::assets {

struct ImportResult {
    bool ok = false;
    std::string error;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t channels = 0;
    std::vector<Uuid> dependencies;
};

/// Satu importer per golongan berkas.
///
/// Dijalankan di thread latar, jadi implementasinya tidak boleh menyentuh apa
/// pun selain berkas yang diberikan — tidak ada World, tidak ada registry, tidak
/// ada ImGui.
class IImporter {
public:
    virtual ~IImporter() = default;
    virtual const char* Name() const = 0;
    virtual bool Handles(AssetType type) const = 0;
    virtual ImportResult Import(const std::filesystem::path& path) const = 0;
};

/// Pemetaan tipe aset ke importer-nya.
class ImporterRegistry {
public:
    static ImporterRegistry& Get();

    void Register(std::unique_ptr<IImporter> importer);
    const IImporter* Find(AssetType type) const;

    /// Mendaftarkan importer bawaan. Dipanggil sekali saat editor dimulai.
    static void RegisterBuiltIn();

private:
    std::vector<std::unique_ptr<IImporter>> importers_;
};

}  // namespace sim::assets
