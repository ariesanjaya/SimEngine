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

/// Membaca deklarasi `properties` sebuah skrip menjadi daftar C++.
///
/// Tipenya disimpulkan dari nilai bawaannya, bukan dituliskan terpisah.
/// `properties = { speed = 1.5, loop = true }` sudah memuat keduanya, dan
/// menuntut pengguna mengulang tipe di sebelah nilainya hanya membuka jalan
/// bagi keduanya untuk bertentangan.
std::vector<scene::ScriptProperty> ReadDeclaration(const sol::table& table) {
    std::vector<scene::ScriptProperty> result;
    const sol::object declared = table["properties"];
    if (!declared.valid() || declared.get_type() != sol::type::table) {
        return result;
    }
    for (const auto& [key, value] : declared.as<sol::table>()) {
        if (key.get_type() != sol::type::string) {
            continue;
        }
        scene::ScriptProperty property;
        property.name = key.as<std::string>();
        switch (value.get_type()) {
            case sol::type::boolean:
                property.kind = scene::ScriptPropertyKind::Bool;
                property.flag = value.as<bool>();
                break;
            case sol::type::number:
                property.kind = scene::ScriptPropertyKind::Number;
                property.number = value.as<float>();
                break;
            case sol::type::string:
                property.kind = scene::ScriptPropertyKind::Text;
                property.text = value.as<std::string>();
                break;
            // Fungsi, tabel bersarang, dan userdata tidak punya widget yang
            // masuk akal di Inspector, jadi dilewati tanpa keluhan — skrip
            // boleh menaruh apa pun di tabelnya sendiri.
            default: continue;
        }
        result.push_back(std::move(property));
    }
    // Urutan iterasi tabel Lua tidak ditentukan. Tanpa pengurutan, daftar di
    // Inspector berganti susunan setiap kali skripnya dimuat ulang.
    std::sort(result.begin(), result.end(),
              [](const scene::ScriptProperty& a, const scene::ScriptProperty& b) {
                  return a.name < b.name;
              });
    return result;
}

sol::object ToLuaValue(sol::state& lua, const scene::ScriptProperty& property) {
    switch (property.kind) {
        case scene::ScriptPropertyKind::Bool: return sol::make_object(lua, property.flag);
        case scene::ScriptPropertyKind::Text: return sol::make_object(lua, property.text);
        case scene::ScriptPropertyKind::Number: break;
    }
    return sol::make_object(lua, property.number);
}

/// Pustaka matematika, ditulis dalam Lua dan bukan sebagai usertype C++.
///
/// Alasannya bukan kemalasan. `sim.get_component` mengembalikan tabel biasa
/// `{x=, y=, z=}`, dan sebuah usertype akan menjadi tipe KEDUA yang harus
/// dikonversi bolak-balik di setiap perbatasan — persis kelas bug yang paling
/// mudah dibuat dan paling sulit dilihat. Dengan operasi yang bekerja pada
/// tabel yang sama, nilai yang dibaca dari komponen bisa langsung dihitung dan
/// langsung ditulis kembali.
///
/// Metatable dipasang hanya oleh konstruktor. Tabel hasil `get_component` tetap
/// polos, jadi `+` padanya gagal dengan pesan Lua yang biasa alih-alih diam
/// menghasilkan angka yang salah.
constexpr const char* kMathPrelude = R"LUA(
local vec3_mt = {}
vec3_mt.__index = vec3_mt

local function vec3(x, y, z)
    return setmetatable({ x = x or 0.0, y = y or 0.0, z = z or 0.0 }, vec3_mt)
end

function vec3_mt.__add(a, b) return vec3(a.x + b.x, a.y + b.y, a.z + b.z) end
function vec3_mt.__sub(a, b) return vec3(a.x - b.x, a.y - b.y, a.z - b.z) end
function vec3_mt.__unm(a) return vec3(-a.x, -a.y, -a.z) end
function vec3_mt.__eq(a, b) return a.x == b.x and a.y == b.y and a.z == b.z end
function vec3_mt.__tostring(a)
    return string.format("vec3(%.3f, %.3f, %.3f)", a.x, a.y, a.z)
end

-- Perkalian menerima skalar di sisi mana pun: `2 * v` sama sahnya dengan `v * 2`.
function vec3_mt.__mul(a, b)
    if type(a) == "number" then a, b = b, a end
    if type(b) == "number" then return vec3(a.x * b, a.y * b, a.z * b) end
    return vec3(a.x * b.x, a.y * b.y, a.z * b.z)
end

function vec3_mt:dot(b) return self.x * b.x + self.y * b.y + self.z * b.z end
function vec3_mt:length() return math.sqrt(self:dot(self)) end
function vec3_mt:cross(b)
    return vec3(self.y * b.z - self.z * b.y,
                self.z * b.x - self.x * b.z,
                self.x * b.y - self.y * b.x)
end
function vec3_mt:normalized()
    local n = self:length()
    -- Vektor nol dikembalikan apa adanya. Membaginya menghasilkan nan yang
    -- menjalar diam-diam ke transform dan baru terlihat sebagai objek hilang.
    if n < 1e-8 then return vec3(0, 0, 0) end
    return vec3(self.x / n, self.y / n, self.z / n)
end

local quat_mt = {}
quat_mt.__index = quat_mt

local function quat(w, x, y, z)
    return setmetatable({ w = w or 1.0, x = x or 0.0, y = y or 0.0, z = z or 0.0 }, quat_mt)
end

function quat_mt.__mul(a, b)
    return quat(a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
                a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w)
end
function quat_mt.__tostring(a)
    return string.format("quat(%.3f, %.3f, %.3f, %.3f)", a.w, a.x, a.y, a.z)
end

-- Memutar sebuah vektor: v' = q * v * q⁻¹, ditulis dalam bentuk yang sudah
-- disederhanakan supaya tidak membuat dua quaternion sementara tiap panggilan.
function quat_mt:rotate(v)
    local u = vec3(self.x, self.y, self.z)
    local s = self.w
    return u * (2.0 * u:dot(v)) + v * (s * s - u:dot(u)) + u:cross(v) * (2.0 * s)
end

local function axis_angle(axis, radians)
    local a = axis:normalized()
    local half = radians * 0.5
    local s = math.sin(half)
    return quat(math.cos(half), a.x * s, a.y * s, a.z * s)
end

sim.vec3 = vec3
sim.quat = quat
sim.axis_angle = axis_angle
sim.up = function() return vec3(0, 1, 0) end
sim.right = function() return vec3(1, 0, 0) end
sim.forward = function() return vec3(0, 0, -1) end
)LUA";

}  // namespace

/// Satu skrip yang menempel pada satu entity.
struct ScriptRuntime::Instance {
    scene::Entity entity = scene::kNullEntity;
    Uuid guid;
    sol::table self;
    /// Berasal dari GraphComponent, bukan ScriptComponent. Dua komponen yang
    /// bentuknya sama tapi berbeda tempat menyimpan properti dan flag `loaded`.
    bool fromGraph = false;
    /// Dimatikan setelah melempar kesalahan. Instance yang gagal tiap frame
    /// akan membanjiri Console enam puluh kali per detik dengan pesan yang sama,
    /// dan pesan pertama — satu-satunya yang berguna — tenggelam seketika.
    bool failed = false;
};

ScriptRuntime::ScriptRuntime() : vm_(std::make_unique<LuaVM>()) {}
ScriptRuntime::~ScriptRuntime() {
    Shutdown();
}

bool ScriptRuntime::Initialize(scene::World& world, assets::AssetDatabase* assets,
                               std::filesystem::path graphCacheDir) {
    world_ = &world;
    assets_ = assets;
    if (!graphCacheDir.empty()) {
        graphs_.Initialize(std::move(graphCacheDir));
    }
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

    // Waktu sejak Play ditekan. `dt` sudah diberikan ke OnUpdate; yang ini
    // untuk skrip yang butuh waktu absolut tanpa menghitungnya sendiri di
    // `self.state` — dan yang perlu nilai yang sama di seluruh skrip pada frame
    // yang sama, bukan akumulasi masing-masing yang perlahan menyimpang.
    sim.set_function("time", [this]() { return elapsed_; });

    const std::string error = vm_->RunString(kMathPrelude, "=(sim.math)");
    if (!error.empty()) {
        SIM_ERROR("Lua", "Math prelude failed: {}", error);
    }
}

std::filesystem::path ScriptRuntime::ResolveChunk(const Uuid& guid, std::string& chunkName,
                                                 bool& fromGraph) {
    if (assets_ == nullptr) {
        return {};
    }
    const assets::AssetRecord* record = assets_->Find(guid);
    if (record == nullptr) {
        SIM_WARN("Lua", "Asset {} is missing", guid.ToString());
        return {};
    }
    const std::filesystem::path source = assets_->AbsolutePath(*record);

    if (record->type != assets::AssetType::Graph) {
        fromGraph = false;
        chunkName = record->relativePath;
        return source;
    }

    // Graph tidak dijalankan langsung; yang dijalankan adalah hasil
    // kompilasinya. EnsureCompiled hanya mengompilasi bila hasil lamanya sudah
    // usang, sehingga memuat level yang graph-nya tidak disunting tidak
    // mengompilasi apa pun.
    fromGraph = true;
    chunkName = GraphCache::ChunkName(record->relativePath);
    return graphs_.EnsureCompiled(guid, source);
}

void ScriptRuntime::LoadFor(scene::Entity entity, const Uuid& guid) {
    std::string chunkName;
    bool fromGraph = false;
    const std::filesystem::path file = ResolveChunk(guid, chunkName, fromGraph);
    if (file.empty()) {
        return;
    }
    LoadChunk(entity, guid, file, chunkName, fromGraph);
}

void ScriptRuntime::LoadChunk(scene::Entity entity, const Uuid& guid,
                              const std::filesystem::path& file, const std::string& chunkName,
                              bool fromGraph) {
    std::ifstream stream(file);
    if (!stream) {
        SIM_WARN("Lua", "Cannot open {}", chunkName);
        return;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();

    sol::state& lua = *vm_->State();
    // Nama chunk diawali '@' supaya Lua memperlakukannya sebagai nama berkas,
    // sehingga traceback menyebut "arena.lua:12" alih-alih menyalin seluruh
    // sumbernya ke dalam pesan kesalahan.
    const sol::protected_function_result loaded =
        lua.safe_script(buffer.str(), sol::script_pass_on_error, "@" + chunkName);
    if (!loaded.valid()) {
        const sol::error error = loaded;
        SIM_ERROR("Lua", "{}: {}", chunkName, error.what());
        return;
    }
    if (loaded.get_type() != sol::type::table) {
        SIM_WARN("Lua", "{} did not return a table; nothing to run", chunkName);
        return;
    }

    auto instance = std::make_unique<Instance>();
    instance->entity = entity;
    instance->guid = guid;
    instance->fromGraph = fromGraph;
    // Tabel yang dikembalikan berkas dipakai langsung sebagai `self`. Dua entity
    // yang memakai skrip sama tetap terpisah karena berkasnya dijalankan ulang
    // untuk masing-masing, menghasilkan tabel baru tiap kali.
    instance->self = loaded;
    instance->self["entity"] = static_cast<uint32_t>(entity);
    if (!instance->self["state"].valid()) {
        instance->self["state"] = lua.create_table();
    }

    // `self.props` berisi nilai yang BERLAKU: bawaan dari deklarasi skrip,
    // ditimpa nilai yang disimpan entity ini. Skrip membacanya tanpa perlu tahu
    // mana yang datang dari mana — dan entity yang belum pernah disunting ikut
    // berubah ketika bawaan di skripnya diubah.
    sol::table props = lua.create_table();
    for (const scene::ScriptProperty& declared : ReadDeclaration(instance->self)) {
        props[declared.name] = ToLuaValue(lua, declared);
    }
    const std::vector<scene::ScriptProperty>* stored = nullptr;
    if (fromGraph) {
        if (const auto* component = world_->TryGet<scene::GraphComponent>(entity)) {
            stored = &component->properties;
        }
    } else if (const auto* component = world_->TryGet<scene::ScriptComponent>(entity)) {
        stored = &component->properties;
    }
    if (stored != nullptr) {
        for (const scene::ScriptProperty& value : *stored) {
            props[value.name] = ToLuaValue(lua, value);
        }
    }
    instance->self["props"] = props;

    instances_.push_back(std::move(instance));
}

std::vector<scene::ScriptProperty> ScriptRuntime::DeclaredProperties(const Uuid& scriptGuid) {
    std::string chunkName;
    bool fromGraph = false;
    const std::filesystem::path file = ResolveChunk(scriptGuid, chunkName, fromGraph);
    if (file.empty()) {
        return {};
    }
    std::ifstream stream(file);
    if (!stream) {
        return {};
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();

    const sol::protected_function_result loaded =
        vm_->State()->safe_script(buffer.str(), sol::script_pass_on_error, "@" + chunkName);
    if (!loaded.valid() || loaded.get_type() != sol::type::table) {
        return {};
    }
    return ReadDeclaration(loaded);
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

    // Graph menempuh jalur yang sama persis: yang berbeda hanya berkas yang
    // dijalankan — hasil kompilasinya, bukan aset yang dirujuk.
    for (const auto raw : world_->Registry().view<scene::GraphComponent>()) {
        const auto entity = static_cast<scene::Entity>(raw);
        auto* component = world_->TryGet<scene::GraphComponent>(entity);
        if (component == nullptr || !component->graph.IsValid()) {
            continue;
        }
        const std::size_t before = instances_.size();
        LoadFor(entity, component->graph.guid);
        component->loaded = instances_.size() > before;
    }

    elapsed_ = 0.0f;
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
    elapsed_ += deltaSeconds;
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
        if (!world_->IsAlive(instance->entity)) {
            continue;
        }
        if (instance->fromGraph) {
            if (auto* component = world_->TryGet<scene::GraphComponent>(instance->entity)) {
                component->loaded = false;
            }
        } else if (auto* component =
                       world_->TryGet<scene::ScriptComponent>(instance->entity)) {
            component->loaded = false;
        }
    }
    instances_.clear();
    running_ = false;
}

void ScriptRuntime::Reload(const Uuid& assetGuid) {
    if (!running_) {
        return;
    }
    // Tabel `state` tiap instance diselamatkan lebih dulu. Itulah yang membuat
    // hot reload berguna: kalau state-nya ikut hilang, memuat ulang skrip sama
    // saja dengan memulai ulang permainan.
    std::vector<std::pair<scene::Entity, sol::table>> preserved;
    for (auto it = instances_.begin(); it != instances_.end();) {
        if ((*it)->guid == assetGuid) {
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
        LoadFor(entity, assetGuid);
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

std::string ScriptRuntime::CheckSyntax(std::string_view code, std::string_view chunkName) {
    const sol::load_result chunk =
        vm_->State()->load(code, "@" + std::string(chunkName), sol::load_mode::text);
    if (chunk.valid()) {
        return {};
    }
    const sol::error error = chunk;
    return error.what();
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
