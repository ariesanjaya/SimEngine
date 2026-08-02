#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Core/TaskPool.h"
#include "Sim/Scene/Components.h"
#include "Sim/Script/LuaVM.h"
#include "Sim/Script/ScriptRuntime.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>

TEST_CASE("LuaVM menjalankan kode dan melaporkan versi") {
    sim::script::LuaVM lua;
    REQUIRE(lua.Initialize());
    CHECK(lua.Version().find("Lua 5.4") != std::string::npos);
    CHECK(lua.RunString("local x = 1 + 1 assert(x == 2)").empty());
}

TEST_CASE("Kesalahan Lua dikembalikan sebagai pesan, bukan exception") {
    sim::script::LuaVM lua;
    REQUIRE(lua.Initialize());

    // Ini yang menjaga editor tetap hidup ketika skrip pengguna salah.
    const std::string error = lua.RunString("error('sengaja gagal')");
    CHECK_FALSE(error.empty());
    CHECK(error.find("sengaja gagal") != std::string::npos);

    // State harus tetap bisa dipakai setelah error.
    CHECK(lua.RunString("return 1").empty());
}

TEST_CASE("Pustaka io dan os tidak tersedia untuk skrip") {
    sim::script::LuaVM lua;
    REQUIRE(lua.Initialize());
    CHECK(lua.RunString("assert(io == nil)").empty());
    CHECK(lua.RunString("assert(os == nil)").empty());
}

TEST_CASE("Kesalahan runtime memuat traceback, bukan hanya pesannya") {
    sim::script::LuaVM lua;
    REQUIRE(lua.Initialize());

    // Kesalahan dilempar dari dalam fungsi yang dipanggil fungsi lain. Yang
    // dibutuhkan pengguna justru jejak pemanggilnya: tanpa itu ia tahu baris
    // mana yang gagal, tapi tidak tahu dari mana baris itu dicapai.
    const std::string error = lua.RunString(
        "local function inner() error('boom') end\n"
        "local function outer() inner() end\n"
        "outer()\n",
        "@arena.lua");

    REQUIRE_FALSE(error.empty());
    CHECK(error.find("boom") != std::string::npos);
    CHECK(error.find("stack traceback") != std::string::npos);
    // Nama chunk berawalan '@' membuat Lua menyebutnya sebagai nama berkas.
    CHECK(error.find("arena.lua:1") != std::string::npos);
}

TEST_CASE("Evaluate menerima ekspresi maupun pernyataan") {
    sim::script::ScriptRuntime runtime;
    sim::scene::World world;
    REQUIRE(runtime.Initialize(world, nullptr));

    // Ekspresi: yang diketik pengguna di konsol biasanya tanpa `return`.
    const sim::script::EvalResult expression = runtime.Evaluate("1 + 1");
    CHECK(expression.ok);
    REQUIRE(expression.values.size() == 1);
    CHECK(expression.values[0].value == "2");

    // Pernyataan tidak menghasilkan nilai, tapi tetap harus berjalan.
    const sim::script::EvalResult statement = runtime.Evaluate("kTest = 7");
    CHECK(statement.ok);
    CHECK(statement.values.empty());
    CHECK(runtime.Evaluate("kTest").values.at(0).value == "7");

    // Kode yang salah menghasilkan pesan, bukan crash.
    const sim::script::EvalResult broken = runtime.Evaluate("1 +");
    CHECK_FALSE(broken.ok);
    CHECK_FALSE(broken.error.empty());
}

TEST_CASE("Tabel hasil evaluasi dipecah jadi anak yang bisa dilipat") {
    sim::script::ScriptRuntime runtime;
    sim::scene::World world;
    REQUIRE(runtime.Initialize(world, nullptr));

    const sim::script::EvalResult result = runtime.Evaluate("{ b = 2, a = 1 }");
    REQUIRE(result.ok);
    REQUIRE(result.values.size() == 1);
    REQUIRE(result.values[0].children.size() == 2);
    // Urutan iterasi tabel Lua tidak ditentukan, jadi hasilnya diurutkan supaya
    // tabel yang sama selalu tampil sama.
    CHECK(result.values[0].children[0].label == "a");
    CHECK(result.values[0].children[1].label == "b");

    // Tabel yang menunjuk dirinya sendiri harus berhenti, bukan berputar.
    const sim::script::EvalResult cyclic =
        runtime.Evaluate("local t = {} t.self = t return t");
    CHECK(cyclic.ok);
}

TEST_CASE("Complete mencari nama global maupun isi tabel") {
    sim::script::ScriptRuntime runtime;
    sim::scene::World world;
    REQUIRE(runtime.Initialize(world, nullptr));

    const std::vector<std::string> globals = runtime.Complete("sim");
    CHECK(std::find(globals.begin(), globals.end(), "sim") != globals.end());

    // Titik memindahkan pencarian ke dalam tabelnya, dan hasilnya tetap berupa
    // nama lengkap supaya bisa langsung menggantikan yang diketik.
    const std::vector<std::string> members = runtime.Complete("sim.get_");
    REQUIRE(members.size() == 1);
    CHECK(members[0] == "sim.get_component");

    CHECK(runtime.Complete("sim.tidak_ada_").empty());
    CHECK(runtime.Complete("bukan_tabel.apa").empty());
}

namespace {

/// Folder sementara yang membersihkan dirinya sendiri.
struct TempScriptDir {
    TempScriptDir() {
        path = std::filesystem::temp_directory_path() /
               ("simscripts_" + std::to_string(::getpid()) + "_" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(path);
    }
    ~TempScriptDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::filesystem::path path;
};

void WriteScript(const std::filesystem::path& path, std::string_view text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
}

}  // namespace

TEST_CASE("hot reload memakai kode baru dan mempertahankan state instance") {
    TempScriptDir temp;
    const std::filesystem::path script = temp.path / "counter.lua";
    WriteScript(script,
                "local S = {}\n"
                "function S:OnStart() self.state.n = self.state.n or 0 end\n"
                "function S:OnUpdate(dt)\n"
                "  self.state.n = self.state.n + 1\n"
                "  kCounter = self.state.n\n"
                "end\n"
                "return S\n");

    sim::TaskPool pool(2);
    sim::assets::AssetDatabase db;
    REQUIRE(db.Initialize({temp.path, &pool, 0.05f}));
    const sim::assets::AssetRecord* record = db.FindByRelativePath("counter.lua");
    REQUIRE(record != nullptr);
    const sim::Uuid guid = record->guid;

    sim::scene::World world;
    const sim::scene::Entity entity = world.Create("Counter");
    world.Add<sim::scene::ScriptComponent>(entity).script = sim::AssetRef{guid};

    sim::script::ScriptRuntime runtime;
    REQUIRE(runtime.Initialize(world, &db));
    runtime.Start();
    for (int i = 0; i < 3; ++i) {
        runtime.Update(0.016f);
    }
    // Nilai dibaca lewat global yang ditulis skrip: itu cara paling langsung
    // untuk melihat isi `self.state` dari luar tanpa membuka runtime-nya.
    REQUIRE(runtime.Evaluate("kCounter").values.at(0).value == "3");

    // Disunting dari luar, persis seperti pengguna menyimpan dari editor teks.
    WriteScript(script,
                "local S = {}\n"
                "function S:OnStart() self.state.n = self.state.n or 0 end\n"
                "function S:OnUpdate(dt)\n"
                "  self.state.n = self.state.n + 10\n"
                "  kCounter = self.state.n\n"
                "end\n"
                "return S\n");

    // Jalur yang sama dengan yang dipakai editor: database yang memberi tahu
    // aset mana yang isinya berganti, bukan runtime yang memindai sendiri.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool reported = false;
    while (!reported && std::chrono::steady_clock::now() < deadline) {
        db.Update(0.1f);
        for (const sim::Uuid& changed : db.ChangedThisUpdate()) {
            if (changed == guid) {
                reported = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(reported);

    runtime.Reload(guid);
    runtime.Update(0.016f);

    // 3 dari kode lama, lalu 10 dari kode baru. Angka 13 sekaligus membuktikan
    // dua hal: kode yang dipakai benar-benar yang baru, dan hitungan yang sudah
    // terkumpul tidak hilang. Reload yang membuang state sama saja dengan
    // memulai ulang permainan.
    sim::scene::ScriptComponent* component =
        world.TryGet<sim::scene::ScriptComponent>(entity);
    REQUIRE(component != nullptr);
    CHECK(component->loaded);
    CHECK(runtime.Evaluate("kCounter").values.at(0).value == "13");

    runtime.Stop();
}

TEST_CASE("Pustaka vec3 dan quat bekerja pada tabel yang sama dengan komponen") {
    sim::script::ScriptRuntime runtime;
    sim::scene::World world;
    REQUIRE(runtime.Initialize(world, nullptr));

    auto eval = [&runtime](const char* code) {
        const sim::script::EvalResult result = runtime.Evaluate(code);
        REQUIRE(result.ok);
        REQUIRE_FALSE(result.values.empty());
        return result.values[0].value;
    };

    CHECK(eval("tostring(sim.vec3(1, 2, 3) + sim.vec3(1, 1, 1))") == "vec3(2.000, 3.000, 4.000)");
    CHECK(eval("tostring(sim.vec3(1, 2, 3) * 2)") == "vec3(2.000, 4.000, 6.000)");
    // Skalar di kiri harus sah juga; menuntut urutan tertentu adalah jebakan
    // yang hanya terlihat saat skrip dijalankan.
    CHECK(eval("tostring(2 * sim.vec3(1, 2, 3))") == "vec3(2.000, 4.000, 6.000)");
    CHECK(eval("sim.vec3(3, 4, 0):length()") == "5.0");
    CHECK(eval("tostring(sim.vec3(1, 0, 0):cross(sim.vec3(0, 1, 0)))") ==
          "vec3(0.000, 0.000, 1.000)");
    // Vektor nol tidak boleh menghasilkan nan.
    CHECK(eval("tostring(sim.vec3(0, 0, 0):normalized())") == "vec3(0.000, 0.000, 0.000)");

    // Setengah putaran mengelilingi Y membalik sumbu X. Dibandingkan dengan
    // toleransi, bukan lewat teksnya: hasilnya melewati sin/cos, dan komponen
    // yang secara matematis nol bisa keluar sebagai -0.
    CHECK(eval("local v = sim.axis_angle(sim.up(), math.pi):rotate(sim.vec3(1, 0, 0))\n"
               "return math.abs(v.x + 1) < 1e-6 and math.abs(v.y) < 1e-6 and "
               "math.abs(v.z) < 1e-6") == "true");

    // Hasilnya berbentuk tabel {x,y,z} biasa, jadi bisa langsung ditulis
    // kembali ke komponen tanpa konversi.
    CHECK(eval("sim.vec3(1, 2, 3).y") == "2");
}
