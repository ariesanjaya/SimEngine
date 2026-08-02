#include "Sim/Scene/Serialization.h"

#include "Sim/Core/Log.h"
#include "Sim/Scene/ComponentRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace sim::scene {
namespace {

/// ordered_json, bukan json biasa: yang biasa mengurutkan kunci menurut abjad,
/// sehingga "Transform" selalu muncul sebelum "Name" dan berkasnya jadi lebih
/// sulit dibaca. Keduanya sama-sama deterministik; yang ini mempertahankan
/// urutan pendaftaran komponen, yang memang urutan bermaknanya.
using Json = nlohmann::ordered_json;

Json WriteVec(const float* values, int count) {
    Json array = Json::array();
    for (int i = 0; i < count; ++i) {
        array.push_back(values[i]);
    }
    return array;
}

void ReadVec(const Json& array, float* values, int count) {
    if (!array.is_array()) {
        return;
    }
    const int available = std::min(count, static_cast<int>(array.size()));
    for (int i = 0; i < available; ++i) {
        if (array[static_cast<std::size_t>(i)].is_number()) {
            values[i] = array[static_cast<std::size_t>(i)].get<float>();
        }
    }
}

Json WriteField(const reflect::FieldDesc& field, const void* data);
void ReadField(const reflect::FieldDesc& field, const Json& value, void* data);

Json WriteValue(reflect::FieldKind kind, const reflect::FieldDesc& field, const void* data) {
    using reflect::FieldKind;
    switch (kind) {
        case FieldKind::Bool: return *static_cast<const bool*>(data);
        case FieldKind::Int: return *static_cast<const int32_t*>(data);
        case FieldKind::UInt: return *static_cast<const uint32_t*>(data);
        case FieldKind::Float: return *static_cast<const float*>(data);
        case FieldKind::Vec2: return WriteVec(&static_cast<const Vec2*>(data)->x, 2);
        case FieldKind::Vec3: return WriteVec(&static_cast<const Vec3*>(data)->x, 3);
        case FieldKind::Vec4: return WriteVec(&static_cast<const Vec4*>(data)->x, 4);
        case FieldKind::Quat: {
            const auto* quat = static_cast<const Quat*>(data);
            // w lebih dulu: urutan yang sama dengan cara quaternion ditulis di
            // hampir semua literatur, dan glm menyimpannya x,y,z,w sehingga
            // menulis apa adanya akan menyesatkan pembaca berkas.
            return Json::array({quat->w, quat->x, quat->y, quat->z});
        }
        case FieldKind::String: return *static_cast<const std::string*>(data);
        case FieldKind::Uuid: return static_cast<const Uuid*>(data)->ToString();
        case FieldKind::Enum: {
            // Enum ditulis sebagai nama, bukan angka: menyisipkan nilai baru di
            // tengah daftar tidak boleh mengubah arti berkas yang sudah ada.
            const auto value = static_cast<std::size_t>(*static_cast<const uint8_t*>(data));
            const auto& names = field.attributes.enumNames;
            return value < names.size() ? Json(names[value]) : Json(static_cast<int>(value));
        }
        case FieldKind::Struct: {
            const reflect::TypeDesc* type = reflect::TypeRegistry::Get().Find(field.type);
            Json object = Json::object();
            if (type != nullptr) {
                for (const reflect::FieldDesc& nested : type->fields) {
                    object[nested.name] = WriteField(nested, nested.AccessConst(data));
                }
            }
            return object;
        }
        case FieldKind::Vector: break;  // ditangani WriteField
    }
    return nullptr;
}

Json WriteField(const reflect::FieldDesc& field, const void* data) {
    if (field.kind != reflect::FieldKind::Vector) {
        return WriteValue(field.kind, field, data);
    }
    const reflect::VectorOps* ops = field.vector;
    Json array = Json::array();
    if (ops == nullptr) {
        return array;
    }
    const std::size_t count = ops->size(data);
    for (std::size_t i = 0; i < count; ++i) {
        reflect::FieldDesc element = field;
        element.kind = ops->elementKind;
        element.type = ops->elementType;
        element.vector = nullptr;
        array.push_back(WriteValue(ops->elementKind, element, ops->atConst(data, i)));
    }
    return array;
}

void ReadValue(reflect::FieldKind kind, const reflect::FieldDesc& field, const Json& value,
               void* data) {
    using reflect::FieldKind;
    switch (kind) {
        case FieldKind::Bool:
            if (value.is_boolean()) {
                *static_cast<bool*>(data) = value.get<bool>();
            }
            break;
        case FieldKind::Int:
            if (value.is_number_integer()) {
                *static_cast<int32_t*>(data) = value.get<int32_t>();
            }
            break;
        case FieldKind::UInt:
            if (value.is_number()) {
                *static_cast<uint32_t*>(data) = value.get<uint32_t>();
            }
            break;
        case FieldKind::Float:
            if (value.is_number()) {
                *static_cast<float*>(data) = value.get<float>();
            }
            break;
        case FieldKind::Vec2: ReadVec(value, &static_cast<Vec2*>(data)->x, 2); break;
        case FieldKind::Vec3: ReadVec(value, &static_cast<Vec3*>(data)->x, 3); break;
        case FieldKind::Vec4: ReadVec(value, &static_cast<Vec4*>(data)->x, 4); break;
        case FieldKind::Quat: {
            if (value.is_array() && value.size() >= 4) {
                auto* quat = static_cast<Quat*>(data);
                quat->w = value[0].get<float>();
                quat->x = value[1].get<float>();
                quat->y = value[2].get<float>();
                quat->z = value[3].get<float>();
            }
            break;
        }
        case FieldKind::String:
            if (value.is_string()) {
                *static_cast<std::string*>(data) = value.get<std::string>();
            }
            break;
        case FieldKind::Uuid:
            if (value.is_string()) {
                *static_cast<Uuid*>(data) = Uuid::Parse(value.get<std::string>());
            }
            break;
        case FieldKind::Enum: {
            const auto& names = field.attributes.enumNames;
            if (value.is_string()) {
                const std::string name = value.get<std::string>();
                const auto it = std::find(names.begin(), names.end(), name);
                if (it != names.end()) {
                    *static_cast<uint8_t*>(data) =
                        static_cast<uint8_t>(std::distance(names.begin(), it));
                }
            } else if (value.is_number_integer()) {
                // Berkas dari sebelum enum ditulis sebagai nama.
                *static_cast<uint8_t*>(data) = static_cast<uint8_t>(value.get<int>());
            }
            break;
        }
        case FieldKind::Struct: {
            const reflect::TypeDesc* type = reflect::TypeRegistry::Get().Find(field.type);
            if (type == nullptr || !value.is_object()) {
                break;
            }
            for (const reflect::FieldDesc& nested : type->fields) {
                const auto it = value.find(nested.name);
                if (it != value.end()) {
                    ReadField(nested, *it, nested.access(data));
                }
            }
            break;
        }
        case FieldKind::Vector: break;
    }
}

void ReadField(const reflect::FieldDesc& field, const Json& value, void* data) {
    if (field.kind != reflect::FieldKind::Vector) {
        ReadValue(field.kind, field, value, data);
        return;
    }
    const reflect::VectorOps* ops = field.vector;
    if (ops == nullptr || !value.is_array()) {
        return;
    }
    ops->resize(data, value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        reflect::FieldDesc element = field;
        element.kind = ops->elementKind;
        element.type = ops->elementType;
        element.vector = nullptr;
        ReadValue(ops->elementKind, element, value[i], ops->at(data, i));
    }
}

/// Menelusuri hierarki dari akar, induk selalu sebelum anak.
///
/// Urutan ini yang membuat pemuatan bisa satu lintasan: saat sebuah entity
/// menyebut GUID induknya, induk itu pasti sudah dibuat.
void CollectDepthFirst(const World& world, Entity entity, std::vector<Entity>& out) {
    out.push_back(entity);
    for (const Entity child : world.ChildrenOf(entity)) {
        CollectDepthFirst(world, child, out);
    }
}

/// Menulis daftar entity yang sudah terurut menjadi dokumen level.
Json WriteEntities(const World& world, const std::vector<Entity>& ordered) {
    Json entities = Json::array();
    entities.get_ref<Json::array_t&>().reserve(ordered.size());

    auto& registry = const_cast<World&>(world).Registry();
    for (const Entity entity : ordered) {
        Json record;
        record["guid"] = world.GuidOf(entity).ToString();
        const Entity parent = world.ParentOf(entity);
        if (IsValid(parent)) {
            record["parent"] = world.GuidOf(parent).ToString();
        }

        Json components = Json::object();
        for (const ComponentOps& ops : ComponentRegistry::Get().All()) {
            void* data = ops.tryGet(registry, World::ToEntt(entity));
            if (data == nullptr) {
                continue;
            }
            Json fields = Json::object();
            for (const reflect::FieldDesc& field : ops.type->fields) {
                fields[field.name] = WriteField(field, field.AccessConst(data));
            }
            components[ops.type->name] = std::move(fields);
        }
        record["components"] = std::move(components);
        entities.push_back(std::move(record));
    }

    Json root;
    root["schemaVersion"] = kLevelSchemaVersion;
    root["entities"] = std::move(entities);
    return root;
}

/// Membuat entity dari array JSON. Berkas selalu menulis induk sebelum anak,
/// jadi satu lintasan cukup: saat sebuah entity menyebut GUID induknya, induk
/// itu pasti sudah ada.
///
/// `fallbackParent` dipakai bila GUID induk tidak ditemukan — kasus itu terjadi
/// saat membangun ulang sub-pohon yang induk aslinya ikut terhapus.
bool ReadEntities(World& world, const Json& array, Entity fallbackParent) {
    auto& registry = world.Registry();
    for (const Json& record : array) {
        const Uuid guid = Uuid::Parse(record.value("guid", std::string{}));
        if (!guid.IsValid()) {
            return false;
        }

        Entity parent = fallbackParent;
        if (const auto it = record.find("parent"); it != record.end() && it->is_string()) {
            const Entity found = world.FindByGuid(Uuid::Parse(it->get<std::string>()));
            if (IsValid(found)) {
                parent = found;
            }
        }

        const Entity entity = world.CreateWithGuid(guid, "Entity", parent);

        const auto components = record.find("components");
        if (components == record.end() || !components->is_object()) {
            continue;
        }
        for (const auto& [typeName, fields] : components->items()) {
            const ComponentOps* ops = ComponentRegistry::Get().Find(typeName);
            if (ops == nullptr) {
                // Komponen tak dikenal dilewati, bukan dianggap error: level
                // yang dibuat dengan plugin yang tidak terpasang harus tetap
                // bisa dibuka.
                SIM_WARN("Scene", "Unknown component '{}' skipped", typeName);
                continue;
            }
            void* data = ops->tryGet(registry, World::ToEntt(entity));
            if (data == nullptr) {
                data = ops->emplace(registry, World::ToEntt(entity));
            }
            for (const reflect::FieldDesc& field : ops->type->fields) {
                const auto it = fields.find(field.name);
                if (it != fields.end()) {
                    ReadField(field, *it, field.access(data));
                }
            }
        }
        world.MarkTransformDirty(entity);
    }
    return true;
}

/// Menaikkan berkas versi 1 ke versi 2: rotasi Euler derajat menjadi quaternion.
void MigrateV1ToV2(Json& root) {
    for (Json& entity : root["entities"]) {
        const auto components = entity.find("components");
        if (components == entity.end()) {
            continue;
        }
        const auto transform = components->find("Transform");
        if (transform == components->end()) {
            continue;
        }
        const auto rotation = transform->find("rotation");
        if (rotation == transform->end() || !rotation->is_array() || rotation->size() != 3) {
            continue;
        }
        const Vec3 euler{(*rotation)[0].get<float>() * kDegToRad,
                         (*rotation)[1].get<float>() * kDegToRad,
                         (*rotation)[2].get<float>() * kDegToRad};
        const Quat quaternion(euler);
        *rotation = Json::array({quaternion.w, quaternion.x, quaternion.y, quaternion.z});
    }
}

}  // namespace

std::string SaveLevelToString(const World& world) {
    std::vector<Entity> ordered;
    ordered.reserve(world.Count());
    for (const Entity root : world.Roots()) {
        CollectDepthFirst(world, root, ordered);
    }
    return WriteEntities(world, ordered).dump(2) + "\n";
}

LevelIoResult SaveLevelToFile(const World& world, const std::filesystem::path& path) {
    LevelIoResult result;
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream file(path);
    if (!file) {
        result.error = "cannot open " + path.string() + " for writing";
        return result;
    }
    file << SaveLevelToString(world);
    result.ok = true;
    result.entityCount = world.Count();
    return result;
}

LevelIoResult LoadLevelFromString(World& world, const std::string& text) {
    LevelIoResult result;

    Json root;
    try {
        root = Json::parse(text);
    } catch (const nlohmann::json::exception& error) {
        result.error = std::string("not valid JSON: ") + error.what();
        return result;
    }

    result.sourceVersion =
        root.value("schemaVersion", 1);  // berkas paling awal belum menulis versi
    if (result.sourceVersion > kLevelSchemaVersion) {
        result.error = "level was written by a newer editor (schema " +
                       std::to_string(result.sourceVersion) + " > " +
                       std::to_string(kLevelSchemaVersion) + ")";
        return result;
    }
    if (!root.contains("entities") || !root["entities"].is_array()) {
        result.error = "level has no entity list";
        return result;
    }

    if (result.sourceVersion < 2) {
        SIM_WARN("Scene", "Level uses schema {} — migrating to {}", result.sourceVersion,
                 kLevelSchemaVersion);
        MigrateV1ToV2(root);
        result.migrated = true;
    }

    world.Clear();
    if (!ReadEntities(world, root["entities"], kNullEntity)) {
        result.error = "entity with a missing or malformed guid";
        return result;
    }

    result.ok = true;
    result.entityCount = world.Count();
    return result;
}

std::string SaveSubtreeToString(const World& world, Entity root) {
    std::vector<Entity> ordered;
    CollectDepthFirst(world, root, ordered);
    return WriteEntities(world, ordered).dump(2) + "\n";
}

bool RestoreSubtree(World& world, const std::string& text, Uuid parentGuid) {
    Json root;
    try {
        root = Json::parse(text);
    } catch (const nlohmann::json::exception&) {
        return false;
    }
    if (!root.contains("entities") || !root["entities"].is_array()) {
        return false;
    }
    const Entity parent = world.FindByGuid(parentGuid);
    return ReadEntities(world, root["entities"], parent);
}

std::string SerializeComponent(const reflect::TypeDesc& type, const void* component) {
    Json fields = Json::object();
    for (const reflect::FieldDesc& field : type.fields) {
        fields[field.name] = WriteField(field, field.AccessConst(component));
    }
    return fields.dump();
}

void DeserializeComponent(const reflect::TypeDesc& type, void* component,
                          const std::string& text) {
    Json fields;
    try {
        fields = Json::parse(text);
    } catch (const nlohmann::json::exception&) {
        return;
    }
    for (const reflect::FieldDesc& field : type.fields) {
        const auto it = fields.find(field.name);
        if (it != fields.end()) {
            ReadField(field, *it, field.access(component));
        }
    }
}

LevelIoResult LoadLevelFromFile(World& world, const std::filesystem::path& path) {
    LevelIoResult result;
    std::ifstream file(path);
    if (!file) {
        result.error = "cannot open " + path.string();
        return result;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return LoadLevelFromString(world, buffer.str());
}

std::vector<std::string> DiffComponentFields(const std::string& first, const std::string& second) {
    std::vector<std::string> different;
    Json a;
    Json b;
    try {
        a = Json::parse(first);
        b = Json::parse(second);
    } catch (const nlohmann::json::exception&) {
        return different;
    }
    if (!a.is_object() || !b.is_object()) {
        return different;
    }
    for (auto it = a.begin(); it != a.end(); ++it) {
        const auto other = b.find(it.key());
        if (other == b.end() || *other != it.value()) {
            different.push_back(it.key());
        }
    }
    // Field yang hanya ada di salah satu cuplikan juga terhitung berbeda —
    // bisa terjadi bila komponennya ditulis oleh versi skema yang berbeda.
    for (auto it = b.begin(); it != b.end(); ++it) {
        if (a.find(it.key()) == a.end()) {
            different.push_back(it.key());
        }
    }
    return different;
}

std::string MergeComponentFields(const std::string& target, const std::string& source,
                                 const std::vector<std::string>& fields) {
    Json result;
    Json from;
    try {
        result = Json::parse(target);
        from = Json::parse(source);
    } catch (const nlohmann::json::exception&) {
        return target;
    }
    if (!result.is_object() || !from.is_object()) {
        return target;
    }
    for (const std::string& field : fields) {
        const auto it = from.find(field);
        if (it != from.end()) {
            result[field] = *it;
        }
    }
    return result.dump();
}

bool IsComponentSnapshot(const reflect::TypeDesc& type, const std::string& text) {
    Json document;
    try {
        document = Json::parse(text);
    } catch (const nlohmann::json::exception&) {
        return false;
    }
    if (!document.is_object()) {
        return false;
    }
    // Cukup satu field yang dikenal. Menuntut seluruh field cocok akan menolak
    // cuplikan dari versi skema yang lebih lama, padahal itu justru salah satu
    // hal yang berguna untuk ditempel.
    return std::any_of(type.fields.begin(), type.fields.end(),
                       [&](const reflect::FieldDesc& field) {
                           return document.contains(field.name);
                       });
}

std::string RemapGuids(const std::string& text, std::string* outRootGuid) {
    Json document;
    try {
        document = Json::parse(text);
    } catch (const nlohmann::json::exception& exception) {
        SIM_WARN("Scene", "RemapGuids received invalid JSON: {}", exception.what());
        return {};
    }
    if (!document.contains("entities") || !document["entities"].is_array() ||
        document["entities"].empty()) {
        return {};
    }

    std::unordered_map<std::string, std::string> remap;
    for (auto& record : document["entities"]) {
        const std::string oldGuid = record.value("guid", std::string{});
        if (oldGuid.empty()) {
            continue;
        }
        const std::string newGuid = Uuid::Generate().ToString();
        remap.emplace(oldGuid, newGuid);
        record["guid"] = newGuid;
    }
    for (auto& record : document["entities"]) {
        const auto it = record.find("parent");
        if (it == record.end() || !it->is_string()) {
            continue;
        }
        const auto mapped = remap.find(it->get<std::string>());
        if (mapped != remap.end()) {
            *it = mapped->second;
        } else {
            // Induk di luar teks ini: entity tersebut adalah akar salinan, dan
            // penempatannya ditentukan pemanggil.
            record.erase("parent");
        }
    }

    if (outRootGuid != nullptr) {
        *outRootGuid = document["entities"].front().value("guid", std::string{});
    }
    return document.dump();
}

}  // namespace sim::scene
