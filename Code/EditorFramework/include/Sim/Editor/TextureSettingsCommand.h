#pragma once

#include "Sim/Assets/TextureSettings.h"
#include "Sim/Editor/Command.h"

#include <filesystem>
#include <string>

namespace sim::editor {

/// Mengubah pengaturan impor sebuah tekstur, lewat riwayat undo yang sama
/// dengan suntingan scene.
///
/// **Lewat riwayat, tidak seperti dokumen terrain.** Terrain adalah dokumen yang
/// dibuka dan ditutup, dan `CommandHistory` tidak punya cakupan dokumen — jadi
/// membuka terrain lain akan meninggalkan command yang membatalkan goresan pada
/// heightmap yang berbeda. Pengaturan tekstur tidak punya masalah itu: perintah
/// ini memegang **jalur berkasnya**, jadi ia berlaku pada aset yang sama
/// betapapun jauh pengguna sudah berpindah pilihan.
class SetTextureSettingsCommand final : public ICommand {
public:
    SetTextureSettingsCommand(std::filesystem::path texture, assets::TextureSettings before,
                              assets::TextureSettings after);

    void Do() override;
    void Undo() override;
    std::string Name() const override { return "Texture Settings"; }
    /// Menyerap perubahan berikutnya pada tekstur yang sama — satu geseran
    /// slider adalah satu langkah undo, bukan puluhan.
    bool MergeWith(const ICommand& next) override;

private:
    void Apply(const assets::TextureSettings& settings);

    std::filesystem::path texture_;
    assets::TextureSettings before_;
    assets::TextureSettings after_;
};

}  // namespace sim::editor
