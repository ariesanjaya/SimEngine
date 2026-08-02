#pragma once

#include "Sim/Core/Math.h"
#include "Sim/Core/AssetRef.h"
#include "Sim/Core/Uuid.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <typeindex>
#include <vector>

namespace sim::reflect {

/// Identitas tipe di dalam satu proses.
///
/// Sengaja `std::type_index` dan bukan hash nama: yang perlu stabil lintas-sesi
/// adalah nama tipe dan nama field yang tertulis di berkas level, bukan
/// pengenal di memori. Memakai type_index menghilangkan seluruh kelas bug
/// tabrakan hash tanpa biaya apa pun.
using TypeKey = std::type_index;

/// Bentuk data sebuah field. Menentukan cara Inspector menggambarnya dan cara
/// serialisasi membaca/menulisnya.
enum class FieldKind : uint8_t {
    Bool,
    Int,
    UInt,
    Float,
    Vec2,
    Vec3,
    Vec4,
    Quat,
    String,
    Uuid,
    /// Rujukan ke aset (`sim::AssetRef`). Dibedakan dari Uuid biasa karena
    /// Inspector menggambarnya sebagai kotak yang menerima seretan dari Asset
    /// Browser, lengkap dengan nama aset yang sedang dirujuk.
    AssetRef,
    Enum,
    /// Struct yang juga terdaftar di TypeRegistry; digambar dan diserialisasi
    /// secara rekursif.
    Struct,
    /// std::vector<T>; elemen dijelaskan oleh FieldDesc::vector.
    Vector,
};

/// Petunjuk tambahan yang dipakai Inspector, dan sebagian juga serialisasi.
struct Attributes {
    /// Rentang untuk slider. Aktif bila max > min.
    float min = 0.0f;
    float max = 0.0f;
    /// Kecepatan seret. 0 = biarkan Inspector memilih dari rentangnya.
    float speed = 0.0f;
    std::string tooltip;
    /// Pengelompokan di Inspector. Kosong = ikut urutan deklarasi.
    std::string category;
    bool readOnly = false;
    /// Tetap diserialisasi, tapi tidak digambar. Untuk data turunan yang perlu
    /// bertahan tapi tidak masuk akal disunting tangan.
    bool hidden = false;
    /// Vec3/Vec4 yang digambar sebagai color picker, bukan tiga angka.
    bool isColor = false;
    /// Nilai disimpan radian tapi ditampilkan derajat. Konversi terjadi hanya
    /// di lapisan UI — berkas level selalu radian.
    bool degrees = false;
    /// Nama nilai enum, indeks = nilai. Diserialisasi sebagai nama supaya
    /// menyisipkan nilai baru di tengah tidak merusak berkas lama.
    std::vector<std::string> enumNames;
};

struct TypeDesc;

/// Operasi type-erased untuk field bertipe std::vector.
struct VectorOps {
    std::size_t (*size)(const void*) = nullptr;
    void* (*at)(void*, std::size_t) = nullptr;
    const void* (*atConst)(const void*, std::size_t) = nullptr;
    void (*resize)(void*, std::size_t) = nullptr;
    FieldKind elementKind = FieldKind::Float;
    TypeKey elementType = typeid(void);
};

struct FieldDesc {
    /// Nama yang tertulis di berkas level. Mengubahnya memutus berkas lama —
    /// gunakan jalur migrasi bila memang perlu.
    std::string name;
    /// Nama yang ditampilkan Inspector. Kosong = diturunkan dari `name`.
    std::string label;
    FieldKind kind = FieldKind::Float;
    /// Untuk kind Struct: tipe struct-nya.
    TypeKey type = typeid(void);
    /// Alamat field di dalam sebuah objek. Dibuat dari pointer-to-member,
    /// bukan offsetof — offsetof pada tipe yang punya std::string bersifat
    /// conditionally-supported, sedangkan cara ini selalu benar.
    void* (*access)(void*) = nullptr;
    Attributes attributes;
    const VectorOps* vector = nullptr;

    const void* AccessConst(const void* object) const {
        return access(const_cast<void*>(object));
    }
};

struct TypeDesc {
    /// Nama stabil yang tertulis di berkas level, mis. "Transform".
    std::string name;
    TypeKey key = typeid(void);
    std::size_t size = 0;
    std::vector<FieldDesc> fields;

    const FieldDesc* FindField(std::string_view fieldName) const;
};

}  // namespace sim::reflect
