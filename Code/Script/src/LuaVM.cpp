#include "Sim/Script/LuaVM.h"

#include "Sim/Core/Log.h"

#include <sol/sol.hpp>

namespace sim::script {
namespace {

/// Penangan kesalahan yang menempelkan traceback ke pesannya.
///
/// Tanpa ini yang sampai ke Console hanya "arena.lua:12: attempt to index a nil
/// value" — benar, tapi tidak menyebut siapa yang memanggil baris itu. Pada
/// skrip yang memanggil helper-nya sendiri, jejak pemanggil justru yang paling
/// dibutuhkan.
///
/// Ditulis sebagai lua_CFunction, bukan `debug.traceback`, supaya pustaka
/// `debug` tidak perlu dibuka untuk skrip pengguna.
int Traceback(lua_State* L) {
    const char* message = lua_tostring(L, 1);
    if (message == nullptr) {
        // Skrip boleh melempar apa saja, termasuk tabel. luaL_traceback
        // menuntut string, jadi objek non-string dijadikan keterangan singkat.
        message = "(error object is not a string)";
    }
    // Level 1: bingkai yang melempar, bukan penangan ini sendiri.
    luaL_traceback(L, L, message, 1);
    return 1;
}

}  // namespace

LuaVM::LuaVM() = default;

LuaVM::~LuaVM() {
    Shutdown();
}

bool LuaVM::Initialize() {
    if (state_ != nullptr) {
        return true;
    }
    state_ = new sol::state();

    // Sengaja tanpa pustaka `io` dan `os`: skrip gameplay tidak punya alasan
    // menyentuh berkas atau proses secara langsung, dan membatasinya sejak awal
    // jauh lebih mudah daripada mencabutnya setelah ada skrip yang memakainya.
    // Akses berkas yang sah nanti lewat API aset di E6.
    state_->open_libraries(sol::lib::base, sol::lib::package, sol::lib::coroutine,
                           sol::lib::string, sol::lib::table, sol::lib::math,
                           sol::lib::utf8);

    // print() diarahkan ke Console editor supaya keluaran skrip terlihat di
    // tempat yang sama dengan log lainnya.
    state_->set_function("print", [](sol::this_state ts, sol::variadic_args args) {
        lua_State* L = ts;
        std::string line;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i != 0) {
                line += '\t';
            }
            // luaL_tolstring menghormati metamethod __tostring dan menangani
            // angka/nil/boolean, jadi print() di Lua tetap berperilaku normal.
            const char* text = luaL_tolstring(L, args[i].stack_index(), nullptr);
            line += text != nullptr ? text : "(nil)";
            lua_pop(L, 1);
        }
        SIM_INFO("Lua", "{}", line);
    });

    // Berlaku untuk setiap sol::protected_function, termasuk OnStart/OnUpdate
    // dan potongan yang diketik di Lua Console.
    lua_State* L = state_->lua_state();
    lua_pushcfunction(L, &Traceback);
    sol::protected_function::set_default_handler(sol::object(L, -1));
    lua_pop(L, 1);

    SIM_INFO("Lua", "{} ready", Version());
    return true;
}

std::string LuaVM::RunString(std::string_view code, std::string_view chunkName) {
    if (state_ == nullptr) {
        return "Lua is not initialised";
    }

    const sol::protected_function_result result =
        state_->safe_script(code, sol::script_pass_on_error, std::string(chunkName));
    if (!result.valid()) {
        const sol::error error = result;
        SIM_ERROR("Lua", "{}", error.what());
        return error.what();
    }
    return {};
}

std::string LuaVM::Version() const {
    return LUA_RELEASE;
}

void LuaVM::Shutdown() {
    delete state_;
    state_ = nullptr;
}

}  // namespace sim::script
