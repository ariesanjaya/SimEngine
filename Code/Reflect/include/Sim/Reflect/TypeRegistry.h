#pragma once

#include "Sim/Reflect/Types.h"

#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace sim::reflect {
namespace detail {

template <class T>
struct MemberPointerTraits;

template <class Class, class Member>
struct MemberPointerTraits<Member Class::*> {
    using Owner = Class;
    using Type = Member;
};

template <class T>
struct IsVector : std::false_type {};

template <class T, class A>
struct IsVector<std::vector<T, A>> : std::true_type {
    using Element = T;
};

/// Menentukan FieldKind dari tipe C++-nya.
///
/// Apa pun yang tidak dikenali dianggap Struct, dan Struct wajib terdaftar.
/// Kesalahan itu baru ketahuan saat serialisasi berjalan, jadi TypeRegistry
/// memeriksanya di Validate().
template <class T>
constexpr FieldKind KindOf() {
    using U = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<U, bool>) {
        return FieldKind::Bool;
    } else if constexpr (std::is_enum_v<U>) {
        return FieldKind::Enum;
    } else if constexpr (std::is_same_v<U, uint8_t> || std::is_same_v<U, uint16_t> ||
                         std::is_same_v<U, uint32_t> || std::is_same_v<U, uint64_t>) {
        return FieldKind::UInt;
    } else if constexpr (std::is_integral_v<U>) {
        return FieldKind::Int;
    } else if constexpr (std::is_floating_point_v<U>) {
        return FieldKind::Float;
    } else if constexpr (std::is_same_v<U, Vec2>) {
        return FieldKind::Vec2;
    } else if constexpr (std::is_same_v<U, Vec3>) {
        return FieldKind::Vec3;
    } else if constexpr (std::is_same_v<U, Vec4>) {
        return FieldKind::Vec4;
    } else if constexpr (std::is_same_v<U, Quat>) {
        return FieldKind::Quat;
    } else if constexpr (std::is_same_v<U, std::string>) {
        return FieldKind::String;
    } else if constexpr (std::is_same_v<U, ::sim::AssetRef>) {
        // Harus diperiksa sebelum Struct: AssetRef memang sebuah struct, dan
        // tanpa cabang ini ia akan jatuh ke cabang terakhir lalu dicari-cari
        // pendaftaran tipenya yang tidak pernah ada.
        return FieldKind::AssetRef;
    } else if constexpr (std::is_same_v<U, ::sim::Uuid>) {
        return FieldKind::Uuid;
    } else if constexpr (IsVector<U>::value) {
        return FieldKind::Vector;
    } else {
        return FieldKind::Struct;
    }
}

template <class T>
const VectorOps* VectorOpsFor() {
    using Element = typename IsVector<T>::Element;
    static const VectorOps ops{
        [](const void* v) { return static_cast<const T*>(v)->size(); },
        [](void* v, std::size_t i) -> void* { return &(*static_cast<T*>(v))[i]; },
        [](const void* v, std::size_t i) -> const void* { return &(*static_cast<const T*>(v))[i]; },
        [](void* v, std::size_t n) { static_cast<T*>(v)->resize(n); },
        KindOf<Element>(),
        TypeKey(typeid(Element)),
    };
    return &ops;
}

}  // namespace detail

class TypeRegistry;

/// Antarmuka berantai untuk mendaftarkan sebuah tipe.
///
/// Menambah field ke komponen berarti satu baris di sini — Inspector,
/// serialisasi, dan (di E6) binding Lua ikut otomatis. Itulah alasan reflection
/// ada: tanpa ini, satu field baru berarti menyunting tiga tempat berbeda dan
/// menemukan yang terlupa lewat bug.
template <class T>
class TypeBuilder {
public:
    TypeBuilder(TypeRegistry& registry, TypeDesc& desc) : registry_(registry), desc_(desc) {}

    template <auto MemberPtr>
    TypeBuilder& Field(std::string name) {
        using Traits = detail::MemberPointerTraits<decltype(MemberPtr)>;
        static_assert(std::is_same_v<typename Traits::Owner, T>,
                      "Field() dipanggil dengan anggota dari tipe lain");
        using Member = typename Traits::Type;

        FieldDesc field;
        field.name = std::move(name);
        field.kind = detail::KindOf<Member>();
        field.type = TypeKey(typeid(Member));
        field.access = [](void* object) -> void* {
            return &(static_cast<T*>(object)->*MemberPtr);
        };
        if constexpr (detail::IsVector<Member>::value) {
            field.vector = detail::VectorOpsFor<Member>();
        }
        desc_.fields.push_back(std::move(field));
        return *this;
    }

    /// Semua penyetel atribut berlaku pada field terakhir yang ditambahkan.
    TypeBuilder& Label(std::string text) {
        Last().label = std::move(text);
        return *this;
    }
    TypeBuilder& Range(float min, float max) {
        Last().attributes.min = min;
        Last().attributes.max = max;
        return *this;
    }
    TypeBuilder& Speed(float speed) {
        Last().attributes.speed = speed;
        return *this;
    }
    TypeBuilder& Tooltip(std::string text) {
        Last().attributes.tooltip = std::move(text);
        return *this;
    }
    TypeBuilder& Category(std::string text) {
        Last().attributes.category = std::move(text);
        return *this;
    }
    TypeBuilder& ReadOnly() {
        Last().attributes.readOnly = true;
        return *this;
    }
    TypeBuilder& Hidden() {
        Last().attributes.hidden = true;
        return *this;
    }
    TypeBuilder& Color() {
        Last().attributes.isColor = true;
        return *this;
    }
    TypeBuilder& Degrees() {
        Last().attributes.degrees = true;
        return *this;
    }
    TypeBuilder& EnumNames(std::vector<std::string> names) {
        Last().attributes.enumNames = std::move(names);
        return *this;
    }

private:
    FieldDesc& Last() { return desc_.fields.back(); }

    TypeRegistry& registry_;
    TypeDesc& desc_;
};

/// Katalog seluruh tipe yang bisa dipantulkan.
///
/// Satu instance global. Bukan karena singleton itu bagus, tapi karena
/// deskripsi tipe bersifat statis untuk seluruh proses dan menyalurkannya lewat
/// parameter ke setiap pemanggil serialisasi dan setiap widget Inspector hanya
/// menambah kebisingan tanpa memberi fleksibilitas yang akan dipakai.
class TypeRegistry {
public:
    static TypeRegistry& Get();

    template <class T>
    TypeBuilder<T> Type(std::string name) {
        const TypeKey key(typeid(T));
        TypeDesc desc;
        desc.name = std::move(name);
        desc.key = key;
        desc.size = sizeof(T);

        auto [it, inserted] = types_.emplace(key, std::move(desc));
        if (inserted) {
            byName_.emplace(it->second.name, key);
            order_.push_back(key);
        }
        return TypeBuilder<T>(*this, it->second);
    }

    const TypeDesc* Find(TypeKey key) const;
    const TypeDesc* Find(std::string_view name) const;

    template <class T>
    const TypeDesc* Find() const {
        return Find(TypeKey(typeid(T)));
    }

    /// Semua tipe menurut urutan pendaftaran.
    const std::vector<TypeKey>& Order() const { return order_; }

    /// Memeriksa bahwa setiap field bertipe Struct atau vector-of-Struct benar
    /// merujuk tipe yang terdaftar, dan setiap enum punya daftar nama.
    ///
    /// Dijalankan sekali setelah pendaftaran. Tanpa ini, tipe yang lupa
    /// didaftarkan baru ketahuan saat sebuah level gagal dimuat di tangan
    /// pengguna, bukan saat editor dijalankan.
    bool Validate(std::vector<std::string>& problems) const;

private:
    std::unordered_map<TypeKey, TypeDesc> types_;
    std::unordered_map<std::string, TypeKey> byName_;
    std::vector<TypeKey> order_;
};

}  // namespace sim::reflect
