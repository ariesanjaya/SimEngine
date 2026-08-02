#pragma once

#include "Sim/Reflect/TypeRegistry.h"

#include <entt/entity/registry.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace sim::scene {

/// Operasi type-erased untuk satu jenis komponen.
///
/// Reflection tahu bentuk data sebuah komponen, tapi tidak tahu cara
/// menambah/mengambilnya dari entt::registry. Di sinilah kedua hal itu
/// dipertemukan, sehingga Inspector dan serialisasi bisa bekerja dengan
/// komponen apa pun tanpa satu pun cabang khusus per tipe.
struct ComponentOps {
    const reflect::TypeDesc* type = nullptr;
    void* (*tryGet)(entt::registry&, entt::entity) = nullptr;
    void* (*emplace)(entt::registry&, entt::entity) = nullptr;
    void (*remove)(entt::registry&, entt::entity) = nullptr;
    /// Komponen inti (Transform, Name, Id) tidak boleh dihapus: kode lain
    /// berasumsi selalu ada, dan menghapusnya hanya menghasilkan entity rusak.
    bool removable = true;
    /// Muncul di daftar "Add Component". Komponen inti tidak.
    bool addable = true;
};

/// Daftar seluruh jenis komponen menurut urutan pendaftaran.
///
/// Urutannya penting dan bukan detail: itulah urutan komponen tertulis di
/// berkas level, dan kriteria terima E3 menuntut simpan-muat-simpan menghasilkan
/// byte yang sama persis.
class ComponentRegistry {
public:
    static ComponentRegistry& Get();

    template <class T>
    void Register(bool removable = true, bool addable = true) {
        const reflect::TypeDesc* type = reflect::TypeRegistry::Get().Find<T>();
        if (type == nullptr) {
            return;  // belum dipantulkan; Validate() yang akan melaporkannya
        }
        ComponentOps ops;
        ops.type = type;
        ops.tryGet = [](entt::registry& registry, entt::entity entity) -> void* {
            return registry.try_get<T>(entity);
        };
        ops.emplace = [](entt::registry& registry, entt::entity entity) -> void* {
            return &registry.emplace_or_replace<T>(entity);
        };
        ops.remove = [](entt::registry& registry, entt::entity entity) {
            registry.remove<T>(entity);
        };
        ops.removable = removable;
        ops.addable = addable;
        components_.push_back(ops);
    }

    const std::vector<ComponentOps>& All() const { return components_; }
    const ComponentOps* Find(std::string_view typeName) const;

private:
    std::vector<ComponentOps> components_;
};

}  // namespace sim::scene
