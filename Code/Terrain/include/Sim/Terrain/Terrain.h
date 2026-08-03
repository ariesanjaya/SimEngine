#pragma once

#include "Sim/Core/Math.h"

#include <cstddef>
#include <cstdint>
#include <memory>
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

    // --- goresan dan undo ----------------------------------------------------

    /// Sebuah goresan adalah satu satuan undo, bukan satu sentuhan.
    ///
    /// Menyeret brush menghasilkan puluhan sentuhan; kalau tiap sentuhan menjadi
    /// satu langkah undo, membatalkan satu goresan menuntut puluhan kali Ctrl+Z
    /// dan tidak ada yang bisa memakainya.
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
    std::size_t UndoBytes() const { return undoBytes_; }

private:
    struct Tile {
        std::vector<Sample> samples;
    };

    /// Satu blok yang disalin sebelum disentuh. `image` menyimpan isi *lama*
    /// saat direkam; setelah undo ia menyimpan isi *baru*, karena undo menukar
    /// isinya dengan yang hidup. Satu salinan melayani undo dan redo sekaligus.
    struct BlockImage {
        int tile = 0;
        int block = 0;
        std::vector<Sample> image;
    };

    struct Stroke {
        std::vector<BlockImage> blocks;
        std::size_t bytes = 0;
    };

    Tile& MaterializeTile(int index);
    void CaptureBlock(int tileIndex, int blockIndex);
    void SwapStroke(Stroke& stroke);
    void TrimJournal();

    int BlocksPerSide() const { return (desc_.tileSamples + kBlockSize - 1) / kBlockSize; }

    TerrainDesc desc_;
    Sample base_ = 0;
    std::vector<std::unique_ptr<Tile>> tiles_;

    bool inStroke_ = false;
    Stroke current_;
    /// Blok yang sudah disalin dalam goresan berjalan, sebagai kunci gabungan
    /// (tile, blok). Tanpa ini, satu blok yang disentuh puluhan kali dalam satu
    /// goresan akan disalin puluhan kali.
    std::unordered_set<uint64_t> captured_;

    std::vector<Stroke> undo_;
    std::vector<Stroke> redo_;
    std::size_t undoBytes_ = 0;
};

}  // namespace sim::terrain
