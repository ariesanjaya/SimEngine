#include "Sim/Platform/FileDialog.h"

#include "Sim/Core/Log.h"
#include "Sim/Core/MainThreadQueue.h"
#include "Sim/Platform/Window.h"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>

#include <memory>
#include <string>
#include <utility>

namespace sim::platform {
namespace {

const Window* g_parent = nullptr;

/// Yang dititipkan ke SDL sebagai `userdata`.
///
/// Dialokasi di heap dan dihapus di dalam callback-nya. SDL menjamin callback
/// dipanggil tepat sekali — termasuk saat gagal maupun dibatalkan — jadi ini
/// bukan kebocoran yang menunggu terjadi melainkan kepemilikan yang berpindah
/// ke SDL selama dialognya terbuka.
struct PendingDialog {
    FileDialogCallback done;
};

void SDLCALL OnChosen(void* userdata, const char* const* filelist, int /*filter*/) {
    std::unique_ptr<PendingDialog> pending(static_cast<PendingDialog*>(userdata));
    if (pending == nullptr || !pending->done) {
        return;
    }

    FileDialogResult result;
    if (filelist == nullptr) {
        const char* message = SDL_GetError();
        result.error = message != nullptr && message[0] != '\0' ? message
                                                                : "the system dialog failed";
    } else {
        // **Disalin di sini, bukan diteruskan apa adanya.** `filelist` dibebaskan
        // SDL begitu callback ini kembali, sedangkan hasilnya baru dipakai satu
        // frame kemudian di main thread — meneruskan pointernya berarti membaca
        // memori yang sudah dilepas.
        for (const char* const* it = filelist; *it != nullptr; ++it) {
            result.paths.emplace_back(*it);
        }
    }

    // **Selalu diserahkan lewat antrian main thread.** SDL boleh memanggil balik
    // dari thread mana pun, dan yang menunggu hasilnya adalah panel editor —
    // yang state-nya tidak pernah dikunci karena memang hanya disentuh satu
    // thread. Menjalankannya di tempat berarti perusakan yang muncul sebagai
    // panel yang sesekali salah menggambar, bukan sebagai galat.
    MainThreadQueue::Get().Submit(
        [done = std::move(pending->done), result = std::move(result)]() { done(result); });
}

/// `default_location` SDL menerima null, bukan string kosong.
const char* OrNull(std::string_view text, std::string& storage) {
    if (text.empty()) {
        return nullptr;
    }
    storage.assign(text);
    return storage.c_str();
}

SDL_Window* ParentHandle() {
    return g_parent != nullptr ? g_parent->Handle() : nullptr;
}

}  // namespace

void SetDialogParentWindow(const Window* window) {
    g_parent = window;
}

void ShowOpenFolderDialog(std::string_view defaultLocation, FileDialogCallback done) {
    auto pending = std::make_unique<PendingDialog>();
    pending->done = std::move(done);
    std::string location;
    SDL_ShowOpenFolderDialog(&OnChosen, pending.release(), ParentHandle(),
                             OrNull(defaultLocation, location), /*allow_many=*/false);
}

void ShowOpenFileDialog(std::span<const FileDialogFilter> filters,
                        std::string_view defaultLocation, FileDialogCallback done) {
    auto pending = std::make_unique<PendingDialog>();
    pending->done = std::move(done);

    // Penyaringnya disalin ke bentuk SDL lebih dulu, dan penyimpanannya hidup
    // sampai panggilan selesai: SDL membaca isinya selama `SDL_Show...`
    // berjalan, bukan sesudahnya.
    std::vector<SDL_DialogFileFilter> converted;
    converted.reserve(filters.size());
    for (const FileDialogFilter& filter : filters) {
        converted.push_back(SDL_DialogFileFilter{filter.name.c_str(), filter.pattern.c_str()});
    }

    std::string location;
    SDL_ShowOpenFileDialog(&OnChosen, pending.release(), ParentHandle(),
                           converted.empty() ? nullptr : converted.data(),
                           static_cast<int>(converted.size()),
                           OrNull(defaultLocation, location), /*allow_many=*/false);
}

}  // namespace sim::platform
