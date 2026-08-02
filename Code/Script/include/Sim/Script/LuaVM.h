#pragma once

#include <string>
#include <string_view>

namespace sol {
class state;
}

namespace sim::script {

/// Pembungkus satu state Lua.
///
/// Di E1 isinya baru sebatas membuka pustaka standar dan menjalankan potongan
/// kode — cukup untuk membuktikan rantai build Lua + sol2 bekerja. Binding
/// engine, ScriptComponent, dan hot reload masuk di E6 (docs/PLAN-EDITOR.md).
///
/// Kesalahan Lua tidak pernah dilempar keluar sebagai exception: skrip pengguna
/// yang salah tidak boleh menjatuhkan editor. Semua error dikembalikan sebagai
/// string dan ikut tercatat ke Console.
class LuaVM {
public:
    LuaVM();
    ~LuaVM();

    LuaVM(const LuaVM&) = delete;
    LuaVM& operator=(const LuaVM&) = delete;

    bool Initialize();
    void Shutdown();

    /// Menjalankan potongan Lua. Mengembalikan pesan error, atau string kosong
    /// bila berhasil.
    std::string RunString(std::string_view code, std::string_view chunkName = "=(chunk)");

    /// Versi Lua yang tertaut, mis. "Lua 5.4".
    std::string Version() const;

    sol::state* State() { return state_; }

private:
    sol::state* state_ = nullptr;
};

}  // namespace sim::script
