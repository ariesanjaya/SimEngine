#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Terrain/Terrain.h"
#include "Sim/Terrain/TerrainBrush.h"
#include "Sim/Terrain/TerrainIo.h"

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
    REQUIRE(SaveHeightmapPng(source, png).ok);

    Terrain loaded(desc);
    const TerrainIoResult result = LoadHeightmapPng(loaded, png);
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
    REQUIRE(SaveHeightmapPng(terrain, png).ok);
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
    REQUIRE(SaveHeightmapPng(small, png).ok);

    Terrain big(TerrainDesc{32, 2, 2});
    const TerrainIoResult result = LoadHeightmapPng(big, png);
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
    REQUIRE(ReadHeightmapPng(png, samples, w, h).ok);
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
    const std::string text = SaveDocumentToString(loadedDocument);
    TerrainDocument again;
    REQUIRE(LoadDocumentFromString(again, text).ok);
    CHECK(SaveDocumentToString(again) == text);
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
    REQUIRE(SaveHeightmapPng(source, png).ok);

    Terrain loaded(desc);
    REQUIRE(LoadHeightmapPng(loaded, png).ok);

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
