#pragma once

#include "Sim/Core/AssetRef.h"
#include "Sim/Core/Math.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace sim::terrain {

/// Satu sampel tinggi.
///
/// **16 bit bulat, bukan float.** Ini bukan penghematan memori semata: kriteria
/// terima E7.3 menuntut round-trip PNG 16-bit tanpa kehilangan presisi, dan itu
/// bukan sifat importernya melainkan sifat penyimpanannya. Dengan float32 di
/// dalam, tidak ada importer yang bisa memenuhinya — setiap ekspor membulatkan.
/// Dengan uint16 di dalam, ekspor dan impor sama-sama sekadar menyalin.
///
/// 65536 tingkat pada rentang tinggi 1000 m berarti langkah 1,5 cm. Itu lebih
/// halus daripada yang bisa dilihat pada terrain seluas kilometer, dan sama
/// dengan yang dipakai heightmap PNG/RAW yang dipertukarkan dengan World Machine
/// dan sejenisnya.
using Sample = uint16_t;

inline constexpr Sample kSampleMax = 65535;

struct TerrainDesc {
    /// Sampel per sisi tile.
    int tileSamples = 512;
    int tilesX = 4;
    int tilesY = 4;
    /// Jarak antar sampel, meter.
    float sampleSpacing = 1.0f;
    /// Pemetaan sampel → meter. `minHeight` untuk 0, `maxHeight` untuk 65535.
    float minHeight = 0.0f;
    float maxHeight = 1000.0f;
    /// Tinggi terrain yang belum disentuh, meter.
    float baseHeight = 0.0f;
    /// Batas byte jurnal undo. Goresan terlama dibuang saat terlampaui.
    ///
    /// Ada batasnya karena jurnal tanpa batas adalah kebocoran yang tumbuh
    /// sebanding dengan lama sesi — dan pada terrain 4×4 km, beberapa ratus
    /// goresan besar sudah cukup untuk menghabiskan RAM mesin.
    std::size_t undoBudgetBytes = 128u * 1024u * 1024u;
};

/// Bobot satu layer material pada satu sampel. 0 = tidak terlihat, 255 = penuh.
using Weight = uint8_t;

inline constexpr Weight kWeightMax = 255;

/// Batas jumlah layer material.
///
/// Bukan batas penyimpanannya melainkan batas yang jujur: setiap layer tambahan
/// adalah satu peta bobot seukuran seluruh terrain, dan tidak ada perender yang
/// memadukan puluhan layer dalam satu lintasan. Angka yang lebih besar hanya
/// akan menghasilkan terrain yang tidak bisa digambar.
inline constexpr int kMaxLayers = 16;

/// Satu layer material terrain.
struct TerrainLayer {
    std::string name = "Layer";
    /// Material yang dicat layer ini.
    AssetRef material;
    /// Warna wakil layer di peta 2D panel.
    ///
    /// Panel tidak bisa menggambar tekstur material — tidak ada jalan dari panel
    /// ke `rhi::Device` — jadi warna inilah yang mewakili layer sampai viewport
    /// 3D ada. Ia juga tetap berguna setelah itu: peta dari atas yang memakai
    /// warna datar per layer memperlihatkan batas cat jauh lebih jelas daripada
    /// tekstur yang sebenarnya.
    Vec3 color{0.55f, 0.52f, 0.45f};
    /// Meter per pengulangan tekstur.
    float tileSize = 8.0f;
    /// Berkas peta bobot pendamping, relatif terhadap `.simterrain`-nya.
    ///
    /// Kosong pada layer dasar — bobotnya sisa, bukan data — dan kosong pada
    /// layer yang belum pernah dicat.
    std::string weightFile;
};

/// Persegi panjang dalam koordinat sampel global, batas kanan/bawah eksklusif.
struct SampleRect {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;

    bool Empty() const { return x1 <= x0 || y1 <= y0; }
    int Width() const { return x1 - x0; }
    int Height() const { return y1 - y0; }
};

/// Heightmap berubin dengan undo per goresan.
///
/// **Sampel dialamati secara global, dan setiap sampel hanya tinggal di satu
/// tile.** Tile (tx,ty) memiliki rentang setengah terbuka
/// `[tx*S, (tx+1)*S) × [ty*S, (ty+1)*S)` — tidak ada baris tepi yang disalin ke
/// tetangganya.
///
/// Itu yang membuat "retakan data" di batas tile mustahil, bukan sekadar
/// jarang. Retakan adalah gejala dari dua salinan satu kebenaran: engine yang
/// menyimpan baris tepi di kedua tile harus menulis keduanya setiap kali, dan
/// satu jalur kode yang lupa melakukannya menghasilkan celah yang baru terlihat
/// setelah di-render. Dengan satu salinan, tidak ada yang bisa berbeda.
///
/// Ongkosnya pindah ke pembuatan mesh: quad yang menyeberangi batas tile perlu
/// satu baris dari tetangganya, jadi mesh tile membaca `RawAt` alih-alih hanya
/// array tile-nya sendiri. Itu pertukaran yang benar — membaca tetangga adalah
/// operasi yang jelas dan terlokalisasi, sedangkan menjaga dua salinan tetap
/// sama adalah kewajiban yang tersebar ke setiap penulis.
///
/// **Tile dialokasikan saat pertama ditulis.** Terrain 4×4 km dengan heightmap
/// 2048² per tile berukuran dua gigabyte kalau seluruhnya penghuni memori;
/// yang dibayar hanyalah yang benar-benar disunting.
class Terrain {
public:
    /// Sisi blok jurnal undo, dalam sampel.
    ///
    /// Undo menyalin blok, bukan tile. Menyalin seluruh tile 2048² berarti 8 MB
    /// untuk satu sentuhan brush selebar sepuluh meter — persis lonjakan memori
    /// yang dilarang kriteria terima. 64² sampel = 8 KB per blok, dan sebuah
    /// goresan hanya menyalin blok yang benar-benar disentuhnya.
    static constexpr int kBlockSize = 64;

    explicit Terrain(const TerrainDesc& desc = {});

    const TerrainDesc& Desc() const { return desc_; }
    int SamplesX() const { return desc_.tilesX * desc_.tileSamples; }
    int SamplesY() const { return desc_.tilesY * desc_.tileSamples; }
    /// Ukuran dunia dalam meter.
    float WorldWidth() const { return static_cast<float>(SamplesX() - 1) * desc_.sampleSpacing; }
    float WorldDepth() const { return static_cast<float>(SamplesY() - 1) * desc_.sampleSpacing; }

    /// Nilai mentah pada koordinat sampel global. Di luar batas dijepit ke tepi,
    /// bukan dianggap galat: brush yang menyentuh pinggir peta adalah hal biasa,
    /// dan setiap pemanggil yang harus memeriksa batas sendiri adalah satu
    /// pemanggil yang bisa lupa.
    Sample RawAt(int x, int y) const;
    void SetRawAt(int x, int y, Sample value);

    float HeightAt(int x, int y) const { return ToMeters(RawAt(x, y)); }
    void SetHeightAt(int x, int y, float meters) { SetRawAt(x, y, ToSample(meters)); }

    /// Tinggi terinterpolasi bilinear pada posisi dunia (meter).
    float HeightAtWorld(float worldX, float worldZ) const;

    /// Normal permukaan pada posisi dunia, ternormalisasi.
    ///
    /// **Gradien analitik dari permukaan bilinear yang sama**, bukan beda hingga
    /// dari `HeightAtWorld` di empat titik di sekitarnya. Bukan demi kecepatan
    /// saja — walaupun empat sampel jauh lebih murah daripada enam belas — tapi
    /// karena beda hingga dengan langkah sembarang menjawab pertanyaan yang
    /// berbeda dari yang ditanyakan: ia melaporkan lereng rata-rata selebar
    /// langkahnya, jadi normalnya tidak pernah cocok dengan permukaan yang
    /// benar-benar digambar dari sampel yang sama. Gradien analitik cocok
    /// menurut definisi.
    Vec3 NormalAtWorld(float worldX, float worldZ) const;

    float ToMeters(Sample value) const;
    Sample ToSample(float meters) const;

    /// Rentang sampel yang tersentuh sebuah lingkaran dunia, sudah dijepit ke
    /// peta.
    SampleRect RectForCircle(float worldX, float worldZ, float radius) const;

    /// Menyalin seluruh heightmap, baris demi baris. Dipakai ekspor: memanggil
    /// `RawAt` per sampel pada peta 16k² berarti seperempat miliar pembagian
    /// hanya untuk mencari tile yang sama berulang-ulang.
    void ReadAll(std::vector<Sample>& out) const;

    /// Kebalikannya. **Tidak masuk jurnal undo**: mengimpor heightmap adalah
    /// operasi dokumen, bukan goresan brush — dan menjurnalnya berarti menyalin
    /// seluruh peta ke dalam riwayat, yaitu lonjakan memori yang justru sedang
    /// dihindari.
    void WriteAll(const Sample* samples);

    // --- layer material dan bobotnya -----------------------------------------

    /// **Bobot layer 0 tidak disimpan. Ia sisa: `255 − Σ(bobot layer lain)`.**
    ///
    /// Itu yang membuat "total bobot tiap sampel selalu 255" menjadi sifat
    /// bentuknya, bukan kewajiban yang harus diingat setiap penulis — dan
    /// invarian itulah yang menentukan apakah hasil paint bisa diduga. Kalau
    /// totalnya bebas, sebuah sampel bisa berakhir bertotal 40, dan perender
    /// harus memilih di antara dua kesalahan: menormalkan saat menggambar
    /// (sehingga menghapus semua layer tidak menghapus apa pun, karena sisa
    /// sekecil apa pun diregangkan kembali menjadi penuh) atau tidak menormalkan
    /// (sehingga ada bercak gelap yang tidak dicat siapa pun). Dengan layer dasar
    /// sebagai sisa, tidak ada sampel yang bisa kehilangan seluruh materialnya.
    ///
    /// Konsekuensinya layer 0 tidak bisa dihapus maupun dipindah, dan mengecat
    /// layer 0 berarti menghapus layer di atasnya — yang memang persis arti
    /// "mengecat kembali ke dasar".
    int LayerCount() const { return static_cast<int>(layers_.size()); }
    const TerrainLayer& Layer(int index) const;
    TerrainLayer& Layer(int index);

    /// Menambah layer. Mengembalikan indeksnya, atau -1 kalau sudah penuh.
    int AddLayer(const TerrainLayer& layer);
    /// Menghapus layer beserta peta bobotnya. Bobot yang dilepaskannya kembali
    /// ke layer dasar — itu bukan pilihan melainkan akibat langsung dari dasar
    /// yang berupa sisa.
    bool RemoveLayer(int index);
    /// Memindahkan layer beserta peta bobotnya. Layer 0 tidak ikut serta.
    bool MoveLayer(int from, int to);
    /// Mengganti seluruh daftar layer. Peta bobot layer yang bertahan **tidak**
    /// dipertahankan: dipakai saat memuat berkas, ketika terrainnya memang baru.
    void SetLayers(const std::vector<TerrainLayer>& layers);

    Weight WeightAt(int layer, int x, int y) const;
    /// Menetapkan bobot sebuah layer, lalu menyusutkan layer lain secukupnya
    /// supaya totalnya tetap 255.
    void SetWeightAt(int layer, int x, int y, Weight weight);

    /// Sebuah layer punya ubin terwujud, yaitu pernah dicat. Dipakai I/O untuk
    /// tidak menulis berkas bobot yang seluruhnya nol.
    bool LayerPainted(int layer) const;
    void ClearLayerWeights(int layer);

    void ReadWeights(int layer, std::vector<Weight>& out) const;
    /// Menulis peta bobot mentah, tanpa menyentuh layer lain — jadi ia **bisa**
    /// meninggalkan total di atas 255. Dipakai pemuat berkas, yang memanggil
    /// `NormalizeWeights()` setelah seluruh layer masuk.
    void WriteWeights(int layer, const Weight* weights);

    /// Menyusutkan bobot sampai totalnya kembali ≤ 255 di mana pun terlampaui.
    ///
    /// Ada karena peta bobot datang dari berkas yang bisa disunting di luar
    /// editor. Invarian yang hanya dijaga oleh jalur paint adalah invarian yang
    /// batal begitu ada jalan masuk kedua.
    void NormalizeWeights();

    // --- peta hole -----------------------------------------------------------

    /// **Hole adalah sifat quad, bukan sifat sampel.**
    ///
    /// Yang dihapus perender adalah segi empat di antara empat sampel, bukan
    /// sampelnya. Kalau hole disimpan per sampel, ada dua pilihan dan keduanya
    /// salah: quad dihapus bila salah satu sudutnya ditandai — maka lubang yang
    /// tergambar selalu lebih besar daripada yang dicat dan lubang selebar satu
    /// quad mustahil dibuat; atau quad dihapus hanya bila keempat sudutnya
    /// ditandai — maka lubang kecil tidak pernah muncul sama sekali. Disimpan
    /// per quad, yang dicat dan yang tergambar adalah benda yang sama.
    ///
    /// Quad (x,y) membentang antara sampel x..x+1 dan y..y+1, jadi kolom dan
    /// baris terakhir peta tidak memiliki quad; `HoleAt` di sana selalu false.
    /// Keduanya tetap disediakan penyimpanannya supaya peta hole berukuran sama
    /// dengan heightmap-nya, dan berkas pendampingnya bisa dibuka bersisian di
    /// penyunting gambar mana pun.
    bool HoleAt(int x, int y) const;
    void SetHoleAt(int x, int y, bool hole);
    std::size_t HoleCount() const { return holeCount_; }
    void ClearHoles();

    void ReadHoles(std::vector<uint8_t>& out) const;
    void WriteHoles(const uint8_t* holes);

    // --- goresan dan undo ----------------------------------------------------

    /// Sebuah goresan adalah satu satuan undo, bukan satu sentuhan.
    ///
    /// Menyeret brush menghasilkan puluhan sentuhan; kalau tiap sentuhan menjadi
    /// satu langkah undo, membatalkan satu goresan menuntut puluhan kali Ctrl+Z
    /// dan tidak ada yang bisa memakainya.
    ///
    /// Satu goresan mencakup **ketiga peta**: tinggi, bobot layer, dan hole.
    /// Bukan karena satu goresan menyentuh ketiganya — tidak pernah — melainkan
    /// karena riwayat yang terpisah per peta berarti Ctrl+Z yang artinya
    /// bergantung pada tab mana yang sedang terbuka, dan itu tidak bisa ditebak
    /// siapa pun.
    void BeginStroke();
    void EndStroke();
    bool InStroke() const { return inStroke_; }

    bool Undo();
    bool Redo();
    std::size_t UndoDepth() const { return undo_.size(); }
    std::size_t RedoDepth() const { return redo_.size(); }
    void ClearHistory();

    /// Byte yang benar-benar dialokasikan: tile yang sudah terwujud ditambah
    /// jurnal undo. Dipakai test anggaran memori dan panel.
    std::size_t BytesResident() const;
    std::size_t TilesResident() const;
    /// Apakah sebuah ubin sudah pernah ditulis.
    ///
    /// Dipakai yang menggambar dan yang menabrak: ubin yang belum pernah
    /// disentuh datar setinggi `baseHeight`, dan membangun geometri untuknya
    /// berarti membayar seluruh peta demi bentuk yang sudah diketahui.
    bool TileResident(int tileX, int tileY) const;
    std::size_t UndoBytes() const { return undoBytes_; }

private:
    struct Tile {
        std::vector<Sample> samples;
    };

    /// Ubin byte, dipakai peta bobot maupun peta hole.
    ///
    /// Keduanya berbentuk sama persis — satu byte per sampel, dialokasikan saat
    /// pertama ditulis — jadi keduanya memakai jalur salin, jurnal, dan alokasi
    /// yang sama. Hole memang cukup satu bit, dan mengemasnya delapan kali lebih
    /// hemat; yang dibayar untuk kemasan itu bukan hanya kode pengemasnya
    /// melainkan blok jurnal yang tidak lagi sejajar dengan batas kata pada
    /// ukuran ubin yang bukan kelipatan 64. Hole jarang, dan ubin yang tidak
    /// berlubang tidak dialokasikan sama sekali — jadi yang dihemat pengemasan
    /// itu adalah delapan per sembilan dari sesuatu yang sudah mendekati nol.
    struct ByteTile {
        std::vector<uint8_t> bytes;
    };

    struct LayerData {
        TerrainLayer desc;
        std::vector<std::unique_ptr<ByteTile>> tiles;
    };

    /// Satu blok yang disalin sebelum disentuh. `image` menyimpan isi *lama*
    /// saat direkam; setelah undo ia menyimpan isi *baru*, karena undo menukar
    /// isinya dengan yang hidup. Satu salinan melayani undo dan redo sekaligus.
    struct BlockImage {
        int tile = 0;
        int block = 0;
        std::vector<Sample> image;
    };

    /// Blok byte dalam jurnal. `layer` ≥ 0 menunjuk peta bobot layer itu, -1
    /// menunjuk peta hole.
    struct ByteBlockImage {
        int layer = 0;
        int tile = 0;
        int block = 0;
        std::vector<uint8_t> image;
    };

    struct Stroke {
        std::vector<BlockImage> blocks;
        std::vector<ByteBlockImage> byteBlocks;
        std::size_t bytes = 0;

        bool Empty() const { return blocks.empty() && byteBlocks.empty(); }
    };

    Tile& MaterializeTile(int index);
    void CaptureBlock(int tileIndex, int blockIndex);
    void SwapStroke(Stroke& stroke);
    void TrimJournal();

    /// Peta byte milik sebuah layer, atau peta hole untuk `layer` negatif.
    std::vector<std::unique_ptr<ByteTile>>& ByteTiles(int layer);
    const std::vector<std::unique_ptr<ByteTile>>& ByteTiles(int layer) const;
    ByteTile& MaterializeByteTile(int layer, int index);
    void CaptureByteBlock(int layer, int tileIndex, int blockIndex);
    uint8_t ByteAt(int layer, int x, int y) const;
    void SetByteAt(int layer, int x, int y, uint8_t value);
    void ReadBytes(int layer, std::vector<uint8_t>& out) const;
    void WriteBytes(int layer, const uint8_t* bytes);
    /// Menyusutkan layer eksplisit selain `keep` sampai jumlahnya tidak melebihi
    /// `budget`. Pembulatannya ke bawah dengan sengaja: pembulatan ke terdekat
    /// bisa membuat jumlahnya justru melewati anggaran, dan invarian yang dijaga
    /// dengan "hampir" bukan invarian.
    void ShrinkOtherWeights(int keep, int x, int y, int budget);

    int TileCount() const { return desc_.tilesX * desc_.tilesY; }
    int BlocksPerSide() const { return (desc_.tileSamples + kBlockSize - 1) / kBlockSize; }

    TerrainDesc desc_;
    Sample base_ = 0;
    std::vector<std::unique_ptr<Tile>> tiles_;
    std::vector<LayerData> layers_;
    std::vector<std::unique_ptr<ByteTile>> holes_;
    /// Dijaga bertahap, bukan dihitung saat ditanya: panel menampilkannya tiap
    /// frame, dan memindai seluruh peta hole untuk itu adalah membaca puluhan
    /// megabyte demi satu angka.
    std::size_t holeCount_ = 0;

    bool inStroke_ = false;
    Stroke current_;
    /// Blok yang sudah disalin dalam goresan berjalan, sebagai kunci gabungan
    /// (peta, layer, tile, blok). Tanpa ini, satu blok yang disentuh puluhan kali
    /// dalam satu goresan akan disalin puluhan kali.
    std::unordered_set<uint64_t> captured_;

    std::vector<Stroke> undo_;
    std::vector<Stroke> redo_;
    std::size_t undoBytes_ = 0;
};

}  // namespace sim::terrain
