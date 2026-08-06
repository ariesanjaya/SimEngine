#include "Sim/Vegetation/Vegetation.h"

#include <algorithm>
#include <cmath>

namespace sim::vegetation {
namespace {

/// Sudut jadi kosinus, dikuantisasi ke 1/65536.
///
/// **Aturan kemiringan diuji lewat kosinus normal, bukan lewat tangen
/// lerengnya.** Bukan sekadar menghemat satu pembagian: `ny` sebuah normal
/// ternormalisasi selalu di antara 0 dan 1, jadi ambangnya berada di rentang
/// yang terkondisi baik, sedangkan tangen meledak menuju tak hingga di dekat
/// tegak lurus dan kehilangan seluruh presisinya persis di sana.
///
/// Kuantisasinya yang membuat ambang ini bit-identik di mesin lain. `std::cos`
/// tidak dijamin sama sampai bit terakhir di antara pustaka matematika, dan
/// walaupun selisih satu ULP hanya membalik kandidat yang tepat berada di
/// ambang, "tepat di ambang" bukan himpunan kosong pada sebaran satu juta titik.
/// Dihitung dalam double lalu dibulatkan ke bawah pada kisi 1/65536, selisih
/// sebesar itu hilang jauh sebelum sampai ke perbandingan.
///
/// Ini satu-satunya libm di jalur penerimaan kandidat, dan ia dipanggil sekali
/// per sebaran, bukan sekali per kandidat.
float SlopeCosine(float degrees) {
    const double radians = static_cast<double>(std::clamp(degrees, 0.0f, 90.0f)) *
                           (3.14159265358979323846 / 180.0);
    return static_cast<float>(std::floor(std::cos(radians) * 65536.0) / 65536.0);
}

bool RemoveByPlacement(std::vector<Instance>& list, const Instance& instance) {
    const auto it = std::find_if(list.begin(), list.end(), [&](const Instance& candidate) {
        return candidate.position == instance.position && candidate.yaw == instance.yaw &&
               candidate.scale == instance.scale;
    });
    if (it == list.end()) {
        return false;
    }
    list.erase(it);
    return true;
}

/// Membuang setiap instance yang posisinya ada di dalam `keys`, dalam satu
/// lintasan.
///
/// Satu lintasan, bukan satu pencarian per instance: sebuah goresan penghapus
/// bisa mengangkat ribuan instance sekaligus, dan mengembalikan lalu
/// menghapusnya lagi satu per satu pada daftar berisi sejuta adalah miliaran
/// perbandingan untuk satu Ctrl+Y.
void RemoveKeyed(std::vector<Instance>& list, const std::unordered_set<uint64_t>& keys) {
    if (keys.empty()) {
        return;
    }
    const auto tail =
        std::remove_if(list.begin(), list.end(), [&](const Instance& instance) {
            return keys.count(InstanceKey(instance.position.x, instance.position.z)) != 0;
        });
    list.erase(tail, list.end());
}

uint64_t CaptureKey(int layer, int32_t cell) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(layer)) << 32) |
           static_cast<uint32_t>(cell);
}

}  // namespace

// --- acak ---------------------------------------------------------------------

uint64_t Rng::NextU64() {
    // splitmix64. Dipilih karena seluruhnya perkalian, geser, dan XOR bilangan
    // bulat 64-bit — tidak ada satu pun operasi yang boleh berbeda antar
    // kompilator, dan keadaannya cukup satu kata sehingga membuat aliran baru
    // per sel kisi tidak berongkos.
    state_ += 0x9E3779B97F4A7C15ull;
    uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

uint64_t CellSeed(uint32_t seed, int32_t cellX, int32_t cellY) {
    // Koordinat sel dicampur lewat splitmix64 juga, bukan sekadar dijumlahkan.
    // Sel bertetangga hanya berbeda satu pada masukannya, dan PRNG mana pun yang
    // diberi benih berdekatan menghasilkan keluaran awal yang berkorelasi —
    // yang terlihat sebagai baris dan kolom pohon yang sejajar.
    uint64_t mixed = static_cast<uint64_t>(seed) * 0xD6E8FEB86659FD93ull;
    mixed ^= static_cast<uint64_t>(static_cast<uint32_t>(cellX)) * 0x9E3779B97F4A7C15ull;
    mixed ^= static_cast<uint64_t>(static_cast<uint32_t>(cellY)) * 0xC2B2AE3D27D4EB4Full;
    Rng rng(mixed);
    return rng.NextU64();
}

uint64_t PackKey(int32_t x, int32_t z) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(z);
}

void UnpackKey(uint64_t key, int32_t& x, int32_t& z) {
    x = static_cast<int32_t>(static_cast<uint32_t>(key >> 32));
    z = static_cast<int32_t>(static_cast<uint32_t>(key));
}

uint64_t InstanceKey(float worldX, float worldZ) {
    return PackKey(static_cast<int32_t>(std::lround(worldX * 1000.0f)),
                   static_cast<int32_t>(std::lround(worldZ * 1000.0f)));
}

// --- instance -----------------------------------------------------------------

Instance MakeInstance(const VegetationLayer& layer, float worldX, float worldZ, float height,
                      const Vec3& normal, float scaleRoll, float yawRoll) {
    const float alignment = std::clamp(layer.alignToNormal, 0.0f, 1.0f);
    const float minScale = std::min(layer.minScale, layer.maxScale);
    const float maxScale = std::max(layer.minScale, layer.maxScale);

    Instance instance;
    instance.position = Vec3(worldX, height + layer.offsetY, worldZ);
    instance.up =
        glm::normalize(Vec3(0.0f, 1.0f, 0.0f) * (1.0f - alignment) + normal * alignment);
    instance.yaw = layer.randomYaw ? yawRoll * kTwoPi : 0.0f;
    instance.scale = minScale + (maxScale - minScale) * scaleRoll;
    return instance;
}

// --- peta kepadatan -----------------------------------------------------------

void DensityMap::Reset(int width, int height, float cellSize) {
    const bool same = width == width_ && height == height_ && cellSize == cellSize_;
    width_ = std::max(width, 0);
    height_ = std::max(height, 0);
    cellSize_ = cellSize > 0.0f ? cellSize : 1.0f;
    if (!same) {
        cells_.clear();
    }
}

uint8_t DensityMap::At(int x, int y) const {
    if (cells_.empty()) {
        return 255;
    }
    const int cx = std::clamp(x, 0, width_ - 1);
    const int cy = std::clamp(y, 0, height_ - 1);
    return cells_[static_cast<std::size_t>(cy) * static_cast<std::size_t>(width_) +
                  static_cast<std::size_t>(cx)];
}

void DensityMap::SetAt(int x, int y, uint8_t value) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_ || width_ <= 0 || height_ <= 0) {
        return;
    }
    if (cells_.empty()) {
        if (value == 255) {
            return;  // menulis nilai bawaan di atas peta kosong tidak mengalokasikan
        }
        cells_.assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), 255);
    }
    cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
           static_cast<std::size_t>(x)] = value;
}

float DensityMap::SampleWorld(float worldX, float worldZ) const {
    if (cells_.empty()) {
        return 1.0f;
    }
    const float fx = worldX / cellSize_;
    const float fy = worldZ / cellSize_;
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);

    const float c00 = static_cast<float>(At(x0, y0));
    const float c10 = static_cast<float>(At(x0 + 1, y0));
    const float c01 = static_cast<float>(At(x0, y0 + 1));
    const float c11 = static_cast<float>(At(x0 + 1, y0 + 1));
    const float mixed = (c00 * (1.0f - tx) + c10 * tx) * (1.0f - ty) +
                        (c01 * (1.0f - tx) + c11 * tx) * ty;
    return mixed * (1.0f / 255.0f);
}

void DensityMap::SetCells(const uint8_t* values) {
    if (values == nullptr || width_ <= 0 || height_ <= 0) {
        return;
    }
    cells_.assign(values, values + static_cast<std::size_t>(width_) *
                                       static_cast<std::size_t>(height_));
}

void DensityMap::SwapCells(std::vector<uint8_t>& cells) {
    const std::size_t expected =
        static_cast<std::size_t>(std::max(width_, 0)) * static_cast<std::size_t>(std::max(height_, 0));
    if (!cells.empty() && cells.size() != expected) {
        return;
    }
    cells_.swap(cells);
}

// --- layer --------------------------------------------------------------------

const VegetationLayer& Vegetation::Layer(int index) const {
    static const VegetationLayer kFallback;
    if (index < 0 || index >= LayerCount()) {
        return kFallback;
    }
    return layers_[static_cast<std::size_t>(index)].desc;
}

VegetationLayer& Vegetation::Layer(int index) {
    static VegetationLayer fallback;
    if (index < 0 || index >= LayerCount()) {
        fallback = VegetationLayer{};
        return fallback;
    }
    return layers_[static_cast<std::size_t>(index)].desc;
}

int Vegetation::AddLayer(const VegetationLayer& layer) {
    if (LayerCount() >= kMaxLayers) {
        return -1;
    }
    LayerData data;
    data.desc = layer;
    data.density.Reset(densityWidth_, densityHeight_, densityCell_);
    layers_.push_back(std::move(data));
    // Riwayat sengaja dibiarkan: layer baru selalu masuk di belakang, jadi tidak
    // ada indeks yang bergeser dan tidak ada catatan lama yang berubah arti.
    return LayerCount() - 1;
}

bool Vegetation::RemoveLayer(int index) {
    if (index < 0 || index >= LayerCount()) {
        return false;
    }
    layers_.erase(layers_.begin() + index);
    // Riwayat menyebut layer menurut indeksnya, dan indeks bergeser saat sebuah
    // layer hilang. Membiarkan riwayatnya berarti Ctrl+Z yang menanam kembali
    // pohon ke layer tetangganya.
    ClearHistory();
    return true;
}

bool Vegetation::MoveLayer(int from, int to) {
    if (from < 0 || from >= LayerCount() || to < 0 || to >= LayerCount() || from == to) {
        return false;
    }
    LayerData moved = std::move(layers_[static_cast<std::size_t>(from)]);
    layers_.erase(layers_.begin() + from);
    layers_.insert(layers_.begin() + to, std::move(moved));
    ClearHistory();
    return true;
}

void Vegetation::SetLayers(const std::vector<VegetationLayer>& layers) {
    layers_.clear();
    for (const VegetationLayer& layer : layers) {
        if (LayerCount() >= kMaxLayers) {
            break;
        }
        LayerData data;
        data.desc = layer;
        data.density.Reset(densityWidth_, densityHeight_, densityCell_);
        layers_.push_back(std::move(data));
    }
    ClearHistory();
}

// --- kisi kepadatan -----------------------------------------------------------

void Vegetation::SetDensityCellSize(float meters) {
    const float clamped = std::clamp(meters, 0.25f, 64.0f);
    if (clamped == densityCell_) {
        return;
    }
    densityCell_ = clamped;
    for (LayerData& data : layers_) {
        data.density.Clear();
        data.density.Reset(densityWidth_, densityHeight_, densityCell_);
    }
    ClearHistory();
}

void Vegetation::Fit(const Terrain& terrain) {
    // Satu sel lebih lebar daripada pembagian bulatnya, supaya tepi kanan dan
    // tepi bawah dunia masih punya simpul untuk dicuplik bilinear.
    const int width =
        std::max(2, static_cast<int>(std::ceil(terrain.WorldWidth() / densityCell_)) + 1);
    const int height =
        std::max(2, static_cast<int>(std::ceil(terrain.WorldDepth() / densityCell_)) + 1);
    if (width == densityWidth_ && height == densityHeight_) {
        for (LayerData& data : layers_) {
            data.density.Reset(width, height, densityCell_);
        }
        return;
    }
    densityWidth_ = width;
    densityHeight_ = height;
    for (LayerData& data : layers_) {
        // Peta yang ukurannya tidak cocok dibuang, bukan diskalakan. Menskala
        // ulang peta kepadatan diam-diam memindahkan tepi hutan tanpa ada yang
        // memindahkannya — dan hasilnya terlihat masuk akal, jadi tidak ada yang
        // menyadarinya.
        data.density.Clear();
        data.density.Reset(width, height, densityCell_);
    }
    ClearHistory();
}

void Vegetation::SetDensityGrid(int width, int height) {
    densityWidth_ = std::max(width, 0);
    densityHeight_ = std::max(height, 0);
    for (LayerData& data : layers_) {
        data.density.Reset(densityWidth_, densityHeight_, densityCell_);
    }
    ClearHistory();
}

DensityMap& Vegetation::Density(int layer) {
    static DensityMap fallback;
    if (layer < 0 || layer >= LayerCount()) {
        fallback = DensityMap{};
        return fallback;
    }
    return layers_[static_cast<std::size_t>(layer)].density;
}

const DensityMap& Vegetation::Density(int layer) const {
    static const DensityMap kFallback;
    if (layer < 0 || layer >= LayerCount()) {
        return kFallback;
    }
    return layers_[static_cast<std::size_t>(layer)].density;
}

void Vegetation::PaintDensity(int layer, int cellX, int cellY, uint8_t value) {
    if (layer < 0 || layer >= LayerCount()) {
        return;
    }
    DensityMap& map = layers_[static_cast<std::size_t>(layer)].density;
    if (cellX < 0 || cellY < 0 || cellX >= map.Width() || cellY >= map.Height()) {
        return;
    }
    const auto cell = static_cast<int32_t>(cellY * map.Width() + cellX);
    const uint8_t before = map.At(cellX, cellY);
    if (before == value) {
        return;
    }
    CaptureDensityCell(layer, cell, before);
    map.SetAt(cellX, cellY, value);
}

void Vegetation::ClearDensity(int layer) {
    if (layer < 0 || layer >= LayerCount()) {
        return;
    }
    DensityMap& map = layers_[static_cast<std::size_t>(layer)].density;
    if (!map.Painted()) {
        return;
    }
    // Peta lama diambil utuh ke dalam jurnal, lalu yang hidup dikosongkan —
    // kosong dan "seluruhnya 255" memang sama artinya, dan yang kosong tidak
    // menghuni memori maupun menulis berkas.
    DensityImage image;
    image.layer = layer;
    map.SwapCells(image.cells);
    if (inStroke_) {
        current_.bytes += image.cells.size();
        current_.images.push_back(std::move(image));
    }
}

// --- sebaran ------------------------------------------------------------------

std::size_t Vegetation::Scatter(const Terrain& terrain, int layerIndex) {
    if (layerIndex < 0 || layerIndex >= LayerCount()) {
        return 0;
    }
    LayerData& data = layers_[static_cast<std::size_t>(layerIndex)];
    const VegetationLayer& desc = data.desc;
    const PlacementRules& rules = desc.rules;
    std::vector<Instance>& out = data.instances;
    out.clear();

    const float spacing = std::max(rules.minDistance, kMinInstanceDistance);
    const float worldWidth = terrain.WorldWidth();
    const float worldDepth = terrain.WorldDepth();
    const int cols = std::max(1, static_cast<int>(std::ceil(worldWidth / spacing)));
    const int rows = std::max(1, static_cast<int>(std::ceil(worldDepth / spacing)));
    const float minDistanceSq = spacing * spacing;

    const float density = std::clamp(rules.density, 0.0f, 1.0f);
    const float cosMin = SlopeCosine(rules.minSlopeDegrees);
    const float cosMax = SlopeCosine(rules.maxSlopeDegrees);
    const float sampleSpacing = terrain.Desc().sampleSpacing;
    const bool checkHoles = rules.avoidHoles && terrain.HoleCount() > 0;
    const bool checkWeight =
        rules.terrainLayer >= 0 && rules.terrainLayer < terrain.LayerCount();
    const auto minWeight = static_cast<int>(std::clamp(rules.minTerrainWeight, 0, 255));

    // **Jendela dua baris sel, bukan seluruh kisinya.**
    //
    // Uji jarak minimum hanya perlu melihat sel yang bersebelahan, dan sisi sel
    // sama dengan jarak minimumnya — jadi setiap titik yang bisa menolak sebuah
    // kandidat berada paling jauh satu sel darinya. Karena kisi dipindai baris
    // demi baris, baris di bawah kandidat masih kosong: yang harus diingat
    // hanyalah baris sekarang dan baris sebelumnya. Tanpa ini, terrain empat
    // kilometer dengan jarak minimum satu meter menuntut setengah gigabyte kisi
    // hanya untuk sebuah uji yang jangkauannya satu meter.
    std::vector<Vec2> window(static_cast<std::size_t>(cols) * 2u *
                             static_cast<std::size_t>(kInstancesPerCell));
    std::vector<uint8_t> counts(static_cast<std::size_t>(cols) * 2u, 0);

    // Perkiraan hasil, sekadar untuk memesan tempat: pemadatan Poisson mendekati
    // 0,7 instance per sel. Salah tebak hanya berongkos satu realokasi.
    const auto estimate = static_cast<std::size_t>(
        std::min(static_cast<double>(cols) * static_cast<double>(rows) *
                     static_cast<double>(density) * 0.7,
                 4.0e6));
    out.reserve(estimate);

    for (int cy = 0; cy < rows; ++cy) {
        const std::size_t row = static_cast<std::size_t>(cy & 1);
        const std::size_t prevRow = static_cast<std::size_t>((cy + 1) & 1);
        std::fill(counts.begin() + static_cast<std::ptrdiff_t>(row * static_cast<std::size_t>(cols)),
                  counts.begin() +
                      static_cast<std::ptrdiff_t>((row + 1) * static_cast<std::size_t>(cols)),
                  static_cast<uint8_t>(0));

        for (int cx = 0; cx < cols; ++cx) {
            Rng rng(CellSeed(rules.seed, cx, cy));
            for (int candidate = 0; candidate < kCandidatesPerCell; ++candidate) {
                // **Setiap kandidat menarik jumlah undian yang sama, apa pun
                // keputusan aturannya.** Menarik undian skala hanya setelah
                // kandidat diterima terdengar hemat, tapi itu membuat aliran
                // acaknya bergantung pada aturan — dan mengubah rentang tinggi
                // lalu akan memindahkan setiap pohon yang tetap lolos, bukan
                // hanya menghapus yang tidak. Dengan jumlah undian tetap,
                // mengubah aturan hanya memilih bagian mana dari sebaran yang
                // sama yang bertahan; itulah yang membuat penghapusan tangan
                // masih menunjuk instance yang sama setelahnya.
                const float u = rng.NextFloat();
                const float v = rng.NextFloat();
                const float gate = rng.NextFloat();
                const float scaleRoll = rng.NextFloat();
                const float yawRoll = rng.NextFloat();

                const float x = (static_cast<float>(cx) + u) * spacing;
                const float z = (static_cast<float>(cy) + v) * spacing;
                if (x > worldWidth || z > worldDepth) {
                    continue;
                }

                // **Pemadatannya diputuskan sebelum aturan mana pun dilihat.**
                //
                // Urutan sebaliknya terdengar lebih hemat — buat apa menguji
                // jarak pada kandidat yang toh akan ditolak aturan tingginya —
                // tapi ia menghancurkan sifat yang membuat seluruh rancangan ini
                // berguna. Kandidat yang ditolak aturan lalu tidak menempati
                // slot, jadi tetangga yang tadinya terhalang olehnya menjadi
                // diterima: mengetatkan satu aturan bukan menipiskan hutan
                // melainkan menyusunnya ulang, dan setiap penghapusan tangan
                // yang menunjuk posisi lama menunjuk ke tempat yang sudah tidak
                // ada isinya.
                //
                // Diputuskan lebih dulu, susunan titiknya hanya ditentukan
                // benih, jarak minimum, dan ukuran dunia. Setiap aturan — tinggi,
                // kemiringan, layer terrain, lubang, dan peta kepadatan yang
                // dicat — menjadi murni pengurang: ia memilih siapa dari susunan
                // yang sama itu yang berdiri. Kerapatan di dalam daerah yang
                // lolos tidak ikut berkurang, karena susunannya memang menutupi
                // seluruh dunia sejak awal.
                bool blocked = false;
                for (int ny = cy - 1; ny <= cy && !blocked; ++ny) {
                    if (ny < 0) {
                        continue;
                    }
                    const std::size_t slot = ny == cy ? row : prevRow;
                    for (int nx = cx - 1; nx <= cx + 1 && !blocked; ++nx) {
                        if (nx < 0 || nx >= cols) {
                            continue;
                        }
                        const std::size_t cell = slot * static_cast<std::size_t>(cols) +
                                                 static_cast<std::size_t>(nx);
                        const int count = counts[cell];
                        for (int i = 0; i < count; ++i) {
                            const Vec2& point =
                                window[cell * static_cast<std::size_t>(kInstancesPerCell) +
                                       static_cast<std::size_t>(i)];
                            const float dx = point.x - x;
                            const float dz = point.y - z;
                            if (dx * dx + dz * dz < minDistanceSq) {
                                blocked = true;
                                break;
                            }
                        }
                    }
                }
                if (blocked) {
                    continue;
                }

                const std::size_t cell =
                    row * static_cast<std::size_t>(cols) + static_cast<std::size_t>(cx);
                if (counts[cell] >= kInstancesPerCell) {
                    continue;  // tidak mungkin menurut geometri; dijaga supaya tetap begitu
                }
                window[cell * static_cast<std::size_t>(kInstancesPerCell) +
                       static_cast<std::size_t>(counts[cell])] = Vec2(x, z);
                ++counts[cell];

                // Mulai dari sini semuanya pengurang. Diurutkan menurut
                // ongkosnya: satu cuplikan peta kepadatan dulu, baru terrain.
                if (gate >= density * data.density.SampleWorld(x, z)) {
                    continue;
                }
                const float height = terrain.HeightAtWorld(x, z);
                if (height < rules.minHeight || height > rules.maxHeight) {
                    continue;
                }
                const Vec3 normal = terrain.NormalAtWorld(x, z);
                if (normal.y > cosMin || normal.y < cosMax) {
                    continue;
                }
                if (checkWeight) {
                    const int sx = std::clamp(
                        static_cast<int>(std::lround(x / sampleSpacing)), 0, terrain.SamplesX() - 1);
                    const int sy = std::clamp(
                        static_cast<int>(std::lround(z / sampleSpacing)), 0, terrain.SamplesY() - 1);
                    if (terrain.WeightAt(rules.terrainLayer, sx, sy) < minWeight) {
                        continue;
                    }
                }
                if (checkHoles) {
                    const int qx = static_cast<int>(std::floor(x / sampleSpacing));
                    const int qy = static_cast<int>(std::floor(z / sampleSpacing));
                    if (terrain.HoleAt(qx, qy)) {
                        continue;
                    }
                }
                if (!data.removedSet.empty() && data.removedSet.count(InstanceKey(x, z)) != 0) {
                    continue;
                }

                out.push_back(MakeInstance(desc, x, z, height, normal, scaleRoll, yawRoll));
            }
        }
    }

    // Yang ditanam tangan menyusul di belakang, apa adanya: aturan penempatan
    // menjawab "di mana boleh tumbuh sendiri", bukan "di mana boleh ditanam".
    out.insert(out.end(), data.added.begin(), data.added.end());
    return out.size();
}

std::size_t Vegetation::ScatterAll(const Terrain& terrain) {
    std::size_t total = 0;
    for (int layer = 0; layer < LayerCount(); ++layer) {
        total += Scatter(terrain, layer);
    }
    return total;
}

void Vegetation::ClearInstances(int layer) {
    if (layer < 0 || layer >= LayerCount()) {
        return;
    }
    layers_[static_cast<std::size_t>(layer)].instances.clear();
    layers_[static_cast<std::size_t>(layer)].instances.shrink_to_fit();
}

const std::vector<Instance>& Vegetation::Instances(int layer) const {
    static const std::vector<Instance> kEmpty;
    if (layer < 0 || layer >= LayerCount()) {
        return kEmpty;
    }
    return layers_[static_cast<std::size_t>(layer)].instances;
}

std::size_t Vegetation::InstanceCount() const {
    std::size_t total = 0;
    for (const LayerData& data : layers_) {
        total += data.instances.size();
    }
    return total;
}

std::size_t Vegetation::RefreshHeights(const Terrain& terrain, float minX, float minZ, float maxX,
                                       float maxZ) {
    std::size_t touched = 0;
    for (LayerData& data : layers_) {
        const float alignment = std::clamp(data.desc.alignToNormal, 0.0f, 1.0f);
        const float offset = data.desc.offsetY;
        for (Instance& instance : data.instances) {
            if (instance.position.x < minX || instance.position.x > maxX ||
                instance.position.z < minZ || instance.position.z > maxZ) {
                continue;
            }
            const Vec3 normal = terrain.NormalAtWorld(instance.position.x, instance.position.z);
            instance.position.y =
                terrain.HeightAtWorld(instance.position.x, instance.position.z) + offset;
            instance.up = glm::normalize(Vec3(0.0f, 1.0f, 0.0f) * (1.0f - alignment) +
                                         normal * alignment);
            ++touched;
        }
        // Yang ditanam tangan ikut menempel ulang, dengan perhitungan yang sama
        // persis. Kalau tidak, memahat di bawah sebatang pohon yang ditanam
        // sendiri akan meninggalkannya melayang — dan itu justru pohon yang
        // paling diperhatikan orang. Salinannya di `added` harus ikut bergerak
        // bersama salinannya di `instances`: keduanya dicocokkan lewat posisi
        // saat dihapus, dan dua salinan yang berbeda tinggi tidak lagi cocok.
        for (Instance& instance : data.added) {
            if (instance.position.x < minX || instance.position.x > maxX ||
                instance.position.z < minZ || instance.position.z > maxZ) {
                continue;
            }
            const Vec3 normal = terrain.NormalAtWorld(instance.position.x, instance.position.z);
            instance.position.y =
                terrain.HeightAtWorld(instance.position.x, instance.position.z) + offset;
            instance.up = glm::normalize(Vec3(0.0f, 1.0f, 0.0f) * (1.0f - alignment) +
                                         normal * alignment);
        }
    }
    return touched;
}

std::size_t Vegetation::RefreshHeights(const Terrain& terrain) {
    return RefreshHeights(terrain, 0.0f, 0.0f, terrain.WorldWidth(), terrain.WorldDepth());
}

// --- suntingan tangan ---------------------------------------------------------

void Vegetation::Plant(int layer, const Instance& instance) {
    if (layer < 0 || layer >= LayerCount()) {
        return;
    }
    LayerData& data = layers_[static_cast<std::size_t>(layer)];
    Instance planted = instance;
    planted.manual = true;
    data.added.push_back(planted);
    data.instances.push_back(planted);
    if (inStroke_) {
        current_.planted.push_back(PlantedInstance{layer, planted});
        current_.bytes += sizeof(PlantedInstance);
    }
}

std::size_t Vegetation::Erase(int layer, float worldX, float worldZ, float radius) {
    if (layer < 0 || layer >= LayerCount() || radius <= 0.0f) {
        return 0;
    }
    LayerData& data = layers_[static_cast<std::size_t>(layer)];
    const float radiusSq = radius * radius;

    std::size_t erased = 0;
    const auto tail = std::remove_if(
        data.instances.begin(), data.instances.end(), [&](const Instance& instance) {
            const float dx = instance.position.x - worldX;
            const float dz = instance.position.z - worldZ;
            if (dx * dx + dz * dz > radiusSq) {
                return false;
            }
            ErasedInstance record;
            record.layer = layer;
            record.instance = instance;
            if (instance.manual) {
                RemoveByPlacement(data.added, instance);
            } else {
                record.key = InstanceKey(instance.position.x, instance.position.z);
                InsertRemovedKey(data, record.key);
            }
            if (inStroke_) {
                current_.erased.push_back(record);
                current_.bytes += sizeof(ErasedInstance);
            }
            ++erased;
            return true;
        });
    data.instances.erase(tail, data.instances.end());
    return erased;
}

void Vegetation::ClearManual(int layer) {
    if (layer < 0 || layer >= LayerCount()) {
        return;
    }
    LayerData& data = layers_[static_cast<std::size_t>(layer)];
    data.added.clear();
    data.removed.clear();
    data.removedSet.clear();
    ClearHistory();
}

std::size_t Vegetation::AddedCount(int layer) const {
    if (layer < 0 || layer >= LayerCount()) {
        return 0;
    }
    return layers_[static_cast<std::size_t>(layer)].added.size();
}

std::size_t Vegetation::RemovedCount(int layer) const {
    if (layer < 0 || layer >= LayerCount()) {
        return 0;
    }
    return layers_[static_cast<std::size_t>(layer)].removed.size();
}

const std::vector<Instance>& Vegetation::Added(int layer) const {
    static const std::vector<Instance> kEmpty;
    if (layer < 0 || layer >= LayerCount()) {
        return kEmpty;
    }
    return layers_[static_cast<std::size_t>(layer)].added;
}

const std::vector<uint64_t>& Vegetation::Removed(int layer) const {
    static const std::vector<uint64_t> kEmpty;
    if (layer < 0 || layer >= LayerCount()) {
        return kEmpty;
    }
    return layers_[static_cast<std::size_t>(layer)].removed;
}

void Vegetation::SetManual(int layer, const std::vector<Instance>& added,
                           const std::vector<uint64_t>& removed) {
    if (layer < 0 || layer >= LayerCount()) {
        return;
    }
    LayerData& data = layers_[static_cast<std::size_t>(layer)];
    data.added = added;
    for (Instance& instance : data.added) {
        instance.manual = true;
    }
    data.removed = removed;
    data.removedSet.clear();
    data.removedSet.insert(removed.begin(), removed.end());
    ClearHistory();
}

void Vegetation::InsertRemovedKey(LayerData& data, uint64_t key) {
    if (data.removedSet.insert(key).second) {
        data.removed.push_back(key);
    }
}

void Vegetation::EraseRemovedKey(LayerData& data, uint64_t key) {
    if (data.removedSet.erase(key) == 0) {
        return;
    }
    const auto it = std::find(data.removed.begin(), data.removed.end(), key);
    if (it != data.removed.end()) {
        data.removed.erase(it);
    }
}

// --- goresan dan undo ---------------------------------------------------------

void Vegetation::BeginStroke() {
    if (inStroke_) {
        return;
    }
    inStroke_ = true;
    current_ = Stroke{};
    captured_.clear();
}

void Vegetation::EndStroke() {
    if (!inStroke_) {
        return;
    }
    inStroke_ = false;
    captured_.clear();
    if (current_.Empty()) {
        return;
    }
    PushStroke();
}

void Vegetation::PushStroke() {
    undo_.push_back(std::move(current_));
    current_ = Stroke{};
    undoBytes_ += undo_.back().bytes;
    // Redo dibuang begitu ada goresan baru: cabang riwayat yang bisa dimasuki
    // kembali setelah menyunting adalah riwayat yang tidak bisa ditebak siapa
    // pun.
    redo_.clear();
    TrimJournal();
}

void Vegetation::CaptureDensityCell(int layer, int32_t cell, uint8_t before) {
    if (!inStroke_) {
        return;
    }
    if (!captured_.insert(CaptureKey(layer, cell)).second) {
        return;
    }
    current_.density.push_back(DensityCell{layer, cell, before});
    current_.bytes += sizeof(DensityCell);
}

void Vegetation::ApplyStroke(Stroke& stroke, bool undoing) {
    for (DensityCell& record : stroke.density) {
        if (record.layer < 0 || record.layer >= LayerCount()) {
            continue;
        }
        DensityMap& map = layers_[static_cast<std::size_t>(record.layer)].density;
        if (map.Width() <= 0) {
            continue;
        }
        const int x = record.cell % map.Width();
        const int y = record.cell / map.Width();
        const uint8_t live = map.At(x, y);
        map.SetAt(x, y, record.before);
        record.before = live;  // satu salinan melayani undo dan redo sekaligus
    }
    for (DensityImage& record : stroke.images) {
        if (record.layer < 0 || record.layer >= LayerCount()) {
            continue;
        }
        layers_[static_cast<std::size_t>(record.layer)].density.SwapCells(record.cells);
    }

    // Yang dibuang dikumpulkan dulu menjadi himpunan kunci per layer, lalu
    // dibuang dalam satu lintasan. Yang dikembalikan cukup ditempelkan di
    // belakang: daftar instance tidak punya urutan yang berarti — menyebar ulang
    // menyusunnya kembali dari awal, dan tidak ada yang membacanya berurutan.
    std::vector<std::unordered_set<uint64_t>> drop(static_cast<std::size_t>(LayerCount()));
    const std::vector<ErasedInstance>& erased = stroke.erased;
    const std::vector<PlantedInstance>& planted = stroke.planted;

    if (undoing) {
        for (const PlantedInstance& record : planted) {
            if (record.layer < 0 || record.layer >= LayerCount()) {
                continue;
            }
            drop[static_cast<std::size_t>(record.layer)].insert(
                InstanceKey(record.instance.position.x, record.instance.position.z));
        }
    } else {
        for (const ErasedInstance& record : erased) {
            if (record.layer < 0 || record.layer >= LayerCount()) {
                continue;
            }
            drop[static_cast<std::size_t>(record.layer)].insert(
                InstanceKey(record.instance.position.x, record.instance.position.z));
        }
    }
    for (int layer = 0; layer < LayerCount(); ++layer) {
        LayerData& data = layers_[static_cast<std::size_t>(layer)];
        RemoveKeyed(data.instances, drop[static_cast<std::size_t>(layer)]);
        RemoveKeyed(data.added, drop[static_cast<std::size_t>(layer)]);
    }

    if (undoing) {
        for (const ErasedInstance& record : erased) {
            if (record.layer < 0 || record.layer >= LayerCount()) {
                continue;
            }
            LayerData& data = layers_[static_cast<std::size_t>(record.layer)];
            if (record.instance.manual) {
                data.added.push_back(record.instance);
            } else {
                EraseRemovedKey(data, record.key);
            }
            data.instances.push_back(record.instance);
        }
        return;
    }

    for (const ErasedInstance& record : erased) {
        if (record.layer >= 0 && record.layer < LayerCount() && !record.instance.manual) {
            InsertRemovedKey(layers_[static_cast<std::size_t>(record.layer)], record.key);
        }
    }
    for (const PlantedInstance& record : planted) {
        if (record.layer < 0 || record.layer >= LayerCount()) {
            continue;
        }
        LayerData& data = layers_[static_cast<std::size_t>(record.layer)];
        data.added.push_back(record.instance);
        data.instances.push_back(record.instance);
    }
}

bool Vegetation::Undo() {
    if (inStroke_ || undo_.empty()) {
        return false;
    }
    Stroke stroke = std::move(undo_.back());
    undo_.pop_back();
    undoBytes_ -= std::min(undoBytes_, stroke.bytes);
    ApplyStroke(stroke, true);
    redo_.push_back(std::move(stroke));
    return true;
}

bool Vegetation::Redo() {
    if (inStroke_ || redo_.empty()) {
        return false;
    }
    Stroke stroke = std::move(redo_.back());
    redo_.pop_back();
    ApplyStroke(stroke, false);
    undoBytes_ += stroke.bytes;
    undo_.push_back(std::move(stroke));
    return true;
}

void Vegetation::ClearHistory() {
    undo_.clear();
    redo_.clear();
    current_ = Stroke{};
    captured_.clear();
    inStroke_ = false;
    undoBytes_ = 0;
}

void Vegetation::TrimJournal() {
    while (undoBytes_ > undoBudgetBytes && undo_.size() > 1) {
        undoBytes_ -= std::min(undoBytes_, undo_.front().bytes);
        undo_.erase(undo_.begin());
    }
}

std::size_t Vegetation::BytesResident() const {
    std::size_t bytes = 0;
    for (const LayerData& data : layers_) {
        bytes += data.instances.capacity() * sizeof(Instance);
        bytes += data.added.capacity() * sizeof(Instance);
        bytes += data.removed.capacity() * sizeof(uint64_t);
        bytes += data.removedSet.size() * (sizeof(uint64_t) * 2);
        bytes += data.density.Bytes();
    }
    return bytes + undoBytes_;
}

}  // namespace sim::vegetation
