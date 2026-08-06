#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Terrain/TerrainBrush.h"
#include "Sim/Terrain/TerrainIo.h"
#include "Sim/Vegetation/Vegetation.h"
#include "Sim/Vegetation/VegetationBrush.h"
#include "Sim/Vegetation/VegetationIo.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

using namespace sim;
using namespace sim::vegetation;

namespace {

/// Folder sementara yang bersih untuk berkas uji, dihapus saat selesai.
class TempDir {
public:
    explicit TempDir(const std::string& name)
        : path_(std::filesystem::temp_directory_path() / ("sim-veg-" + name)) {
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

terrain::Terrain MakeTerrain(int tileSamples = 128, int tiles = 2, float spacing = 1.0f) {
    terrain::TerrainDesc desc;
    desc.tileSamples = tileSamples;
    desc.tilesX = tiles;
    desc.tilesY = tiles;
    desc.sampleSpacing = spacing;
    desc.minHeight = 0.0f;
    desc.maxHeight = 200.0f;
    desc.baseHeight = 10.0f;
    return terrain::Terrain(desc);
}

VegetationLayer MakeLayer(float minDistance = 4.0f, uint32_t seed = 7) {
    VegetationLayer layer;
    layer.name = "Pines";
    layer.rules.minDistance = minDistance;
    layer.rules.seed = seed;
    layer.rules.maxSlopeDegrees = 90.0f;
    return layer;
}

/// Hash seluruh sebaran, dari bit mentah setiap angka.
///
/// Bit mentah dan bukan nilainya: klaim yang diuji adalah "byte yang sama",
/// bukan "kira-kira di tempat yang sama", dan perbandingan float dengan toleransi
/// tidak bisa membedakan keduanya.
uint64_t HashInstances(const std::vector<Instance>& instances) {
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        for (int byte = 0; byte < 4; ++byte) {
            hash ^= static_cast<uint64_t>((bits >> (byte * 8)) & 0xFFu);
            hash *= 1099511628211ull;
        }
    };
    for (const Instance& instance : instances) {
        mix(instance.position.x);
        mix(instance.position.y);
        mix(instance.position.z);
        mix(instance.up.x);
        mix(instance.up.y);
        mix(instance.up.z);
        mix(instance.yaw);
        mix(instance.scale);
    }
    return hash;
}

std::unordered_set<uint64_t> KeysOf(const std::vector<Instance>& instances) {
    std::unordered_set<uint64_t> keys;
    keys.reserve(instances.size());
    for (const Instance& instance : instances) {
        keys.insert(InstanceKey(instance.position.x, instance.position.z));
    }
    return keys;
}

/// Kerucut di tengah peta, supaya ada rentang tinggi dan kemiringan yang nyata
/// untuk diuji aturannya.
void SculptCone(terrain::Terrain& terrain, float peakMeters) {
    const float spacing = terrain.Desc().sampleSpacing;
    const float centreX = terrain.WorldWidth() * 0.5f;
    const float centreZ = terrain.WorldDepth() * 0.5f;
    const float radius = terrain.WorldWidth() * 0.4f;
    for (int y = 0; y < terrain.SamplesY(); ++y) {
        for (int x = 0; x < terrain.SamplesX(); ++x) {
            const float dx = static_cast<float>(x) * spacing - centreX;
            const float dz = static_cast<float>(y) * spacing - centreZ;
            const float distance = std::sqrt(dx * dx + dz * dz);
            const float t = std::clamp(1.0f - distance / radius, 0.0f, 1.0f);
            terrain.SetHeightAt(x, y, terrain.Desc().baseHeight + peakMeters * t);
        }
    }
}

terrain::PaintBrush DensityBrush(float radius = 12.0f) {
    terrain::PaintBrush brush;
    brush.radius = radius;
    brush.strength = 40.0f;
    brush.falloff = 0.4f;
    brush.target = 0.0f;
    return brush;
}

}  // namespace

// --- acak deterministik -------------------------------------------------------

TEST_CASE("Aliran acak hanya bergantung pada benihnya") {
    Rng a(12345);
    Rng b(12345);
    for (int i = 0; i < 64; ++i) {
        CHECK(a.NextU64() == b.NextU64());
    }
}

TEST_CASE("Pecahan acak selalu di dalam [0,1)") {
    Rng rng(CellSeed(3, -17, 9001));
    for (int i = 0; i < 100000; ++i) {
        const float value = rng.NextFloat();
        REQUIRE(value >= 0.0f);
        REQUIRE(value < 1.0f);
    }
}

TEST_CASE("Sel bertetangga tidak menghasilkan aliran yang berkorelasi") {
    // Kalau benih sel sekadar dijumlahkan, undian pertama dua sel bersebelahan
    // bergerak searah — dan itu terlihat sebagai baris pohon yang sejajar.
    // Yang diuji di sini bukan mutunya sebagai PRNG melainkan bahwa undian
    // pertamanya tersebar, bukan menempel.
    int lower = 0;
    for (int cell = 0; cell < 1000; ++cell) {
        Rng rng(CellSeed(1, cell, 0));
        if (rng.NextFloat() < 0.5f) {
            ++lower;
        }
    }
    CHECK(lower > 400);
    CHECK(lower < 600);
}

TEST_CASE("Kunci instance bolak-balik utuh") {
    const uint64_t key = InstanceKey(-123.456f, 789.012f);
    int32_t x = 0;
    int32_t z = 0;
    UnpackKey(key, x, z);
    CHECK(x == -123456);
    CHECK(z == 789012);
    CHECK(PackKey(x, z) == key);
}

// --- sebaran ------------------------------------------------------------------

TEST_CASE("Sebaran menghormati jarak minimum") {
    terrain::Terrain terrain = MakeTerrain();
    Vegetation vegetation;
    vegetation.Fit(terrain);
    const int layer = vegetation.AddLayer(MakeLayer(6.0f));

    const std::size_t count = vegetation.Scatter(terrain, layer);
    REQUIRE(count > 100);

    // Diperiksa lewat kisi sendiri, bukan n² perbandingan: seribu instance
    // berarti setengah juta pasangan, dan itu masih murah — tapi klaimnya harus
    // tetap terbaca kalau nanti jumlahnya naik.
    const std::vector<Instance>& instances = vegetation.Instances(layer);
    std::size_t tooClose = 0;
    for (std::size_t i = 0; i < instances.size(); ++i) {
        for (std::size_t j = i + 1; j < instances.size(); ++j) {
            const float dx = instances[i].position.x - instances[j].position.x;
            const float dz = instances[i].position.z - instances[j].position.z;
            if (dx * dx + dz * dz < 6.0f * 6.0f) {
                ++tooClose;
            }
        }
    }
    CHECK(tooClose == 0);
}

TEST_CASE("Sebaran tidak keluar dari batas terrain") {
    terrain::Terrain terrain = MakeTerrain();
    Vegetation vegetation;
    vegetation.Fit(terrain);
    const int layer = vegetation.AddLayer(MakeLayer(3.0f));
    vegetation.Scatter(terrain, layer);

    for (const Instance& instance : vegetation.Instances(layer)) {
        REQUIRE(instance.position.x >= 0.0f);
        REQUIRE(instance.position.z >= 0.0f);
        REQUIRE(instance.position.x <= terrain.WorldWidth());
        REQUIRE(instance.position.z <= terrain.WorldDepth());
    }
}

TEST_CASE("Benih yang sama menghasilkan sebaran yang sama persis") {
    terrain::Terrain terrain = MakeTerrain();
    SculptCone(terrain, 60.0f);

    Vegetation first;
    first.Fit(terrain);
    const int a = first.AddLayer(MakeLayer(3.0f, 4242));
    first.Scatter(terrain, a);

    Vegetation second;
    second.Fit(terrain);
    // Layer lain lebih dulu, lalu layer yang sama: sebaran tidak boleh
    // bergantung pada apa pun di luar aturannya sendiri, termasuk urutan
    // pembuatan layer.
    second.AddLayer(MakeLayer(9.0f, 1));
    const int b = second.AddLayer(MakeLayer(3.0f, 4242));
    second.ScatterAll(terrain);

    REQUIRE(first.Instances(a).size() == second.Instances(b).size());
    CHECK(HashInstances(first.Instances(a)) == HashInstances(second.Instances(b)));
}

TEST_CASE("Menyebar dua kali dari objek yang sama tidak menumpuk") {
    terrain::Terrain terrain = MakeTerrain();
    Vegetation vegetation;
    vegetation.Fit(terrain);
    const int layer = vegetation.AddLayer(MakeLayer(4.0f));

    const std::size_t first = vegetation.Scatter(terrain, layer);
    const std::size_t second = vegetation.Scatter(terrain, layer);
    CHECK(first == second);
}

TEST_CASE("Mengubah aturan menyaring sebaran yang sama, bukan memindahkannya") {
    // Inilah sifat yang membuat penghapusan tangan bertahan: undian ditarik
    // dengan jumlah tetap per kandidat, jadi aturan hanya memilih siapa yang
    // lolos — bukan di mana yang lolos berdiri.
    terrain::Terrain terrain = MakeTerrain();
    SculptCone(terrain, 80.0f);

    Vegetation vegetation;
    vegetation.Fit(terrain);
    VegetationLayer layer = MakeLayer(4.0f, 99);
    const int index = vegetation.AddLayer(layer);
    vegetation.Scatter(terrain, index);
    const std::unordered_set<uint64_t> before = KeysOf(vegetation.Instances(index));

    vegetation.Layer(index).rules.maxHeight = 40.0f;
    vegetation.Scatter(terrain, index);
    const std::vector<Instance>& after = vegetation.Instances(index);

    REQUIRE(!after.empty());
    REQUIRE(after.size() < before.size());
    for (const Instance& instance : after) {
        REQUIRE(before.count(InstanceKey(instance.position.x, instance.position.z)) == 1);
    }
}

TEST_CASE("Aturan tinggi benar-benar menyaring") {
    terrain::Terrain terrain = MakeTerrain();
    SculptCone(terrain, 100.0f);

    Vegetation vegetation;
    vegetation.Fit(terrain);
    VegetationLayer layer = MakeLayer(3.0f);
    layer.rules.minHeight = 40.0f;
    layer.rules.maxHeight = 70.0f;
    const int index = vegetation.AddLayer(layer);
    REQUIRE(vegetation.Scatter(terrain, index) > 0);

    for (const Instance& instance : vegetation.Instances(index)) {
        const float height = terrain.HeightAtWorld(instance.position.x, instance.position.z);
        REQUIRE(height >= 40.0f);
        REQUIRE(height <= 70.0f);
    }
}

TEST_CASE("Aturan kemiringan benar-benar menyaring") {
    // Kerucutnya sengaja curam — lerengnya sekitar 44° di seluruh permukaannya —
    // jadi batas 30° harus menyisakan dataran di luarnya saja, dan batas 60°
    // harus menerima keduanya. Satu terrain, dua batas: yang diuji perbedaan
    // keputusannya, bukan sekadar bahwa ada sesuatu yang lolos.
    terrain::Terrain terrain = MakeTerrain();
    SculptCone(terrain, 100.0f);
    const float centreX = terrain.WorldWidth() * 0.5f;
    const float centreZ = terrain.WorldDepth() * 0.5f;
    const float coneRadius = terrain.WorldWidth() * 0.4f;

    Vegetation vegetation;
    vegetation.Fit(terrain);
    VegetationLayer layer = MakeLayer(3.0f);
    layer.rules.maxSlopeDegrees = 30.0f;
    const int index = vegetation.AddLayer(layer);
    const std::size_t gentle = vegetation.Scatter(terrain, index);
    REQUIRE(gentle > 0);

    for (const Instance& instance : vegetation.Instances(index)) {
        const float dx = instance.position.x - centreX;
        const float dz = instance.position.z - centreZ;
        REQUIRE(std::sqrt(dx * dx + dz * dz) > coneRadius - 2.0f);
        const Vec3 normal = terrain.NormalAtWorld(instance.position.x, instance.position.z);
        REQUIRE(normal.y >= std::cos(30.0f * kDegToRad) - 0.001f);
    }

    vegetation.Layer(index).rules.maxSlopeDegrees = 60.0f;
    CHECK(vegetation.Scatter(terrain, index) > gentle);
}

TEST_CASE("Aturan layer terrain menyaring menurut bobot cat") {
    terrain::Terrain terrain = MakeTerrain();
    terrain::TerrainLayer rock;
    rock.name = "Rock";
    const int rockLayer = terrain.AddLayer(rock);
    REQUIRE(rockLayer == 1);
    // Separuh kiri dicat penuh, separuh kanan dibiarkan.
    for (int y = 0; y < terrain.SamplesY(); ++y) {
        for (int x = 0; x < terrain.SamplesX() / 2; ++x) {
            terrain.SetWeightAt(rockLayer, x, y, terrain::kWeightMax);
        }
    }

    Vegetation vegetation;
    vegetation.Fit(terrain);
    VegetationLayer layer = MakeLayer(3.0f);
    layer.rules.terrainLayer = rockLayer;
    layer.rules.minTerrainWeight = 200;
    const int index = vegetation.AddLayer(layer);
    REQUIRE(vegetation.Scatter(terrain, index) > 0);

    const float middle = terrain.WorldWidth() * 0.5f;
    for (const Instance& instance : vegetation.Instances(index)) {
        REQUIRE(instance.position.x <= middle + terrain.Desc().sampleSpacing);
    }
}

TEST_CASE("Lubang terrain tidak ditumbuhi") {
    terrain::Terrain terrain = MakeTerrain();
    for (int y = 40; y < 200; ++y) {
        for (int x = 40; x < 200; ++x) {
            terrain.SetHoleAt(x, y, true);
        }
    }
    REQUIRE(terrain.HoleCount() > 0);

    Vegetation vegetation;
    vegetation.Fit(terrain);
    const int index = vegetation.AddLayer(MakeLayer(2.0f));
    REQUIRE(vegetation.Scatter(terrain, index) > 0);

    const float spacing = terrain.Desc().sampleSpacing;
    for (const Instance& instance : vegetation.Instances(index)) {
        const int qx = static_cast<int>(std::floor(instance.position.x / spacing));
        const int qy = static_cast<int>(std::floor(instance.position.z / spacing));
        REQUIRE(!terrain.HoleAt(qx, qy));
    }
}

TEST_CASE("Kepadatan nol tidak menumbuhkan apa pun") {
    terrain::Terrain terrain = MakeTerrain();
    Vegetation vegetation;
    vegetation.Fit(terrain);
    VegetationLayer layer = MakeLayer(3.0f);
    layer.rules.density = 0.0f;
    const int index = vegetation.AddLayer(layer);
    CHECK(vegetation.Scatter(terrain, index) == 0);
}

TEST_CASE("Instance menempel di permukaan yang dipahat") {
    terrain::Terrain terrain = MakeTerrain();
    SculptCone(terrain, 50.0f);

    Vegetation vegetation;
    vegetation.Fit(terrain);
    VegetationLayer layer = MakeLayer(4.0f);
    layer.offsetY = 0.0f;
    const int index = vegetation.AddLayer(layer);
    REQUIRE(vegetation.Scatter(terrain, index) > 0);

    for (const Instance& instance : vegetation.Instances(index)) {
        const float height = terrain.HeightAtWorld(instance.position.x, instance.position.z);
        REQUIRE(instance.position.y == doctest::Approx(height).epsilon(0.0001));
    }
}

// --- peta kepadatan -----------------------------------------------------------

TEST_CASE("Peta kepadatan yang belum dicat tidak menghuni memori") {
    terrain::Terrain terrain = MakeTerrain();
    Vegetation vegetation;
    vegetation.Fit(terrain);
    const int index = vegetation.AddLayer(MakeLayer());
    CHECK(!vegetation.Density(index).Painted());
    CHECK(vegetation.Density(index).Bytes() == 0);
    CHECK(vegetation.Density(index).SampleWorld(10.0f, 10.0f) == 1.0f);

    // Menulis nilai bawaan di atas peta kosong tetap tidak mengalokasikan.
    vegetation.PaintDensity(index, 3, 3, 255);
    CHECK(!vegetation.Density(index).Painted());
}

TEST_CASE("Mengecat kepadatan nol menghapus vegetasi di bawah kuas") {
    terrain::Terrain terrain = MakeTerrain();
    Vegetation vegetation;
    vegetation.Fit(terrain);
    const int index = vegetation.AddLayer(MakeLayer(3.0f));
    const std::size_t before = vegetation.Scatter(terrain, index);
    REQUIRE(before > 0);

    const float centreX = terrain.WorldWidth() * 0.5f;
    const float centreZ = terrain.WorldDepth() * 0.5f;
    vegetation.BeginStroke();
    for (int step = 0; step < 20; ++step) {
        ApplyDensityDab(vegetation, DensityBrush(20.0f), index, centreX, centreZ, 1.0f / 60.0f);
    }
    vegetation.EndStroke();

    const std::size_t after = vegetation.Scatter(terrain, index);
    CHECK(after < before);
    // Yang di tengah lingkaran habis; yang di luarnya tidak tersentuh.
    for (const Instance& instance : vegetation.Instances(index)) {
        const float dx = instance.position.x - centreX;
        const float dz = instance.position.z - centreZ;
        REQUIRE(std::sqrt(dx * dx + dz * dz) > 5.0f);
    }
}

TEST_CASE("Goresan cat kepadatan bisa dibatalkan") {
    terrain::Terrain terrain = MakeTerrain();
    Vegetation vegetation;
    vegetation.Fit(terrain);
    const int index = vegetation.AddLayer(MakeLayer(3.0f));
    const std::size_t before = vegetation.Scatter(terrain, index);

    vegetation.BeginStroke();
    for (int step = 0; step < 20; ++step) {
        ApplyDensityDab(vegetation, DensityBrush(20.0f), index, 60.0f, 60.0f, 1.0f / 60.0f);
    }
    vegetation.EndStroke();
    REQUIRE(vegetation.Density(index).Painted());
    REQUIRE(vegetation.Scatter(terrain, index) < before);

    REQUIRE(vegetation.Undo());
    CHECK(vegetation.Scatter(terrain, index) == before);
    REQUIRE(vegetation.Redo());
    CHECK(vegetation.Scatter(terrain, index) < before);
}

TEST_CASE("Membersihkan kepadatan bisa dibatalkan tanpa menjurnal tiap sel") {
    terrain::Terrain terrain = MakeTerrain();
    Vegetation vegetation;
    vegetation.Fit(terrain);
    const int index = vegetation.AddLayer(MakeLayer(3.0f));

    vegetation.BeginStroke();
    for (int step = 0; step < 20; ++step) {
        ApplyDensityDab(vegetation, DensityBrush(20.0f), index, 60.0f, 60.0f, 1.0f / 60.0f);
    }
    vegetation.EndStroke();
    const std::vector<uint8_t> painted = vegetation.Density(index).Cells();
    REQUIRE(!painted.empty());
    const std::size_t journalBefore = vegetation.UndoBytes();

    vegetation.BeginStroke();
    vegetation.ClearDensity(index);
    vegetation.EndStroke();
    CHECK(!vegetation.Density(index).Painted());
    // Seluruh peta masuk jurnal sebagai satu salinan, bukan sebagai satu catatan
    // per sel — satu catatan per sel akan jauh lebih besar daripada petanya.
    CHECK(vegetation.UndoBytes() - journalBefore <= painted.size() + 64u);

    REQUIRE(vegetation.Undo());
    CHECK(vegetation.Density(index).Cells() == painted);
}

TEST_CASE("Kuas ratakan tidak memakan keluarannya sendiri") {
    terrain::Terrain terrain = MakeTerrain();
    Vegetation vegetation;
    vegetation.Fit(terrain);
    const int index = vegetation.AddLayer(MakeLayer(3.0f));

    // Satu blok tajam, lalu satu sentuhan ratakan tepat di tengahnya. Kalau
    // nilai tetangga dibaca dari peta yang sedang ditulis, hasilnya miring ke
    // arah pemindaian — sisi kiri dan sisi kanan blok tidak lagi setangkup.
    for (int y = 10; y < 30; ++y) {
        for (int x = 10; x < 30; ++x) {
            vegetation.PaintDensity(index, x, y, 0);
        }
    }
    const float cell = vegetation.Density(index).CellSize();
    const float centre = 20.0f * cell;
    terrain::PaintBrush brush = DensityBrush(12.0f * cell);
    brush.falloff = 0.0f;
    ApplySmoothDab(vegetation, brush, index, centre, centre, 1.0f / 60.0f);

    for (int offset = 1; offset <= 8; ++offset) {
        CHECK(vegetation.Density(index).At(20 - offset, 20) ==
              vegetation.Density(index).At(20 + offset, 20));
    }
}

// --- suntingan tangan ---------------------------------------------------------

TEST_CASE("Penghapusan tangan bertahan setelah aturan diubah") {
    terrain::Terrain terrain = MakeTerrain();
    SculptCone(terrain, 60.0f);

    Vegetation vegetation;
    vegetation.Fit(terrain);
    const int index = vegetation.AddLayer(MakeLayer(4.0f, 31337));
    REQUIRE(vegetation.Scatter(terrain, index) > 10);

    const Instance victim = vegetation.Instances(index).front();
    const uint64_t key = InstanceKey(victim.position.x, victim.position.z);
    vegetation.BeginStroke();
    CHECK(vegetation.Erase(index, victim.position.x, victim.position.z, 0.5f) == 1);
    vegetation.EndStroke();
    CHECK(vegetation.RemovedCount(index) == 1);

    // Aturan yang tidak menyentuh posisi diubah — skala, kepadatan, penyelarasan.
    vegetation.Layer(index).maxScale = 3.0f;
    vegetation.Layer(index).alignToNormal = 1.0f;
    vegetation.Scatter(terrain, index);
    CHECK(KeysOf(vegetation.Instances(index)).count(key) == 0);

    // Dan tetangganya tetap ada: menghapus satu tidak boleh menumbuhkan yang
    // lain maupun menghapus yang lain.
    CHECK(vegetation.Instances(index).size() > 10);
}

TEST_CASE("Instance yang dihapus tetap memegang tempatnya dalam pemadatan") {
    terrain::Terrain terrain = MakeTerrain();
    Vegetation vegetation;
    vegetation.Fit(terrain);
    const int index = vegetation.AddLayer(MakeLayer(5.0f, 555));
    const std::size_t before = vegetation.Scatter(terrain, index);
    const std::unordered_set<uint64_t> keysBefore = KeysOf(vegetation.Instances(index));

    const Instance victim = vegetation.Instances(index)[before / 2];
    vegetation.Erase(index, victim.position.x, victim.position.z, 0.5f);
    const std::size_t after = vegetation.Scatter(terrain, index);

    REQUIRE(after == before - 1);
    // Tidak ada satu pun instance baru: sebaran yang tersisa persis himpunan
    // semula dikurangi satu.
    for (const Instance& instance : vegetation.Instances(index)) {
        REQUIRE(keysBefore.count(InstanceKey(instance.position.x, instance.position.z)) == 1);
    }
}

TEST_CASE("Menanam tangan bertahan melewati sebaran ulang") {
    terrain::Terrain terrain = MakeTerrain();
    Vegetation vegetation;
    vegetation.Fit(terrain);
    const int index = vegetation.AddLayer(MakeLayer(4.0f));
    vegetation.Scatter(terrain, index);

    ApplyPlantDab(vegetation, terrain, index, 33.5f, 77.25f);
    CHECK(vegetation.AddedCount(index) == 1);
    const uint64_t key = InstanceKey(33.5f, 77.25f);

    vegetation.Layer(index).rules.density = 0.2f;
    vegetation.Scatter(terrain, index);
    CHECK(vegetation.AddedCount(index) == 1);
    CHECK(KeysOf(vegetation.Instances(index)).count(key) == 1);
}

TEST_CASE("Yang ditanam tangan tidak tunduk pada aturan penempatan") {
    terrain::Terrain terrain = MakeTerrain();
    SculptCone(terrain, 100.0f);

    Vegetation vegetation;
    vegetation.Fit(terrain);
    VegetationLayer layer = MakeLayer(4.0f);
    layer.rules.maxHeight = 20.0f;
    const int index = vegetation.AddLayer(layer);
    vegetation.Scatter(terrain, index);

    // Tepat di puncak, jauh di atas batas aturannya.
    const float peakX = terrain.WorldWidth() * 0.5f;
    const float peakZ = terrain.WorldDepth() * 0.5f;
    ApplyPlantDab(vegetation, terrain, index, peakX, peakZ);
    vegetation.Scatter(terrain, index);
    CHECK(KeysOf(vegetation.Instances(index)).count(InstanceKey(peakX, peakZ)) == 1);
}

TEST_CASE("Goresan penghapus bisa dibatalkan dan diulang") {
    terrain::Terrain terrain = MakeTerrain();
    Vegetation vegetation;
    vegetation.Fit(terrain);
    const int index = vegetation.AddLayer(MakeLayer(3.0f));
    const std::size_t before = vegetation.Scatter(terrain, index);
    const uint64_t hashBefore = HashInstances(vegetation.Instances(index));

    vegetation.BeginStroke();
    const std::size_t erased = ApplyEraseDab(vegetation, index, terrain.WorldWidth() * 0.5f,
                                             terrain.WorldDepth() * 0.5f, 25.0f);
    vegetation.EndStroke();
    REQUIRE(erased > 5);
    REQUIRE(vegetation.Instances(index).size() == before - erased);

    REQUIRE(vegetation.Undo());
    CHECK(vegetation.Instances(index).size() == before);
    CHECK(vegetation.RemovedCount(index) == 0);
    // Urutan daftar tidak dijanjikan setelah undo, tapi isinya harus sama —
    // dan menyebar ulang mengembalikan urutannya juga.
    vegetation.Scatter(terrain, index);
    CHECK(HashInstances(vegetation.Instances(index)) == hashBefore);

    REQUIRE(vegetation.Redo());
    CHECK(vegetation.Instances(index).size() == before - erased);
    CHECK(vegetation.RemovedCount(index) == erased);
}

TEST_CASE("Membatalkan penanaman mengeluarkannya dari daftar suntingan") {
    terrain::Terrain terrain = MakeTerrain();
    Vegetation vegetation;
    vegetation.Fit(terrain);
    const int index = vegetation.AddLayer(MakeLayer(4.0f));
    vegetation.Scatter(terrain, index);
    const std::size_t before = vegetation.Instances(index).size();

    vegetation.BeginStroke();
    ApplyPlantDab(vegetation, terrain, index, 12.0f, 34.0f);
    ApplyPlantDab(vegetation, terrain, index, 56.0f, 78.0f);
    vegetation.EndStroke();
    REQUIRE(vegetation.AddedCount(index) == 2);
    REQUIRE(vegetation.Instances(index).size() == before + 2);

    REQUIRE(vegetation.Undo());
    CHECK(vegetation.AddedCount(index) == 0);
    CHECK(vegetation.Instances(index).size() == before);
    REQUIRE(vegetation.Redo());
    CHECK(vegetation.AddedCount(index) == 2);
    CHECK(vegetation.Instances(index).size() == before + 2);
}

TEST_CASE("Menghapus instance yang ditanam tangan mengembalikannya utuh") {
    terrain::Terrain terrain = MakeTerrain();
    Vegetation vegetation;
    vegetation.Fit(terrain);
    const int index = vegetation.AddLayer(MakeLayer(4.0f));

    ApplyPlantDab(vegetation, terrain, index, 20.0f, 20.0f);
    REQUIRE(vegetation.AddedCount(index) == 1);

    vegetation.BeginStroke();
    CHECK(vegetation.Erase(index, 20.0f, 20.0f, 1.0f) == 1);
    vegetation.EndStroke();
    CHECK(vegetation.AddedCount(index) == 0);
    // Ia manual, jadi menghapusnya tidak menambah kunci ke daftar hapus — tidak
    // ada instance sebaran yang harus ditahan.
    CHECK(vegetation.RemovedCount(index) == 0);

    REQUIRE(vegetation.Undo());
    CHECK(vegetation.AddedCount(index) == 1);
    CHECK(vegetation.Instances(index).size() == 1);
}

// --- terrain berubah di bawah vegetasi ----------------------------------------

TEST_CASE("Memahat di bawah vegetasi menyesuaikan tingginya, bukan tempatnya") {
    terrain::Terrain terrain = MakeTerrain();
    Vegetation vegetation;
    vegetation.Fit(terrain);
    VegetationLayer layer = MakeLayer(4.0f);
    layer.offsetY = 0.0f;
    const int index = vegetation.AddLayer(layer);
    REQUIRE(vegetation.Scatter(terrain, index) > 0);

    const std::vector<Instance> before = vegetation.Instances(index);
    const std::unordered_set<uint64_t> keysBefore = KeysOf(before);

    terrain::Brush brush;
    brush.kind = terrain::BrushKind::Raise;
    brush.radius = 30.0f;
    brush.strength = 40.0f;
    const float centreX = terrain.WorldWidth() * 0.5f;
    const float centreZ = terrain.WorldDepth() * 0.5f;
    terrain.BeginStroke();
    terrain::ApplyDab(terrain, brush, centreX, centreZ, 0.5f);
    terrain.EndStroke();

    const std::size_t touched =
        vegetation.RefreshHeights(terrain, centreX - 30.0f, centreZ - 30.0f, centreX + 30.0f,
                                  centreZ + 30.0f);
    REQUIRE(touched > 0);

    // XZ setiap instance tidak bergerak sedikit pun, dan yang ada di bawah kuas
    // ikut naik.
    CHECK(KeysOf(vegetation.Instances(index)) == keysBefore);
    std::size_t raised = 0;
    for (const Instance& instance : vegetation.Instances(index)) {
        const float dx = instance.position.x - centreX;
        const float dz = instance.position.z - centreZ;
        if (std::sqrt(dx * dx + dz * dz) < 10.0f) {
            CHECK(instance.position.y ==
                  doctest::Approx(terrain.HeightAtWorld(instance.position.x, instance.position.z))
                      .epsilon(0.0001));
            ++raised;
        }
    }
    CHECK(raised > 0);
}

TEST_CASE("Menempelkan ulang di luar persegi tidak menyentuh apa pun") {
    terrain::Terrain terrain = MakeTerrain();
    Vegetation vegetation;
    vegetation.Fit(terrain);
    const int index = vegetation.AddLayer(MakeLayer(4.0f));
    vegetation.Scatter(terrain, index);
    const uint64_t hash = HashInstances(vegetation.Instances(index));

    CHECK(vegetation.RefreshHeights(terrain, -100.0f, -100.0f, -50.0f, -50.0f) == 0);
    CHECK(HashInstances(vegetation.Instances(index)) == hash);
}

// --- layer --------------------------------------------------------------------

TEST_CASE("Batas jumlah layer ditegakkan") {
    Vegetation vegetation;
    for (int i = 0; i < kMaxLayers; ++i) {
        REQUIRE(vegetation.AddLayer(MakeLayer()) == i);
    }
    CHECK(vegetation.AddLayer(MakeLayer()) == -1);
    CHECK(vegetation.LayerCount() == kMaxLayers);
}

TEST_CASE("Memindahkan layer membawa instance dan suntingannya") {
    terrain::Terrain terrain = MakeTerrain();
    Vegetation vegetation;
    vegetation.Fit(terrain);
    const int first = vegetation.AddLayer(MakeLayer(4.0f, 1));
    const int second = vegetation.AddLayer(MakeLayer(9.0f, 2));
    vegetation.ScatterAll(terrain);
    const uint64_t hash = HashInstances(vegetation.Instances(second));
    vegetation.Erase(second, vegetation.Instances(second).front().position.x,
                     vegetation.Instances(second).front().position.z, 0.5f);
    const std::size_t removed = vegetation.RemovedCount(second);
    REQUIRE(removed == 1);
    REQUIRE(first == 0);

    const std::size_t survivors = vegetation.Instances(second).size();
    REQUIRE(vegetation.MoveLayer(second, 0));
    CHECK(vegetation.Layer(0).rules.seed == 2);
    CHECK(vegetation.RemovedCount(0) == removed);
    CHECK(vegetation.Instances(0).size() == survivors);
    // Bukan hash semula: satu instance sudah dihapus sebelum layernya pindah,
    // dan yang harus ikut pindah adalah keadaan sesudah penghapusan itu.
    CHECK(HashInstances(vegetation.Instances(0)) != hash);
}

// --- berkas -------------------------------------------------------------------

TEST_CASE("Aturan, suntingan, dan peta kepadatan bolak-balik lewat berkas") {
    TempDir dir("roundtrip");
    terrain::Terrain terrain = MakeTerrain();
    SculptCone(terrain, 40.0f);

    Vegetation vegetation;
    vegetation.Fit(terrain);
    VegetationLayer layer = MakeLayer(4.0f, 8888);
    layer.name = "Oaks";
    layer.minScale = 0.6f;
    layer.maxScale = 2.4f;
    layer.alignToNormal = 0.75f;
    layer.lodDistance = 42.0f;
    layer.rules.minHeight = 12.0f;
    layer.rules.maxSlopeDegrees = 35.0f;
    const int index = vegetation.AddLayer(layer);
    vegetation.Scatter(terrain, index);

    vegetation.BeginStroke();
    for (int step = 0; step < 10; ++step) {
        ApplyDensityDab(vegetation, DensityBrush(16.0f), index, 40.0f, 40.0f, 1.0f / 60.0f);
    }
    vegetation.EndStroke();
    vegetation.Scatter(terrain, index);
    vegetation.Erase(index, vegetation.Instances(index).front().position.x,
                     vegetation.Instances(index).front().position.z, 0.5f);
    ApplyPlantDab(vegetation, terrain, index, 55.5f, 66.25f);
    vegetation.Scatter(terrain, index);

    VegetationDocument document;
    document.name = "Forest";
    const std::filesystem::path path = dir / "Forest.simveg";
    REQUIRE(SaveVegetation(vegetation, document, path).ok);

    Vegetation loaded;
    VegetationDocument loadedDocument;
    const VegetationIoResult result = LoadVegetation(loaded, loadedDocument, path);
    REQUIRE(result.ok);
    loaded.Fit(terrain);
    loaded.ScatterAll(terrain);

    CHECK(loadedDocument.name == "Forest");
    REQUIRE(loaded.LayerCount() == 1);
    CHECK(loaded.Layer(0).name == "Oaks");
    CHECK(loaded.Layer(0).minScale == doctest::Approx(0.6f));
    CHECK(loaded.Layer(0).alignToNormal == doctest::Approx(0.75f));
    CHECK(loaded.Layer(0).lodDistance == doctest::Approx(42.0f));
    CHECK(loaded.Layer(0).rules.minHeight == doctest::Approx(12.0f));
    CHECK(loaded.Layer(0).rules.maxSlopeDegrees == doctest::Approx(35.0f));
    CHECK(loaded.Layer(0).rules.seed == 8888);
    CHECK(loaded.AddedCount(0) == vegetation.AddedCount(0));
    CHECK(loaded.RemovedCount(0) == vegetation.RemovedCount(0));
    CHECK(loaded.Density(0).Cells() == vegetation.Density(0).Cells());
    CHECK(HashInstances(loaded.Instances(0)) == HashInstances(vegetation.Instances(0)));
}

TEST_CASE("Layer tanpa cat tidak menulis berkas kepadatan") {
    TempDir dir("nodensity");
    Vegetation vegetation;
    vegetation.AddLayer(MakeLayer());

    VegetationDocument document;
    document.name = "Bare";
    const std::filesystem::path path = dir / "Bare.simveg";
    REQUIRE(SaveVegetation(vegetation, document, path).ok);
    CHECK(!std::filesystem::exists(dir / "Bare_d0.png"));

    Vegetation loaded;
    VegetationDocument loadedDocument;
    REQUIRE(LoadVegetation(loaded, loadedDocument, path).ok);
    CHECK(loaded.Layer(0).densityFile.empty());
    CHECK(!loaded.Density(0).Painted());
}

TEST_CASE("Peta kepadatan berukuran salah ditolak dan menyebutkan kedua ukurannya") {
    TempDir dir("mismatch");
    terrain::Terrain terrain = MakeTerrain();
    Vegetation vegetation;
    vegetation.Fit(terrain);
    const int index = vegetation.AddLayer(MakeLayer());
    vegetation.PaintDensity(index, 1, 1, 0);
    REQUIRE(SaveDensityPng(vegetation, index, dir / "mask.png").ok);

    Vegetation other;
    other.SetDensityGrid(8, 8);
    other.AddLayer(MakeLayer());
    const VegetationIoResult result = LoadDensityPng(other, 0, dir / "mask.png");
    CHECK(!result.ok);
    CHECK(result.error.find("8x8") != std::string::npos);
    CHECK(result.error.find(std::to_string(vegetation.DensityWidth())) != std::string::npos);
}

TEST_CASE("Mask dari luar editor dicuplik ulang ke kisi kepadatan") {
    TempDir dir("import");
    // Mask 64x64 dengan separuh kiri hitam dan separuh kanan putih, ditulis
    // lewat enkoder yang sama yang dipakai peta bobot terrain.
    constexpr int kSide = 64;
    std::vector<uint8_t> mask(static_cast<std::size_t>(kSide) * kSide, 0);
    for (int y = 0; y < kSide; ++y) {
        for (int x = kSide / 2; x < kSide; ++x) {
            mask[static_cast<std::size_t>(y) * kSide + static_cast<std::size_t>(x)] = 255;
        }
    }
    const std::vector<unsigned char> png = terrain::EncodeMaskPng(mask.data(), kSide, kSide);
    REQUIRE(!png.empty());
    {
        std::ofstream stream(dir / "mask.png", std::ios::binary);
        stream.write(reinterpret_cast<const char*>(png.data()),
                     static_cast<std::streamsize>(png.size()));
    }

    terrain::Terrain terrain = MakeTerrain();
    Vegetation vegetation;
    vegetation.Fit(terrain);
    const int index = vegetation.AddLayer(MakeLayer(3.0f));
    REQUIRE(vegetation.DensityWidth() != kSide);  // kalau sama, tidak ada yang diuji

    const VegetationIoResult result = ImportDensityPng(vegetation, index, dir / "mask.png");
    REQUIRE(result.ok);
    CHECK(result.sourceWidth == kSide);
    CHECK(result.sourceHeight == kSide);
    REQUIRE(vegetation.Density(index).Painted());
    CHECK(vegetation.Density(index).Width() == vegetation.DensityWidth());

    // Tepi gambar berimpit dengan tepi dunia, bukan bergeser setengah sel.
    CHECK(vegetation.Density(index).At(0, 0) == 0);
    CHECK(vegetation.Density(index).At(vegetation.DensityWidth() - 1, 0) == 255);

    // Dan mask itu benar-benar dipakai sebaran: kiri kosong, kanan tumbuh.
    REQUIRE(vegetation.Scatter(terrain, index) > 0);
    const float middle = terrain.WorldWidth() * 0.5f;
    std::size_t left = 0;
    for (const Instance& instance : vegetation.Instances(index)) {
        if (instance.position.x < middle - terrain.WorldWidth() * 0.05f) {
            ++left;
        }
    }
    CHECK(left == 0);
}

TEST_CASE("Berkas pendamping berukuran salah tetap ditolak, bukan dicuplik ulang") {
    // Jalur dokumen dan jalur impor sengaja berbeda: yang satu membaca berkas
    // yang ditulis editor sendiri, yang lain menerima gambar dari mana saja.
    TempDir dir("companion");
    terrain::Terrain terrain = MakeTerrain();
    Vegetation vegetation;
    vegetation.Fit(terrain);
    const int index = vegetation.AddLayer(MakeLayer());
    vegetation.PaintDensity(index, 1, 1, 0);
    REQUIRE(SaveDensityPng(vegetation, index, dir / "mask.png").ok);

    Vegetation other;
    other.SetDensityGrid(8, 8);
    other.AddLayer(MakeLayer());
    CHECK(!LoadDensityPng(other, 0, dir / "mask.png").ok);
    CHECK(ImportDensityPng(other, 0, dir / "mask.png").ok);
    CHECK(other.Density(0).Width() == 8);
}

// --- kriteria terima ----------------------------------------------------------

TEST_CASE("Sejuta instance disebar di bawah sepuluh detik dan tersimpan di bawah 5 MB") {
    TempDir dir("million");
    // Terrain 8 km persegi, tanpa satu pun ubin terwujud: yang diuji di sini
    // ongkos sebarannya, bukan ongkos heightmap-nya.
    terrain::Terrain terrain = MakeTerrain(1024, 4, 2.0f);

    Vegetation vegetation;
    vegetation.Fit(terrain);
    const int index = vegetation.AddLayer(MakeLayer(6.0f, 2024));

    const auto start = std::chrono::steady_clock::now();
    const std::size_t count = vegetation.Scatter(terrain, index);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();

    INFO("instances=" << count << " elapsed=" << elapsed << "ms");
    CHECK(count >= 1000000u);
    CHECK(elapsed < 10000);

    VegetationDocument document;
    document.name = "Million";
    const std::filesystem::path path = dir / "Million.simveg";
    REQUIRE(SaveVegetation(vegetation, document, path).ok);
    const std::uintmax_t bytes = std::filesystem::file_size(path);
    INFO("file=" << bytes << " bytes");
    CHECK(bytes < 5u * 1024u * 1024u);
}

TEST_CASE("Sebaran dikunci pada nilai hash yang tetap") {
    // Nilai ini dihitung dari algoritma yang ada sekarang. Ia tidak bisa
    // membuktikan bahwa mesin lain menghasilkan angka yang sama — tidak ada test
    // satu mesin yang bisa — tapi ia menutup satu-satunya cara sifat itu hilang
    // dalam praktik: seseorang menukar aliran acaknya, urutan undiannya, atau
    // urutan pemindaiannya dengan yang "setara". Kalau nilai ini berubah, sebaran
    // yang tersimpan di proyek siapa pun ikut berubah.
    terrain::Terrain terrain = MakeTerrain(128, 2, 1.0f);
    SculptCone(terrain, 55.0f);

    Vegetation vegetation;
    vegetation.Fit(terrain);
    VegetationLayer layer = MakeLayer(3.0f, 20260807);
    layer.rules.maxSlopeDegrees = 38.0f;
    layer.rules.minHeight = 15.0f;
    layer.alignToNormal = 0.5f;
    const int index = vegetation.AddLayer(layer);

    const std::size_t count = vegetation.Scatter(terrain, index);
    const uint64_t hash = HashInstances(vegetation.Instances(index));
    INFO("count=" << count << " hash=" << hash);
    CHECK(count == 1788u);
    CHECK(hash == 17396189034241626862ull);
}
