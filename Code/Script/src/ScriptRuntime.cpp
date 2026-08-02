#include "Sim/Script/ScriptRuntime.h"

#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Core/Log.h"
#include "Sim/Reflect/TypeRegistry.h"
#include "Sim/Scene/ComponentRegistry.h"
#include "Sim/Scene/Components.h"
#include "Sim/Script/LuaVM.h"

#include <sol/sol.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace sim::script {
namespace {

using reflect::FieldDesc;
using reflect::FieldKind;

/// Membaca satu field komponen menjadi nilai Lua.
///
/// Dituntun FieldDesc, bukan cabang per komponen. Itulah yang membuat komponen
/// baru langsung terjangkau dari Lua tanpa satu baris pun ditambahkan di sini —
/// imbalan yang sama seperti yang sudah didapat Inspector dan serialisasi.
sol::object FieldToLua(sol::state& lua, const FieldDesc& field, const void* data) {
    switch (field.kind) {
        case FieldKind::Bool: return sol::make_object(lua, *static_cast<const bool*>(data));
        case FieldKind::Int: return sol::make_object(lua, *static_cast<const int*>(data));
        case FieldKind::UInt:
            return sol::make_object(lua, static_cast<int>(*static_cast<const uint8_t*>(data)));
        case FieldKind::Float: return sol::make_object(lua, *static_cast<const float*>(data));
        case FieldKind::Vec3: {
            const auto* v = static_cast<const Vec3*>(data);
            return sol::make_object(lua, lua.create_table_with("x", v->x, "y", v->y, "z", v->z));
        }
        case FieldKind::Vec4: {
            const auto* v = static_cast<const Vec4*>(data);
            return sol::make_object(
                lua, lua.create_table_with("x", v->x, "y", v->y, "z", v->z, "w", v->w));
        }
        case FieldKind::Quat: {
            const auto* q = static_cast<const Quat*>(data);
            return sol::make_object(
                lua, lua.create_table_with("w", q->w, "x", q->x, "y", q->y, "z", q->z));
        }
        case FieldKind::String:
            return sol::make_object(lua, *static_cast<const std::string*>(data));
        case FieldKind::Uuid:
            return sol::make_object(lua, static_cast<const Uuid*>(data)->ToString());
        case FieldKind::AssetRef:
            return sol::make_object(lua, static_cast<const AssetRef*>(data)->guid.ToString());
        case FieldKind::Enum: {
            const auto index = static_cast<std::size_t>(*static_cast<const uint8_t*>(data));
            const auto& names = field.attributes.enumNames;
            return sol::make_object(lua, index < names.size() ? names[index] : std::string{});
        }
        default: break;
    }
    return sol::nil;
}

/// Menulis nilai Lua kembali ke field komponen. Nilai yang tipenya tidak cocok
/// diabaikan diam-diam — skrip pengguna tidak boleh merusak data scene hanya
/// karena salah ketik.
void LuaToField(const sol::object& value, const FieldDesc& field, void* data) {
    switch (field.kind) {
        case FieldKind::Bool:
            if (value.is<bool>()) {
                *static_cast<bool*>(data) = value.as<bool>();
            }
            break;
        case FieldKind::Int:
            if (value.is<int>()) {
                *static_cast<int*>(data) = value.as<int>();
            }
            break;
        case FieldKind::UInt:
            if (value.is<int>()) {
                *static_cast<uint8_t*>(data) = static_cast<uint8_t>(value.as<int>());
            }
            break;
        case FieldKind::Float:
            if (value.is<float>()) {
                *static_cast<float*>(data) = value.as<float>();
            }
            break;
        case FieldKind::Vec3:
            if (value.is<sol::table>()) {
                const sol::table t = value.as<sol::table>();
                auto* v = static_cast<Vec3*>(data);
                *v = Vec3(t.get_or("x", v->x), t.get_or("y", v->y), t.get_or("z", v->z));
            }
            break;
        case FieldKind::Vec4:
            if (value.is<sol::table>()) {
                const sol::table t = value.as<sol::table>();
                auto* v = static_cast<Vec4*>(data);
                *v = Vec4(t.get_or("x", v->x), t.get_or("y", v->y), t.get_or("z", v->z),
                          t.get_or("w", v->w));
            }
            break;
        case FieldKind::Quat:
            if (value.is<sol::table>()) {
                const sol::table t = value.as<sol::table>();
                auto* q = static_cast<Quat*>(data);
                *q = Quat(t.get_or("w", q->w), t.get_or("x", q->x), t.get_or("y", q->y),
                          t.get_or("z", q->z));
            }
            break;
        case FieldKind::String:
            if (value.is<std::string>()) {
                *static_cast<std::string*>(data) = value.as<std::string>();
            }
            break;
        case FieldKind::Enum:
            if (value.is<std::string>()) {
                const std::string name = value.as<std::string>();
                const auto& names = field.attributes.enumNames;
                for (std::size_t i = 0; i < names.size(); ++i) {
                    if (names[i] == name) {
                        *static_cast<uint8_t*>(data) = static_cast<uint8_t>(i);
                        break;
                    }
                }
            }
            break;
        default: break;
    }
}

}  // namespace

/// Satu skrip yang menempel pada satu entity.
struct ScriptRuntime::Instance {
    scene::Entity entity = scene::kNullEntity;
    Uuid guid;
    sol::table self;
    /// Dimatikan setelah melempar kesalahan. Instance yang gagal tiap frame
    /// akan membanjiri Console enam puluh kali per detik dengan pesan yang sama,
    /// dan pesan pertama — satu-satunya yang berguna — tenggelam seketika.
    bool failed = false;
};

ScriptRuntime::ScriptRuntime() : vm_(std::make_unique<LuaVM>()) {}
ScriptRuntime::~ScriptRuntime() {
    Shutdown();
}

bool ScriptRuntime::Initialize(scene::World& world, assets::AssetDatabase* assets) {
    world_ = &world;
    assets_ = assets;
    if (!vm_->Initialize()) {
        return false;
    }
    RegisterBindings();
    return true;
}

void ScriptRuntime::Shutdown() {
    instances_.clear();
    running_ = false;
}

void ScriptRuntime::RegisterBindings() {
    sol::state& lua = *vm_->State();
    sol::table sim = lua.create_named_table("sim");

    sim.set_function("log", [](const std::string& message) {
        SIM_INFO("Lua", "{}", message);
    });
    sim.set_function("warn", [](const std::string& message) {
        SIM_WARN("Lua", "{}", message);
    });

    sim.set_function("name_of", [this](uint32_t raw) -> std::string {
        const auto entity = static_cast<scene::Entity>(raw);
        return world_->IsAlive(entity) ? world_->NameOf(entity) : std::string{};
    });

    // Akses komponen lewat nama, dituntun reflection. Satu pasang fungsi untuk
    // seluruh tipe komponen — sekarang dan yang ditambahkan nanti.
    sim.set_function("get_component", [this, &lua](uint32_t raw, const std::string& type)
                         -> sol::object {
        const auto entity = static_cast<scene::Entity>(raw);
        if (!world_->IsAlive(entity)) {
            return sol::nil;
        }
        const scene::ComponentOps* ops = scene::ComponentRegistry::Get().Find(type);
        if (ops == nullptr) {
            return sol::nil;
        }
        void* data = ops->tryGet(world_->Registry(), scene::World::ToEntt(entity));
        if (data == nullptr) {
            return sol::nil;
        }
        sol::table result = lua.create_table();
        for (const FieldDesc& field : ops->type->fields) {
            result[field.name] = FieldToLua(lua, field, field.access(data));
        }
        return result;
    });

    sim.set_function("set_component",
                     [this](uint32_t raw, const std::string& type, const sol::table& values) {
                         const auto entity = static_cast<scene::Entity>(raw);
                         if (!world_->IsAlive(entity)) {
                             return;
                         }
                         const scene::ComponentOps* ops =
                             scene::ComponentRegistry::Get().Find(type);
                         if (ops == nullptr) {
                             return;
                         }
                         void* data = ops->tryGet(world_->Registry(),
                                                  scene::World::ToEntt(entity));
                         if (data == nullptr) {
                             return;
                         }
                         for (const FieldDesc& field : ops->type->fields) {
                             const sol::object value = values[field.name];
                             if (value.valid()) {
                                 LuaToField(value, field, field.access(data));
                             }
                         }
                         // Transform yang berubah harus merambat ke keturunannya.
                         world_->MarkTransformDirty(entity);
                     });
}

void ScriptRuntime::LoadFor(scene::Entity entity, const Uuid& guid) {
    if (assets_ == nullptr) {
        return;
    }
    const assets::AssetRecord* record = assets_->Find(guid);
    if (record == nullptr) {
        SIM_WARN("Lua", "Script asset {} is missing", guid.ToString());
        return;
    }

    std::ifstream stream(assets_->AbsolutePath(*record));
    if (!stream) {
        SIM_WARN("Lua", "Cannot open script {}", record->relativePath);
        return;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();

    sol::state& lua = *vm_->State();
    // Nama chunk diawali '@' supaya Lua memperlakukannya sebagai nama berkas,
    // sehingga traceback menyebut "arena.lua:12" alih-alih menyalin seluruh
    // sumbernya ke dalam pesan kesalahan.
    const sol::protected_function_result loaded =
        lua.safe_script(buffer.str(), sol::script_pass_on_error,
                        "@" + record->relativePath);
    if (!loaded.valid()) {
        const sol::error error = loaded;
        SIM_ERROR("Lua", "{}: {}", record->relativePath, error.what());
        return;
    }
    if (loaded.get_type() != sol::type::table) {
        SIM_WARN("Lua", "{} did not return a table; nothing to run", record->relativePath);
        return;
    }

    auto instance = std::make_unique<Instance>();
    instance->entity = entity;
    instance->guid = guid;
    // Tabel yang dikembalikan berkas dipakai langsung sebagai `self`. Dua entity
    // yang memakai skrip sama tetap terpisah karena berkasnya dijalankan ulang
    // untuk masing-masing, menghasilkan tabel baru tiap kali.
    instance->self = loaded;
    instance->self["entity"] = static_cast<uint32_t>(entity);
    if (!instance->self["state"].valid()) {
        instance->self["state"] = lua.create_table();
    }
    instances_.push_back(std::move(instance));
}

void ScriptRuntime::Start() {
    instances_.clear();
    if (world_ == nullptr) {
        return;
    }

    for (const auto raw : world_->Registry().view<scene::ScriptComponent>()) {
        const auto entity = static_cast<scene::Entity>(raw);
        auto* component = world_->TryGet<scene::ScriptComponent>(entity);
        if (component == nullptr || !component->script.IsValid()) {
            continue;
        }
        const std::size_t before = instances_.size();
        LoadFor(entity, component->script.guid);
        component->loaded = instances_.size() > before;
    }

    running_ = true;
    for (const auto& instance : instances_) {
        const sol::protected_function start = instance->self["OnStart"];
        if (!start.valid()) {
            continue;
        }
        const sol::protected_function_result result = start(instance->self);
        if (!result.valid()) {
            const sol::error error = result;
            SIM_ERROR("Lua", "OnStart failed: {}", error.what());
            instance->failed = true;
        }
    }
    SIM_INFO("Lua", "Play started with {} script instance(s)", instances_.size());
}

void ScriptRuntime::Update(float deltaSeconds) {
    if (!running_) {
        return;
    }
    for (const auto& instance : instances_) {
        if (instance->failed || !world_->IsAlive(instance->entity)) {
            continue;
        }
        const sol::protected_function update = instance->self["OnUpdate"];
        if (!update.valid()) {
            continue;
        }
        const sol::protected_function_result result = update(instance->self, deltaSeconds);
        if (!result.valid()) {
            const sol::error error = result;
            SIM_ERROR("Lua", "OnUpdate failed: {}", error.what());
            instance->failed = true;
        }
    }
}

void ScriptRuntime::Stop() {
    for (const auto& instance : instances_) {
        if (world_->IsAlive(instance->entity)) {
            if (auto* component = world_->TryGet<scene::ScriptComponent>(instance->entity)) {
                component->loaded = false;
            }
        }
    }
    instances_.clear();
    running_ = false;
}

void ScriptRuntime::Reload(const Uuid& scriptGuid) {
    if (!running_) {
        return;
    }
    // Tabel `state` tiap instance diselamatkan lebih dulu. Itulah yang membuat
    // hot reload berguna: kalau state-nya ikut hilang, memuat ulang skrip sama
    // saja dengan memulai ulang permainan.
    std::vector<std::pair<scene::Entity, sol::table>> preserved;
    for (auto it = instances_.begin(); it != instances_.end();) {
        if ((*it)->guid == scriptGuid) {
            preserved.emplace_back((*it)->entity, (*it)->self["state"]);
            it = instances_.erase(it);
        } else {
            ++it;
        }
    }
    if (preserved.empty()) {
        return;
    }

    for (const auto& [entity, state] : preserved) {
        const std::size_t before = instances_.size();
        LoadFor(entity, scriptGuid);
        if (instances_.size() > before) {
            instances_.back()->self["state"] = state;
            const sol::protected_function start = instances_.back()->self["OnStart"];
            if (start.valid()) {
                const sol::protected_function_result result = start(instances_.back()->self);
                if (!result.valid()) {
                    const sol::error error = result;
                    SIM_ERROR("Lua", "OnStart failed after reload: {}", error.what());
                    instances_.back()->failed = true;
                }
            }
        }
    }
    SIM_INFO("Lua", "Reloaded {} script instance(s)", preserved.size());
}

namespace {

/// Kedalaman maksimum saat memecah tabel untuk ditampilkan.
///
/// Tabel Lua boleh menunjuk dirinya sendiri, dan tanpa batas ini mengetik `_G`
/// di konsol akan berputar sampai kehabisan memori.
constexpr int kMaxTableDepth = 3;

/// Teks satu nilai Lua, menghormati metamethod __tostring.
std::string ToDisplayString(lua_State* L, int index) {
    const int absolute = lua_absindex(L, index);
    const char* text = luaL_tolstring(L, absolute, nullptr);
    std::string result = text != nullptr ? text : "nil";
    lua_pop(L, 1);
    return result;
}

EvalNode DescribeValue(const sol::object& value, std::string label, int depth) {
    EvalNode node;
    node.label = std::move(label);
    lua_State* L = value.lua_state();
    value.push();
    node.value = ToDisplayString(L, -1);
    lua_pop(L, 1);

    if (value.get_type() != sol::type::table || depth >= kMaxTableDepth) {
        return node;
    }
    const sol::table table = value.as<sol::table>();
    for (const auto& [key, entry] : table) {
        key.push();
        std::string keyText = ToDisplayString(L, -1);
        lua_pop(L, 1);
        node.children.push_back(DescribeValue(entry, std::move(keyText), depth + 1));
    }
    // Urutan iterasi tabel Lua tidak ditentukan; tanpa pengurutan, isi tabel
    // yang sama tampil berbeda tiap kali dievaluasi.
    std::sort(node.children.begin(), node.children.end(),
              [](const EvalNode& a, const EvalNode& b) { return a.label < b.label; });
    return node;
}

}  // namespace

EvalResult ScriptRuntime::Evaluate(std::string_view code) {
    EvalResult result;
    sol::state& lua = *vm_->State();

    sol::load_result chunk = lua.load("return " + std::string(code), "=(repl)");
    if (!chunk.valid()) {
        // Bukan ekspresi. Kesalahan dari percobaan pertama sengaja dibuang:
        // yang dilihat pengguna harus kesalahan kodenya sendiri, bukan
        // kesalahan sintaks dari `return` yang kita sisipkan.
        chunk = lua.load(code, "=(repl)");
    }
    if (!chunk.valid()) {
        const sol::error error = chunk;
        result.ok = false;
        result.error = error.what();
        return result;
    }

    const sol::protected_function function = chunk;
    const sol::protected_function_result call = function();
    if (!call.valid()) {
        const sol::error error = call;
        result.ok = false;
        result.error = error.what();
        return result;
    }

    const int count = call.return_count();
    for (int i = 0; i < count; ++i) {
        const sol::object value = call.get<sol::object>(i);
        result.values.push_back(DescribeValue(value, {}, 0));
    }
    return result;
}

std::vector<std::string> ScriptRuntime::Complete(std::string_view prefix) {
    std::vector<std::string> matches;
    sol::state& lua = *vm_->State();

    // Bagian sebelum titik terakhir menentukan tabel yang ditelusuri, sisanya
    // adalah awalan yang dicocokkan.
    const std::size_t dot = prefix.rfind('.');
    std::string_view partial = prefix;
    sol::table scope = lua.globals();
    if (dot != std::string_view::npos) {
        sol::object current = lua.globals();
        std::string_view path = prefix.substr(0, dot);
        partial = prefix.substr(dot + 1);
        std::size_t start = 0;
        while (start <= path.size()) {
            const std::size_t next = path.find('.', start);
            const std::string_view part =
                path.substr(start, next == std::string_view::npos ? path.size() - start
                                                                  : next - start);
            if (current.get_type() != sol::type::table) {
                return matches;
            }
            current = current.as<sol::table>()[std::string(part)];
            if (next == std::string_view::npos) {
                break;
            }
            start = next + 1;
        }
        if (current.get_type() != sol::type::table) {
            return matches;
        }
        scope = current.as<sol::table>();
    }

    const std::string head(prefix.substr(0, prefix.size() - partial.size()));
    for (const auto& [key, value] : scope) {
        if (key.get_type() != sol::type::string) {
            continue;
        }
        const std::string name = key.as<std::string>();
        if (name.compare(0, partial.size(), partial) == 0) {
            matches.push_back(head + name);
        }
    }
    std::sort(matches.begin(), matches.end());
    return matches;
}

}  // namespace sim::script
