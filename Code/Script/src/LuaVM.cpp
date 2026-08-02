#include "Sim/Script/LuaVM.h"

#include "Sim/Core/Log.h"

#include <sol/sol.hpp>

namespace sim::script {

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
