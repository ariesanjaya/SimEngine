#pragma once

#include "Sim/Core/Uuid.h"

namespace sim {

/// Rujukan ke sebuah aset.
///
/// Isinya GUID, bukan path. Itulah yang membuat memindahkan atau mengganti nama
/// berkas aset tidak memutus level mana pun yang memakainya: yang tertulis di
/// berkas level adalah GUID, dan AssetDatabase yang menerjemahkannya ke lokasi
/// berkas saat ini.
///
/// Tipe tersendiri, bukan `Uuid` telanjang, supaya reflection bisa
/// membedakannya: field bertipe ini digambar Inspector sebagai kotak yang
/// menerima seretan dari Asset Browser, sedangkan `Uuid` biasa digambar sebagai
/// teks.
struct AssetRef {
    Uuid guid;

    bool IsValid() const { return guid.IsValid(); }
    void Clear() { guid = Uuid{}; }

    friend bool operator==(const AssetRef& a, const AssetRef& b) { return a.guid == b.guid; }
    friend bool operator!=(const AssetRef& a, const AssetRef& b) { return !(a == b); }
};

}  // namespace sim
