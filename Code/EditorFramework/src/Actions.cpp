#include "Sim/Editor/Actions.h"

#include "Sim/Core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>

namespace sim::editor {
namespace {

struct NamedKey {
    const char* name;
    ImGuiKey key;
};

/// Tabel nama tombol milik kita sendiri.
///
/// Hanya tombol yang masuk akal sebagai pintasan editor. Sengaja tidak
/// mencakup seluruh ImGuiKey: nama yang tidak pernah dipakai hanya menambah
/// permukaan yang harus dijaga stabil selamanya.
constexpr std::array<NamedKey, 38> kNamedKeys{{
    {"Tab", ImGuiKey_Tab},
    {"Left", ImGuiKey_LeftArrow},
    {"Right", ImGuiKey_RightArrow},
    {"Up", ImGuiKey_UpArrow},
    {"Down", ImGuiKey_DownArrow},
    {"PageUp", ImGuiKey_PageUp},
    {"PageDown", ImGuiKey_PageDown},
    {"Home", ImGuiKey_Home},
    {"End", ImGuiKey_End},
    {"Insert", ImGuiKey_Insert},
    {"Delete", ImGuiKey_Delete},
    {"Backspace", ImGuiKey_Backspace},
    {"Space", ImGuiKey_Space},
    {"Enter", ImGuiKey_Enter},
    {"Escape", ImGuiKey_Escape},
    {"Comma", ImGuiKey_Comma},
    {"Minus", ImGuiKey_Minus},
    {"Period", ImGuiKey_Period},
    {"Slash", ImGuiKey_Slash},
    {"Semicolon", ImGuiKey_Semicolon},
    {"Equal", ImGuiKey_Equal},
    {"LeftBracket", ImGuiKey_LeftBracket},
    {"Backslash", ImGuiKey_Backslash},
    {"RightBracket", ImGuiKey_RightBracket},
    {"Apostrophe", ImGuiKey_Apostrophe},
    {"GraveAccent", ImGuiKey_GraveAccent},
    {"F1", ImGuiKey_F1},
    {"F2", ImGuiKey_F2},
    {"F3", ImGuiKey_F3},
    {"F4", ImGuiKey_F4},
    {"F5", ImGuiKey_F5},
    {"F6", ImGuiKey_F6},
    // F7..F10 sempat bolong di sini, dan bolongnya tidak terlihat sampai ada
    // aksi yang memakainya: tabel ini yang menerjemahkan pintasan ke teks dan
    // sebaliknya, jadi tombol yang tidak ada namanya tetap berfungsi tapi tidak
    // bisa ditulis ke `shortcuts.json` — pengguna yang mengubah pintasannya lalu
    // kehilangan yang ini tanpa satu pun pesan.
    {"F7", ImGuiKey_F7},
    {"F8", ImGuiKey_F8},
    {"F9", ImGuiKey_F9},
    {"F10", ImGuiKey_F10},
    {"F11", ImGuiKey_F11},
    {"F12", ImGuiKey_F12},
}};

std::string KeyToString(ImGuiKey key) {
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z) {
        return std::string(1, static_cast<char>('A' + (key - ImGuiKey_A)));
    }
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9) {
        return std::string(1, static_cast<char>('0' + (key - ImGuiKey_0)));
    }
    for (const NamedKey& named : kNamedKeys) {
        if (named.key == key) {
            return named.name;
        }
    }
    return {};
}

ImGuiKey KeyFromString(std::string_view text) {
    if (text.size() == 1) {
        const char c = text[0];
        if (c >= 'A' && c <= 'Z') {
            return static_cast<ImGuiKey>(ImGuiKey_A + (c - 'A'));
        }
        if (c >= 'a' && c <= 'z') {
            return static_cast<ImGuiKey>(ImGuiKey_A + (c - 'a'));
        }
        if (c >= '0' && c <= '9') {
            return static_cast<ImGuiKey>(ImGuiKey_0 + (c - '0'));
        }
    }
    for (const NamedKey& named : kNamedKeys) {
        if (text == named.name) {
            return named.key;
        }
    }
    return ImGuiKey_None;
}

}  // namespace

std::string ActionRegistry::ChordToString(ImGuiKeyChord chord) {
    const auto key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
    const std::string keyName = KeyToString(key);
    if (keyName.empty()) {
        return {};
    }

    std::string result;
    // Urutan modifier dikunci supaya teks yang sama selalu dihasilkan untuk
    // chord yang sama — kalau tidak, config akan berubah tanpa sebab.
    if ((chord & ImGuiMod_Ctrl) != 0) {
        result += "Ctrl+";
    }
    if ((chord & ImGuiMod_Shift) != 0) {
        result += "Shift+";
    }
    if ((chord & ImGuiMod_Alt) != 0) {
        result += "Alt+";
    }
    if ((chord & ImGuiMod_Super) != 0) {
        result += "Super+";
    }
    return result + keyName;
}

ImGuiKeyChord ActionRegistry::ChordFromString(std::string_view text) {
    ImGuiKeyChord chord = 0;
    std::size_t start = 0;
    while (true) {
        const std::size_t plus = text.find('+', start);
        const std::string_view token = text.substr(start, plus - start);
        if (plus == std::string_view::npos) {
            const ImGuiKey key = KeyFromString(token);
            if (key == ImGuiKey_None) {
                return ImGuiKey_None;
            }
            return chord | key;
        }
        if (token == "Ctrl") {
            chord |= ImGuiMod_Ctrl;
        } else if (token == "Shift") {
            chord |= ImGuiMod_Shift;
        } else if (token == "Alt") {
            chord |= ImGuiMod_Alt;
        } else if (token == "Super") {
            chord |= ImGuiMod_Super;
        } else {
            return ImGuiKey_None;
        }
        start = plus + 1;
    }
}

void ActionRegistry::Register(Action action) {
    if (index_.find(action.id) != index_.end()) {
        SIM_WARN("Editor", "Action id '{}' registered twice", action.id);
        return;
    }
    index_.emplace(action.id, actions_.size());
    actions_.push_back(std::move(action));
}

const Action* ActionRegistry::Find(std::string_view id) const {
    const auto it = index_.find(std::string(id));
    return it == index_.end() ? nullptr : &actions_[it->second];
}

Action* ActionRegistry::FindMutable(std::string_view id) {
    const auto it = index_.find(std::string(id));
    return it == index_.end() ? nullptr : &actions_[it->second];
}

bool ActionRegistry::IsEnabled(std::string_view id) const {
    const Action* action = Find(id);
    if (action == nullptr || !action->execute) {
        return false;
    }
    return !action->enabled || action->enabled();
}

bool ActionRegistry::Invoke(std::string_view id) {
    Action* action = FindMutable(id);
    if (action == nullptr || !action->execute) {
        SIM_WARN("Editor", "Unknown action '{}'", id);
        return false;
    }
    if (action->enabled && !action->enabled()) {
        return false;
    }
    action->execute();
    return true;
}

ImGuiKeyChord ActionRegistry::Shortcut(std::string_view id) const {
    const auto it = overrides_.find(std::string(id));
    if (it != overrides_.end()) {
        return it->second;
    }
    const Action* action = Find(id);
    return action != nullptr ? action->defaultShortcut : ImGuiKey_None;
}

void ActionRegistry::SetShortcut(std::string_view id, ImGuiKeyChord chord) {
    const Action* action = Find(id);
    if (action == nullptr) {
        return;
    }
    if (chord == action->defaultShortcut) {
        overrides_.erase(std::string(id));
    } else {
        overrides_[std::string(id)] = chord;
    }
}

void ActionRegistry::ResetShortcut(std::string_view id) {
    overrides_.erase(std::string(id));
}

std::string ActionRegistry::ShortcutText(std::string_view id) const {
    return ChordToString(Shortcut(id));
}

std::string ActionRegistry::FindConflict(ImGuiKeyChord chord, std::string_view exceptId) const {
    if (chord == ImGuiKey_None) {
        return {};
    }
    for (const Action& action : actions_) {
        if (action.id == exceptId) {
            continue;
        }
        if (Shortcut(action.id) == chord) {
            return action.id;
        }
    }
    return {};
}

void ActionRegistry::ProcessShortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    // Saat kursor teks aktif, tombol milik pengguna. Tanpa ini, mengetik "z"
    // di kotak nama akan memicu Undo.
    if (io.WantTextInput) {
        return;
    }

    for (const Action& action : actions_) {
        const ImGuiKeyChord chord = Shortcut(action.id);
        if (chord == ImGuiKey_None || !action.execute) {
            continue;
        }
        if (action.enabled && !action.enabled()) {
            continue;
        }
        if (ImGui::IsKeyChordPressed(chord)) {
            action.execute();
        }
    }
}

bool ActionRegistry::MenuItem(std::string_view id) {
    Action* action = FindMutable(id);
    if (action == nullptr) {
        return false;
    }
    const std::string label =
        action->icon.empty() ? action->label : action->icon + "  " + action->label;
    const std::string shortcut = ShortcutText(id);
    const bool enabled = !action->enabled || action->enabled();

    if (ImGui::MenuItem(label.c_str(), shortcut.empty() ? nullptr : shortcut.c_str(), false,
                        enabled)) {
        if (action->execute) {
            action->execute();
        }
        return true;
    }
    return false;
}

bool ActionRegistry::Save(const std::filesystem::path& path) const {
    nlohmann::json root;
    root["version"] = 1;
    // Hanya yang berbeda dari bawaan yang disimpan. Dengan begitu perubahan
    // pintasan bawaan di rilis berikutnya tetap sampai ke pengguna yang belum
    // pernah mengubahnya.
    nlohmann::json shortcuts = nlohmann::json::object();
    for (const auto& [id, chord] : overrides_) {
        const std::string text = ChordToString(chord);
        shortcuts[id] = text;  // string kosong berarti "tanpa pintasan"
    }
    root["shortcuts"] = std::move(shortcuts);

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path);
    if (!file) {
        SIM_WARN("Editor", "Cannot write shortcuts to {}", path.string());
        return false;
    }
    file << root.dump(2) << '\n';
    return true;
}

bool ActionRegistry::Load(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }

    nlohmann::json root;
    try {
        file >> root;
    } catch (const nlohmann::json::exception& error) {
        SIM_WARN("Editor", "Shortcut config is not valid JSON ({}): {}", path.string(),
                 error.what());
        return false;
    }

    const auto shortcuts = root.find("shortcuts");
    if (shortcuts == root.end() || !shortcuts->is_object()) {
        return false;
    }

    overrides_.clear();
    for (const auto& [id, value] : shortcuts->items()) {
        if (!value.is_string()) {
            continue;
        }
        const std::string text = value.get<std::string>();
        const ImGuiKeyChord chord = text.empty() ? ImGuiKey_None : ChordFromString(text);
        if (chord == ImGuiKey_None && !text.empty()) {
            SIM_WARN("Editor", "Unrecognised shortcut '{}' for action '{}', ignored", text, id);
            continue;
        }
        // Aksi yang tidak dikenal dibiarkan tersimpan? Tidak — dibuang, supaya
        // config tidak menumpuk sisa dari versi lama.
        if (Find(id) != nullptr) {
            overrides_[id] = chord;
        }
    }
    return true;
}

}  // namespace sim::editor
