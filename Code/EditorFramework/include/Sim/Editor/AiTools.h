#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace sim::ai {
class ResourceRegistry;
class ToolRegistry;
}

namespace sim::editor {

class EditorApp;

/// Mendaftarkan tool MCP yang dilayani editor.
///
/// **Arahnya satu, dan itu disengaja.** EditorFramework melihat AIBridge;
/// AIBridge tidak tahu apa-apa tentang editor. Itulah yang membuat modul yang
/// sama nanti bisa dipakai `SimHeadless`, yang mendaftarkan himpunan tool yang
/// berbeda karena tidak punya panel maupun viewport.
///
/// `app` harus hidup lebih lama daripada `tools`: handler-nya memegangnya lewat
/// referensi, karena tool yang menyalin keadaan editor akan menjawab agen dengan
/// keadaan pada saat pendaftaran, bukan pada saat ditanya.

/// Menangkap isi jendela editor sebagai PNG.
///
/// **Sebuah callback, dan itulah yang menjaga batas modulnya.** Tangkapan layar
/// hanya bisa diambil oleh yang memegang swapchain, yaitu yang boleh melihat
/// Vulkan — dan EditorFramework tidak boleh. Composition root adalah satu-satunya
/// tempat yang melihat keduanya, jadi ia yang menyediakannya.
///
/// Mengembalikan false dan mengisi `error` bila gagal. Kosong berarti editor ini
/// tidak bisa menangkap layar sama sekali, dan tool-nya tidak didaftarkan —
/// lebih baik daripada tool yang ada tapi selalu gagal.
/// Sepetak piksel di dalam jendela. Nullptr berarti seluruh jendela.
struct CaptureRect {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

/// `crop` nullptr berarti seluruh jendela. Rect yang keluar batas gambar
/// **dijepit**, bukan ditolak: yang meminta memakai satuan logis dan
/// menerjemahkannya sendiri, jadi selisih satu piksel di tepi adalah pembulatan,
/// bukan kesalahan yang layak menggagalkan tangkapan.
using ScreenshotFn =
    std::function<bool(const CaptureRect* crop, std::vector<uint8_t>& pngBytes,
                       std::string& error)>;

void RegisterEditorTools(ai::ToolRegistry& tools, ai::ResourceRegistry& resources,
                         EditorApp& app, ScreenshotFn screenshot = {});

/// Tool yang membaca dan menyusun scene (track AI, A1).
///
/// Terpisah dari `RegisterEditorTools` karena keduanya menjawab pertanyaan yang
/// berbeda: yang satu tentang editor sebagai program, yang lain tentang isi
/// project. `SimHeadless` nanti memakai yang kedua tanpa yang pertama — ia tidak
/// punya menu, panel, maupun jendela untuk ditangkap.
void RegisterSceneTools(ai::ToolRegistry& tools, ai::ResourceRegistry& resources,
                        EditorApp& app);

/// Tool yang mengubah entity (track AI, A1).
///
/// Semuanya lewat `CommandHistory`, dan setiap panggilan dibungkus satu transaksi
/// bernama `AI: <tool>` — satu tool call jadi tepat satu entri undo, dan panel
/// History menyebutkan asalnya. Itu yang membuat "batalkan semua yang dilakukan
/// agen" bisa ditulis nanti tanpa mesin baru.
void RegisterEntityTools(ai::ToolRegistry& tools, EditorApp& app);

/// Tool aset dan project (track AI, A2).
void RegisterAssetTools(ai::ToolRegistry& tools, EditorApp& app);

/// Tool authoring: Lua dan graph material (track AI, A3).
///
/// Terpisah dari `RegisterAssetTools` karena keduanya menjawab pertanyaan yang
/// berbeda — yang satu tentang berkas sebagai berkas, yang lain tentang isinya
/// sebagai karya. Tool di sini memeriksa isi itu sebelum menuliskannya: sintaks
/// Lua sebelum berkas skrip tersimpan, validasi graph sebelum material tersimpan.
void RegisterAuthoringTools(ai::ToolRegistry& tools, EditorApp& app);

/// Menyelesaikan sebuah jalur relatif terhadap akar project, menolak apa pun
/// yang keluar darinya. Mengembalikan pesan galat, atau string kosong.
///
/// **Terbuka karena dipakai bersama, dan karena aturan ini layak diuji
/// langsung.** Inilah yang berdiri di antara agen dan seluruh berkas di mesin
/// ini. Tool aset dan tool authoring keduanya memanggilnya — menyalinnya ke
/// berkas kedua adalah cara paling pasti membuat keduanya berbeda tanpa ada yang
/// menyadarinya. Dan uji yang harus menempuh dua lapisan untuk sampai ke aturan
/// yang diujinya adalah uji yang akan ditulis setengah.
std::string ResolveProjectPath(const std::filesystem::path& root, const std::string& relative,
                               std::filesystem::path& absolute);

}  // namespace sim::editor
