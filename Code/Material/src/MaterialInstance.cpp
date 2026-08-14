#include "Sim/Material/MaterialInstance.h"

#include "Sim/Core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace sim::material {
namespace {

using Json = nlohmann::ordered_json;

/// Angka pertama di dalam `text` mulai dari `pos`, atau nullopt.
///
/// Ditulis sendiri, bukan lewat sscanf: yang diurai adalah teks milik pengguna,
/// dan `strtof` bergantung pada locale — di locale yang memakai koma sebagai
/// pemisah desimal, "0.8" terbaca sebagai 0. Berkas yang isinya berubah arti
/// menurut setelan sistem adalah kelas bug yang tidak boleh dibuka.
bool NextNumber(std::string_view text, std::size_t& pos, float& out) {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == ',' || text[pos] == '(')) {
        ++pos;
    }
    const std::size_t begin = pos;
    while (pos < text.size() && text[pos] != ',' && text[pos] != ')' && text[pos] != ' ') {
        ++pos;
    }
    if (begin == pos) {
        return false;
    }
    const std::string_view token = text.substr(begin, pos - begin);
    const auto result = std::from_chars(token.data(), token.data() + token.size(), out);
    return result.ec == std::errc{};
}

/// Teks sebuah float tanpa nol berlebih di belakang, dan tanpa bergantung
/// locale — alasan yang sama dengan NextNumber.
std::string FormatFloat(float value) {
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    std::string text(buffer.data(), result.ptr);
    // Slang menerima "1" untuk float, tapi menulis "1.0" membuat berkasnya
    // terbaca sebagai angka pecahan sekali lihat.
    if (text.find('.') == std::string::npos && text.find('e') == std::string::npos) {
        text += ".0";
    }
    return text;
}

}  // namespace

MaterialValue ParseValue(ValueKind kind, std::string_view text) {
    MaterialValue value;
    value.kind = kind;

    const int wanted = ComponentCount(kind);
    std::size_t pos = 0;
    // Melewati nama tipe di depan, bila ada: "float3(...)" maupun "(...)"
    // maupun "0.8" sama-sama diterima.
    const std::size_t open = text.find('(');
    if (open != std::string_view::npos) {
        pos = open + 1;
    }

    std::array<float, 4> parsed{0.0f, 0.0f, 0.0f, 0.0f};
    int count = 0;
    float number = 0.0f;
    while (count < 4 && NextNumber(text, pos, number)) {
        parsed[static_cast<std::size_t>(count++)] = number;
    }
    if (count == 0) {
        return value;
    }
    // Satu angka mengisi seluruh komponen — `float3(0.8)` berarti (0.8,0.8,0.8),
    // sama seperti di Slang.
    if (count == 1) {
        parsed.fill(parsed[0]);
    }
    for (int i = 0; i < wanted && i < 4; ++i) {
        value.components[static_cast<std::size_t>(i)] = parsed[static_cast<std::size_t>(i)];
    }
    return value;
}

std::string FormatValue(const MaterialValue& value) {
    const int count = ComponentCount(value.kind);
    if (count <= 1) {
        return FormatFloat(value.components[0]);
    }
    std::string text = std::string(ToString(value.kind)) + "(";
    for (int i = 0; i < count; ++i) {
        if (i > 0) {
            text += ", ";
        }
        text += FormatFloat(value.components[static_cast<std::size_t>(i)]);
    }
    return text + ")";
}

const ParameterOverride* MaterialInstance::Find(std::string_view name) const {
    const auto it = std::find_if(
        overrides.begin(), overrides.end(),
        [name](const ParameterOverride& entry) { return entry.name == name; });
    return it == overrides.end() ? nullptr : &*it;
}

void MaterialInstance::Set(std::string_view name, const MaterialValue& value) {
    const auto it = std::find_if(
        overrides.begin(), overrides.end(),
        [name](const ParameterOverride& entry) { return entry.name == name; });
    if (it != overrides.end()) {
        it->value = value;
        return;
    }
    overrides.push_back({std::string(name), value});
}

void MaterialInstance::Clear(std::string_view name) {
    overrides.erase(std::remove_if(overrides.begin(), overrides.end(),
                                   [name](const ParameterOverride& entry) {
                                       return entry.name == name;
                                   }),
                    overrides.end());
}

Uuid MaterialInstance::Texture(const Uuid& node) const {
    const auto it = std::find_if(textures.begin(), textures.end(),
                                 [&node](const TextureOverride& entry) {
                                     return entry.node == node;
                                 });
    return it == textures.end() ? Uuid{} : it->texture;
}

void MaterialInstance::SetTexture(const Uuid& node, const Uuid& texture) {
    if (!node.IsValid()) {
        return;
    }
    const auto it = std::find_if(textures.begin(), textures.end(),
                                 [&node](const TextureOverride& entry) {
                                     return entry.node == node;
                                 });
    if (!texture.IsValid()) {
        // GUID tak sah membuang penggantiannya alih-alih menyimpan entri
        // kosong. Dua cara menyatakan "pakai punya induk" berarti setiap
        // pembaca harus memeriksa keduanya, dan yang memeriksa satu saja akan
        // salah separuh waktu.
        if (it != textures.end()) {
            textures.erase(it);
        }
        return;
    }
    if (it != textures.end()) {
        it->texture = texture;
        return;
    }
    textures.push_back({node, texture});
}

std::vector<ResolvedParameter> ResolveParameters(const MaterialGraph& parent,
                                                 const MaterialInstance& instance) {
    std::vector<ResolvedParameter> result;
    result.reserve(parent.parameters.size());

    for (const MaterialParameter& declared : parent.parameters) {
        ResolvedParameter resolved;
        resolved.name = declared.name;
        resolved.kind = declared.kind;
        resolved.tooltip = declared.tooltip;
        resolved.minValue = declared.minValue;
        resolved.maxValue = declared.maxValue;
        resolved.isColor = declared.isColor;
        resolved.value = ParseValue(declared.kind, declared.defaultValue);

        // Timpaan yang tipenya tidak lagi cocok diabaikan: sebuah angka yang
        // menjadi warna tidak punya nilai lama yang masuk akal, dan memakainya
        // apa adanya akan menaruh satu komponen ke tempat yang salah.
        if (const ParameterOverride* over = instance.Find(declared.name);
            over != nullptr && over->value.kind == declared.kind) {
            resolved.value = over->value;
            resolved.overridden = true;
        }
        result.push_back(std::move(resolved));
    }
    return result;
}

std::string SaveInstanceToString(const MaterialInstance& instance) {
    Json root;
    root["version"] = kInstanceSchemaVersion;
    root["parent"] = instance.parent.ToString();

    Json overrides = Json::array();
    for (const ParameterOverride& entry : instance.overrides) {
        Json object;
        object["name"] = entry.name;
        object["kind"] = ToString(entry.value.kind);
        object["value"] = FormatValue(entry.value);
        overrides.push_back(std::move(object));
    }
    root["overrides"] = std::move(overrides);

    // Ditulis hanya bila ada. Berkas instance dari sebelum tekstur ada tetap
    // terbaca, dan yang tidak memasang tekstur tetap sekecil sebelumnya.
    if (!instance.textures.empty()) {
        Json textures = Json::array();
        for (const TextureOverride& entry : instance.textures) {
            Json object;
            object["node"] = entry.node.ToString();
            object["texture"] = entry.texture.ToString();
            textures.push_back(std::move(object));
        }
        root["textures"] = std::move(textures);
    }

    return root.dump(2) + "\n";
}

MaterialIoResult SaveInstanceToFile(const MaterialInstance& instance,
                                    const std::filesystem::path& path) {
    MaterialIoResult result;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        result.error = "cannot open " + path.string();
        return result;
    }
    stream << SaveInstanceToString(instance);
    result.ok = static_cast<bool>(stream);
    if (!result.ok) {
        result.error = "write failed: " + path.string();
    }
    return result;
}

MaterialIoResult LoadInstanceFromString(MaterialInstance& instance, const std::string& text) {
    MaterialIoResult result;
    Json root;
    try {
        root = Json::parse(text);
    } catch (const nlohmann::json::exception& error) {
        result.error = error.what();
        return result;
    }
    if (!root.is_object()) {
        result.error = "root is not an object";
        return result;
    }

    instance = MaterialInstance{};
    result.sourceVersion = root.value("version", kInstanceSchemaVersion);
    instance.parent = Uuid::Parse(root.value("parent", std::string{}));
    if (!instance.parent.IsValid()) {
        result.error = "instance has no parent material";
        return result;
    }

    if (const auto overrides = root.find("overrides");
        overrides != root.end() && overrides->is_array()) {
        for (const Json& entry : *overrides) {
            ParameterOverride over;
            over.name = entry.value("name", std::string{});
            if (over.name.empty()) {
                continue;
            }
            const ValueKind kind = ValueKindFromString(entry.value("kind", std::string("float")));
            over.value = ParseValue(kind, entry.value("value", std::string{}));
            // Timpaan kedua untuk parameter yang sama dibuang, sejalan dengan
            // dua kabel ke satu pin: yang kedua akan diam-diam dikalahkan yang
            // pertama, dan perilaku yang ditentukan urutan penyimpanan tidak
            // terlihat di panel mana pun.
            if (instance.Find(over.name) != nullptr) {
                SIM_WARN("Material", "Dropping a second override for '{}'", over.name);
                continue;
            }
            instance.overrides.push_back(std::move(over));
        }
    }

    if (const auto textures = root.find("textures");
        textures != root.end() && textures->is_array()) {
        for (const Json& entry : *textures) {
            const Uuid node = Uuid::Parse(entry.value("node", std::string{}));
            const Uuid texture = Uuid::Parse(entry.value("texture", std::string{}));
            if (!node.IsValid() || !texture.IsValid()) {
                continue;
            }
            // Alasan yang sama dengan timpaan ganda di atas: yang kedua akan
            // diam-diam dikalahkan yang pertama, dan perilaku yang ditentukan
            // urutan penyimpanan tidak terlihat di panel mana pun.
            if (instance.Texture(node).IsValid()) {
                SIM_WARN("Material", "Dropping a second texture for node {}", node.ToString());
                continue;
            }
            instance.textures.push_back({node, texture});
        }
    }

    result.ok = true;
    return result;
}

MaterialIoResult LoadInstanceFromFile(MaterialInstance& instance,
                                      const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        MaterialIoResult result;
        result.error = "cannot open " + path.string();
        return result;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return LoadInstanceFromString(instance, buffer.str());
}

}  // namespace sim::material
