#pragma once

#include "Sim/Core/Uuid.h"
#include "Sim/Scene/Components.h"
#include "Sim/Scene/WorldSettings.h"

#include <entt/entity/registry.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace sim::scene {

/// Kumpulan entity beserta hierarki dan transformnya.
///
/// Pembungkus tipis di atas entt::registry. Yang ditambahkan World di atasnya
/// hanya tiga hal yang tidak bisa dikerjakan entt sendiri, tapi mahal kalau
/// salah: hierarki yang selalu konsisten dua arah, transform dunia yang
/// dihitung malas, dan GUID yang bertahan lintas-sesi.
class World {
public:
    World();

    Entity Create(std::string name = "Entity", Entity parent = kNullEntity);
    /// Membuat entity dengan GUID tertentu. Dipakai saat memuat level.
    Entity CreateWithGuid(Uuid guid, std::string name, Entity parent = kNullEntity);

    /// Menghapus entity beserta seluruh keturunannya.
    void Destroy(Entity entity);
    void Clear();

    bool IsAlive(Entity entity) const;
    /// Jumlah entity yang hidup.
    ///
    /// Dihitung dari daftar akar dan keturunannya, bukan dari storage entt:
    /// API storage-nya berubah antar-versi minor, sedangkan hierarki kita
    /// sendiri sudah menyimpan semua entity yang bisa dijangkau.
    std::size_t Count() const;

    // --- hierarki -----------------------------------------------------------

    /// Mengubah induk. Menolak dan mengembalikan false bila akan membentuk
    /// siklus (menjadikan entity sebagai induk dari keturunannya sendiri).
    bool SetParent(Entity child, Entity parent);
    Entity ParentOf(Entity entity) const;
    const std::vector<Entity>& ChildrenOf(Entity entity) const;
    const std::vector<Entity>& Roots() const { return roots_; }
    bool IsDescendantOf(Entity entity, Entity ancestor) const;

    // --- transform ----------------------------------------------------------

    /// Matriks dunia, dihitung ulang hanya bila ada yang berubah.
    const Mat4& WorldMatrix(Entity entity);
    /// Menandai transform entity dan seluruh keturunannya perlu dihitung ulang.
    /// Wajib dipanggil setelah mengubah TransformComponent secara langsung.
    void MarkTransformDirty(Entity entity);

    // --- komponen -----------------------------------------------------------

    template <class T, class... Args>
    T& Add(Entity entity, Args&&... args) {
        return registry_.emplace_or_replace<T>(ToEntt(entity), std::forward<Args>(args)...);
    }
    template <class T>
    T* TryGet(Entity entity) {
        return registry_.try_get<T>(ToEntt(entity));
    }
    template <class T>
    const T* TryGet(Entity entity) const {
        return registry_.try_get<T>(ToEntt(entity));
    }
    template <class T>
    bool Has(Entity entity) const {
        return registry_.all_of<T>(ToEntt(entity));
    }
    template <class T>
    void Remove(Entity entity) {
        registry_.remove<T>(ToEntt(entity));
    }

    // --- pengaturan level ---------------------------------------------------

    /// Bagaimana level ini disinari.
    ///
    /// **Dipegang World, bukan sebuah entity.** Sebuah adegan punya tepat satu
    /// tingkat pencahayaan; menaruhnya di entity berarti "yang mana yang berlaku
    /// kalau ada dua" harus dijawab di setiap tempat yang membacanya. Alasan
    /// lengkapnya di WorldSettings.h.
    const WorldSettings& Settings() const { return settings_; }
    void SetSettings(const WorldSettings& settings) { settings_ = settings; }

    Uuid GuidOf(Entity entity) const;
    Entity FindByGuid(Uuid guid) const;
    const std::string& NameOf(Entity entity) const;
    void SetName(Entity entity, std::string name);

    entt::registry& Registry() { return registry_; }
    const entt::registry& Registry() const { return registry_; }

    static entt::entity ToEntt(Entity entity) { return static_cast<entt::entity>(entity); }
    static Entity FromEntt(entt::entity entity) { return static_cast<Entity>(entity); }

private:
    void DetachFromParent(Entity entity);
    void DestroyRecursive(Entity entity, std::vector<Entity>& scratch);

    entt::registry registry_;
    WorldSettings settings_;
    std::vector<Entity> roots_;
    std::unordered_map<Uuid, Entity> byGuid_;
    /// Dikembalikan ChildrenOf() untuk entity tanpa anak, supaya pemanggil tidak
    /// perlu menangani pointer kosong.
    std::vector<Entity> emptyChildren_;
    std::string emptyName_;
};

}  // namespace sim::scene
