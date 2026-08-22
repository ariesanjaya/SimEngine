#pragma once

#include "Sim/Assets/MeshData.h"
#include "Sim/Core/Uuid.h"
#include "Sim/Whitebox/WhiteboxMesh.h"

#include <filesystem>
#include <string>
#include <unordered_map>

/// Whitebox yang sedang terbuka, dibagi panel dan viewport.
///
/// **Satu tempat, bukan satu salinan per pemakai.** Panel menyunting dan
/// viewport menggambar — kalau keduanya memuat sendiri-sendiri, yang tergambar
/// adalah bentuk sebelum suntingan terakhir, dan tidak ada satu pun pesan yang
/// menjelaskan mengapa.
namespace sim::view {

/// Sisi yang sedang disorot, beserta aset pemiliknya.
///
/// **Bukan disimpan di dalam meshnya.** Seleksi adalah keadaan penyunting, dan
/// menaruhnya di dalam aset berarti membuka berkas yang sama di dua tempat
/// saling menggeser sorotan — dan menyimpannya ke disk sebagai bagian bentuk.
struct SideSelection {
    Uuid asset;
    whitebox::PolygonHandle polygon = whitebox::PolygonHandle::Invalid;

    bool Has(const Uuid& guid) const {
        return asset == guid && whitebox::IsValid(polygon);
    }
};

class WhiteboxStore {
public:
    /// Whitebox milik sebuah aset, dimuat bila perlu.
    ///
    /// Mengembalikan null bila berkasnya tidak ada atau tidak bisa dibaca —
    /// dan **mencatat kegagalannya** supaya berkas rusak tidak diurai ulang enam
    /// puluh kali per detik sambil membanjiri log dengan pesan yang sama.
    whitebox::WhiteboxMesh* Get(const Uuid& guid, const std::filesystem::path& path);

    /// Whitebox yang sudah dimuat, tanpa mencoba memuat.
    whitebox::WhiteboxMesh* Find(const Uuid& guid);

    /// Menaruh whitebox yang sudah jadi di bawah sebuah guid.
    ///
    /// Bentuk mendahului berkas: whitebox baru lahir sebagai kubus di dalam
    /// editor, dan `Get` tidak bisa memuatnya karena belum ada yang ditulis.
    whitebox::WhiteboxMesh& Adopt(const Uuid& guid, whitebox::WhiteboxMesh mesh);

    /// Menandai sebuah whitebox berubah. Viewport memakai versinya untuk tahu
    /// kapan harus mengunggah ulang geometrinya.
    void MarkDirty(const Uuid& guid);
    uint64_t Version(const Uuid& guid) const;

    /// Menulis kembali ke berkasnya. False beserta sebabnya bila gagal.
    bool Save(const Uuid& guid, const std::filesystem::path& path, std::string& error);

    /// Segitiga yang bisa digambar, dibangun ulang hanya ketika versinya naik.
    ///
    /// **Bukan tiap frame.** Membangun `MeshData` untuk blockout memang murah,
    /// tetapi mengunggahnya ke GPU tidak — dan tanpa penanda versi satu-satunya
    /// pilihan adalah mengunggah ulang tiap frame atau tidak pernah.
    const assets::MeshData* BuiltMesh(const Uuid& guid);

    /// Membuang yang sudah dimuat. Dipakai saat project berganti.
    void Clear();

    /// Sisi yang sedang disorot.
    ///
    /// **Di sini, bukan di panel.** Panel yang memilihnya lewat daftar dan
    /// viewport yang memilihnya lewat sinar — dua salinan berarti dua sorotan
    /// yang berbeda, dan yang tergerakkan gizmo bukan yang tersorot di daftar.
    const SideSelection& Selected() const { return selected_; }
    void Select(const Uuid& guid, whitebox::PolygonHandle polygon);
    void ClearSelection() { selected_ = {}; }

private:
    struct Entry {
        whitebox::WhiteboxMesh mesh;
        uint64_t version = 1;
        bool failed = false;
        assets::MeshData built;
        uint64_t builtVersion = 0;
    };

    std::unordered_map<Uuid, Entry> entries_;
    SideSelection selected_;
};

}  // namespace sim::view
