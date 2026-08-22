#include "Sim/Editor/EditorScripting.h"

#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Core/Log.h"
#include "Sim/Editor/Command.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelManager.h"
#include "Sim/SceneView/Selection.h"

#if SIM_WITH_LUA
#include "Sim/Script/LuaVM.h"
#include "Sim/Script/ScriptRuntime.h"

#include <sol/sol.hpp>
#endif

#include <imgui.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace sim::editor {

#if SIM_WITH_LUA
namespace {

/// Perubahan yang dilakukan Lua, dibungkus supaya masuk riwayat undo utama.
///
/// Do() dan Undo() adalah dua fungsi Lua yang diberikan skrip. Kesalahan di
/// dalamnya dicatat dan ditelan, tidak dilempar keluar: sebuah exception yang
/// keluar dari Undo() akan meninggalkan CommandHistory di keadaan setengah
/// jalan, dan riwayat yang tidak konsisten lebih berbahaya daripada satu
/// perubahan yang gagal.
class LuaCommand final : public ICommand {
public:
    LuaCommand(std::string name, sol::protected_function doFn, sol::protected_function undoFn)
        : name_(std::move(name)), do_(std::move(doFn)), undo_(std::move(undoFn)) {}

    void Do() override { Call(do_, "do"); }
    void Undo() override { Call(undo_, "undo"); }
    std::string Name() const override { return name_; }

private:
    void Call(const sol::protected_function& function, const char* phase) const {
        if (!function.valid()) {
            return;
        }
        const sol::protected_function_result result = function();
        if (!result.valid()) {
            const sol::error error = result;
            SIM_ERROR("Lua", "{} ({}) failed: {}", name_, phase, error.what());
        }
    }

    std::string name_;
    sol::protected_function do_;
    sol::protected_function undo_;
};

/// Panel yang isinya digambar sebuah fungsi Lua.
class LuaPanel final : public Panel {
public:
    /// `drawingFlag` dibagi, bukan dipinjam: panel hidup di PanelManager, yang
    /// dihancurkan setelah EditorScripting. Pointer mentah ke dalam Impl akan
    /// menggantung di jendela itu — tidak berbahaya hari ini karena tidak ada
    /// yang menyentuhnya, tapi itu jenis jaminan yang hilang diam-diam.
    LuaPanel(std::string id, std::string title, sol::protected_function draw,
             std::shared_ptr<bool> drawingFlag)
        : Panel(std::move(id), std::move(title), PanelCategory::Authoring),
          draw_(std::move(draw)),
          drawingFlag_(std::move(drawingFlag)) {}

    void OnDraw(EditorContext& /*context*/) override {
        if (failed_ || !draw_.valid()) {
            ImGui::TextDisabled("This panel's script stopped after an error.");
            return;
        }
        *drawingFlag_ = true;
        const sol::protected_function_result result = draw_();
        *drawingFlag_ = false;
        if (!result.valid()) {
            const sol::error error = result;
            SIM_ERROR("Lua", "Panel '{}' failed: {}", Title(), error.what());
            // Panel yang gagal dimatikan, alasan yang sama dengan instance
            // skrip: kesalahan tiap frame membanjiri Console enam puluh kali
            // per detik dan menenggelamkan pesan pertama yang berguna.
            failed_ = true;
        }
    }

    /// Mengganti fungsi gambarnya saat skrip dimuat ulang, sekaligus memberinya
    /// kesempatan kedua setelah gagal.
    void Rebind(sol::protected_function draw) {
        draw_ = std::move(draw);
        failed_ = false;
    }

private:
    sol::protected_function draw_;
    std::shared_ptr<bool> drawingFlag_;
    bool failed_ = false;
};

/// Id panel dari judulnya: huruf kecil, spasi jadi garis bawah.
///
/// Harus stabil terhadap teks yang sama, karena id inilah yang dipakai ImGui
/// untuk mengingat posisi dock panel antar sesi.
std::string IdFromTitle(std::string_view title) {
    std::string id = "lua.";
    for (const char c : title) {
        id += (c == ' ' || c == '\t') ? '_' : static_cast<char>(std::tolower(c));
    }
    return id;
}

}  // namespace
#endif  // SIM_WITH_LUA

struct EditorScripting::Impl {
#if SIM_WITH_LUA
    script::ScriptRuntime* runtime = nullptr;
    EditorContext* context = nullptr;
    PanelManager* panels = nullptr;
    std::filesystem::path folder;

    struct MenuItem {
        std::string label;
        sol::protected_function callback;
    };
    std::vector<MenuItem> menuItems;

    struct PendingPanel {
        std::string title;
        sol::protected_function draw;
    };
    std::vector<PendingPanel> pending;

    /// True hanya selagi sebuah LuaPanel menggambar. Penjaga untuk sim.ui.
    std::shared_ptr<bool> drawingPanel = std::make_shared<bool>(false);
#endif
};

EditorScripting::EditorScripting() : impl_(std::make_unique<Impl>()) {}
EditorScripting::~EditorScripting() = default;

#if SIM_WITH_LUA

void EditorScripting::Initialize(script::ScriptRuntime* runtime, EditorContext* context,
                                 PanelManager* panels, std::filesystem::path folder) {
    if (runtime == nullptr) {
        return;
    }
    impl_->runtime = runtime;
    impl_->context = context;
    impl_->panels = panels;
    impl_->folder = std::move(folder);

    sol::state& lua = *runtime->VM().State();
    sol::table sim = lua["sim"];
    sol::table editorTable = lua.create_table();
    sim["editor"] = editorTable;

    Impl* impl = impl_.get();

    editorTable.set_function("menu", [impl](const std::string& label,
                                            sol::protected_function callback) {
        // Nama yang sama menggantikan yang lama alih-alih menumpuk. Tanpa itu,
        // memuat ulang skrip editor akan menggandakan setiap item menunya.
        for (Impl::MenuItem& existing : impl->menuItems) {
            if (existing.label == label) {
                existing.callback = std::move(callback);
                return;
            }
        }
        impl->menuItems.push_back({label, std::move(callback)});
    });

    editorTable.set_function("panel", [impl](const std::string& title,
                                             sol::protected_function draw) {
        impl->pending.push_back({title, std::move(draw)});
    });

    editorTable.set_function("command", [impl](const std::string& name,
                                               sol::protected_function doFn,
                                               sol::object undoFn) {
        if (impl->context == nullptr || impl->context->history == nullptr) {
            return;
        }
        sol::protected_function undo;
        if (undoFn.is<sol::protected_function>()) {
            undo = undoFn.as<sol::protected_function>();
        }
        impl->context->history->CloseMergeGroup();
        impl->context->history->Execute<LuaCommand>(name, std::move(doFn), std::move(undo));
    });

    editorTable.set_function("log", [](const std::string& message) {
        SIM_INFO("Lua", "{}", message);
    });

    editorTable.set_function("selection_count", [impl]() -> int {
        if (impl->context == nullptr || impl->context->selection == nullptr) {
            return 0;
        }
        return static_cast<int>(impl->context->selection->Items().size());
    });

    // --- pemrosesan aset secara batch ---------------------------------------
    //
    // Aset dirujuk lewat path relatifnya, bukan GUID. Keduanya sah, tapi path
    // adalah yang bisa dibaca dan dicocokkan pola di dalam skrip; GUID hanya
    // berguna kalau skripnya menyimpan rujukan antar sesi, dan untuk itu ia
    // sudah tersedia di tiap entri sebagai `guid`.
    editorTable.set_function("assets", [impl, &lua](sol::object filter) {
        sol::table list = lua.create_table();
        if (impl->context == nullptr || impl->context->assets == nullptr) {
            return list;
        }
        const std::string wanted = filter.is<std::string>() ? filter.as<std::string>() : "";
        int index = 0;
        for (const assets::AssetRecord& record : impl->context->assets->All()) {
            const char* type = assets::ToString(record.type);
            if (!wanted.empty() && wanted != type) {
                continue;
            }
            sol::table entry = lua.create_table();
            entry["name"] = record.name;
            entry["path"] = record.relativePath;
            entry["type"] = type;
            entry["guid"] = record.guid.ToString();
            list[++index] = entry;
        }
        return list;
    });

    // Mengganti nama dan memindahkan sengaja TIDAK lewat CommandHistory: yang
    // berubah adalah berkas di disk, dan riwayat undo di sini hanya menjanjikan
    // pembatalan yang tidak bisa ditepatinya begitu berkasnya disentuh dari
    // luar. Panel Asset Browser pun menempuh jalur langsung yang sama.
    const auto findAsset = [impl](const std::string& path) -> const assets::AssetRecord* {
        if (impl->context == nullptr || impl->context->assets == nullptr) {
            return nullptr;
        }
        return impl->context->assets->FindByRelativePath(path);
    };

    editorTable.set_function(
        "rename_asset",
        [impl, findAsset](const std::string& path,
                          const std::string& newName) -> std::pair<bool, std::string> {
            const assets::AssetRecord* record = findAsset(path);
            if (record == nullptr) {
                return {false, "no asset at " + path};
            }
            std::string error;
            const bool ok = impl->context->assets->Rename(record->guid, newName, error);
            return {ok, error};
        });

    editorTable.set_function(
        "move_asset",
        [impl, findAsset](const std::string& path,
                          const std::string& folder) -> std::pair<bool, std::string> {
            const assets::AssetRecord* record = findAsset(path);
            if (record == nullptr) {
                return {false, "no asset at " + path};
            }
            std::string error;
            const bool ok = impl->context->assets->Move(record->guid, folder, error);
            return {ok, error};
        });

    // --- sim.ui ------------------------------------------------------------
    sol::table ui = lua.create_table();
    sim["ui"] = ui;

    // Sengaja sempit. Membuka seluruh ImGui ke Lua berarti setiap Begin tanpa
    // End yang cocok menjatuhkan editor lewat assert di dalam ImGui, dan yang
    // terlihat pengguna hanyalah editor yang hilang tanpa pesan.
    const auto guard = [impl](const char* name) {
        if (!*impl->drawingPanel) {
            throw std::runtime_error(std::string("sim.ui.") + name +
                                     " may only be called from a panel callback");
        }
    };

    ui.set_function("text", [guard](const std::string& value) {
        guard("text");
        ImGui::TextUnformatted(value.c_str());
    });
    ui.set_function("separator", [guard]() {
        guard("separator");
        ImGui::Separator();
    });
    ui.set_function("same_line", [guard]() {
        guard("same_line");
        ImGui::SameLine();
    });
    ui.set_function("button", [guard](const std::string& label) {
        guard("button");
        return ImGui::Button(label.c_str());
    });
    ui.set_function("checkbox", [guard](const std::string& label, bool value) {
        guard("checkbox");
        // Nilainya dikembalikan, bukan diubah lewat referensi: Lua tidak punya
        // pointer ke boolean, dan `value = sim.ui.checkbox("x", value)` adalah
        // bentuk yang paling sulit dipakai secara salah.
        bool current = value;
        ImGui::Checkbox(label.c_str(), &current);
        return current;
    });
    ui.set_function("input_float", [guard](const std::string& label, float value) {
        guard("input_float");
        float current = value;
        ImGui::DragFloat(label.c_str(), &current, 0.01f);
        return current;
    });

    SIM_INFO("Editor", "Editor scripting API ready (sim.editor, sim.ui)");
    ReloadAll();
}

void EditorScripting::Shutdown() {
    impl_->menuItems.clear();
    impl_->pending.clear();
    impl_->runtime = nullptr;
}

void EditorScripting::FlushPending() {
    if (impl_->panels == nullptr || impl_->pending.empty()) {
        return;
    }
    std::vector<Impl::PendingPanel> pending;
    pending.swap(impl_->pending);
    for (Impl::PendingPanel& entry : pending) {
        const std::string id = IdFromTitle(entry.title);
        // Judul yang sama menggantikan isinya, bukan menambah panel kedua.
        // Memuat ulang skrip editor karena itu tidak meninggalkan tumpukan
        // panel kembar yang semuanya menggambar hal yang sama.
        //
        // Keadaan buka/tutupnya sengaja tidak disentuh: panel yang ditutup
        // pengguna tidak boleh terbuka lagi hanya karena berkasnya disimpan
        // ulang. Yang baru pertama kali didaftarkan tetap muncul, karena panel
        // memang lahir dalam keadaan terbuka.
        if (Panel* existing = impl_->panels->Find(id)) {
            static_cast<LuaPanel*>(existing)->Rebind(std::move(entry.draw));
            continue;
        }
        impl_->panels->Add(std::make_unique<LuaPanel>(id, entry.title, std::move(entry.draw),
                                                      impl_->drawingPanel));
    }
}

void EditorScripting::DrawMenu() {
    if (impl_->menuItems.empty()) {
        ImGui::TextDisabled("No editor scripts loaded");
        return;
    }
    for (const Impl::MenuItem& item : impl_->menuItems) {
        if (!ImGui::MenuItem(item.label.c_str())) {
            continue;
        }
        const sol::protected_function_result result = item.callback();
        if (!result.valid()) {
            const sol::error error = result;
            SIM_ERROR("Lua", "Menu '{}' failed: {}", item.label, error.what());
        }
    }
}

std::vector<std::string> EditorScripting::MenuLabels() const {
    std::vector<std::string> labels;
    labels.reserve(impl_->menuItems.size());
    for (const Impl::MenuItem& item : impl_->menuItems) {
        labels.push_back(item.label);
    }
    return labels;
}

void EditorScripting::ReloadAll() {
    if (impl_->runtime == nullptr) {
        return;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(impl_->folder, error)) {
        return;
    }

    // Diurutkan supaya urutan menu tidak bergantung pada urutan direktori, yang
    // berbeda antar sistem berkas.
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(impl_->folder, error)) {
        if (entry.is_regular_file() && entry.path().extension() == ".lua") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    impl_->menuItems.clear();
    for (const std::filesystem::path& file : files) {
        std::ifstream stream(file);
        if (!stream) {
            continue;
        }
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        const std::string name = file.filename().string();
        // RunString melaporkan kesalahannya sendiri ke Console lengkap dengan
        // traceback; yang tersisa di sini hanyalah tidak berpura-pura berhasil.
        if (impl_->runtime->VM().RunString(buffer.str(), "@" + name).empty()) {
            SIM_INFO("Editor", "Editor script loaded: {}", name);
        }
    }
}

#else  // SIM_WITH_LUA

void EditorScripting::Initialize(script::ScriptRuntime*, EditorContext*, PanelManager*,
                                 std::filesystem::path) {}
void EditorScripting::Shutdown() {}
void EditorScripting::FlushPending() {}
void EditorScripting::DrawMenu() {
    ImGui::TextDisabled("This build was configured without Lua");
}
std::vector<std::string> EditorScripting::MenuLabels() const {
    return {};
}
void EditorScripting::ReloadAll() {}

#endif  // SIM_WITH_LUA

}  // namespace sim::editor
