#pragma once

#include "Sim/Core/AssetRef.h"
#include "Sim/Core/Math.h"
#include "Sim/Terrain/Terrain.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace sim::vegetation {

using sim::terrain::Terrain;

/// Batas jumlah layer vegetasi.
///
/// Sama dengan batas layer material, dan dengan alasan yang sebangun: setiap
/// layer adalah satu daftar instance dan satu peta kepadatan seukuran seluruh
/// terrain. Angka yang lebih besar hanya menghasilkan hutan yang tidak bisa
/// digambar.
inline constexpr int kMaxLayers = 16;

// --- acak deterministik -------------------------------------------------------

/// Aliran acak deterministik: splitmix64, seluruhnya bilangan bulat.
///
/// **Bukan `std::mt19937` beserta `std::uniform_real_distribution`.** Mesin
/// Mersenne-nya memang menghasilkan barisan bit yang sama di mana pun, tapi
/// distribusinya tidak: standar tidak menetapkan bagaimana sebuah distribusi
/// mengubah bit menjadi angka, jadi dua pustaka standar boleh memberi hasil
/// berbeda dari benih yang sama. Kriteria terima E7.4 menuntut sebaran yang sama
/// persis di mesin berbeda — dan itu berarti setiap langkah dari benih sampai
/// posisi harus ditulis di sini, bukan dipinjam.
class Rng {
public:
    explicit Rng(uint64_t seed) : state_(seed) {}

    uint64_t NextU64();
    uint32_t NextU32() { return static_cast<uint32_t>(NextU64() >> 32); }

    /// Pecahan [0,1).
    ///
    /// 24 bit teratas dikali pangkat dua yang eksak. Perkalian dengan pangkat
    /// dua tidak pernah membulatkan pada FPU IEEE-754 mana pun, dan tidak ada
    /// pembagian maupun libm di jalurnya — jadi bit hasilnya ditentukan
    /// sepenuhnya oleh bit masukannya.
    float NextFloat() { return static_cast<float>(NextU32() >> 8) * 0x1.0p-24f; }
    float NextFloat(float low, float high) { return low + (high - low) * NextFloat(); }

private:
    uint64_t state_;
};

/// Benih satu sel kisi sebaran.
///
/// Setiap sel punya alirannya sendiri, bukan satu aliran panjang yang dibaca
/// berurutan. Itu yang membuat sebaran tidak bergantung pada urutan kunjungan
/// maupun pada berapa banyak kandidat yang ditolak sebelumnya — dan karena itu
/// sebuah sel bisa dihitung ulang sendirian, oleh siapa pun, kapan pun.
uint64_t CellSeed(uint32_t seed, int32_t cellX, int32_t cellY);

/// Identitas sebuah instance hasil sebaran: posisi XZ-nya, dibulatkan ke
/// milimeter.
///
/// Dipakai daftar hapus manual. **Kuncinya posisi, bukan nomor urut**, karena
/// nomor urut bergeser begitu sebuah aturan diubah — dan penghapusan yang
/// bergeser menghapus pohon yang salah, diam-diam. Posisi tidak bergeser: yang
/// menentukannya hanya benih dan jarak minimum, jadi mengubah rentang tinggi,
/// kemiringan, kepadatan, skala, atau layer terrain tidak memindahkan satu pun
/// instance yang tetap lolos.
///
/// Kuncinya unik menurut konstruksi: sebaran menjamin jarak antar-instance tidak
/// kurang dari `minDistance`, dan `minDistance` dijepit jauh di atas satu
/// milimeter — jadi dua instance tidak mungkin membulat ke kunci yang sama.
///
/// Satu-satunya perubahan aturan yang membatalkan daftar hapus adalah mengubah
/// `minDistance` atau `seed`, karena keduanya memang memindahkan semuanya.
uint64_t InstanceKey(float worldX, float worldZ);
uint64_t PackKey(int32_t x, int32_t z);
void UnpackKey(uint64_t key, int32_t& x, int32_t& z);

// --- aturan dan layer ---------------------------------------------------------

/// Aturan penempatan satu layer.
struct PlacementRules {
    float minHeight = -10000.0f;
    float maxHeight = 10000.0f;
    float minSlopeDegrees = 0.0f;
    float maxSlopeDegrees = 45.0f;
    /// Layer material terrain yang harus ada di bawahnya, -1 kalau bebas.
    int terrainLayer = -1;
    /// Bobot minimum layer itu, 0..255.
    int minTerrainWeight = 128;
    /// Jarak minimum antar-instance, meter (Poisson disk).
    float minDistance = 4.0f;
    /// Bagian kandidat lolos yang benar-benar ditanam, 0..1. Dikalikan peta
    /// kepadatan yang dicat.
    float density = 1.0f;
    uint32_t seed = 1;
    /// Lubang terrain tidak ditumbuhi. Bisa dimatikan untuk vegetasi yang
    /// memang menutupi mulut gua.
    bool avoidHoles = true;
};

/// Jarak minimum terkecil yang diizinkan, meter.
///
/// Bukan sekadar penjaga pembagian nol. Sel kisi sebaran berukuran persis
/// `minDistance`, dan seluruh anggaran memori serta jumlah kandidat mengikuti
/// ukuran itu; jarak sekecil satu sentimeter pada terrain sekilometer berarti
/// sepuluh miliar sel. Batas ini juga yang membuat kunci milimeter aman.
inline constexpr float kMinInstanceDistance = 0.05f;

/// Kisi kepadatan yang dicat tangan. Satu byte per sel, 255 = penuh.
///
/// **Peta yang belum dicat tidak menyimpan apa pun**, dan dibaca sebagai 255 di
/// mana-mana. Itu bukan penghematan memori semata: ia yang membedakan "layer ini
/// belum pernah dicat" dari "layer ini dicat nol di mana-mana", dan tanpa
/// pembedaan itu tidak ada cara memutuskan berkas mana yang perlu ditulis.
///
/// **Resolusinya kasar dengan sengaja.** Kepadatan adalah medan yang halus —
/// tepi hutan, rumpun di lembah — dan menyimpannya sehalus heightmap berarti
/// membayar puluhan megabyte per layer untuk gradasi yang tidak bisa dilihat
/// siapa pun. Yang dibayar sebaliknya adalah pencuplikan bilinear, supaya tepi
/// hutan tidak berbentuk kotak-kotak sebesar selnya.
class DensityMap {
public:
    void Reset(int width, int height, float cellSize);

    int Width() const { return width_; }
    int Height() const { return height_; }
    float CellSize() const { return cellSize_; }
    bool Painted() const { return !cells_.empty(); }
    std::size_t Bytes() const { return cells_.size(); }

    uint8_t At(int x, int y) const;
    void SetAt(int x, int y, uint8_t value);

    /// Kepadatan terinterpolasi bilinear pada posisi dunia, 0..1.
    float SampleWorld(float worldX, float worldZ) const;

    void Clear() { cells_.clear(); }
    const std::vector<uint8_t>& Cells() const { return cells_; }
    /// Menulis seluruh kisi. Menuntut `width*height` byte.
    void SetCells(const uint8_t* values);
    /// Menukar isi kisi dengan yang diberikan; kosong berarti belum dicat.
    /// Dipakai jurnal undo, dengan alasan yang sama seperti blok jurnal terrain:
    /// satu salinan melayani undo dan redo sekaligus.
    void SwapCells(std::vector<uint8_t>& cells);

private:
    int width_ = 0;
    int height_ = 0;
    float cellSize_ = 1.0f;
    std::vector<uint8_t> cells_;
};

/// Satu layer vegetasi.
struct VegetationLayer {
    std::string name = "Layer";
    /// Mesh atau prefab yang ditanam.
    AssetRef model;
    /// Warna wakil layer di peta 2D panel — alasannya sama dengan warna layer
    /// terrain: panel tidak punya jalan ke `rhi::Device`, jadi tidak ada yang
    /// bisa menggambar mesh-nya. Ia tetap berguna setelah viewport 3D ada:
    /// sebaran satu juta titik hanya terbaca sebagai peta warna datar.
    Vec3 color{0.35f, 0.55f, 0.28f};

    PlacementRules rules;

    float minScale = 0.8f;
    float maxScale = 1.2f;
    bool randomYaw = true;
    /// 0 = selalu tegak, 1 = mengikuti normal permukaan sepenuhnya.
    float alignToNormal = 0.0f;
    /// Pergeseran vertikal setelah menempel di permukaan, meter. Negatif
    /// menanamkan pangkalnya sedikit supaya tidak melayang di lereng.
    float offsetY = -0.05f;

    float lodDistance = 60.0f;
    float billboardDistance = 150.0f;
    float cullDistance = 400.0f;
    bool visible = true;

    /// Berkas peta kepadatan pendamping, relatif terhadap `.simveg`-nya. Kosong
    /// pada layer yang belum pernah dicat.
    std::string densityFile;
};

/// Satu instance yang ditanam.
struct Instance {
    Vec3 position{0.0f};
    /// Sumbu tegak instance, setelah dicampur `alignToNormal`.
    Vec3 up{0.0f, 1.0f, 0.0f};
    /// Radian, di sekitar sumbu Y.
    float yaw = 0.0f;
    float scale = 1.0f;
    /// Ditanam tangan, bukan hasil sebaran. Dibawa di instance-nya sendiri, dan
    /// bukan disimpulkan dari letaknya di dalam daftar, supaya membatalkan
    /// sebuah penghapusan cukup menaruhnya kembali di mana saja.
    bool manual = false;
};

/// Menyusun sebuah instance dari permukaan yang sudah dicuplik.
///
/// Menerima tinggi dan normal yang sudah jadi, bukan terrain-nya, karena
/// pemanggil terpanasnya — sebaran — sudah memegang keduanya dan mencuplik ulang
/// berarti melipatgandakan bagian termahal dari seluruh proses. Yang penting ia
/// satu-satunya tempat yang memutuskan bagaimana undian menjadi skala, rotasi,
/// dan kemiringan: rumus kedua di jalur menanam tangan berarti pohon yang
/// ditanam sendiri berdiri sedikit berbeda dari tetangganya, dan tidak ada yang
/// bisa menunjuk sebabnya.
Instance MakeInstance(const VegetationLayer& layer, float worldX, float worldZ, float height,
                      const Vec3& normal, float scaleRoll, float yawRoll);

/// Kumpulan layer vegetasi beserta instance-nya.
///
/// **Instance tidak disimpan; aturan dan benihnya yang disimpan.** Satu juta
/// instance adalah 36 MB di memori dan puluhan megabyte di berkas apa pun yang
/// menuliskannya satu per satu — dan seluruhnya bisa dihitung ulang dari
/// beberapa ratus byte aturan. Yang tidak bisa dihitung ulang hanyalah
/// suntingan tangan, dan hanya itu yang ikut ditulis.
///
/// **Sebaran adalah fungsi murni dari (aturan, benih, terrain).** Itu yang
/// membuat "benih yang sama menghasilkan sebaran yang sama di mesin berbeda"
/// menjadi sifat bentuknya, bukan sesuatu yang harus dijaga. Konsekuensinya
/// menyebar ulang selalu menyebar ulang **seluruh layer**: pemadatan Poisson
/// bergantung pada urutan, jadi menyebar ulang sepotong akan diam-diam berbeda
/// dari yang dijanjikan benihnya di sepanjang tepi potongan itu.
///
/// **Susunan titiknya ditentukan sebelum aturan mana pun dilihat**, jadi benih,
/// jarak minimum, dan ukuran dunia sajalah yang menentukan di mana sesuatu
/// *bisa* tumbuh. Setiap aturan lain — tinggi, kemiringan, layer terrain,
/// lubang, kepadatan yang dicat — hanya memilih siapa dari susunan itu yang
/// benar-benar berdiri. Karena itu mengetatkan sebuah aturan menipiskan hutan
/// tanpa memindahkan satu pun pohon yang tetap lolos, dan penghapusan tangan
/// yang menunjuk sebuah posisi masih menunjuk pohon yang sama sesudahnya.
class Vegetation {
public:
    /// Sisi sel kisi kepadatan bawaan, meter.
    static constexpr float kDefaultDensityCell = 2.0f;

    /// Kandidat yang dicoba per sel kisi sebaran.
    ///
    /// Satu kandidat per sel meninggalkan lubang di mana-mana — kandidat yang
    /// ditolak tetangganya tidak pernah dicoba lagi, jadi pemadatannya berhenti
    /// jauh di bawah yang dijanjikan `minDistance`. Empat sudah mendekati
    /// pemadatan Poisson maksimum; lebih banyak hanya menambah ongkos pada
    /// kandidat yang hampir selalu ditolak.
    static constexpr int kCandidatesPerCell = 4;

    /// Instance terbanyak yang muat dalam satu sel kisi sebaran.
    ///
    /// Persis empat, dan itu bisa dibuktikan, bukan ditebak: sel berukuran
    /// `minDistance` persegi bisa dibagi empat menjadi sub-persegi yang
    /// diagonalnya `minDistance/√2` — lebih pendek daripada jarak minimum — jadi
    /// dua instance tidak mungkin berbagi sub-persegi. Empat sudut sel memang
    /// bisa terisi seluruhnya, jadi empat juga tidak bisa dikurangi.
    static constexpr int kInstancesPerCell = 4;

    /// Batas byte jurnal undo, sama alasannya dengan jurnal terrain.
    std::size_t undoBudgetBytes = 64u * 1024u * 1024u;

    // --- layer ---------------------------------------------------------------

    int LayerCount() const { return static_cast<int>(layers_.size()); }
    const VegetationLayer& Layer(int index) const;
    VegetationLayer& Layer(int index);

    /// Menambah layer. Mengembalikan indeksnya, atau -1 kalau sudah penuh.
    int AddLayer(const VegetationLayer& layer);
    bool RemoveLayer(int index);
    bool MoveLayer(int from, int to);
    /// Mengganti seluruh daftar layer, membuang instance dan suntingan tangan.
    /// Dipakai pemuat berkas, ketika dokumennya memang baru.
    void SetLayers(const std::vector<VegetationLayer>& layers);

    // --- kisi kepadatan -------------------------------------------------------

    float DensityCellSize() const { return densityCell_; }
    /// Mengubah resolusi kisi kepadatan. **Membuang cat yang sudah ada**:
    /// mencuplik ulang peta ke kisi lain menghasilkan tepi hutan yang bergeser
    /// tanpa ada yang menggesernya.
    void SetDensityCellSize(float meters);

    /// Menyesuaikan kisi kepadatan dengan ukuran terrain. Dipanggil setelah
    /// membuka dokumen dan sebelum mengecat.
    void Fit(const Terrain& terrain);

    int DensityWidth() const { return densityWidth_; }
    int DensityHeight() const { return densityHeight_; }
    /// Menentukan ukuran kisi langsung. Dipakai pemuat berkas, yang mengetahui
    /// ukurannya dari PNG pendampingnya — terrain-nya sendiri baru bisa dicari
    /// setelah GUID di berkas ini terbaca.
    void SetDensityGrid(int width, int height);

    DensityMap& Density(int layer);
    const DensityMap& Density(int layer) const;

    /// Menulis satu sel kepadatan lewat jurnal undo.
    void PaintDensity(int layer, int cellX, int cellY, uint8_t value);
    void ClearDensity(int layer);

    // --- sebaran --------------------------------------------------------------

    /// Menyebar ulang satu layer. Mengembalikan jumlah instance-nya.
    std::size_t Scatter(const Terrain& terrain, int layer);
    std::size_t ScatterAll(const Terrain& terrain);
    /// Membuang instance sebuah layer tanpa menyentuh aturan maupun suntingan
    /// tangannya.
    void ClearInstances(int layer);

    const std::vector<Instance>& Instances(int layer) const;
    std::size_t InstanceCount() const;

    /// Menempelkan ulang instance ke permukaan di dalam persegi dunia.
    ///
    /// **Bukan menyebar ulang.** Memahat di bawah hutan yang sudah jadi lalu
    /// menyebar ulang berarti setiap goresan brush mengocok seluruh hutan:
    /// pohon berpindah, hilang, dan muncul di tempat lain sementara yang
    /// diminta hanyalah tanah di bawahnya naik. Menempelkan ulang menjaga XZ
    /// setiap instance tetap di tempatnya dan hanya memperbarui apa yang memang
    /// berubah — tingginya dan normal permukaannya.
    ///
    /// Aturan penempatan sengaja **tidak** diperiksa ulang di sini. Instance
    /// yang lerengnya menjadi terlalu curam karena goresan tetap berdiri sampai
    /// disebar ulang; menghapusnya di tengah goresan berarti pohon yang lenyap
    /// di bawah kuas sculpt tanpa ada yang menghapusnya.
    std::size_t RefreshHeights(const Terrain& terrain, float minX, float minZ, float maxX,
                               float maxZ);
    std::size_t RefreshHeights(const Terrain& terrain);

    // --- suntingan tangan -----------------------------------------------------

    /// Menanam satu instance. Ia bertahan melewati sebaran ulang.
    void Plant(int layer, const Instance& instance);
    /// Menghapus instance di dalam lingkaran. Mengembalikan jumlahnya.
    std::size_t Erase(int layer, float worldX, float worldZ, float radius);
    /// Membuang seluruh suntingan tangan sebuah layer.
    void ClearManual(int layer);

    std::size_t AddedCount(int layer) const;
    std::size_t RemovedCount(int layer) const;
    const std::vector<Instance>& Added(int layer) const;
    /// Kunci instance sebaran yang dihapus tangan.
    const std::vector<uint64_t>& Removed(int layer) const;
    void SetManual(int layer, const std::vector<Instance>& added,
                   const std::vector<uint64_t>& removed);

    // --- goresan dan undo -----------------------------------------------------

    /// Satu goresan adalah satu satuan undo, sama seperti goresan terrain, dan
    /// ia mencakup cat kepadatan maupun tanam/hapus per-instance — riwayat yang
    /// terpisah per alat berarti Ctrl+Z yang artinya bergantung pada tab mana
    /// yang sedang terbuka.
    void BeginStroke();
    void EndStroke();
    bool InStroke() const { return inStroke_; }

    bool Undo();
    bool Redo();
    std::size_t UndoDepth() const { return undo_.size(); }
    std::size_t RedoDepth() const { return redo_.size(); }
    void ClearHistory();

    /// Byte yang benar-benar dialokasikan: instance, peta kepadatan yang dicat,
    /// suntingan tangan, dan jurnal undo.
    std::size_t BytesResident() const;
    std::size_t UndoBytes() const { return undoBytes_; }

private:
    struct LayerData {
        VegetationLayer desc;
        DensityMap density;
        std::vector<Instance> instances;
        std::vector<Instance> added;
        std::vector<uint64_t> removed;
        std::unordered_set<uint64_t> removedSet;
    };

    /// Satu sel kepadatan yang disalin sebelum disentuh.
    struct DensityCell {
        int layer = 0;
        int32_t cell = 0;
        uint8_t before = 0;
    };

    /// Seluruh peta kepadatan sebuah layer, disalin sekaligus.
    ///
    /// Ada di samping `DensityCell`, bukan menggantikannya, karena kedua
    /// operasinya berbeda ukuran dua orde: sebuah goresan kuas menyentuh puluhan
    /// sel, sedangkan "kembalikan kepadatan" menyentuh semuanya. Menjurnalkan
    /// yang kedua sel demi sel berarti puluhan juta catatan — lebih besar
    /// daripada peta yang sedang disalinnya.
    struct DensityImage {
        int layer = 0;
        std::vector<uint8_t> cells;
    };

    /// Instance yang dihapus dalam sebuah goresan, beserta apa yang harus
    /// dikembalikan bersamanya.
    struct ErasedInstance {
        int layer = 0;
        Instance instance;
        /// Kunci yang masuk ke daftar hapus. Nol untuk instance manual, yang
        /// dikembalikan ke `added` alih-alih dikeluarkan dari `removed`.
        uint64_t key = 0;
    };

    struct PlantedInstance {
        int layer = 0;
        Instance instance;
    };

    struct Stroke {
        std::vector<DensityCell> density;
        std::vector<DensityImage> images;
        std::vector<ErasedInstance> erased;
        std::vector<PlantedInstance> planted;
        std::size_t bytes = 0;

        bool Empty() const {
            return density.empty() && images.empty() && erased.empty() && planted.empty();
        }
    };

    void CaptureDensityCell(int layer, int32_t cell, uint8_t before);
    void ApplyStroke(Stroke& stroke, bool undoing);
    void TrimJournal();
    void PushStroke();

    void InsertRemovedKey(LayerData& data, uint64_t key);
    void EraseRemovedKey(LayerData& data, uint64_t key);

    std::vector<LayerData> layers_;
    float densityCell_ = kDefaultDensityCell;
    int densityWidth_ = 0;
    int densityHeight_ = 0;

    bool inStroke_ = false;
    Stroke current_;
    /// Sel kepadatan yang sudah disalin dalam goresan berjalan, sebagai kunci
    /// gabungan (layer, sel). Tanpa ini, sel yang disentuh puluhan kali dalam
    /// satu goresan akan disalin puluhan kali — dan undo-nya mengembalikan
    /// keadaan di tengah goresan, bukan sebelumnya.
    std::unordered_set<uint64_t> captured_;

    std::vector<Stroke> undo_;
    std::vector<Stroke> redo_;
    std::size_t undoBytes_ = 0;
};

}  // namespace sim::vegetation
