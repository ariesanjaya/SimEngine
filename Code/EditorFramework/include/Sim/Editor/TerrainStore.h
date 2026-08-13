#pragma once

#include "Sim/Core/Uuid.h"
#include "Sim/Terrain/Terrain.h"
#include "Sim/Assets/MeshData.h"
#include "Sim/Terrain/TerrainIo.h"
#include "Sim/Terrain/TerrainMesh.h"

#include <filesystem>
#include <string>
#include <unordered_map>

/// Terrain yang sedang terbuka, dibagi panel, viewport, dan fisika.
///
/// **Satu dokumen, banyak pembaca.** Panel menyunting, viewport menggambar,
/// fisika menabrak. Kalau ketiganya memuat sendiri-sendiri, yang tergambar
/// adalah bentuk sebelum goresan terakhir dan yang ditabrak adalah bentuk
/// sebelum itu lagi — dan tidak ada satu pun pesan yang menjelaskannya.
///
/// Bentuknya sengaja sama dengan `WhiteboxStore`: sebuah terrain jauh lebih
/// besar, tetapi persoalannya persis sama, dan dua jawaban berbeda untuk satu
/// persoalan adalah dua tempat yang harus dibetulkan setiap kali.
namespace sim::editor {

class TerrainStore {
public:
    /// Terrain milik sebuah aset, dimuat bila perlu.
    ///
    /// Mengembalikan null bila berkasnya tidak bisa dibaca — dan **mencatat
    /// kegagalannya**, supaya berkas rusak tidak diurai ulang enam puluh kali
    /// per detik sambil membanjiri log dengan pesan yang sama.
    terrain::Terrain* Get(const Uuid& guid, const std::filesystem::path& path);

    /// Yang sudah dimuat, tanpa mencoba memuat.
    terrain::Terrain* Find(const Uuid& guid);

    /// Dokumen pendamping sebuah terrain: layer, nama berkas peta, dan
    /// pengaturan yang tersimpan di JSON-nya.
    terrain::TerrainDocument* Document(const Uuid& guid);

    /// Menaruh terrain yang sudah jadi di bawah sebuah guid.
    ///
    /// Bentuk mendahului berkas: terrain baru lahir di dalam editor, dan `Get`
    /// tidak bisa memuatnya karena belum ada yang ditulis.
    terrain::Terrain& Adopt(const Uuid& guid, terrain::Terrain value,
                            terrain::TerrainDocument document);

    /// Menandai sebuah terrain berubah: versinya naik dan ia menjadi belum
    /// tersimpan. Viewport memakai versinya untuk tahu kapan mengunggah ulang.
    void MarkDirty(const Uuid& guid);
    uint64_t Version(const Uuid& guid) const;
    /// True bila ada perubahan yang belum ditulis ke berkasnya.
    bool Dirty(const Uuid& guid) const;

    /// Menulis kembali ke berkasnya. False beserta sebabnya bila gagal.
    bool Save(const Uuid& guid, const std::filesystem::path& path, std::string& error);

    /// Mesh sebuah ubin, dibangun ulang **hanya** ketika bentuknya berubah atau
    /// perinciannya berganti.
    ///
    /// Bukan kemewahan: `SceneView::Build` berjalan tiap frame, dan membangun
    /// ulang ubin 512² per frame adalah ratusan ribu simpul yang dihitung untuk
    /// hasil yang sama persis. Yang menentukan "berubah" adalah revisi ubin itu
    /// **beserta kedelapan tetangganya** — meshnya membaca satu baris dari
    /// tetangga dan normalnya membaca satu sampel lagi ke luar, jadi menyunting
    /// ubin sebelah menggeser tepi ubin ini.
    ///
    /// Mengembalikan null bila terrainnya tidak ada.
    const assets::MeshData* TileMesh(const Uuid& guid, int tileX, int tileY, int lod,
                                     const terrain::TileNeighborLods& neighbors);

    /// Nomor yang naik setiap kali mesh sebuah ubin benar-benar dibangun ulang.
    ///
    /// Inilah yang diserahkan ke `IViewportRenderer::AcquireMeshData` sebagai
    /// kunci unggahan. Versi dokumen tidak bisa dipakai: ia naik untuk seluruh
    /// terrain, jadi menggores satu sudut peta akan mengunggah ulang keenam
    /// puluh empat ubinnya.
    uint64_t TileUpload(const Uuid& guid, int tileX, int tileY) const;

    /// Membuang yang sudah dimuat. Dipakai saat project berganti.
    void Clear();

    std::size_t OpenCount() const { return entries_.size(); }

private:
    /// Mesh sebuah ubin beserta keadaan yang menghasilkannya.
    ///
    /// Keadaan itu ikut disimpan, bukan hanya hasilnya: yang membandingkan
    /// hasil dengan hasil harus membangun dulu untuk tahu ia tidak perlu
    /// membangun.
    struct CachedTile {
        assets::MeshData mesh;
        uint64_t builtRevision = 0;
        int builtLod = -1;
        terrain::TileNeighborLods builtNeighbors;
        uint64_t upload = 1;
    };

    struct Entry {
        terrain::Terrain terrain;
        terrain::TerrainDocument document;
        uint64_t version = 1;
        bool failed = false;
        bool dirty = false;
        /// Diindeks `tileY * tilesX + tileX`.
        std::unordered_map<int, CachedTile> tiles;
    };

    Entry* FindEntry(const Uuid& guid);
    const Entry* FindEntry(const Uuid& guid) const;

    std::unordered_map<Uuid, Entry> entries_;
};

}  // namespace sim::editor
