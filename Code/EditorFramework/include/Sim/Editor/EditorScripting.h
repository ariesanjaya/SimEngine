#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace sim::script {
class ScriptRuntime;
}

namespace sim::editor {

struct EditorContext;
class PanelManager;

/// Menjadikan editor sendiri bisa diperluas dari Lua.
///
/// **Kenapa di EditorFramework dan bukan di Script.** Modul `Script` tidak boleh
/// melihat ImGui — sama seperti ia tidak melihat Vulkan — supaya ia tetap bisa
/// dipakai runtime tanpa editor. Yang boleh melihat keduanya hanya modul ini,
/// jadi di sinilah `sim.editor` dan `sim.ui` didaftarkan ke state Lua yang sudah
/// berjalan.
///
/// **Apa yang bisa dilakukan skrip editor.**
///
/// ```lua
/// sim.editor.menu("Reset selection", function() ... end)
///
/// sim.editor.panel("Notes", function()
///     sim.ui.text("halo")
///     if sim.ui.button("Kerjakan") then ... end
/// end)
///
/// sim.editor.command("Rename", function() ... end, function() ... end)
///
/// for _, asset in ipairs(sim.editor.assets("Texture")) do
///     sim.editor.rename_asset(asset.path, "tex_" .. asset.name)
/// end
/// ```
///
/// `sim.editor.command` menjalankan perubahannya lewat CommandHistory yang sama
/// dengan yang dipakai panel. Itu bukan kemudahan tambahan melainkan syarat:
/// satu jalur tulis yang lolos dari undo sudah cukup membuat Ctrl+Z tidak bisa
/// dipercaya, dan pengguna tidak punya cara tahu perubahan mana yang aman.
///
/// Pengecualiannya operasi berkas — `rename_asset` dan `move_asset` — yang
/// menempuh jalur langsung, persis seperti Asset Browser. Riwayat undo di sini
/// hanya menjanjikan pembatalan yang tidak bisa ditepatinya begitu berkasnya
/// disentuh dari luar editor.
///
/// `sim.ui.*` hanya sah di dalam callback panel. Di luar itu ia melempar
/// kesalahan Lua alih-alih menggambar ke jendela sembarang — widget yang muncul
/// di tempat yang tidak diduga jauh lebih sulit dilacak daripada pesan error.
class EditorScripting {
public:
    EditorScripting();
    ~EditorScripting();

    EditorScripting(const EditorScripting&) = delete;
    EditorScripting& operator=(const EditorScripting&) = delete;

    /// Mendaftarkan `sim.editor` dan `sim.ui`, lalu menjalankan seluruh `.lua`
    /// di `folder`. Aman dipanggil dengan runtime null: editor yang dibangun
    /// tanpa Lua cukup tidak punya menu Scripts.
    void Initialize(script::ScriptRuntime* runtime, EditorContext* context, PanelManager* panels,
                    std::filesystem::path folder);
    void Shutdown();

    /// Memasang panel yang didaftarkan Lua sejak frame lalu.
    ///
    /// Terpisah dari pendaftarannya dengan sengaja: `sim.editor.panel` bisa
    /// dipanggil dari dalam callback menu — yang berjalan di tengah PanelManager
    /// menggambar — dan menambah panel saat itu juga akan mengubah daftar yang
    /// sedang ditelusuri.
    void FlushPending();

    /// Isi menu Scripts. Dipanggil dari dalam BeginMenu().
    void DrawMenu();

    /// Label item menu yang terdaftar, dalam urutan tampilnya.
    std::vector<std::string> MenuLabels() const;

    /// Membuang seluruh registrasi lalu menjalankan ulang berkasnya.
    ///
    /// Menjalankan ulang saja tidak cukup: sebuah item menu yang dihapus dari
    /// berkasnya akan tetap tinggal di menu, dan pengguna tidak punya cara
    /// membedakannya dari item yang masih ada. Panel yang judulnya tetap sama
    /// dipertahankan beserta posisi dock-nya — yang diganti hanya fungsi
    /// gambarnya.
    void ReloadAll();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sim::editor
