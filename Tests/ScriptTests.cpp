#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Script/LuaVM.h"

#include <doctest/doctest.h>

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
