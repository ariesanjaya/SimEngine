#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Terrain/Terrain.h"
#include "Sim/Terrain/TerrainBrush.h"
#include "Sim/Terrain/TerrainIo.h"

// Diminta sendiri: Terrain memakai Sim::ImageIO secara PRIVATE. Kriteria terima
// I3 berbunyi berbeda tergantung ada tidaknya libtiff, dan uji yang tidak bisa
// membedakan keduanya akan melewat diam di salah satunya.
#include "Sim/ImageIO/ImageIO.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace sim;
using namespace sim::terrain;

namespace {

/// Folder sementara yang bersih untuk berkas uji, dihapus saat selesai.
class TempDir {
public:
    explicit TempDir(const std::string& name)
        : path_(std::filesystem::temp_directory_path() / ("sim-terrain-" + name)) {
        std::error_code code;
        std::filesystem::remove_all(path_, code);
        std::filesystem::create_directories(path_, code);
    }
    ~TempDir() {
        std::error_code code;
        std::filesystem::remove_all(path_, code);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::filesystem::path operator/(const std::string& leaf) const { return path_ / leaf; }

private:
    std::filesystem::path path_;
};

/// Hash seluruh heightmap. Dipakai untuk klaim "persis seperti semula" —
/// membandingkan jutaan sampel satu per satu di dalam CHECK membuat keluaran
/// test tidak terbaca ketika gagal.
uint64_t HashHeights(const Terrain& terrain) {
    std::vector<Sample> samples;
    terrain.ReadAll(samples);
    uint64_t hash = 1469598103934665603ull;
    for (const Sample sample : samples) {
        hash ^= sample;
        hash *= 1099511628211ull;
    }
    return hash;
}

/// Apakah dua tinggi sama, sejauh penyimpanan 16-bit bisa membedakannya.
///
/// Terrain menyimpan tinggi sebagai uint16 di dalam rentangnya, jadi setiap
/// nilai dibulatkan ke kelipatan `(maxHeight - minHeight) / 65535`. Menuntut
/// kesamaan yang lebih ketat dari itu berarti menguji sesuatu yang memang tidak
/// dijanjikan — dan test yang menuntutnya gagal karena pembulatan, bukan karena
/// ada yang salah.
bool SameHeight(const Terrain& terrain, float actual, float expected) {
    const float step = (terrain.Desc().maxHeight - terrain.Desc().minHeight) / 65535.0f;
    return std::abs(actual - expected) <= step * 1.5f;
}

Brush RaiseBrush() {
    Brush brush;
    brush.kind = BrushKind::Raise;
    brush.radius = 12.0f;
    brush.strength = 40.0f;
    brush.falloff = 0.7f;
    return brush;
}

PaintBrush LayerPaintBrush() {
    PaintBrush brush;
    brush.radius = 12.0f;
    brush.strength = 4.0f;
    brush.falloff = 0.5f;
    brush.target = 1.0f;
    return brush;
}

uint64_t HashWeights(const Terrain& terrain, int layer) {
    std::vector<Weight> weights;
    terrain.ReadWeights(layer, weights);
    uint64_t hash = 1469598103934665603ull;
    for (const Weight weight : weights) {
        hash ^= weight;
        hash *= 1099511628211ull;
    }
    return hash;
}

uint64_t HashHoles(const Terrain& terrain) {
    std::vector<uint8_t> holes;
    terrain.ReadHoles(holes);
    uint64_t hash = 1469598103934665603ull;
    for (const uint8_t hole : holes) {
        hash ^= hole;
        hash *= 1099511628211ull;
    }
    return hash;
}

/// Total bobot seluruh layer pada sebuah sampel. Menurut bentuk penyimpanannya
/// ia selalu 255 — layer dasar adalah sisanya — jadi angka lain berarti ada
/// jalur tulis yang melanggar invariannya.
int TotalWeight(const Terrain& terrain, int x, int y) {
    int total = 0;
    for (int layer = 0; layer < terrain.LayerCount(); ++layer) {
        total += terrain.WeightAt(layer, x, y);
    }
    return total;
}

TerrainLayer NamedLayer(const std::string& name) {
    TerrainLayer layer;
    layer.name = name;
    return layer;
}

}  // namespace

TEST_CASE("Sampel di luar batas dijepit, bukan menabrak") {
    Terrain terrain(TerrainDesc{16, 2, 2, 1.0f, 0.0f, 100.0f, 10.0f, 1024});
    terrain.SetHeightAt(0, 0, 50.0f);

    CHECK(SameHeight(terrain, terrain.HeightAt(-5, -5), 50.0f));
    CHECK(SameHeight(terrain, terrain.HeightAt(9999, 9999), 10.0f));
    // Menulis di luar batas tidak diam-diam melipat ke tepi: itu akan membuat
    // brush di pinggir peta menumpuk seluruh sisanya ke satu baris.
    terrain.SetHeightAt(-1, -1, 90.0f);
    CHECK(SameHeight(terrain, terrain.HeightAt(0, 0), 50.0f));
}

TEST_CASE("Tinggi bolak-balik meter dan sampel tanpa hanyut") {
    Terrain terrain(TerrainDesc{16, 1, 1, 1.0f, -100.0f, 900.0f});
    // Yang dijanjikan bukan "sama persis dengan meter yang diminta" — uint16
    // memang membulatkan. Yang dijanjikan adalah **titik tetap**: membaca lalu
    // menulis kembali nilai yang sama tidak menggesernya. Dengan pemotongan
    // alih-alih pembulatan, setiap putaran menurunkannya satu langkah, dan brush
    // berkekuatan nol pun perlahan meratakan terrain.
    const float first = terrain.HeightAt(3, 3);
    for (int i = 0; i < 64; ++i) {
        terrain.SetHeightAt(3, 3, terrain.HeightAt(3, 3));
    }
    CHECK(terrain.HeightAt(3, 3) == doctest::Approx(first));
    CHECK(SameHeight(terrain, first, 0.0f));

    terrain.SetHeightAt(1, 1, 250.0f);
    const float stored = terrain.HeightAt(1, 1);
    terrain.SetHeightAt(1, 1, stored);
    CHECK(terrain.HeightAt(1, 1) == doctest::Approx(stored));
    CHECK(SameHeight(terrain, stored, 250.0f));
}

TEST_CASE("Bobot brush penuh di pusat dan nol di tepi") {
    Brush brush;
    brush.radius = 10.0f;

    brush.falloff = 1.0f;
    CHECK(BrushWeight(brush, 0.0f) == doctest::Approx(1.0f));
    CHECK(BrushWeight(brush, 10.0f) == doctest::Approx(0.0f));
    CHECK(BrushWeight(brush, 11.0f) == doctest::Approx(0.0f));
    CHECK(BrushWeight(brush, 5.0f) == doctest::Approx(0.5f));

    // falloff 0 berarti tepi tajam — dibutuhkan untuk memahat bentuk bersudut,
    // dan tidak bisa didapat dari kurva yang selalu melembut.
    brush.falloff = 0.0f;
    CHECK(BrushWeight(brush, 9.99f) == doctest::Approx(1.0f));
    CHECK(BrushWeight(brush, 10.0f) == doctest::Approx(0.0f));
}

TEST_CASE("Kriteria 1: memahat di batas tile sama persis dengan tanpa ubin") {
    // Dua terrain yang identik ukurannya, satu dibagi 4×4 ubin dan satu utuh.
    const TerrainDesc tiled{32, 4, 4, 1.0f, 0.0f, 200.0f, 20.0f};
    TerrainDesc single = tiled;
    single.tileSamples = 128;
    single.tilesX = 1;
    single.tilesY = 1;

    Terrain a(tiled);
    Terrain b(single);
    REQUIRE(a.SamplesX() == b.SamplesX());

    // Pusat brush tepat di persilangan empat ubin, dengan jari-jari yang
    // melewatinya jauh — persis kasus yang menghasilkan retakan pada engine yang
    // menyimpan baris tepi di dua tempat.
    const Brush brush = RaiseBrush();
    const float seam = 32.0f;
    for (Terrain* terrain : {&a, &b}) {
        terrain->BeginStroke();
        ApplyDab(*terrain, brush, seam, seam, 0.1f);
        ApplyDab(*terrain, brush, seam + 3.0f, seam, 0.1f);
        ApplyDab(*terrain, brush, seam, seam + 3.0f, 0.1f);
        terrain->EndStroke();
    }

    // Pernyataan yang sebenarnya ingin dikunci bukan "tidak ada lompatan di
    // jahitan" melainkan yang lebih kuat: **pengubinan tidak terlihat sama
    // sekali**. Terrain berubin dan terrain utuh menghasilkan heightmap yang
    // sama byte-per-byte untuk goresan yang sama.
    CHECK(HashHeights(a) == HashHeights(b));

    // Dan memang ada sesuatu yang tergores di kedua sisi jahitan, supaya
    // kesamaan di atas bukan kesamaan dua peta yang sama-sama kosong.
    const int mid = 32;
    CHECK(a.HeightAt(mid - 1, mid) > 20.5f);
    CHECK(a.HeightAt(mid, mid) > 20.5f);
    CHECK(a.HeightAt(mid, mid - 1) > 20.5f);

    // Tidak ada lompatan di jahitan: beda antar sampel bertetangga di sana
    // sebanding dengan beda di tempat lain pada lereng yang sama.
    const float acrossSeam = std::abs(a.HeightAt(mid, mid) - a.HeightAt(mid - 1, mid));
    const float insideTile = std::abs(a.HeightAt(mid + 4, mid) - a.HeightAt(mid + 3, mid));
    CHECK(acrossSeam <= insideTile + 0.5f);
}

TEST_CASE("Kriteria 2: undo satu goresan mengembalikan heightmap persis") {
    Terrain terrain(TerrainDesc{64, 3, 3, 1.0f, 0.0f, 500.0f, 100.0f});

    // Sedikit relief awal supaya yang dipulihkan bukan sekadar bidang rata.
    Brush noise;
    noise.kind = BrushKind::Noise;
    noise.radius = 60.0f;
    noise.strength = 30.0f;
    noise.falloff = 0.2f;
    noise.noiseFrequency = 0.08f;
    terrain.BeginStroke();
    ApplyDab(terrain, noise, 90.0f, 90.0f, 1.0f);
    terrain.EndStroke();
    terrain.ClearHistory();

    const uint64_t before = HashHeights(terrain);

    // Satu goresan = banyak sentuhan. Kalau tiap sentuhan menjadi satu langkah
    // undo, membatalkan satu goresan menuntut puluhan kali Ctrl+Z.
    const Brush brush = RaiseBrush();
    terrain.BeginStroke();
    for (int i = 0; i < 40; ++i) {
        ApplyDab(terrain, brush, 60.0f + static_cast<float>(i), 90.0f, 0.05f);
    }
    terrain.EndStroke();

    const uint64_t after = HashHeights(terrain);
    REQUIRE(after != before);
    REQUIRE(terrain.UndoDepth() == 1);

    REQUIRE(terrain.Undo());
    CHECK(HashHeights(terrain) == before);
    CHECK(terrain.UndoDepth() == 0);

    REQUIRE(terrain.Redo());
    CHECK(HashHeights(terrain) == after);

    // Undo di luar goresan saja; membatalkan di tengah seretan akan
    // meninggalkan setengah goresan yang tidak bisa dibatalkan lagi.
    terrain.BeginStroke();
    CHECK_FALSE(terrain.Undo());
    terrain.EndStroke();
}

TEST_CASE("Goresan yang tidak menyentuh apa pun tidak masuk riwayat") {
    Terrain terrain(TerrainDesc{32, 2, 2});
    terrain.BeginStroke();
    terrain.EndStroke();
    // Satu Ctrl+Z yang tidak mengubah apa-apa terlihat seperti undo yang rusak.
    CHECK(terrain.UndoDepth() == 0);
}

TEST_CASE("Kriteria 3: terrain 4x4 km dengan ubin 2048 tetap ringan saat disunting") {
    // 8×8 ubin 2048² = 16384² sampel. Kalau seluruhnya penghuni memori itu
    // 512 MB; yang dibayar hanya ubin yang benar-benar disentuh.
    TerrainDesc desc;
    desc.tileSamples = 2048;
    desc.tilesX = 8;
    desc.tilesY = 8;
    desc.sampleSpacing = 0.25f;  // 16384 × 0,25 m = 4096 m
    Terrain terrain(desc);

    CHECK(terrain.WorldWidth() == doctest::Approx(4095.75f));
    CHECK(terrain.TilesResident() == 0);
    CHECK(terrain.BytesResident() == 0);

    // Jauh di dalam satu ubin: sebuah goresan yang kebetulan menyeberang batas
    // ubin akan mewujudkan dua ubin dengan benar, tapi membuat pernyataan test
    // ini kabur.
    const Brush brush = RaiseBrush();
    terrain.BeginStroke();
    for (int i = 0; i < 30; ++i) {
        ApplyDab(terrain, brush, 200.0f + static_cast<float>(i), 200.0f, 0.05f);
    }
    terrain.EndStroke();

    constexpr std::size_t kTileBytes = 2048ull * 2048ull * sizeof(Sample);  // 8 MB
    CHECK(terrain.TilesResident() == 1);

    // Jurnal undo menyimpan blok, bukan ubin. Menyalin seluruh ubin untuk satu
    // goresan selebar dua puluh meter adalah persis lonjakan yang dilarang.
    INFO("jurnal undo: ", terrain.UndoBytes(), " byte");
    CHECK(terrain.UndoBytes() < kTileBytes / 8);
    CHECK(terrain.BytesResident() < kTileBytes * 2);

    // Goresan kedua di ubin lain menambah satu ubin, bukan membangunkan semuanya.
    terrain.BeginStroke();
    ApplyDab(terrain, brush, 2000.0f, 2000.0f, 0.05f);
    terrain.EndStroke();
    CHECK(terrain.TilesResident() == 2);
}

TEST_CASE("Jurnal undo dibatasi, dan yang dibuang yang terlama") {
    TerrainDesc desc;
    desc.tileSamples = 128;
    desc.tilesX = 2;
    desc.tilesY = 2;
    desc.undoBudgetBytes = 64u * 1024u;  // sangat kecil, supaya batasnya terlihat
    Terrain terrain(desc);

    const Brush brush = RaiseBrush();
    for (int i = 0; i < 20; ++i) {
        terrain.BeginStroke();
        ApplyDab(terrain, brush, 40.0f + static_cast<float>(i), 40.0f, 0.05f);
        terrain.EndStroke();
    }
    CHECK(terrain.UndoBytes() <= desc.undoBudgetBytes);
    // Undo satu langkah harus selalu bisa, seberapa besar pun goresannya —
    // karena itu yang dibuang selalu yang terlama.
    CHECK(terrain.UndoDepth() >= 1);
    CHECK(terrain.Undo());
}

TEST_CASE("Kriteria 4: round-trip PNG 16-bit tanpa kehilangan presisi") {
    TempDir dir("png");
    TerrainDesc desc{64, 2, 2, 1.0f, 0.0f, 1000.0f, 0.0f};
    Terrain source(desc);

    // Pola yang memakai seluruh rentang 16-bit, termasuk kedua ujungnya. Pola
    // yang hanya memakai bagian tengah akan lulus walau delapan bit teratas atau
    // terbawah hilang.
    for (int y = 0; y < source.SamplesY(); ++y) {
        for (int x = 0; x < source.SamplesX(); ++x) {
            const auto value = static_cast<Sample>((x * 517 + y * 8191) & 0xffff);
            source.SetRawAt(x, y, value);
        }
    }
    source.SetRawAt(0, 0, 0);
    source.SetRawAt(1, 0, kSampleMax);

    const std::filesystem::path png = dir / "height.png";
    REQUIRE(SaveHeightmapImage(source, png).ok);

    Terrain loaded(desc);
    const TerrainIoResult result = LoadHeightmapImage(loaded, png);
    INFO(result.error);
    REQUIRE(result.ok);

    for (int y = 0; y < source.SamplesY(); ++y) {
        for (int x = 0; x < source.SamplesX(); ++x) {
            if (source.RawAt(x, y) != loaded.RawAt(x, y)) {
                INFO("sampel (", x, ",", y, ")");
                REQUIRE(source.RawAt(x, y) == loaded.RawAt(x, y));
            }
        }
    }
    CHECK(HashHeights(source) == HashHeights(loaded));
}

TEST_CASE("PNG yang ditulis benar-benar terkompresi, bukan sekadar RAW berbungkus") {
    TempDir dir("compress");
    // Terrain berelief halus — bentuk yang sesungguhnya, tempat filter PNG
    // memang bekerja. PNG seukuran RAW tidak ada gunanya: RAW sudah tersedia.
    Terrain terrain(TerrainDesc{128, 2, 2, 1.0f, 0.0f, 500.0f, 100.0f});
    Brush noise;
    noise.kind = BrushKind::Noise;
    noise.radius = 200.0f;
    noise.strength = 40.0f;
    noise.falloff = 0.1f;
    noise.noiseFrequency = 0.02f;
    ApplyDab(terrain, noise, 128.0f, 128.0f, 1.0f);

    const std::filesystem::path png = dir / "h.png";
    const std::filesystem::path raw = dir / "h.raw";
    REQUIRE(SaveHeightmapImage(terrain, png).ok);
    REQUIRE(SaveHeightmapRaw(terrain, raw).ok);

    const auto pngSize = std::filesystem::file_size(png);
    const auto rawSize = std::filesystem::file_size(raw);
    INFO("png ", pngSize, " byte, raw ", rawSize, " byte");
    CHECK(pngSize < rawSize);
}

TEST_CASE("Round-trip RAW, dan ukuran yang tidak cocok ditolak dengan jelas") {
    TempDir dir("raw");
    TerrainDesc desc{32, 2, 2, 1.0f, 0.0f, 400.0f, 0.0f};
    Terrain source(desc);
    for (int y = 0; y < source.SamplesY(); ++y) {
        for (int x = 0; x < source.SamplesX(); ++x) {
            source.SetRawAt(x, y, static_cast<Sample>((x * 61 + y * 4093) & 0xffff));
        }
    }

    const std::filesystem::path raw = dir / "h.raw";
    REQUIRE(SaveHeightmapRaw(source, raw).ok);

    Terrain loaded(desc);
    REQUIRE(LoadHeightmapRaw(loaded, raw).ok);
    CHECK(HashHeights(source) == HashHeights(loaded));

    // RAW tidak bisa membawa ukurannya sendiri, jadi ukuran yang tidak cocok
    // hanya bisa ketahuan dari jumlah byte-nya — dan harus dilaporkan, bukan
    // dibaca separuh.
    TerrainDesc other = desc;
    other.tilesX = 3;
    Terrain wrong(other);
    const TerrainIoResult mismatch = LoadHeightmapRaw(wrong, raw);
    CHECK_FALSE(mismatch.ok);
    CHECK(mismatch.error.find("expects") != std::string::npos);
}

TEST_CASE("Heightmap PNG berukuran lain ditolak, bukan diskala diam-diam") {
    TempDir dir("mismatch");
    Terrain small(TerrainDesc{32, 1, 1});
    const std::filesystem::path png = dir / "small.png";
    REQUIRE(SaveHeightmapImage(small, png).ok);

    Terrain big(TerrainDesc{32, 2, 2});
    const TerrainIoResult result = LoadHeightmapImage(big, png);
    // Menskala ulang diam-diam adalah cara paling halus untuk merusak peta
    // seseorang: hasilnya terlihat masuk akal dan tetap salah.
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("32x32") != std::string::npos);
    CHECK(result.error.find("64x64") != std::string::npos);

    // Tapi ukurannya tetap bisa dibaca, supaya panel bisa menawarkan
    // menyesuaikan terrain alih-alih sekadar menolak.
    std::vector<Sample> samples;
    int w = 0;
    int h = 0;
    REQUIRE(ReadHeightmapImage(png, samples, w, h).ok);
    CHECK(w == 32);
    CHECK(h == 32);
}

TEST_CASE("Dokumen .simterrain disimpan lalu dimuat identik, bersama heightmapnya") {
    TempDir dir("doc");
    TerrainDocument document;
    document.name = "Lembah";
    document.desc = TerrainDesc{64, 2, 3, 0.5f, -50.0f, 750.0f, 12.0f};

    Terrain terrain(document.desc);
    const Brush brush = RaiseBrush();
    terrain.BeginStroke();
    ApplyDab(terrain, brush, 30.0f, 30.0f, 0.5f);
    terrain.EndStroke();

    const std::filesystem::path path = dir / "Lembah.simterrain";
    REQUIRE(SaveTerrain(terrain, document, path).ok);
    CHECK(std::filesystem::exists(dir / "Lembah_height.png"));

    Terrain loadedTerrain;
    TerrainDocument loadedDocument;
    const TerrainIoResult result = LoadTerrain(loadedTerrain, loadedDocument, path);
    INFO(result.error);
    REQUIRE(result.ok);

    CHECK(loadedDocument.name == "Lembah");
    CHECK(loadedDocument.desc.tileSamples == 64);
    CHECK(loadedDocument.desc.tilesY == 3);
    CHECK(loadedDocument.desc.sampleSpacing == doctest::Approx(0.5f));
    CHECK(loadedDocument.desc.minHeight == doctest::Approx(-50.0f));
    CHECK(loadedDocument.desc.baseHeight == doctest::Approx(12.0f));
    CHECK(HashHeights(loadedTerrain) == HashHeights(terrain));

    // Byte-per-byte sama, seperti `.simfx` dan `.simmat`: menyimpan dokumen yang
    // tidak disunting tidak boleh menghasilkan diff palsu.
    std::vector<TerrainLayer> layers;
    for (int index = 0; index < loadedTerrain.LayerCount(); ++index) {
        layers.push_back(loadedTerrain.Layer(index));
    }
    const std::string text = SaveDocumentToString(loadedDocument, layers);
    TerrainDocument again;
    std::vector<TerrainLayer> againLayers;
    REQUIRE(LoadDocumentFromString(again, againLayers, text).ok);
    CHECK(SaveDocumentToString(again, againLayers) == text);
}

TEST_CASE("Flatten menuju tinggi tujuan, tidak melewatinya") {
    Terrain terrain(TerrainDesc{32, 1, 1, 1.0f, 0.0f, 200.0f, 100.0f});
    Brush brush;
    brush.kind = BrushKind::Flatten;
    brush.radius = 8.0f;
    brush.strength = 4.0f;
    brush.falloff = 0.0f;
    brush.targetHeight = 40.0f;

    for (int i = 0; i < 50; ++i) {
        ApplyDab(terrain, brush, 16.0f, 16.0f, 0.1f);
    }
    CHECK(terrain.HeightAt(16, 16) == doctest::Approx(40.0f).epsilon(0.02));
    // Konvergen, bukan berosilasi melewati tujuannya.
    CHECK(terrain.HeightAt(16, 16) >= 39.0f);
    CHECK(terrain.HeightAt(16, 16) <= 41.0f);
    // Di luar jari-jari tidak tersentuh sama sekali.
    CHECK(SameHeight(terrain, terrain.HeightAt(30, 16), 100.0f));
}

TEST_CASE("Smooth membaca salinan, jadi hasilnya tidak bergantung urutan") {
    const TerrainDesc desc{64, 1, 1, 1.0f, 0.0f, 200.0f, 50.0f};

    const auto build = [&desc]() {
        Terrain terrain(desc);
        // Menara tajam: kalau smooth membaca sampel yang baru saja ditulisnya,
        // menaranya akan miring ke arah penelusuran alih-alih melebar merata.
        terrain.SetHeightAt(32, 32, 150.0f);
        return terrain;
    };

    Terrain terrain = build();
    Brush smooth;
    smooth.kind = BrushKind::Smooth;
    smooth.radius = 10.0f;
    smooth.strength = 5.0f;
    smooth.falloff = 0.0f;
    ApplyDab(terrain, smooth, 32.0f, 32.0f, 0.2f);

    // Simetris di keempat arah: itu yang hanya mungkin kalau seluruh sampel
    // membaca keadaan yang sama.
    CHECK(terrain.HeightAt(31, 32) == doctest::Approx(terrain.HeightAt(33, 32)));
    CHECK(terrain.HeightAt(32, 31) == doctest::Approx(terrain.HeightAt(32, 33)));
    CHECK(terrain.HeightAt(31, 32) > 50.5f);
    CHECK(terrain.HeightAt(32, 32) < 150.0f);
}

TEST_CASE("Ramp mengikuti garis dan berhenti di ujungnya") {
    Terrain terrain(TerrainDesc{64, 1, 1, 1.0f, 0.0f, 200.0f, 10.0f});
    Brush brush;
    brush.radius = 5.0f;
    brush.falloff = 0.0f;

    ApplyRamp(terrain, brush, Vec3(10.0f, 20.0f, 32.0f), Vec3(50.0f, 60.0f, 32.0f));

    CHECK(terrain.HeightAt(10, 32) == doctest::Approx(20.0f).epsilon(0.01));
    CHECK(terrain.HeightAt(30, 32) == doctest::Approx(40.0f).epsilon(0.01));
    CHECK(terrain.HeightAt(50, 32) == doctest::Approx(60.0f).epsilon(0.01));
    // Dijepit ke ujung ruas: tanpa itu ramp merambat tak berhingga ke kedua arah.
    CHECK(SameHeight(terrain, terrain.HeightAt(60, 32), 10.0f));
    CHECK(SameHeight(terrain, terrain.HeightAt(30, 45), 10.0f));
}

TEST_CASE("Erosi termal menurunkan kecuraman tanpa membuang material") {
    Terrain terrain(TerrainDesc{64, 1, 1, 1.0f, 0.0f, 400.0f, 100.0f});
    // Pilar dengan dinding tegak — lereng yang jauh melebihi sudut talus mana pun.
    for (int y = 28; y < 36; ++y) {
        for (int x = 28; x < 36; ++x) {
            terrain.SetHeightAt(x, y, 300.0f);
        }
    }

    const auto totalHeight = [&terrain]() {
        double sum = 0.0;
        for (int y = 0; y < terrain.SamplesY(); ++y) {
            for (int x = 0; x < terrain.SamplesX(); ++x) {
                sum += terrain.HeightAt(x, y);
            }
        }
        return sum;
    };
    const double before = totalHeight();
    // Dindingnya persis di antara y=27 (dasar) dan y=28 (puncak pilar).
    const float steepBefore = std::abs(terrain.HeightAt(32, 28) - terrain.HeightAt(32, 27));
    REQUIRE(steepBefore > 100.0f);

    ApplyThermalErosion(terrain, SampleRect{20, 20, 44, 44}, 30, 35.0f, 0.4f);

    const float steepAfter = std::abs(terrain.HeightAt(32, 28) - terrain.HeightAt(32, 27));
    INFO("kecuraman dinding ", steepBefore, " → ", steepAfter);
    CHECK(steepAfter < steepBefore);
    CHECK(terrain.HeightAt(32, 32) < 300.0f);
    CHECK(terrain.HeightAt(32, 27) > 100.5f);  // material meluncur keluar pilar

    // Material berpindah, bukan menguap. Toleransinya longgar karena kuantisasi
    // 16-bit membulatkan setiap perpindahan, tapi kebocoran yang sesungguhnya
    // akan jauh melampauinya.
    const double after = totalHeight();
    INFO("total tinggi ", before, " → ", after);
    CHECK(after == doctest::Approx(before).epsilon(0.01));
}

TEST_CASE("Tinggi dunia terinterpolasi bilinear di antara sampel") {
    Terrain terrain(TerrainDesc{32, 1, 1, 2.0f, 0.0f, 100.0f, 0.0f});
    terrain.SetHeightAt(0, 0, 0.0f);
    terrain.SetHeightAt(1, 0, 40.0f);
    terrain.SetHeightAt(0, 1, 0.0f);
    terrain.SetHeightAt(1, 1, 40.0f);

    // Jarak antar sampel 2 m, jadi 1 m adalah tepat setengah jalan.
    CHECK(terrain.HeightAtWorld(0.0f, 0.0f) == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(terrain.HeightAtWorld(1.0f, 0.0f) == doctest::Approx(20.0f).epsilon(0.01));
    CHECK(terrain.HeightAtWorld(2.0f, 0.0f) == doctest::Approx(40.0f).epsilon(0.01));
}

TEST_CASE("Goresan tidak bergantung laju frame") {
    const TerrainDesc desc{64, 2, 2, 1.0f, 0.0f, 300.0f, 50.0f};
    const Brush brush = RaiseBrush();

    // Lintasan yang sama, dua laju frame: satu mesin melaporkan 30 frame per
    // detik, satu lagi 60. Yang digores sama, jadi yang dihasilkan harus sama.
    const auto paint = [&](int frames) {
        Terrain terrain(desc);
        BrushStroke stroke;
        stroke.Begin(terrain, 20.0f, 64.0f);
        const float dt = 0.5f / static_cast<float>(frames);
        for (int i = 1; i <= frames; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(frames);
            stroke.Advance(terrain, brush, 20.0f + 60.0f * t, 64.0f, dt);
        }
        stroke.End(terrain);
        return terrain;
    };

    const Terrain slow = paint(6);
    const Terrain fast = paint(30);

    // Mengalikan kekuatan brush dengan dt frame terdengar benar tapi tidak:
    // setiap sentuhan dibulatkan ke sampel 16-bit, jadi jumlah sentuhan yang
    // berbeda menghasilkan bukit yang berbeda — bukan berbeda sedikit.
    CHECK(HashHeights(slow) == HashHeights(fast));
    CHECK(slow.HeightAt(50, 64) > 51.0f);
}

TEST_CASE("Satu goresan tetap satu langkah undo, berapa pun sentuhannya") {
    Terrain terrain(TerrainDesc{64, 2, 2, 1.0f, 0.0f, 300.0f, 50.0f});
    const uint64_t before = HashHeights(terrain);

    BrushStroke stroke;
    stroke.Begin(terrain, 20.0f, 64.0f);
    for (int i = 0; i < 40; ++i) {
        stroke.Advance(terrain, RaiseBrush(), 20.0f + static_cast<float>(i) * 2.0f, 64.0f,
                       1.0f / 60.0f);
    }
    stroke.End(terrain);

    CHECK(stroke.Dabs() > 30);
    REQUIRE(terrain.UndoDepth() == 1);
    REQUIRE(terrain.Undo());
    CHECK(HashHeights(terrain) == before);
}

TEST_CASE("Sendatan panjang dibatasi materialnya, bukan jumlah sentuhannya") {
    const TerrainDesc desc{64, 2, 2, 1.0f, 0.0f, 300.0f, 50.0f};
    const Brush brush = RaiseBrush();

    Terrain stalled(desc);
    BrushStroke stroke;
    stroke.Begin(stalled, 64.0f, 64.0f);
    // Satu frame yang tertahan satu detik penuh, kursor diam.
    stroke.Advance(stalled, brush, 64.0f, 64.0f, 1.0f);
    stroke.End(stalled);

    // Yang dibatasi jumlah waktunya. Sedetik penuh di satu titik akan menggerus
    // terrain jauh lebih dalam daripada yang diminta siapa pun; seperempat detik
    // adalah yang benar-benar diterapkan.
    const float rise = stalled.HeightAt(64, 64) - 50.0f;
    INFO("naik ", rise, " m");
    CHECK(rise == doctest::Approx(brush.strength * BrushStroke::kMaxCatchUpSeconds).epsilon(0.02));

    // Dan goresan biasa pada mesin lambat TIDAK kehilangan materialnya: membatasi
    // jumlah sentuhan akan memotongnya diam-diam di sini.
    Terrain slow(desc);
    BrushStroke slowStroke;
    slowStroke.Begin(slow, 64.0f, 64.0f);
    slowStroke.Advance(slow, brush, 64.0f, 64.0f, 0.2f);
    slowStroke.End(slow);
    CHECK(slow.HeightAt(64, 64) - 50.0f == doctest::Approx(brush.strength * 0.2f).epsilon(0.02));
}

TEST_CASE("Memuat heightmap tidak mewujudkan ubin yang seluruhnya datar") {
    TempDir dir("sparse");
    TerrainDesc desc{64, 4, 4, 1.0f, 0.0f, 500.0f, 100.0f};

    Terrain source(desc);
    // Satu bukit di pojok kiri atas saja; lima belas ubin lainnya tetap datar.
    Brush brush = RaiseBrush();
    brush.radius = 20.0f;
    ApplyDab(source, brush, 30.0f, 30.0f, 0.2f);
    REQUIRE(source.TilesResident() == 1);

    const std::filesystem::path png = dir / "sparse.png";
    REQUIRE(SaveHeightmapImage(source, png).ok);

    Terrain loaded(desc);
    REQUIRE(LoadHeightmapImage(loaded, png).ok);

    // Berkas heightmap memuat seluruh peta, jadi tanpa penyaringan ini memuat
    // terrain akan membatalkan seluruh guna alokasi malas: membuka terrain
    // 4x4 km langsung menghuni memori sepenuhnya, padahal bagian yang datar
    // tidak menyimpan apa pun yang belum diketahui.
    CHECK(loaded.TilesResident() == 1);
    CHECK(HashHeights(loaded) == HashHeights(source));

    // Dan yang datar tetap terbaca sebagai tinggi dasar, bukan sebagai nol.
    CHECK(SameHeight(loaded, loaded.HeightAt(200, 200), 100.0f));
}

TEST_CASE("Seretan cepat meninggalkan garis, bukan manik-manik") {
    Terrain terrain(TerrainDesc{128, 2, 2, 1.0f, 0.0f, 300.0f, 50.0f});
    Brush brush = RaiseBrush();
    brush.radius = 8.0f;

    // Satu frame saja, tapi kursornya melompat jauh melampaui jari-jari brush —
    // persis yang terjadi saat menyeret cepat.
    BrushStroke stroke;
    stroke.Begin(terrain, 20.0f, 128.0f);
    stroke.Advance(terrain, brush, 200.0f, 128.0f, 1.0f / 60.0f);
    stroke.End(terrain);

    // Yang diuji kesinambungannya, bukan besarnya: satu frame hanya membawa
    // 1/60 detik material, dan itu memang tipis. Yang tidak boleh ada adalah
    // celah — titik di sepanjang lintasan yang sama sekali tidak tersentuh.
    float lowest = 1e9f;
    float highest = -1e9f;
    for (int x = 30; x <= 190; x += 2) {
        const float rise = terrain.HeightAt(x, 128) - 50.0f;
        lowest = std::min(lowest, rise);
        highest = std::max(highest, rise);
    }
    INFO("terendah ", lowest, " tertinggi ", highest, " di sepanjang lintasan");
    CHECK(highest > 0.0f);
    // Tanpa penyebaran menurut jarak, titik di antara dua sentuhan tetap di
    // tinggi dasar dan `lowest` menjadi nol.
    CHECK(lowest > highest * 0.5f);

    // Dan di luar lintasan tetap tidak tersentuh.
    CHECK(SameHeight(terrain, terrain.HeightAt(100, 200), 50.0f));
}

TEST_CASE("Menyeret cepat tidak menumpuk lebih banyak material daripada pelan") {
    const TerrainDesc desc{128, 2, 2, 1.0f, 0.0f, 300.0f, 50.0f};
    Brush brush = RaiseBrush();
    brush.radius = 8.0f;

    const auto totalRise = [&](int frames) {
        Terrain terrain(desc);
        BrushStroke stroke;
        stroke.Begin(terrain, 20.0f, 128.0f);
        for (int i = 1; i <= frames; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(frames);
            stroke.Advance(terrain, brush, 20.0f + 180.0f * t, 128.0f, 0.5f / frames);
        }
        stroke.End(terrain);
        double sum = 0.0;
        for (int y = 100; y < 160; ++y) {
            for (int x = 0; x < 220; ++x) {
                sum += terrain.HeightAt(x, y) - 50.0;
            }
        }
        return sum;
    };

    // Jatah waktu dibagi rata ke seluruh sentuhan, jadi lintasan dan durasi yang
    // sama memindahkan material yang kira-kira sama — berapa pun frame yang
    // sempat dilaporkan mesinnya.
    const double coarse = totalRise(3);
    const double fine = totalRise(30);
    INFO("kasar ", coarse, " vs halus ", fine);
    CHECK(coarse == doctest::Approx(fine).epsilon(0.05));
}

// --- layer material dan bobot splat ------------------------------------------

TEST_CASE("Layer dasar adalah sisa, jadi terrain baru tidak menyimpan bobot apa pun") {
    Terrain terrain(TerrainDesc{64, 2, 2, 1.0f, 0.0f, 100.0f, 0.0f});
    REQUIRE(terrain.LayerCount() == 1);

    const std::size_t empty = terrain.BytesResident();
    CHECK(terrain.WeightAt(0, 0, 0) == 255);
    CHECK(terrain.WeightAt(0, 90, 90) == 255);
    CHECK(TotalWeight(terrain, 90, 90) == 255);

    // Membaca bobot tidak boleh mewujudkan apa pun. Peta yang lahir dari
    // dibaca adalah peta yang membuat "belum pernah dicat" mustahil dibedakan
    // dari "dicat nol".
    CHECK(terrain.BytesResident() == empty);
    CHECK_FALSE(terrain.LayerPainted(0));
}

TEST_CASE("Total bobot tiap sampel tetap 255, apa pun urutan mengecatnya") {
    Terrain terrain(TerrainDesc{64, 2, 2, 1.0f, 0.0f, 100.0f, 0.0f});
    REQUIRE(terrain.AddLayer(NamedLayer("Rumput")) == 1);
    REQUIRE(terrain.AddLayer(NamedLayer("Batu")) == 2);
    REQUIRE(terrain.AddLayer(NamedLayer("Pasir")) == 3);

    const PaintBrush brush = LayerPaintBrush();
    // Tiga sapuan yang saling menumpuk, dengan pusat berbeda supaya ada sampel
    // yang menerima satu, dua, dan tiga layer sekaligus.
    terrain.BeginStroke();
    for (int i = 0; i < 30; ++i) {
        ApplyLayerDab(terrain, brush, 1, 60.0f, 60.0f, 1.0f / 60.0f);
        ApplyLayerDab(terrain, brush, 2, 68.0f, 60.0f, 1.0f / 60.0f);
        ApplyLayerDab(terrain, brush, 3, 64.0f, 68.0f, 1.0f / 60.0f);
    }
    terrain.EndStroke();

    int explicitOverflow = 0;
    int wrongTotal = 0;
    for (int y = 40; y < 90; ++y) {
        for (int x = 40; x < 90; ++x) {
            if (TotalWeight(terrain, x, y) != 255) {
                ++wrongTotal;
            }
            int sum = 0;
            for (int layer = 1; layer < terrain.LayerCount(); ++layer) {
                sum += terrain.WeightAt(layer, x, y);
            }
            if (sum > 255) {
                ++explicitOverflow;
            }
        }
    }
    INFO("total salah pada ", wrongTotal, " sampel, kelebihan pada ", explicitOverflow);
    CHECK(wrongTotal == 0);
    CHECK(explicitOverflow == 0);

    // Dan yang dicat memang ada — kalau seluruhnya nol, total 255 hanya berarti
    // layer dasar tidak pernah tersentuh.
    //
    // Yang diuji perbandingannya, bukan angkanya: di titik ini ketiga kuas
    // tumpang tindih, jadi ketiganya berbagi 255 dan tidak satu pun mendekati
    // penuh. Yang harus benar adalah layer yang pusat kuasnya di sini mendapat
    // bagian terbesar.
    CHECK(terrain.WeightAt(0, 60, 60) < 60);
    CHECK(terrain.WeightAt(1, 60, 60) > terrain.WeightAt(2, 60, 60));
    CHECK(terrain.WeightAt(1, 60, 60) > terrain.WeightAt(3, 60, 60));
}

TEST_CASE("Mengecat sampai penuh benar-benar mencapai 255") {
    Terrain terrain(TerrainDesc{32, 1, 1, 1.0f, 0.0f, 100.0f, 0.0f});
    REQUIRE(terrain.AddLayer(NamedLayer("Rumput")) == 1);

    PaintBrush brush = LayerPaintBrush();
    brush.radius = 6.0f;
    brush.falloff = 0.0f;

    terrain.BeginStroke();
    for (int i = 0; i < 120; ++i) {
        ApplyLayerDab(terrain, brush, 1, 16.0f, 16.0f, 1.0f / 60.0f);
    }
    terrain.EndStroke();

    // Dengan pembulatan ke terdekat, sentuhan terakhir sebelum penuh selalu
    // membulat kembali dan bobotnya berhenti di 254: "cat sampai penuh" menjadi
    // mustahil, dan tidak ada jumlah sapuan yang menolongnya.
    CHECK(terrain.WeightAt(1, 16, 16) == 255);
    CHECK(terrain.WeightAt(0, 16, 16) == 0);
}

TEST_CASE("Mengecat layer dasar menghapus layer di atasnya") {
    Terrain terrain(TerrainDesc{32, 1, 1, 1.0f, 0.0f, 100.0f, 0.0f});
    REQUIRE(terrain.AddLayer(NamedLayer("Rumput")) == 1);
    terrain.SetWeightAt(1, 10, 10, 200);
    REQUIRE(terrain.WeightAt(1, 10, 10) == 200);

    terrain.SetWeightAt(0, 10, 10, 255);
    CHECK(terrain.WeightAt(1, 10, 10) == 0);
    CHECK(terrain.WeightAt(0, 10, 10) == 255);
    CHECK(TotalWeight(terrain, 10, 10) == 255);
}

TEST_CASE("Menghapus di atas yang belum pernah dicat tidak mengalokasikan apa pun") {
    Terrain terrain(TerrainDesc{256, 4, 4, 1.0f, 0.0f, 100.0f, 0.0f});
    REQUIRE(terrain.AddLayer(NamedLayer("Rumput")) == 1);
    const std::size_t empty = terrain.BytesResident();

    PaintBrush brush = LayerPaintBrush();
    brush.radius = 200.0f;
    brush.target = 0.0f;

    terrain.BeginStroke();
    for (int i = 0; i < 20; ++i) {
        ApplyLayerDab(terrain, brush, 1, 500.0f, 500.0f, 1.0f / 60.0f);
    }
    terrain.EndStroke();

    // Menghapus yang tidak ada adalah operasi yang tidak melakukan apa-apa. Kalau
    // ia tetap mewujudkan ubin, sekali sapu penghapus di atas terrain 4x4 km
    // sudah cukup untuk menghuni seluruh petanya.
    INFO("sebelum ", empty, " sesudah ", terrain.BytesResident());
    CHECK(terrain.BytesResident() == empty);
    CHECK(terrain.UndoDepth() == 0);
}

TEST_CASE("Menghapus layer mengembalikan bobotnya ke dasar") {
    Terrain terrain(TerrainDesc{32, 1, 1, 1.0f, 0.0f, 100.0f, 0.0f});
    REQUIRE(terrain.AddLayer(NamedLayer("Rumput")) == 1);
    REQUIRE(terrain.AddLayer(NamedLayer("Batu")) == 2);
    terrain.SetWeightAt(1, 8, 8, 100);
    terrain.SetWeightAt(2, 8, 8, 80);
    REQUIRE(terrain.WeightAt(0, 8, 8) == 75);

    REQUIRE(terrain.RemoveLayer(1));
    CHECK(terrain.LayerCount() == 2);
    CHECK(terrain.Layer(1).name == "Batu");
    CHECK(terrain.WeightAt(1, 8, 8) == 80);
    CHECK(terrain.WeightAt(0, 8, 8) == 175);
    CHECK(TotalWeight(terrain, 8, 8) == 255);

    // Layer dasar bukan salah satu dari mereka: bobotnya sisa, jadi tidak ada
    // yang bisa dihapus.
    CHECK_FALSE(terrain.RemoveLayer(0));
}

TEST_CASE("Memindahkan layer membawa peta bobotnya") {
    Terrain terrain(TerrainDesc{32, 1, 1, 1.0f, 0.0f, 100.0f, 0.0f});
    REQUIRE(terrain.AddLayer(NamedLayer("Rumput")) == 1);
    REQUIRE(terrain.AddLayer(NamedLayer("Batu")) == 2);
    terrain.SetWeightAt(1, 5, 5, 120);
    terrain.SetWeightAt(2, 6, 6, 60);

    REQUIRE(terrain.MoveLayer(1, 2));
    CHECK(terrain.Layer(1).name == "Batu");
    CHECK(terrain.Layer(2).name == "Rumput");
    // Nama dan bobot berpindah bersama. Kalau hanya deskripsinya yang pindah,
    // cat berpindah ke material yang salah tanpa satu pun tanda.
    CHECK(terrain.WeightAt(2, 5, 5) == 120);
    CHECK(terrain.WeightAt(1, 6, 6) == 60);

    CHECK_FALSE(terrain.MoveLayer(0, 1));
}

TEST_CASE("Batas jumlah layer ditegakkan") {
    Terrain terrain(TerrainDesc{16, 1, 1, 1.0f, 0.0f, 100.0f, 0.0f});
    for (int index = 1; index < kMaxLayers; ++index) {
        CHECK(terrain.AddLayer(NamedLayer("L" + std::to_string(index))) == index);
    }
    CHECK(terrain.LayerCount() == kMaxLayers);
    CHECK(terrain.AddLayer(NamedLayer("Kelebihan")) == -1);
    CHECK(terrain.LayerCount() == kMaxLayers);
}

TEST_CASE("Undo satu goresan cat mengembalikan bobot persis") {
    Terrain terrain(TerrainDesc{64, 2, 2, 1.0f, 0.0f, 100.0f, 0.0f});
    REQUIRE(terrain.AddLayer(NamedLayer("Rumput")) == 1);
    REQUIRE(terrain.AddLayer(NamedLayer("Batu")) == 2);

    const PaintBrush brush = LayerPaintBrush();
    terrain.BeginStroke();
    for (int i = 0; i < 20; ++i) {
        ApplyLayerDab(terrain, brush, 1, 50.0f, 50.0f, 1.0f / 60.0f);
    }
    terrain.EndStroke();

    const uint64_t before1 = HashWeights(terrain, 1);
    const uint64_t before2 = HashWeights(terrain, 2);

    terrain.BeginStroke();
    for (int i = 0; i < 20; ++i) {
        ApplyLayerDab(terrain, brush, 2, 52.0f, 50.0f, 1.0f / 60.0f);
    }
    terrain.EndStroke();
    REQUIRE(HashWeights(terrain, 2) != before2);
    // Sapuan kedua menggerus layer pertama karena totalnya dijaga 255, jadi undo
    // harus memulihkan KEDUANYA — bukan hanya layer yang dicat.
    REQUIRE(HashWeights(terrain, 1) != before1);

    REQUIRE(terrain.Undo());
    CHECK(HashWeights(terrain, 1) == before1);
    CHECK(HashWeights(terrain, 2) == before2);

    REQUIRE(terrain.Redo());
    CHECK(HashWeights(terrain, 1) != before1);
}

TEST_CASE("Mengecat di batas ubin sama persis dengan tanpa ubin") {
    // Argumen yang sama dengan kriteria 1 untuk tinggi: kalau peta bobot ikut
    // menyimpan baris tepi di dua ubin, jahitannya akan muncul sebagai garis cat
    // yang tidak pernah disapu siapa pun.
    const auto paint = [](Terrain& terrain) {
        REQUIRE(terrain.AddLayer(NamedLayer("Rumput")) == 1);
        PaintBrush brush = LayerPaintBrush();
        brush.radius = 20.0f;
        BrushStroke stroke;
        stroke.Begin(terrain, 40.0f, 64.0f);
        for (int i = 1; i <= 30; ++i) {
            const float x = 40.0f + 2.0f * static_cast<float>(i);
            stroke.Advance(20.0f, x, 64.0f, 1.0f / 60.0f, [&](float px, float pz, float dt) {
                ApplyLayerDab(terrain, brush, 1, px, pz, dt);
            });
        }
        stroke.End(terrain);
    };

    Terrain tiled(TerrainDesc{32, 4, 4, 1.0f, 0.0f, 100.0f, 0.0f});
    Terrain single(TerrainDesc{128, 1, 1, 1.0f, 0.0f, 100.0f, 0.0f});
    paint(tiled);
    paint(single);

    CHECK(HashWeights(tiled, 1) == HashWeights(single, 1));
}

TEST_CASE("Peta bobot yang dimuat dinormalkan, bukan dipercaya begitu saja") {
    Terrain terrain(TerrainDesc{16, 1, 1, 1.0f, 0.0f, 100.0f, 0.0f});
    REQUIRE(terrain.AddLayer(NamedLayer("Rumput")) == 1);
    REQUIRE(terrain.AddLayer(NamedLayer("Batu")) == 2);

    // Dua peta yang masing-masing penuh: jumlahnya 510, keadaan yang tidak bisa
    // dihasilkan jalur paint tapi bisa datang dari berkas yang disunting di luar.
    const std::vector<Weight> full(16u * 16u, 255);
    terrain.WriteWeights(1, full.data());
    terrain.WriteWeights(2, full.data());
    REQUIRE(terrain.WeightAt(1, 4, 4) + terrain.WeightAt(2, 4, 4) == 510);

    terrain.NormalizeWeights();
    CHECK(TotalWeight(terrain, 4, 4) == 255);
    CHECK(terrain.WeightAt(1, 4, 4) + terrain.WeightAt(2, 4, 4) <= 255);
}

// --- peta hole ----------------------------------------------------------------

TEST_CASE("Hole adalah sifat quad, bukan sifat sampel") {
    Terrain terrain(TerrainDesc{32, 2, 2, 1.0f, 0.0f, 100.0f, 0.0f});
    CHECK(terrain.HoleCount() == 0);

    terrain.SetHoleAt(10, 10, true);
    CHECK(terrain.HoleCount() == 1);
    CHECK(terrain.HoleAt(10, 10));
    // Satu quad dicat berarti satu quad hilang. Disimpan per sampel, keempat
    // quad di sekelilingnya akan ikut hilang dan lubang selebar satu quad
    // mustahil dibuat.
    CHECK_FALSE(terrain.HoleAt(9, 10));
    CHECK_FALSE(terrain.HoleAt(11, 10));
    CHECK_FALSE(terrain.HoleAt(10, 9));
    CHECK_FALSE(terrain.HoleAt(10, 11));

    terrain.SetHoleAt(10, 10, true);
    CHECK(terrain.HoleCount() == 1);  // menandai dua kali tetap satu lubang
    terrain.SetHoleAt(10, 10, false);
    CHECK(terrain.HoleCount() == 0);
}

TEST_CASE("Kolom dan baris terakhir tidak punya quad") {
    Terrain terrain(TerrainDesc{16, 1, 1, 1.0f, 0.0f, 100.0f, 0.0f});
    const int last = terrain.SamplesX() - 1;

    terrain.SetHoleAt(last, 4, true);
    terrain.SetHoleAt(4, last, true);
    CHECK(terrain.HoleCount() == 0);
    CHECK_FALSE(terrain.HoleAt(last, 4));

    // Yang tepat di sebelahnya masih quad yang sah.
    terrain.SetHoleAt(last - 1, 4, true);
    CHECK(terrain.HoleCount() == 1);
}

TEST_CASE("Undo goresan hole mengembalikan peta dan jumlahnya") {
    Terrain terrain(TerrainDesc{64, 2, 2, 1.0f, 0.0f, 100.0f, 0.0f});
    PaintBrush brush = LayerPaintBrush();
    brush.radius = 8.0f;

    terrain.BeginStroke();
    ApplyHoleDab(terrain, brush, true, 60.0f, 60.0f);
    terrain.EndStroke();
    const std::size_t cut = terrain.HoleCount();
    const uint64_t hash = HashHoles(terrain);
    REQUIRE(cut > 0);

    terrain.BeginStroke();
    ApplyHoleDab(terrain, brush, true, 70.0f, 60.0f);
    terrain.EndStroke();
    REQUIRE(terrain.HoleCount() > cut);

    REQUIRE(terrain.Undo());
    CHECK(terrain.HoleCount() == cut);
    CHECK(HashHoles(terrain) == hash);

    REQUIRE(terrain.Redo());
    CHECK(terrain.HoleCount() > cut);
}

TEST_CASE("Menutup semua lubang bisa dibatalkan") {
    Terrain terrain(TerrainDesc{32, 2, 2, 1.0f, 0.0f, 100.0f, 0.0f});
    PaintBrush brush = LayerPaintBrush();
    brush.radius = 6.0f;

    terrain.BeginStroke();
    ApplyHoleDab(terrain, brush, true, 30.0f, 30.0f);
    terrain.EndStroke();
    const uint64_t hash = HashHoles(terrain);
    const std::size_t cut = terrain.HoleCount();
    REQUIRE(cut > 0);

    terrain.BeginStroke();
    terrain.ClearHoles();
    terrain.EndStroke();
    REQUIRE(terrain.HoleCount() == 0);

    REQUIRE(terrain.Undo());
    CHECK(terrain.HoleCount() == cut);
    CHECK(HashHoles(terrain) == hash);
}

TEST_CASE("Kuas hole memotong quad utuh, bukan setengah quad") {
    Terrain terrain(TerrainDesc{64, 1, 1, 1.0f, 0.0f, 100.0f, 0.0f});
    PaintBrush brush = LayerPaintBrush();
    brush.radius = 10.0f;
    brush.falloff = 1.0f;

    ApplyHoleDab(terrain, brush, true, 32.0f, 32.0f);

    // Dengan falloff penuh, ambang setengah bobot jatuh di setengah jari-jari.
    // Yang diuji bukan angkanya melainkan bahwa batasnya tajam: ada jarak yang
    // seluruhnya berlubang dan jarak yang seluruhnya utuh, tanpa daerah abu-abu.
    CHECK(terrain.HoleAt(32, 32));
    CHECK(terrain.HoleAt(34, 32));
    CHECK_FALSE(terrain.HoleAt(41, 32));
    CHECK_FALSE(terrain.HoleAt(50, 32));
}

// --- berkas pendamping ---------------------------------------------------------

TEST_CASE("Layer, bobot, dan lubang ikut tersimpan lalu dimuat kembali persis") {
    TempDir dir("splat");
    TerrainDocument document;
    document.name = "Bukit";
    document.desc = TerrainDesc{32, 2, 2, 1.0f, 0.0f, 200.0f, 10.0f};

    Terrain terrain(document.desc);
    TerrainLayer grass = NamedLayer("Rumput");
    grass.color = Vec3(0.2f, 0.7f, 0.3f);
    grass.tileSize = 3.5f;
    REQUIRE(terrain.AddLayer(grass) == 1);
    REQUIRE(terrain.AddLayer(NamedLayer("Belum dicat")) == 2);

    PaintBrush brush = LayerPaintBrush();
    brush.radius = 10.0f;
    terrain.BeginStroke();
    for (int i = 0; i < 20; ++i) {
        ApplyLayerDab(terrain, brush, 1, 30.0f, 30.0f, 1.0f / 60.0f);
    }
    ApplyHoleDab(terrain, brush, true, 40.0f, 20.0f);
    terrain.EndStroke();
    REQUIRE(terrain.HoleCount() > 0);

    const std::filesystem::path path = dir / "Bukit.simterrain";
    REQUIRE(SaveTerrain(terrain, document, path).ok);
    CHECK(std::filesystem::exists(dir / "Bukit_w1.png"));
    CHECK(std::filesystem::exists(dir / "Bukit_holes.png"));
    // Layer yang belum pernah dicat tidak menulis berkas: nama yang tetap dicatat
    // adalah nama yang menunjuk berkas yang tidak ada.
    CHECK_FALSE(std::filesystem::exists(dir / "Bukit_w2.png"));

    Terrain loaded;
    TerrainDocument loadedDocument;
    const TerrainIoResult result = LoadTerrain(loaded, loadedDocument, path);
    INFO(result.error);
    REQUIRE(result.ok);

    REQUIRE(loaded.LayerCount() == 3);
    CHECK(loaded.Layer(1).name == "Rumput");
    CHECK(loaded.Layer(1).tileSize == doctest::Approx(3.5f));
    CHECK(loaded.Layer(1).color.y == doctest::Approx(0.7f));
    CHECK(loaded.Layer(2).name == "Belum dicat");
    CHECK_FALSE(loaded.LayerPainted(2));

    CHECK(HashWeights(loaded, 1) == HashWeights(terrain, 1));
    CHECK(loaded.HoleCount() == terrain.HoleCount());
    CHECK(HashHoles(loaded) == HashHoles(terrain));
}

TEST_CASE("Terrain tanpa lubang tidak menulis peta hole") {
    TempDir dir("noholes");
    TerrainDocument document;
    document.name = "Datar";
    document.desc = TerrainDesc{32, 1, 1, 1.0f, 0.0f, 100.0f, 0.0f};

    Terrain terrain(document.desc);
    const std::filesystem::path path = dir / "Datar.simterrain";
    REQUIRE(SaveTerrain(terrain, document, path).ok);
    CHECK_FALSE(std::filesystem::exists(dir / "Datar_holes.png"));

    Terrain loaded;
    TerrainDocument loadedDocument;
    REQUIRE(LoadTerrain(loaded, loadedDocument, path).ok);
    CHECK(loadedDocument.holeFile.empty());
    CHECK(loaded.HoleCount() == 0);
}

TEST_CASE("Berkas versi 1 tetap terbaca sebagai terrain berlayer tunggal") {
    // Berkas yang ditulis sebelum ada layer: tanpa daftar layer, tanpa peta hole.
    const std::string v1 = R"({
  "version": 1,
  "name": "Lama",
  "heightmap": "Lama_height.png",
  "tileSamples": 32,
  "tilesX": 2,
  "tilesY": 2,
  "sampleSpacing": 1.0,
  "minHeight": 0.0,
  "maxHeight": 500.0,
  "baseHeight": 25.0
})";

    TerrainDocument document;
    std::vector<TerrainLayer> layers;
    const TerrainIoResult result = LoadDocumentFromString(document, layers, v1);
    INFO(result.error);
    REQUIRE(result.ok);
    CHECK(result.sourceVersion == 1);
    CHECK(document.name == "Lama");
    CHECK(document.holeFile.empty());
    CHECK(document.desc.baseHeight == doctest::Approx(25.0f));
    REQUIRE(layers.size() == 1u);
    CHECK(layers[0].weightFile.empty());
}

TEST_CASE("Round-trip PNG 8-bit peta bobot tanpa kehilangan satu tingkat pun") {
    TempDir dir("weightpng");
    const TerrainDesc desc{32, 2, 2, 1.0f, 0.0f, 100.0f, 0.0f};

    Terrain terrain(desc);
    REQUIRE(terrain.AddLayer(NamedLayer("Rumput")) == 1);
    // Nilai yang bervariasi di setiap sampel, bukan sekadar blok penuh: bobot
    // yang seragam akan lolos bahkan lewat enkoder yang membulatkan.
    for (int y = 0; y < terrain.SamplesY(); ++y) {
        for (int x = 0; x < terrain.SamplesX(); ++x) {
            terrain.SetWeightAt(1, x, y, static_cast<Weight>((x * 7 + y * 13) % 256));
        }
    }

    const std::filesystem::path path = dir / "weights.png";
    REQUIRE(SaveWeightPng(terrain, 1, path).ok);

    Terrain loaded(desc);
    REQUIRE(loaded.AddLayer(NamedLayer("Rumput")) == 1);
    const TerrainIoResult result = LoadWeightPng(loaded, 1, path);
    INFO(result.error);
    REQUIRE(result.ok);
    CHECK(HashWeights(loaded, 1) == HashWeights(terrain, 1));

    // Dibaca dengan stb, bukan dengan dekoder buatan sendiri — jadi berkasnya
    // memang PNG yang sah, bukan sekadar konsisten dengan penulisnya.
    Terrain wrongSize(TerrainDesc{16, 1, 1, 1.0f, 0.0f, 100.0f, 0.0f});
    REQUIRE(wrongSize.AddLayer(NamedLayer("Rumput")) == 1);
    const TerrainIoResult mismatch = LoadWeightPng(wrongSize, 1, path);
    CHECK_FALSE(mismatch.ok);
    CHECK(mismatch.error.find("64x64") != std::string::npos);
    CHECK(mismatch.error.find("16x16") != std::string::npos);
}

TEST_CASE("Peta bobot ikut lazy: layer yang dicat sepetak tidak menghuni seluruh peta") {
    Terrain terrain(TerrainDesc{256, 4, 4, 1.0f, 0.0f, 100.0f, 0.0f});
    REQUIRE(terrain.AddLayer(NamedLayer("Rumput")) == 1);
    const std::size_t empty = terrain.BytesResident();

    PaintBrush brush = LayerPaintBrush();
    brush.radius = 20.0f;
    // Jauh di dalam satu ubin, dengan sengaja: sapuan yang menyeberangi batas
    // memang mewujudkan keempat ubin yang disentuhnya, dan yang sedang diuji di
    // sini bukan itu melainkan bahwa ubin yang TIDAK disentuh tetap kosong.
    terrain.BeginStroke();
    for (int i = 0; i < 10; ++i) {
        ApplyLayerDab(terrain, brush, 1, 128.0f, 128.0f, 1.0f / 60.0f);
    }
    terrain.EndStroke();

    // Satu ubin bobot (256² byte) plus beberapa blok jurnal. Peta bobot penuh
    // untuk 16 ubin adalah satu megabyte; yang dibayar hanya yang benar-benar
    // dicat.
    const std::size_t grew = terrain.BytesResident() - empty;
    INFO("tumbuh ", grew, " byte");
    CHECK(grew >= 256u * 256u);
    CHECK(grew < 128u * 1024u);
    CHECK(terrain.LayerPainted(1));
}

TEST_CASE("Profil kuas cat masih berarti setelah kuasnya ditahan") {
    Terrain terrain(TerrainDesc{64, 1, 1, 1.0f, 0.0f, 100.0f, 0.0f});
    REQUIRE(terrain.AddLayer(NamedLayer("Rumput")) == 1);

    PaintBrush brush = LayerPaintBrush();
    brush.radius = 20.0f;
    brush.falloff = 1.0f;

    // Menahan kuas di satu tempat adalah keadaan terburuk bagi aturan
    // "membulat menjauhi": setiap sentuhan mengenai sampel yang sama, jadi
    // langkah minimum satu tingkat di pinggir kuas menumpuk paling cepat di sini.
    terrain.BeginStroke();
    for (int i = 0; i < 30; ++i) {
        ApplyLayerDab(terrain, brush, 1, 32.0f, 32.0f, 1.0f / 60.0f);
    }
    terrain.EndStroke();

    const int centre = terrain.WeightAt(1, 32, 32);
    const int middle = terrain.WeightAt(1, 42, 32);  // setengah jari-jari
    const int rim = terrain.WeightAt(1, 50, 32);     // 90% jari-jari

    INFO("pusat ", centre, " tengah ", middle, " pinggir ", rim);
    CHECK(centre > middle);
    CHECK(middle > rim);
    // Dan di luar jari-jari tetap tidak tersentuh: pembulatan menjauhi hanya
    // berlaku pada sampel yang bobot kuasnya bukan nol.
    CHECK(terrain.WeightAt(1, 53, 32) == 0);
}

// --- I3: heightmap TIFF ------------------------------------------------------

TEST_CASE("heightmap TIFF 16-bit dimuat dengan nilai yang benar") {
    // **Nilainya yang diperiksa, bukan bentuknya.** Heightmap yang dimuat dengan
    // dimensi benar tapi rentang tinggi meleset terlihat persis seperti terrain
    // yang memang begitu — tidak ada galat di mana pun, hanya lembah yang salah
    // dalam.
    //
    // Berkasnya ditulis tangan, di luar libtiff, dan **dimampatkan deflate**:
    // heightmap dari World Machine, Gaea, dan sumber GIS tidak pernah TIFF
    // mentah, jadi fixture tanpa kompresi tidak akan membuktikan apa-apa.
    // Isinya diterangkan di Tests/ImageIOTests.cpp.
    const std::filesystem::path tiff = std::filesystem::path(SIM_IMAGE_DIR) / "ramp16.tif";
    if (!sim::imageio::CanRead(".tif")) {
        MESSAGE("build ini tanpa libtiff — TIFF dilewati");
        std::vector<Sample> ignored;
        int w = 0;
        int h = 0;
        CHECK_FALSE(ReadHeightmapImage(tiff, ignored, w, h).ok);
        return;
    }

    std::vector<Sample> samples;
    int width = 0;
    int height = 0;
    const TerrainIoResult result = ReadHeightmapImage(tiff, samples, width, height);
    INFO(result.error);
    REQUIRE(result.ok);
    CHECK(width == 8);
    CHECK(height == 8);
    REQUIRE(samples.size() == 64);

    // sampel(x,y) = (y*8 + x) * 1024, apa adanya — tanpa penskalaan ulang.
    for (std::size_t i = 0; i < samples.size(); ++i) {
        CHECK(samples[i] == static_cast<Sample>(i * 1024));
    }

    SUBCASE("PNG dan TIFF dengan isi yang sama menghasilkan sampel yang sama") {
        // Dua format, dua pustaka, dua penulis tangan yang berbeda. Kalau
        // keduanya tidak sampai pada angka yang sama, salah satu jalurnya salah.
        std::vector<Sample> fromPng;
        int pngWidth = 0;
        int pngHeight = 0;
        REQUIRE(ReadHeightmapImage(std::filesystem::path(SIM_IMAGE_DIR) / "ramp16.png", fromPng,
                                   pngWidth, pngHeight)
                    .ok);
        CHECK(pngWidth == width);
        CHECK(pngHeight == height);
        CHECK(fromPng == samples);
    }
}

TEST_CASE("heightmap TIFF 32-bit float tidak dipotong diam-diam") {
    if (!sim::imageio::CanRead(".tif")) {
        return;
    }

    // Berkasnya berisi meter, 100..1675 — bentuk yang keluar dari DEM GIS.
    // Menjepitnya ke 0..1 akan meratakan seluruhnya menjadi dataran tinggi
    // seragam; yang benar adalah memetakan rentangnya dan **mencatat** bahwa
    // itu dilakukan.
    std::vector<Sample> samples;
    int width = 0;
    int height = 0;
    const TerrainIoResult result = ReadHeightmapImage(
        std::filesystem::path(SIM_IMAGE_DIR) / "heights-metres.tif", samples, width, height);
    INFO(result.error);
    REQUIRE(result.ok);
    REQUIRE(samples.size() == 64);

    // Rentangnya terpakai penuh: yang terendah menjadi nol, yang tertinggi
    // menjadi puncak. Kalau nilainya terjepit, keduanya akan bertumpuk di ujung
    // atas dan seluruh relief hilang.
    CHECK(samples.front() == 0);
    CHECK(samples.back() == kSampleMax);

    // Dan yang di antaranya tersebar merata, karena sumbernya memang tangga
    // seragam. Ini yang membedakan "dipetakan" dari "dijepit".
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto expected = static_cast<Sample>(
            std::lround(static_cast<double>(i) / 63.0 * static_cast<double>(kSampleMax)));
        CHECK(samples[i] == expected);
    }
}

TEST_CASE("round-trip 16-bit identik lewat kedua jalur penulis") {
    // Kriteria terima I3. Dua penulis yang berbeda — enkoder PNG tangan dan
    // libtiff — harus mengembalikan sampel yang sama persis, bit per bit.
    // Penulis yang kehilangan satu bit di ujung rentang tidak akan terlihat
    // pada terrain mana pun sampai seseorang membandingkan dua berkas.
    TempDir dir("writers");
    TerrainDesc desc{32, 2, 2, 1.0f, 0.0f, 1000.0f, 0.0f};
    Terrain source(desc);

    // Pola yang memakai seluruh rentang termasuk kedua ujungnya, dengan alasan
    // yang sama seperti round-trip PNG di atas.
    for (int y = 0; y < source.SamplesY(); ++y) {
        for (int x = 0; x < source.SamplesX(); ++x) {
            source.SetRawAt(x, y, static_cast<Sample>((x * 517 + y * 8191) & 0xffff));
        }
    }
    source.SetRawAt(0, 0, 0);
    source.SetRawAt(1, 0, kSampleMax);

    std::vector<Sample> original;
    source.ReadAll(original);

    std::vector<std::string> written{"height.png"};
    if (sim::imageio::CanWrite(".tif")) {
        written.emplace_back("height.tif");
    }

    for (const std::string& name : written) {
        INFO("lewat " << name);
        const std::filesystem::path path = dir / name;
        REQUIRE(SaveHeightmapImage(source, path).ok);

        Terrain loaded(desc);
        const TerrainIoResult result = LoadHeightmapImage(loaded, path);
        INFO(result.error);
        REQUIRE(result.ok);

        std::vector<Sample> roundTripped;
        loaded.ReadAll(roundTripped);
        CHECK(roundTripped == original);
    }
}
