#include "Sim/Scene/AssetUsage.h"

#include "Sim/Scene/ComponentRegistry.h"

namespace sim::scene {
namespace {

using reflect::FieldKind;

bool ValueUsesAsset(FieldKind kind, reflect::TypeKey type, const void* data, const Uuid& asset);

/// Apakah sebuah struct terdaftar memuat rujukan ke `asset`.
bool StructUsesAsset(const reflect::TypeDesc& desc, const void* data, const Uuid& asset) {
    for (const reflect::FieldDesc& field : desc.fields) {
        const void* value = field.AccessConst(data);
        if (field.kind == FieldKind::Vector) {
            if (field.vector == nullptr) {
                continue;
            }
            const std::size_t count = field.vector->size(value);
            for (std::size_t i = 0; i < count; ++i) {
                if (ValueUsesAsset(field.vector->elementKind, field.vector->elementType,
                                   field.vector->atConst(value, i), asset)) {
                    return true;
                }
            }
            continue;
        }
        if (ValueUsesAsset(field.kind, field.type, value, asset)) {
            return true;
        }
    }
    return false;
}

bool ValueUsesAsset(FieldKind kind, reflect::TypeKey type, const void* data, const Uuid& asset) {
    if (kind == FieldKind::AssetRef) {
        return static_cast<const AssetRef*>(data)->guid == asset;
    }
    if (kind != FieldKind::Struct) {
        return false;
    }
    // Struct yang tidak terdaftar tidak bisa ditelusuri. Itu bukan diamkan
    // diam-diam melainkan kondisi yang sudah dijaga TypeRegistry::Validate(),
    // yang menolak field Struct tanpa pendaftaran tipenya.
    const reflect::TypeDesc* nested = reflect::TypeRegistry::Get().Find(type);
    return nested != nullptr && StructUsesAsset(*nested, data, asset);
}

void Collect(const World& world, Entity entity, const Uuid& asset,
             std::vector<Entity>& result) {
    // const_cast: ComponentOps::tryGet hanya punya bentuk non-const, sedangkan
    // yang dilakukan di sini murni membaca. Menambah varian const ke seluruh
    // registry hanya demi pemeriksaan ini akan menggandakan setiap ops.
    auto& registry = const_cast<entt::registry&>(world.Registry());
    const entt::entity handle = World::ToEntt(entity);

    for (const ComponentOps& ops : ComponentRegistry::Get().All()) {
        if (ops.type == nullptr || ops.tryGet == nullptr) {
            continue;
        }
        const void* data = ops.tryGet(registry, handle);
        if (data != nullptr && StructUsesAsset(*ops.type, data, asset)) {
            result.push_back(entity);
            break;
        }
    }

    for (const Entity child : world.ChildrenOf(entity)) {
        Collect(world, child, asset, result);
    }
}

}  // namespace

std::vector<Entity> EntitiesUsingAsset(const World& world, const Uuid& asset) {
    std::vector<Entity> result;
    if (!asset.IsValid()) {
        return result;
    }
    for (const Entity root : world.Roots()) {
        Collect(world, root, asset, result);
    }
    return result;
}

}  // namespace sim::scene
