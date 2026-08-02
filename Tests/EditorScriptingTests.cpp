#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Editor/Command.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/EditorScripting.h"
#include "Sim/Editor/PanelManager.h"
#include "Sim/Editor/Selection.h"
#include "Sim/Scene/Components.h"
#include "Sim/Script/ScriptRuntime.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace sim::editor;

namespace {

/// Folder skrip editor sementara yang membersihkan dirinya sendiri.
struct TempEditorScripts {
    TempEditorScripts() {
        path = std::filesystem::temp_directory_path() /
               ("simeditorscripts_" + std::to_string(::getpid()) + "_" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(path);
    }
    ~TempEditorScripts() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    void Write(std::string_view name, std::string_view text) const {
        std::ofstream stream(path / name, std::ios::binary | std::ios::trunc);
        stream << text;
    }

    std::filesystem::path path;
};

bool HasLabel(const std::vector<std::string>& labels, std::string_view wanted) {
    return std::find(labels.begin(), labels.end(), wanted) != labels.end();
}

}  // namespace

TEST_CASE("Skrip editor mendaftarkan menu dan panel dari folder Editor") {
    TempEditorScripts scripts;
    // Dua berkas: yang penting bukan hanya bahwa keduanya berjalan, tapi bahwa
    // urutannya mengikuti nama berkas dan bukan urutan direktori.
    scripts.Write("b_kedua.lua", "sim.editor.menu('Kedua', function() end)\n");
    scripts.Write("a_pertama.lua",
                  "sim.editor.menu('Pertama', function() end)\n"
                  "sim.editor.panel('Catatan', function() end)\n");

    sim::script::ScriptRuntime runtime;
    sim::scene::World world;
    REQUIRE(runtime.Initialize(world, nullptr));

    PanelManager panels;
    EditorContext context;
    EditorScripting scripting;
    scripting.Initialize(&runtime, &context, &panels, scripts.path);

    const std::vector<std::string> labels = scripting.MenuLabels();
    REQUIRE(labels.size() == 2);
    CHECK(labels[0] == "Pertama");
    CHECK(labels[1] == "Kedua");

    // Panel baru muncul di daftar PanelManager, bukan langsung saat didaftarkan:
    // pendaftarannya bisa terjadi di tengah panel lain menggambar.
    CHECK(panels.Find("lua.catatan") == nullptr);
    scripting.FlushPending();
    REQUIRE(panels.Find("lua.catatan") != nullptr);
    CHECK(panels.Find("lua.catatan")->Title() == "Catatan");

    scripting.Shutdown();
}

TEST_CASE("Perubahan dari skrip editor masuk riwayat undo utama") {
    TempEditorScripts scripts;
    // Skrip menyimpan nilainya di global Lua supaya bisa diperiksa dari sini.
    scripts.Write("counter.lua",
                  "hitungan = 0\n"
                  "function naikkan()\n"
                  "    local sebelum = hitungan\n"
                  "    sim.editor.command('Naikkan',\n"
                  "        function() hitungan = sebelum + 1 end,\n"
                  "        function() hitungan = sebelum end)\n"
                  "end\n");

    sim::script::ScriptRuntime runtime;
    sim::scene::World world;
    REQUIRE(runtime.Initialize(world, nullptr));

    PanelManager panels;
    CommandHistory history;
    EditorContext context;
    context.history = &history;

    EditorScripting scripting;
    scripting.Initialize(&runtime, &context, &panels, scripts.path);

    const auto value = [&runtime]() {
        const sim::script::EvalResult result = runtime.Evaluate("hitungan");
        REQUIRE(result.values.size() == 1);
        return result.values[0].value;
    };
    CHECK(value() == "0");

    REQUIRE(runtime.Evaluate("naikkan()").ok);
    CHECK(value() == "1");

    // Inilah syarat yang membuat skrip editor aman dipakai: perubahannya tidak
    // menempuh jalur tulis sendiri, melainkan riwayat yang sama dengan panel
    // C++ — jadi Ctrl+Z membatalkannya seperti perubahan lain.
    REQUIRE(history.Entries().size() == 1);
    CHECK(history.Entries()[0].name == "Naikkan");
    REQUIRE(history.Undo());
    CHECK(value() == "0");
    REQUIRE(history.Redo());
    CHECK(value() == "1");

    scripting.Shutdown();
}

TEST_CASE("Memuat ulang skrip editor tidak menggandakan menu maupun panel") {
    TempEditorScripts scripts;
    scripts.Write("tools.lua",
                  "sim.editor.menu('Tetap', function() end)\n"
                  "sim.editor.menu('Dibuang', function() end)\n"
                  "sim.editor.panel('Alat', function() end)\n");

    sim::script::ScriptRuntime runtime;
    sim::scene::World world;
    REQUIRE(runtime.Initialize(world, nullptr));

    PanelManager panels;
    EditorContext context;
    EditorScripting scripting;
    scripting.Initialize(&runtime, &context, &panels, scripts.path);
    scripting.FlushPending();

    REQUIRE(scripting.MenuLabels().size() == 2);
    const std::size_t panelCount = panels.Panels().size();

    // Berkas yang sama disimpan ulang tanpa salah satu item menunya.
    scripts.Write("tools.lua",
                  "sim.editor.menu('Tetap', function() end)\n"
                  "sim.editor.panel('Alat', function() end)\n");
    scripting.ReloadAll();
    scripting.FlushPending();

    // Item yang hilang dari berkasnya ikut hilang dari menu — kalau tidak,
    // menu perlahan terisi item yang tidak lagi ada di skrip mana pun.
    const std::vector<std::string> labels = scripting.MenuLabels();
    REQUIRE(labels.size() == 1);
    CHECK(HasLabel(labels, "Tetap"));

    // Panel dengan judul sama dipakai ulang, jadi posisi dock-nya bertahan.
    CHECK(panels.Panels().size() == panelCount);

    scripting.Shutdown();
}

TEST_CASE("Skrip editor bisa memproses aset secara batch") {
    TempEditorScripts scripts;

    // Folder aset terpisah dari folder skrip editor: yang diproses skrip adalah
    // isi AssetDatabase, bukan berkas di sebelahnya.
    TempEditorScripts assetRoot;
    assetRoot.Write("tmp_batu.lua", "return {}\n");
    assetRoot.Write("tmp_kayu.lua", "return {}\n");
    assetRoot.Write("catatan.txt", "bukan skrip\n");

    sim::assets::AssetDatabase db;
    REQUIRE(db.Initialize({assetRoot.path, nullptr, 1.0f}));
    db.ScanNow();
    db.Update(0.0f);

    scripts.Write("batch.lua",
                  "terproses = 0\n"
                  "function bersihkan()\n"
                  "    for _, aset in ipairs(sim.editor.assets('Script')) do\n"
                  "        local baru = aset.name:gsub('^tmp_', '')\n"
                  "        if baru ~= aset.name then\n"
                  "            local ok = sim.editor.rename_asset(aset.path, baru)\n"
                  "            if ok then terproses = terproses + 1 end\n"
                  "        end\n"
                  "    end\n"
                  "end\n");

    sim::script::ScriptRuntime runtime;
    sim::scene::World world;
    REQUIRE(runtime.Initialize(world, nullptr));

    PanelManager panels;
    EditorContext context;
    context.assets = &db;
    EditorScripting scripting;
    scripting.Initialize(&runtime, &context, &panels, scripts.path);

    REQUIRE(runtime.Evaluate("bersihkan()").ok);

    // Filternya bekerja: berkas teks tidak ikut tersentuh.
    const sim::script::EvalResult count = runtime.Evaluate("terproses");
    REQUIRE(count.values.size() == 1);
    CHECK(count.values[0].value == "2");

    db.ScanNow();
    db.Update(0.0f);
    CHECK(db.FindByRelativePath("batu.lua") != nullptr);
    CHECK(db.FindByRelativePath("kayu.lua") != nullptr);
    CHECK(db.FindByRelativePath("tmp_batu.lua") == nullptr);
    CHECK(db.FindByRelativePath("catatan.txt") != nullptr);

    scripting.Shutdown();
    db.Shutdown();
}

TEST_CASE("sim.ui hanya sah di dalam callback panel") {
    TempEditorScripts scripts;
    scripts.Write("nakal.lua", "sim.ui.text('di luar panel')\n");

    sim::script::ScriptRuntime runtime;
    sim::scene::World world;
    REQUIRE(runtime.Initialize(world, nullptr));

    PanelManager panels;
    EditorContext context;
    EditorScripting scripting;
    scripting.Initialize(&runtime, &context, &panels, scripts.path);

    // Berkasnya gagal — yang penting justru itu. Menggambar widget di luar
    // callback panel akan menaruhnya di jendela mana pun yang kebetulan sedang
    // terbuka, dan widget yang muncul di tempat tak terduga jauh lebih sulit
    // dilacak daripada sebuah pesan kesalahan.
    CHECK(scripting.MenuLabels().empty());

    // State Lua tetap bisa dipakai setelahnya.
    CHECK(runtime.Evaluate("1 + 1").ok);

    scripting.Shutdown();
}
