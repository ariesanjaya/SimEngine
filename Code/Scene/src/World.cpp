#include "Sim/Scene/World.h"

#include "Sim/Core/Assert.h"

#include <algorithm>

namespace sim::scene {

Mat4 TransformComponent::LocalMatrix() const {
    // T * R * S. Urutan ini yang membuat skala bekerja pada sumbu lokal objek
    // dan rotasi tidak ikut terskala — urutan lain menghasilkan shear begitu
    // skalanya tidak seragam.
    Mat4 matrix = glm::translate(Mat4(1.0f), position);
    matrix *= glm::mat4_cast(rotation);
    return glm::scale(matrix, scale);
}

World::World() {
    RegisterCoreComponents();
}

Entity World::Create(std::string name, Entity parent) {
    return CreateWithGuid(Uuid::Generate(), std::move(name), parent);
}

Entity World::CreateWithGuid(Uuid guid, std::string name, Entity parent) {
    const Entity entity = FromEntt(registry_.create());

    registry_.emplace<IdComponent>(ToEntt(entity), IdComponent{guid});
    registry_.emplace<NameComponent>(ToEntt(entity), NameComponent{std::move(name)});
    registry_.emplace<HierarchyComponent>(ToEntt(entity));
    registry_.emplace<TransformComponent>(ToEntt(entity));
    registry_.emplace<TransformCache>(ToEntt(entity));

    byGuid_.emplace(guid, entity);

    if (IsValid(parent) && IsAlive(parent)) {
        SetParent(entity, parent);
    } else {
        roots_.push_back(entity);
    }
    return entity;
}

std::size_t World::Count() const {
    std::size_t total = 0;
    for (const auto entity : registry_.view<IdComponent>()) {
        (void)entity;
        ++total;
    }
    return total;
}

bool World::IsAlive(Entity entity) const {
    return IsValid(entity) && registry_.valid(ToEntt(entity));
}

void World::Destroy(Entity entity) {
    if (!IsAlive(entity)) {
        return;
    }
    DetachFromParent(entity);
    std::vector<Entity> scratch;
    DestroyRecursive(entity, scratch);
}

void World::DestroyRecursive(Entity entity, std::vector<Entity>& scratch) {
    if (!IsAlive(entity)) {
        return;
    }
    if (auto* hierarchy = TryGet<HierarchyComponent>(entity)) {
        // Disalin dulu: anak yang dihancurkan akan mengubah daftar ini sambil
        // kita menelusurinya.
        scratch = hierarchy->children;
        for (const Entity child : scratch) {
            std::vector<Entity> nested;
            DestroyRecursive(child, nested);
        }
    }
    if (const auto* id = TryGet<IdComponent>(entity)) {
        byGuid_.erase(id->guid);
    }
    registry_.destroy(ToEntt(entity));
}

void World::Clear() {
    registry_.clear();
    roots_.clear();
    byGuid_.clear();
    // **Pengaturan ikut kembali ke bawaannya.** `Clear()` berarti dunia yang
    // baru, dan sebuah dunia baru disinari dengan bawaan — bukan dengan tingkat
    // pencahayaan milik level yang barusan ditutup. Kalau tidak, membuka level
    // lama yang tidak punya blok `"world"` sesudah level ber-`RealTime` akan
    // mewarisi `RealTime` tanpa satu baris pun yang menyebutkannya di berkasnya.
    settings_ = {};
}

void World::DetachFromParent(Entity entity) {
    auto* hierarchy = TryGet<HierarchyComponent>(entity);
    if (hierarchy == nullptr) {
        return;
    }
    if (IsValid(hierarchy->parent) && IsAlive(hierarchy->parent)) {
        auto* parentHierarchy = TryGet<HierarchyComponent>(hierarchy->parent);
        if (parentHierarchy != nullptr) {
            auto& siblings = parentHierarchy->children;
            siblings.erase(std::remove(siblings.begin(), siblings.end(), entity), siblings.end());
        }
    } else {
        roots_.erase(std::remove(roots_.begin(), roots_.end(), entity), roots_.end());
    }
    hierarchy->parent = kNullEntity;
}

bool World::SetParent(Entity child, Entity parent) {
    if (!IsAlive(child)) {
        return false;
    }
    if (child == parent) {
        return false;
    }
    // Menjadikan keturunan sebagai induk akan memutus hierarki jadi cincin:
    // setiap penelusuran ke atas berubah jadi loop tak berujung.
    if (IsValid(parent) && IsDescendantOf(parent, child)) {
        return false;
    }

    DetachFromParent(child);

    auto* hierarchy = TryGet<HierarchyComponent>(child);
    if (IsValid(parent) && IsAlive(parent)) {
        hierarchy->parent = parent;
        TryGet<HierarchyComponent>(parent)->children.push_back(child);
    } else {
        hierarchy->parent = kNullEntity;
        roots_.push_back(child);
    }

    MarkTransformDirty(child);
    return true;
}

Entity World::ParentOf(Entity entity) const {
    const auto* hierarchy = TryGet<HierarchyComponent>(entity);
    return hierarchy != nullptr ? hierarchy->parent : kNullEntity;
}

const std::vector<Entity>& World::ChildrenOf(Entity entity) const {
    const auto* hierarchy = TryGet<HierarchyComponent>(entity);
    return hierarchy != nullptr ? hierarchy->children : emptyChildren_;
}

bool World::IsDescendantOf(Entity entity, Entity ancestor) const {
    Entity current = ParentOf(entity);
    while (IsValid(current)) {
        if (current == ancestor) {
            return true;
        }
        current = ParentOf(current);
    }
    return false;
}

void World::MarkTransformDirty(Entity entity) {
    if (!IsAlive(entity)) {
        return;
    }
    if (auto* cache = TryGet<TransformCache>(entity)) {
        cache->dirty = true;
    }
    for (const Entity child : ChildrenOf(entity)) {
        MarkTransformDirty(child);
    }
}

const Mat4& World::WorldMatrix(Entity entity) {
    auto* cache = TryGet<TransformCache>(entity);
    SIM_ASSERT(cache != nullptr, "WorldMatrix on an entity without a transform");
    if (!cache->dirty) {
        return cache->world;
    }

    const auto* transform = TryGet<TransformComponent>(entity);
    const Mat4 local = transform != nullptr ? transform->LocalMatrix() : Mat4(1.0f);

    const Entity parent = ParentOf(entity);
    if (IsValid(parent) && IsAlive(parent)) {
        // Rekursi ke atas: induk dihitung lebih dulu bila ia sendiri kotor.
        // Kedalamannya sedalam hierarki, bukan sebanyak entity.
        const Mat4 parentWorld = WorldMatrix(parent);
        cache = TryGet<TransformCache>(entity);  // rekursi bisa memindah storage
        cache->world = parentWorld * local;
    } else {
        cache->world = local;
    }
    cache->dirty = false;
    return cache->world;
}

Uuid World::GuidOf(Entity entity) const {
    const auto* id = TryGet<IdComponent>(entity);
    return id != nullptr ? id->guid : Uuid{};
}

Entity World::FindByGuid(Uuid guid) const {
    const auto it = byGuid_.find(guid);
    return it == byGuid_.end() ? kNullEntity : it->second;
}

const std::string& World::NameOf(Entity entity) const {
    const auto* name = TryGet<NameComponent>(entity);
    return name != nullptr ? name->name : emptyName_;
}

void World::SetName(Entity entity, std::string name) {
    if (auto* component = TryGet<NameComponent>(entity)) {
        component->name = std::move(name);
    }
}

}  // namespace sim::scene
