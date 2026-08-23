#pragma once

#include "Sim/Assets/MeshData.h"
#include "Sim/Core/Uuid.h"
#include "Sim/Whitebox/WhiteboxMesh.h"

#include <algorithm>
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

/// Jenis sub-objek yang sedang disunting.
///
/// **Modenya menentukan apa yang bisa diklik, bukan apa yang tergambar.** Simpul
/// dan rusuk tetap terlihat di mode mana pun — perancang perlu melihat bentuk
/// yang sedang ia sunting — tetapi hanya yang sejenis mode aktif yang bisa
/// dipilih. Tanpa itu, sebuah klik di dekat sudut sebuah sisi menjadi tebakan
/// antara tiga jawaban yang sama-sama masuk akal.
enum class SubObject : uint8_t {
    Face,
    Edge,
    Vertex,
};

/// Sub-objek yang sedang disorot, beserta aset pemiliknya.
///
/// **Bukan disimpan di dalam meshnya.** Seleksi adalah keadaan penyunting, dan
/// menaruhnya di dalam aset berarti membuka berkas yang sama di dua tempat
/// saling menggeser sorotan — dan menyimpannya ke disk sebagai bagian bentuk.
///
/// **Himpunan, bukan satu handle.** Sampai W5 ia memuat satu `PolygonHandle`,
/// dan itu cukup selama satu-satunya operasi adalah mendorong satu sisi. Setiap
/// operasi W7.1 menuntut lebih dari satu: perataan menolak kurang dari dua
/// simpul, dan penyisipan rusuk menghubungkan sepasang.
///
/// Ketiga daftar hidup berdampingan alih-alih satu daftar bertipe: berpindah
/// mode lalu kembali mengembalikan seleksi yang tadi — dan seleksi yang hilang
/// hanya karena mata melirik mode lain adalah pekerjaan yang diulang.
struct SideSelection {
    Uuid asset;
    SubObject mode = SubObject::Face;
    std::vector<whitebox::PolygonHandle> polygons;
    std::vector<whitebox::EdgeHandle> edges;
    std::vector<whitebox::VertexHandle> vertices;

    /// Ada sesuatu yang terpilih di aset ini, pada mode yang sedang aktif.
    bool Has(const Uuid& guid) const { return asset == guid && Count() > 0; }

    std::size_t Count() const {
        switch (mode) {
            case SubObject::Face: return polygons.size();
            case SubObject::Edge: return edges.size();
            case SubObject::Vertex: return vertices.size();
        }
        return 0;
    }

    /// Poligon acuan — yang terakhir dipilih.
    ///
    /// Ada untuk pemanggil yang memang hanya bisa memakai satu: gizmo sisi
    /// berdiri di titik berat sebuah poligon, dan panel material menetapkan
    /// material ke satu sisi.
    whitebox::PolygonHandle PrimaryPolygon() const {
        return polygons.empty() ? whitebox::PolygonHandle::Invalid : polygons.back();
    }

    bool Contains(whitebox::PolygonHandle polygon) const {
        return std::find(polygons.begin(), polygons.end(), polygon) != polygons.end();
    }
    bool Contains(whitebox::EdgeHandle edge) const {
        return std::find(edges.begin(), edges.end(), edge) != edges.end();
    }
    bool Contains(whitebox::VertexHandle vertex) const {
        return std::find(vertices.begin(), vertices.end(), vertex) != vertices.end();
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

    /// Berpindah jenis sub-objek. Isi seleksi jenis lain tidak dibuang — lihat
    /// catatan di `SideSelection`.
    void SetSubObject(SubObject mode) { selected_.mode = mode; }
    SubObject SubObjectMode() const { return selected_.mode; }

    /// Menambahkan, membuang, atau membalik satu sub-objek.
    ///
    /// **Berpindah aset mengosongkan seluruhnya.** Seleksi yang separuhnya milik
    /// whitebox lain adalah seleksi yang operasinya tidak punya arti — dan
    /// tidak ada satu pun pesan yang bisa menjelaskan kenapa perataan menolak.
    void Toggle(const Uuid& guid, whitebox::PolygonHandle polygon);
    void Toggle(const Uuid& guid, whitebox::EdgeHandle edge);
    void Toggle(const Uuid& guid, whitebox::VertexHandle vertex);
    void Add(const Uuid& guid, whitebox::EdgeHandle edge);
    void Add(const Uuid& guid, whitebox::VertexHandle vertex);
    void Select(const Uuid& guid, whitebox::PolygonHandle polygon);
    /// Mengosongkan apa yang terpilih — **tanpa menyentuh modenya.**
    ///
    /// Klik di ruang kosong membatalkan pilihan, bukan membatalkan mode.
    /// Perancang yang sedang menyunting simpul dan meleset sekali tidak sedang
    /// meminta kembali ke mode sisi, dan mode yang melompat sendiri adalah klik
    /// berikutnya yang mengenai jenis yang salah.
    void ClearSelection() {
        const SubObject mode = selected_.mode;
        selected_ = {};
        selected_.mode = mode;
    }

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
