#pragma once

#include "Sim/Core/Uuid.h"
#include "Sim/Reflect/Types.h"

#include <functional>
#include <span>
#include <string>

namespace sim::editor {

/// Menerjemahkan GUID aset menjadi nama yang bisa dibaca.
///
/// Dipasang editor sekali saat AssetDatabase siap. Sebuah kait, bukan parameter,
/// karena ini murni urusan tampilan: tanpa penerjemah pun PropertyGrid tetap
/// benar — field aset hanya menampilkan GUID mentah alih-alih nama berkas.
/// Menjadikannya parameter berarti setiap pemanggil DrawField ikut menyeretnya.
using AssetNameResolver = std::function<std::string(const Uuid&)>;
void SetAssetNameResolver(AssetNameResolver resolver);

/// Nama aset untuk sebuah GUID, atau teks pengganti bila tidak dikenal.
std::string ResolveAssetName(const Uuid& guid);

/// Hasil menggambar sekumpulan properti.
struct PropertyGridResult {
    /// Ada nilai yang berubah pada frame ini.
    bool edited = false;
    /// Pengguna melepas kendali — penanda untuk menutup kelompok undo.
    bool finished = false;
};

/// Menggambar seluruh field sebuah tipe dari deskripsinya.
///
/// Inilah pembayaran dari reflection: Inspector tidak punya satu pun cabang
/// khusus per komponen. Menambah field ke sebuah komponen berarti satu baris di
/// pendaftaran tipe, dan field itu langsung muncul di sini, ikut tersimpan ke
/// berkas level, dan (di E6) ikut terjangkau dari Lua.
///
/// Atribut yang dihormati: Range (jadi slider), Speed, Degrees (radian di data,
/// derajat di layar), Color (color picker), ReadOnly, Hidden, Tooltip.
/// `mixedFields` berisi nama field yang nilainya berbeda antar objek terpilih.
/// Field tersebut diberi tanda supaya jelas bahwa angka yang terlihat hanya
/// milik salah satu objek — tanpa penanda, pengguna akan mengira seluruh
/// seleksi sudah bernilai sama. Menyuntingnya tetap berlaku untuk semuanya.
PropertyGridResult DrawProperties(const reflect::TypeDesc& type, void* object,
                                  std::span<const std::string> mixedFields = {});

/// Menggambar satu field. Dipakai DrawProperties dan bisa dipakai panel yang
/// ingin menyusun tata letaknya sendiri.
PropertyGridResult DrawField(const reflect::FieldDesc& field, void* object);

}  // namespace sim::editor
