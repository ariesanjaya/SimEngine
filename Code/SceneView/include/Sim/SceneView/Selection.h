#pragma once

#include "Sim/Scene/World.h"

#include <cstdint>
#include <vector>

namespace sim::view {

/// Menjembatani Entity dan SelectionId.
///
/// Digeser satu supaya nilai 0 tetap berarti "tidak ada": entity pertama entt
/// bernilai 0, dan tanpa pergeseran ini objek pertama di scene akan selalu
/// tampak tidak terpilih.
inline uint64_t ToSelectionId(scene::Entity entity) {
    return static_cast<uint64_t>(entity) + 1;
}
inline scene::Entity ToEntity(uint64_t id) {
    return id == 0 ? scene::kNullEntity : static_cast<scene::Entity>(id - 1);
}

/// Identitas objek yang bisa diseleksi.
///
/// Sengaja tipe buram, bukan tipe entity: di E2 model scene belum ada, dan saat
/// E3 memperkenalkannya, satu-satunya yang perlu berubah adalah alias ini.
/// Panel yang bekerja dengan seleksi tidak ikut tersentuh.
using SelectionId = uint64_t;
inline constexpr SelectionId kInvalidSelection = 0;

/// Himpunan objek terpilih, dengan urutan pemilihan dipertahankan.
///
/// Urutan penting: banyak operasi editor memakai "yang terakhir dipilih"
/// sebagai acuan — align ke objek aktif, parent ke objek aktif — dan itu tidak
/// bisa direkonstruksi dari himpunan yang tidak berurut.
class Selection {
public:
    void Clear();
    /// Mengganti seluruh isi seleksi dengan satu objek.
    void SelectOnly(SelectionId id);
    void Add(SelectionId id);
    void Remove(SelectionId id);
    void Toggle(SelectionId id);
    void SetItems(std::vector<SelectionId> items);

    bool Contains(SelectionId id) const;
    bool Empty() const { return items_.empty(); }
    std::size_t Count() const { return items_.size(); }

    /// Objek acuan operasi: yang paling terakhir ditambahkan.
    SelectionId Primary() const { return items_.empty() ? kInvalidSelection : items_.back(); }
    const std::vector<SelectionId>& Items() const { return items_; }

    /// Naik setiap kali isi seleksi berubah. Panel memakainya untuk mendeteksi
    /// perubahan tanpa menyalin dan membandingkan daftarnya tiap frame.
    uint64_t Version() const { return version_; }

private:
    void Bump() { ++version_; }

    std::vector<SelectionId> items_;
    uint64_t version_ = 0;
};

}  // namespace sim::view
