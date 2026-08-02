#pragma once

#include <imgui.h>

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sim::editor {

/// Satu perintah editor yang bisa dipanggil dari menu, toolbar, pintasan
/// papan ketik, atau (nanti di track A) dari agen lewat MCP.
///
/// Semua perintah melewati registry ini, bukan dipanggil langsung dari tempat
/// tombolnya digambar. Itu yang membuat satu perintah bisa punya banyak pemicu
/// tanpa logikanya diduplikasi, dan yang membuat `editor.execute_action` di
/// PLAN-AI.md cukup satu tool untuk semua menu.
struct Action {
    std::string id;        ///< stabil, dipakai config & MCP: "edit.undo"
    std::string label;     ///< yang dilihat pengguna: "Undo"
    std::string category;  ///< pengelompokan di Preferences: "Edit"
    std::string icon;      ///< opsional, dari Sim/Editor/Icons.h
    ImGuiKeyChord defaultShortcut = ImGuiKey_None;
    std::function<void()> execute;
    /// Opsional. Aksi yang tidak aktif tampil kelabu dan pintasannya diabaikan.
    std::function<bool()> enabled;
};

class ActionRegistry {
public:
    void Register(Action action);

    const Action* Find(std::string_view id) const;
    bool IsEnabled(std::string_view id) const;
    /// Menjalankan aksi bila ada dan aktif. Mengembalikan false bila tidak.
    bool Invoke(std::string_view id);

    ImGuiKeyChord Shortcut(std::string_view id) const;
    void SetShortcut(std::string_view id, ImGuiKeyChord chord);
    void ResetShortcut(std::string_view id);
    std::string ShortcutText(std::string_view id) const;
    /// Id aksi lain yang memakai chord ini, atau kosong. Dipakai Preferences
    /// untuk memperingatkan bentrokan sebelum penggantian diterapkan.
    std::string FindConflict(ImGuiKeyChord chord, std::string_view exceptId) const;

    /// Memeriksa seluruh pintasan sekali per frame. Dipanggil sebelum panel
    /// menggambar, supaya aksi yang mengubah UI sudah berlaku pada frame ini.
    void ProcessShortcuts();

    /// Menggambar item menu untuk sebuah aksi, lengkap dengan ikon, label,
    /// teks pintasan, dan status aktif.
    bool MenuItem(std::string_view id);

    bool Save(const std::filesystem::path& path) const;
    bool Load(const std::filesystem::path& path);

    const std::vector<Action>& All() const { return actions_; }

    /// Teks pintasan yang stabil untuk disimpan ke config, mis. "Ctrl+Shift+S".
    ///
    /// Sengaja tidak memakai ImGui::GetKeyChordName(): dokumentasinya sendiri
    /// menyebut nama tersebut hanya untuk debug dan tidak dijamin stabil
    /// antar-versi, sedangkan berkas config harus tetap terbaca setelah ImGui
    /// diperbarui.
    static std::string ChordToString(ImGuiKeyChord chord);
    static ImGuiKeyChord ChordFromString(std::string_view text);

private:
    Action* FindMutable(std::string_view id);

    std::vector<Action> actions_;
    std::unordered_map<std::string, std::size_t> index_;
    std::unordered_map<std::string, ImGuiKeyChord> overrides_;
};

}  // namespace sim::editor
