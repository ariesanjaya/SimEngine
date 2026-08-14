#include "Sim/Editor/TextureSettingsCommand.h"

#include "Sim/Core/Log.h"

#include <utility>

namespace sim::editor {

SetTextureSettingsCommand::SetTextureSettingsCommand(std::filesystem::path texture,
                                                     assets::TextureSettings before,
                                                     assets::TextureSettings after)
    : texture_(std::move(texture)), before_(before), after_(after) {}

void SetTextureSettingsCommand::Apply(const assets::TextureSettings& settings) {
    if (!assets::SaveTextureSettings(settings, texture_)) {
        // Terdengar keras, dan memang harus: undo yang diam-diam tidak menulis
        // apa pun meninggalkan panel menampilkan satu hal dan disk memuat hal
        // lain — dan yang berikutnya menyimpan akan menimpanya lagi.
        SIM_ERROR("Assets", "cannot write texture settings for {}", texture_.string());
    }
}

void SetTextureSettingsCommand::Do() { Apply(after_); }
void SetTextureSettingsCommand::Undo() { Apply(before_); }

bool SetTextureSettingsCommand::MergeWith(const ICommand& next) {
    const auto* other = dynamic_cast<const SetTextureSettingsCommand*>(&next);
    if (other == nullptr || other->texture_ != texture_) {
        return false;
    }
    // Keadaan "sebelum" milik yang pertama dipertahankan, dan "sesudah" diambil
    // dari yang terakhir: satu gerakan menjadi satu langkah dari awal ke akhir.
    after_ = other->after_;
    return true;
}

}  // namespace sim::editor
