#include "Sim/Terrain/Terrain.h"

#include <algorithm>
#include <cmath>

namespace sim::terrain {
namespace {

/// Peta tinggi diberi nomor -2 dan peta hole -1 supaya keduanya berbagi ruang
/// kunci dengan layer bobot tanpa bisa bertabrakan dengannya.
constexpr int kHeightChannel = -2;
constexpr int kHoleChannel = -1;

uint64_t BlockKey(int channel, int tile, int block) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(channel + 2)) << 48) |
           (static_cast<uint64_t>(static_cast<uint32_t>(tile)) << 24) |
           static_cast<uint32_t>(block);
}

}  // namespace

Terrain::Terrain(const TerrainDesc& desc) : desc_(desc) {
    desc_.tileSamples = std::max(1, desc_.tileSamples);
    desc_.tilesX = std::max(1, desc_.tilesX);
    desc_.tilesY = std::max(1, desc_.tilesY);
    if (desc_.maxHeight <= desc_.minHeight) {
        desc_.maxHeight = desc_.minHeight + 1.0f;
    }
    base_ = ToSample(desc_.baseHeight);
    tiles_.resize(static_cast<std::size_t>(TileCount()));
    tileRevision_.assign(static_cast<std::size_t>(TileCount()), 1u);
    tilePaintRevision_.assign(static_cast<std::size_t>(TileCount()), 1u);
    holes_.resize(static_cast<std::size_t>(TileCount()));

    // Layer dasar selalu ada. Terrain tanpa satu pun layer adalah terrain yang
    // setiap sampelnya tidak punya material — keadaan yang tidak berguna bagi
    // siapa pun dan harus diperiksa di setiap pembaca.
    LayerData base;
    base.desc.name = "Base";
    base.tiles.resize(static_cast<std::size_t>(TileCount()));
    layers_.push_back(std::move(base));
}

float Terrain::ToMeters(Sample value) const {
    return desc_.minHeight +
           (static_cast<float>(value) / static_cast<float>(kSampleMax)) *
               (desc_.maxHeight - desc_.minHeight);
}

Sample Terrain::ToSample(float meters) const {
    const float span = desc_.maxHeight - desc_.minHeight;
    const float unit = (meters - desc_.minHeight) / span;
    // Dibulatkan ke terdekat, bukan dipotong. Pemotongan membuat SetHeightAt
    // diikuti HeightAt menggeser nilainya ke bawah setiap kali, sehingga
    // menerapkan brush dengan kekuatan nol tetap menurunkan terrain perlahan.
    const float scaled = std::round(unit * static_cast<float>(kSampleMax));
    return static_cast<Sample>(std::clamp(scaled, 0.0f, static_cast<float>(kSampleMax)));
}

Sample Terrain::RawAt(int x, int y) const {
    x = std::clamp(x, 0, SamplesX() - 1);
    y = std::clamp(y, 0, SamplesY() - 1);
    const int tx = x / desc_.tileSamples;
    const int ty = y / desc_.tileSamples;
    const std::unique_ptr<Tile>& tile = tiles_[static_cast<std::size_t>(ty * desc_.tilesX + tx)];
    if (tile == nullptr) {
        return base_;
    }
    const int lx = x - tx * desc_.tileSamples;
    const int ly = y - ty * desc_.tileSamples;
    return tile->samples[static_cast<std::size_t>(ly * desc_.tileSamples + lx)];
}

Terrain::Tile& Terrain::MaterializeTile(int index) {
    std::unique_ptr<Tile>& tile = tiles_[static_cast<std::size_t>(index)];
    if (tile == nullptr) {
        tile = std::make_unique<Tile>();
        tile->samples.assign(
            static_cast<std::size_t>(desc_.tileSamples) * static_cast<std::size_t>(desc_.tileSamples),
            base_);
    }
    return *tile;
}

void Terrain::CaptureBlock(int tileIndex, int blockIndex) {
    if (!inStroke_) {
        return;
    }
    if (!captured_.insert(BlockKey(kHeightChannel, tileIndex, blockIndex)).second) {
        return;
    }

    const int perSide = BlocksPerSide();
    const int bx = (blockIndex % perSide) * kBlockSize;
    const int by = (blockIndex / perSide) * kBlockSize;
    const int w = std::min(kBlockSize, desc_.tileSamples - bx);
    const int h = std::min(kBlockSize, desc_.tileSamples - by);

    BlockImage record;
    record.tile = tileIndex;
    record.block = blockIndex;
    record.image.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));

    const Tile& tile = *tiles_[static_cast<std::size_t>(tileIndex)];
    for (int row = 0; row < h; ++row) {
        const Sample* src = &tile.samples[static_cast<std::size_t>((by + row) * desc_.tileSamples + bx)];
        std::copy(src, src + w, record.image.begin() + static_cast<std::ptrdiff_t>(row) * w);
    }

    current_.bytes += record.image.size() * sizeof(Sample);
    current_.blocks.push_back(std::move(record));
}

void Terrain::SetRawAt(int x, int y, Sample value) {
    if (x < 0 || y < 0 || x >= SamplesX() || y >= SamplesY()) {
        return;
    }
    const int tx = x / desc_.tileSamples;
    const int ty = y / desc_.tileSamples;
    const int tileIndex = ty * desc_.tilesX + tx;
    const int lx = x - tx * desc_.tileSamples;
    const int ly = y - ty * desc_.tileSamples;

    Tile& tile = MaterializeTile(tileIndex);
    if (inStroke_) {
        const int perSide = BlocksPerSide();
        CaptureBlock(tileIndex, (ly / kBlockSize) * perSide + (lx / kBlockSize));
    }
    Sample& slot = tile.samples[static_cast<std::size_t>(ly * desc_.tileSamples + lx)];
    if (slot == value) {
        // Menaikkan revisi untuk tulisan yang tidak mengubah apa pun berarti
        // brush berkekuatan nol — atau sapuan yang melewati batas peta —
        // membangun ulang mesh yang sama persis, tiap frame selama tombol
        // ditahan.
        return;
    }
    slot = value;
    BumpTile(tileIndex);
}

void Terrain::BumpTile(int tileIndex) {
    if (tileIndex >= 0 && tileIndex < static_cast<int>(tileRevision_.size())) {
        ++tileRevision_[static_cast<std::size_t>(tileIndex)];
    }
}

void Terrain::BumpTilePaint(int tileIndex) {
    if (tileIndex >= 0 && tileIndex < static_cast<int>(tilePaintRevision_.size())) {
        ++tilePaintRevision_[static_cast<std::size_t>(tileIndex)];
    }
}

uint32_t Terrain::TilePaintRevision(int tileX, int tileY) const {
    if (tileX < 0 || tileY < 0 || tileX >= desc_.tilesX || tileY >= desc_.tilesY) {
        return 0;
    }
    return tilePaintRevision_[static_cast<std::size_t>(tileY * desc_.tilesX + tileX)];
}

uint32_t Terrain::TileRevision(int tileX, int tileY) const {
    if (tileX < 0 || tileY < 0 || tileX >= desc_.tilesX || tileY >= desc_.tilesY) {
        return 0;
    }
    return tileRevision_[static_cast<std::size_t>(tileY * desc_.tilesX + tileX)];
}

float Terrain::HeightAtWorld(float worldX, float worldZ) const {
    const float fx = worldX / desc_.sampleSpacing;
    const float fz = worldZ / desc_.sampleSpacing;
    const int x0 = static_cast<int>(std::floor(fx));
    const int z0 = static_cast<int>(std::floor(fz));
    const float tx = fx - static_cast<float>(x0);
    const float tz = fz - static_cast<float>(z0);

    const float h00 = HeightAt(x0, z0);
    const float h10 = HeightAt(x0 + 1, z0);
    const float h01 = HeightAt(x0, z0 + 1);
    const float h11 = HeightAt(x0 + 1, z0 + 1);
    return (h00 * (1.0f - tx) + h10 * tx) * (1.0f - tz) +
           (h01 * (1.0f - tx) + h11 * tx) * tz;
}

Vec3 Terrain::NormalAtWorld(float worldX, float worldZ) const {
    const float fx = worldX / desc_.sampleSpacing;
    const float fz = worldZ / desc_.sampleSpacing;
    const int x0 = static_cast<int>(std::floor(fx));
    const int z0 = static_cast<int>(std::floor(fz));
    const float tx = fx - static_cast<float>(x0);
    const float tz = fz - static_cast<float>(z0);

    const float h00 = HeightAt(x0, z0);
    const float h10 = HeightAt(x0 + 1, z0);
    const float h01 = HeightAt(x0, z0 + 1);
    const float h11 = HeightAt(x0 + 1, z0 + 1);

    // Turunan permukaan bilinear terhadap x dan z, dalam meter per meter.
    const float dhdx = ((h10 - h00) * (1.0f - tz) + (h11 - h01) * tz) / desc_.sampleSpacing;
    const float dhdz = ((h01 - h00) * (1.0f - tx) + (h11 - h10) * tx) / desc_.sampleSpacing;
    return glm::normalize(Vec3(-dhdx, 1.0f, -dhdz));
}

SampleRect Terrain::RectForCircle(float worldX, float worldZ, float radius) const {
    const float inv = 1.0f / desc_.sampleSpacing;
    SampleRect rect;
    rect.x0 = std::max(0, static_cast<int>(std::floor((worldX - radius) * inv)));
    rect.y0 = std::max(0, static_cast<int>(std::floor((worldZ - radius) * inv)));
    rect.x1 = std::min(SamplesX(), static_cast<int>(std::ceil((worldX + radius) * inv)) + 1);
    rect.y1 = std::min(SamplesY(), static_cast<int>(std::ceil((worldZ + radius) * inv)) + 1);
    return rect;
}

void Terrain::ReadAll(std::vector<Sample>& out) const {
    const int w = SamplesX();
    const int h = SamplesY();
    out.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), base_);
    for (int ty = 0; ty < desc_.tilesY; ++ty) {
        for (int tx = 0; tx < desc_.tilesX; ++tx) {
            const std::unique_ptr<Tile>& tile =
                tiles_[static_cast<std::size_t>(ty * desc_.tilesX + tx)];
            if (tile == nullptr) {
                continue;  // sudah terisi base_
            }
            for (int row = 0; row < desc_.tileSamples; ++row) {
                const Sample* src =
                    &tile->samples[static_cast<std::size_t>(row) *
                                   static_cast<std::size_t>(desc_.tileSamples)];
                Sample* dst = out.data() +
                              static_cast<std::size_t>(ty * desc_.tileSamples + row) *
                                  static_cast<std::size_t>(w) +
                              static_cast<std::size_t>(tx * desc_.tileSamples);
                std::copy(src, src + desc_.tileSamples, dst);
            }
        }
    }
}

void Terrain::WriteAll(const Sample* samples) {
    // Seluruh peta berganti isi, jadi seluruh ubin berganti bentuk. Menandai
    // per ubin di sini berarti memindai apa yang baru saja ditulis untuk tahu
    // apa yang berubah — memindai dua kali untuk jawaban yang sudah pasti.
    for (int index = 0; index < TileCount(); ++index) {
        BumpTile(index);
    }
    const int w = SamplesX();
    for (int ty = 0; ty < desc_.tilesY; ++ty) {
        for (int tx = 0; tx < desc_.tilesX; ++tx) {
            const int index = ty * desc_.tilesX + tx;

            // Ubin yang seluruhnya bertinggi dasar tidak diwujudkan.
            //
            // Tanpa ini, memuat terrain membatalkan seluruh guna alokasi malas:
            // berkas heightmap memuat seluruh peta, jadi membuka terrain 4×4 km
            // langsung menghuni memori sepenuhnya — padahal bagian yang datar
            // tidak menyimpan apa pun yang belum diketahui. Memindai lebih murah
            // daripada mengalokasi, dan jauh lebih murah daripada menahannya.
            if (tiles_[static_cast<std::size_t>(index)] == nullptr) {
                bool uniform = true;
                for (int row = 0; row < desc_.tileSamples && uniform; ++row) {
                    const Sample* src = samples +
                                        static_cast<std::size_t>(ty * desc_.tileSamples + row) *
                                            static_cast<std::size_t>(w) +
                                        static_cast<std::size_t>(tx * desc_.tileSamples);
                    for (int col = 0; col < desc_.tileSamples; ++col) {
                        if (src[col] != base_) {
                            uniform = false;
                            break;
                        }
                    }
                }
                if (uniform) {
                    continue;
                }
            }

            Tile& tile = MaterializeTile(index);
            for (int row = 0; row < desc_.tileSamples; ++row) {
                const Sample* src = samples +
                                    static_cast<std::size_t>(ty * desc_.tileSamples + row) *
                                        static_cast<std::size_t>(w) +
                                    static_cast<std::size_t>(tx * desc_.tileSamples);
                Sample* dst = &tile.samples[static_cast<std::size_t>(row) *
                                            static_cast<std::size_t>(desc_.tileSamples)];
                std::copy(src, src + desc_.tileSamples, dst);
            }
        }
    }
}

// --- peta byte: bobot layer dan hole -----------------------------------------

std::vector<std::unique_ptr<Terrain::ByteTile>>& Terrain::ByteTiles(int layer) {
    return layer >= 0 ? layers_[static_cast<std::size_t>(layer)].tiles : holes_;
}

const std::vector<std::unique_ptr<Terrain::ByteTile>>& Terrain::ByteTiles(int layer) const {
    return layer >= 0 ? layers_[static_cast<std::size_t>(layer)].tiles : holes_;
}

Terrain::ByteTile& Terrain::MaterializeByteTile(int layer, int index) {
    std::unique_ptr<ByteTile>& tile = ByteTiles(layer)[static_cast<std::size_t>(index)];
    if (tile == nullptr) {
        tile = std::make_unique<ByteTile>();
        tile->bytes.assign(static_cast<std::size_t>(desc_.tileSamples) *
                               static_cast<std::size_t>(desc_.tileSamples),
                           0);
    }
    return *tile;
}

void Terrain::CaptureByteBlock(int layer, int tileIndex, int blockIndex) {
    if (!inStroke_) {
        return;
    }
    if (!captured_.insert(BlockKey(layer, tileIndex, blockIndex)).second) {
        return;
    }

    const int perSide = BlocksPerSide();
    const int bx = (blockIndex % perSide) * kBlockSize;
    const int by = (blockIndex / perSide) * kBlockSize;
    const int w = std::min(kBlockSize, desc_.tileSamples - bx);
    const int h = std::min(kBlockSize, desc_.tileSamples - by);

    ByteBlockImage record;
    record.layer = layer;
    record.tile = tileIndex;
    record.block = blockIndex;
    record.image.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));

    const ByteTile& tile = *ByteTiles(layer)[static_cast<std::size_t>(tileIndex)];
    for (int row = 0; row < h; ++row) {
        const uint8_t* src =
            &tile.bytes[static_cast<std::size_t>((by + row) * desc_.tileSamples + bx)];
        std::copy(src, src + w, record.image.begin() + static_cast<std::ptrdiff_t>(row) * w);
    }

    current_.bytes += record.image.size();
    current_.byteBlocks.push_back(std::move(record));
}

uint8_t Terrain::ByteAt(int layer, int x, int y) const {
    x = std::clamp(x, 0, SamplesX() - 1);
    y = std::clamp(y, 0, SamplesY() - 1);
    const int tx = x / desc_.tileSamples;
    const int ty = y / desc_.tileSamples;
    const std::unique_ptr<ByteTile>& tile =
        ByteTiles(layer)[static_cast<std::size_t>(ty * desc_.tilesX + tx)];
    if (tile == nullptr) {
        return 0;
    }
    const int lx = x - tx * desc_.tileSamples;
    const int ly = y - ty * desc_.tileSamples;
    return tile->bytes[static_cast<std::size_t>(ly * desc_.tileSamples + lx)];
}

void Terrain::SetByteAt(int layer, int x, int y, uint8_t value) {
    if (x < 0 || y < 0 || x >= SamplesX() || y >= SamplesY()) {
        return;
    }
    const int tx = x / desc_.tileSamples;
    const int ty = y / desc_.tileSamples;
    const int tileIndex = ty * desc_.tilesX + tx;
    if (ByteTiles(layer)[static_cast<std::size_t>(tileIndex)] == nullptr && value == 0) {
        // Menulis nol ke ubin yang belum ada tidak mengubah apa pun. Tanpa
        // pintasan ini, menghapus di atas daerah yang belum pernah dicat justru
        // mewujudkan seluruh peta bobotnya — kebalikan dari yang diminta.
        return;
    }

    const int lx = x - tx * desc_.tileSamples;
    const int ly = y - ty * desc_.tileSamples;
    ByteTile& tile = MaterializeByteTile(layer, tileIndex);
    if (inStroke_) {
        const int perSide = BlocksPerSide();
        CaptureByteBlock(layer, tileIndex, (ly / kBlockSize) * perSide + (lx / kBlockSize));
    }
    uint8_t& slot = tile.bytes[static_cast<std::size_t>(ly * desc_.tileSamples + lx)];
    const bool changed = slot != value;
    slot = value;
    // **Hanya lubang, bukan bobot layer.** Lubang membuang quad, jadi ia bagian
    // bentuk; bobot layer hanya mengganti warnanya. Menaikkan revisi untuk cat
    // berarti terrain empat kilometer dibangun ulang setiap sapuan kuas.
    if (!changed) {
        return;
    }
    if (layer == kHoleChannel) {
        BumpTile(tileIndex);
    } else {
        BumpTilePaint(tileIndex);
    }
}

void Terrain::ReadBytes(int layer, std::vector<uint8_t>& out) const {
    const int w = SamplesX();
    out.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(SamplesY()), 0);
    for (int ty = 0; ty < desc_.tilesY; ++ty) {
        for (int tx = 0; tx < desc_.tilesX; ++tx) {
            const std::unique_ptr<ByteTile>& tile =
                ByteTiles(layer)[static_cast<std::size_t>(ty * desc_.tilesX + tx)];
            if (tile == nullptr) {
                continue;
            }
            for (int row = 0; row < desc_.tileSamples; ++row) {
                const uint8_t* src = &tile->bytes[static_cast<std::size_t>(row) *
                                                  static_cast<std::size_t>(desc_.tileSamples)];
                uint8_t* dst = out.data() +
                               static_cast<std::size_t>(ty * desc_.tileSamples + row) *
                                   static_cast<std::size_t>(w) +
                               static_cast<std::size_t>(tx * desc_.tileSamples);
                std::copy(src, src + desc_.tileSamples, dst);
            }
        }
    }
}

void Terrain::WriteBytes(int layer, const uint8_t* bytes) {
    const int w = SamplesX();
    for (int ty = 0; ty < desc_.tilesY; ++ty) {
        for (int tx = 0; tx < desc_.tilesX; ++tx) {
            const int index = ty * desc_.tilesX + tx;
            // Alasan yang sama dengan WriteAll: ubin yang seluruhnya nol tidak
            // menyimpan apa pun yang belum diketahui, dan mewujudkannya membuang
            // seluruh guna alokasi malas justru pada saat membuka berkas.
            if (ByteTiles(layer)[static_cast<std::size_t>(index)] == nullptr) {
                bool empty = true;
                for (int row = 0; row < desc_.tileSamples && empty; ++row) {
                    const uint8_t* src = bytes +
                                         static_cast<std::size_t>(ty * desc_.tileSamples + row) *
                                             static_cast<std::size_t>(w) +
                                         static_cast<std::size_t>(tx * desc_.tileSamples);
                    for (int col = 0; col < desc_.tileSamples; ++col) {
                        if (src[col] != 0) {
                            empty = false;
                            break;
                        }
                    }
                }
                if (empty) {
                    continue;
                }
            }

            ByteTile& tile = MaterializeByteTile(layer, index);
            for (int row = 0; row < desc_.tileSamples; ++row) {
                const uint8_t* src = bytes +
                                     static_cast<std::size_t>(ty * desc_.tileSamples + row) *
                                         static_cast<std::size_t>(w) +
                                     static_cast<std::size_t>(tx * desc_.tileSamples);
                uint8_t* dst = &tile.bytes[static_cast<std::size_t>(row) *
                                           static_cast<std::size_t>(desc_.tileSamples)];
                std::copy(src, src + desc_.tileSamples, dst);
            }
        }
    }
}

// --- layer material -----------------------------------------------------------

const TerrainLayer& Terrain::Layer(int index) const {
    return layers_[static_cast<std::size_t>(std::clamp(index, 0, LayerCount() - 1))].desc;
}

TerrainLayer& Terrain::Layer(int index) {
    return layers_[static_cast<std::size_t>(std::clamp(index, 0, LayerCount() - 1))].desc;
}

void Terrain::BumpAllPaint() {
    // Seluruh ubin, karena operasi-operasi ini menggeser **pemetaan** layer ke
    // peta bobotnya — bukan satu nilai di satu tempat. Memindai untuk tahu ubin
    // mana yang benar-benar berubah berarti memindai seluruh peta demi jawaban
    // yang sudah pasti "semuanya".
    for (int index = 0; index < TileCount(); ++index) {
        BumpTilePaint(index);
    }
}

int Terrain::AddLayer(const TerrainLayer& layer) {
    BumpAllPaint();
    if (LayerCount() >= kMaxLayers) {
        return -1;
    }
    LayerData data;
    data.desc = layer;
    data.tiles.resize(static_cast<std::size_t>(TileCount()));
    layers_.push_back(std::move(data));
    return LayerCount() - 1;
}

bool Terrain::RemoveLayer(int index) {
    BumpAllPaint();
    if (index <= 0 || index >= LayerCount()) {
        return false;
    }
    layers_.erase(layers_.begin() + index);
    // Riwayat dibuang: jurnalnya menunjuk layer lewat indeks, dan indeks yang
    // sama sekarang menunjuk layer yang berbeda. Satu Ctrl+Z akan memasang bobot
    // layer yang dihapus ke atas layer yang bukan pemiliknya — kerusakan senyap,
    // yang lebih buruk daripada undo yang hilang.
    ClearHistory();
    return true;
}

bool Terrain::MoveLayer(int from, int to) {
    BumpAllPaint();
    if (from <= 0 || to <= 0 || from >= LayerCount() || to >= LayerCount() || from == to) {
        return false;
    }
    LayerData moved = std::move(layers_[static_cast<std::size_t>(from)]);
    layers_.erase(layers_.begin() + from);
    layers_.insert(layers_.begin() + to, std::move(moved));
    ClearHistory();  // alasan yang sama dengan RemoveLayer
    return true;
}

void Terrain::SetLayers(const std::vector<TerrainLayer>& layers) {
    BumpAllPaint();
    layers_.clear();
    for (const TerrainLayer& layer : layers) {
        LayerData data;
        data.desc = layer;
        data.tiles.resize(static_cast<std::size_t>(TileCount()));
        layers_.push_back(std::move(data));
        if (LayerCount() >= kMaxLayers) {
            break;
        }
    }
    if (layers_.empty()) {
        LayerData base;
        base.desc.name = "Base";
        base.tiles.resize(static_cast<std::size_t>(TileCount()));
        layers_.push_back(std::move(base));
    }
    ClearHistory();
}

Weight Terrain::WeightAt(int layer, int x, int y) const {
    if (layer < 0 || layer >= LayerCount()) {
        return 0;
    }
    if (layer > 0) {
        return ByteAt(layer, x, y);
    }
    int sum = 0;
    for (int other = 1; other < LayerCount(); ++other) {
        sum += ByteAt(other, x, y);
    }
    // Dijepit, bukan diandaikan: peta bobot bisa datang dari berkas yang disunting
    // di luar editor, dan sampel yang totalnya melebihi 255 tidak boleh membuat
    // bobot dasar berputar menjadi angka besar.
    return static_cast<Weight>(std::clamp(kWeightMax - sum, 0, static_cast<int>(kWeightMax)));
}

void Terrain::ShrinkOtherWeights(int keep, int x, int y, int budget) {
    int sum = 0;
    for (int layer = 1; layer < LayerCount(); ++layer) {
        if (layer != keep) {
            sum += ByteAt(layer, x, y);
        }
    }
    if (sum <= budget) {
        // Tidak ada yang ditulis — dan karena itu tidak ada ubin yang diwujudkan
        // dan tidak ada blok yang masuk jurnal. Mengecat layer pertama di atas
        // terrain kosong karena itu tidak menyentuh peta bobot mana pun kecuali
        // miliknya sendiri.
        return;
    }
    for (int layer = 1; layer < LayerCount(); ++layer) {
        if (layer == keep) {
            continue;
        }
        const int weight = ByteAt(layer, x, y);
        if (weight == 0) {
            continue;
        }
        SetByteAt(layer, x, y, static_cast<uint8_t>(weight * budget / sum));
    }
}

void Terrain::SetWeightAt(int layer, int x, int y, Weight weight) {
    if (layer < 0 || layer >= LayerCount()) {
        return;
    }
    if (layer == 0) {
        // Layer dasar tidak punya peta: bobotnya sisa. "Menetapkan" bobot dasar
        // berarti menyusutkan layer di atasnya sampai sisanya sebesar itu — dan
        // itu memang arti mengecat kembali ke dasar.
        ShrinkOtherWeights(-1, x, y, kWeightMax - weight);
        return;
    }
    SetByteAt(layer, x, y, weight);
    ShrinkOtherWeights(layer, x, y, kWeightMax - weight);
}

bool Terrain::LayerPainted(int layer) const {
    if (layer <= 0 || layer >= LayerCount()) {
        return false;
    }
    for (const std::unique_ptr<ByteTile>& tile : ByteTiles(layer)) {
        if (tile != nullptr) {
            return true;
        }
    }
    return false;
}

void Terrain::ClearLayerWeights(int layer) {
    BumpAllPaint();
    if (layer <= 0 || layer >= LayerCount()) {
        return;
    }
    for (int index = 0; index < TileCount(); ++index) {
        if (ByteTiles(layer)[static_cast<std::size_t>(index)] == nullptr) {
            continue;
        }
        const int tx = index % desc_.tilesX;
        const int ty = index / desc_.tilesX;
        for (int row = 0; row < desc_.tileSamples; ++row) {
            for (int col = 0; col < desc_.tileSamples; ++col) {
                if (ByteAt(layer, tx * desc_.tileSamples + col, ty * desc_.tileSamples + row) != 0) {
                    SetByteAt(layer, tx * desc_.tileSamples + col, ty * desc_.tileSamples + row, 0);
                }
            }
        }
    }
}

void Terrain::ReadWeights(int layer, std::vector<Weight>& out) const {
    if (layer <= 0 || layer >= LayerCount()) {
        // Layer dasar tidak punya peta untuk dibaca; membangunnya di sini akan
        // menghasilkan berkas yang tidak boleh ada, karena memuatnya kembali
        // berarti bobot dasar disimpan dua kali dan bisa bertentangan.
        out.clear();
        return;
    }
    ReadBytes(layer, out);
}

void Terrain::WriteWeights(int layer, const Weight* weights) {
    BumpAllPaint();
    if (layer <= 0 || layer >= LayerCount() || weights == nullptr) {
        return;
    }
    WriteBytes(layer, weights);
}

void Terrain::NormalizeWeights() {
    BumpAllPaint();
    // Dengan satu layer eksplisit, totalnya tidak mungkin melebihi 255: sebuah
    // byte tidak bisa.
    if (LayerCount() <= 2) {
        return;
    }
    for (int index = 0; index < TileCount(); ++index) {
        int resident = 0;
        for (int layer = 1; layer < LayerCount(); ++layer) {
            if (ByteTiles(layer)[static_cast<std::size_t>(index)] != nullptr) {
                ++resident;
            }
        }
        if (resident < 2) {
            continue;  // butuh dua layer terwujud untuk bisa melebihi anggaran
        }
        const int tx = (index % desc_.tilesX) * desc_.tileSamples;
        const int ty = (index / desc_.tilesX) * desc_.tileSamples;
        for (int row = 0; row < desc_.tileSamples; ++row) {
            for (int col = 0; col < desc_.tileSamples; ++col) {
                ShrinkOtherWeights(-1, tx + col, ty + row, kWeightMax);
            }
        }
    }
}

// --- peta hole ----------------------------------------------------------------

bool Terrain::HoleAt(int x, int y) const {
    // Kolom dan baris terakhir tidak punya quad, jadi tidak bisa berlubang.
    if (x < 0 || y < 0 || x >= SamplesX() - 1 || y >= SamplesY() - 1) {
        return false;
    }
    return ByteAt(kHoleChannel, x, y) != 0;
}

void Terrain::SetHoleAt(int x, int y, bool hole) {
    if (x < 0 || y < 0 || x >= SamplesX() - 1 || y >= SamplesY() - 1) {
        return;
    }
    const bool was = ByteAt(kHoleChannel, x, y) != 0;
    if (was == hole) {
        return;
    }
    SetByteAt(kHoleChannel, x, y, hole ? 1 : 0);
    if (hole) {
        ++holeCount_;
    } else {
        --holeCount_;
    }
}

void Terrain::ClearHoles() {
    for (int index = 0; index < TileCount(); ++index) {
        if (holes_[static_cast<std::size_t>(index)] == nullptr) {
            continue;
        }
        BumpTile(index);
        const int tx = (index % desc_.tilesX) * desc_.tileSamples;
        const int ty = (index / desc_.tilesX) * desc_.tileSamples;
        for (int row = 0; row < desc_.tileSamples; ++row) {
            for (int col = 0; col < desc_.tileSamples; ++col) {
                SetHoleAt(tx + col, ty + row, false);
            }
        }
    }
}

void Terrain::ReadHoles(std::vector<uint8_t>& out) const {
    ReadBytes(kHoleChannel, out);
    // Ditulis sebagai 0/255, bukan 0/1: berkasnya adalah gambar, dan gambar yang
    // seluruhnya bernilai satu terlihat hitam pekat di penyunting mana pun.
    const int w = SamplesX();
    const int h = SamplesY();
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t& value = out[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                                 static_cast<std::size_t>(x)];
            // Kolom dan baris terakhir dinolkan: di sana tidak ada quad, jadi apa
            // pun yang tersimpan tidak berarti dan tidak boleh ikut terbaca.
            value = (x < w - 1 && y < h - 1 && value != 0) ? 255 : 0;
        }
    }
}

void Terrain::WriteHoles(const uint8_t* holes) {
    for (int index = 0; index < TileCount(); ++index) {
        BumpTile(index);
    }
    if (holes == nullptr) {
        return;
    }
    WriteBytes(kHoleChannel, holes);
    // Dihitung ulang dari ubin yang terwujud saja. Memindai seluruh peta berarti
    // seperempat miliar langkah pada terrain 16k² — untuk menghitung lubang yang
    // menurut definisinya hanya ada di ubin yang terwujud.
    holeCount_ = 0;
    for (int index = 0; index < TileCount(); ++index) {
        if (holes_[static_cast<std::size_t>(index)] == nullptr) {
            continue;
        }
        const int tx = (index % desc_.tilesX) * desc_.tileSamples;
        const int ty = (index / desc_.tilesX) * desc_.tileSamples;
        const ByteTile& tile = *holes_[static_cast<std::size_t>(index)];
        for (int row = 0; row < desc_.tileSamples; ++row) {
            if (ty + row >= SamplesY() - 1) {
                break;
            }
            const int columns = std::min(desc_.tileSamples, SamplesX() - 1 - tx);
            const uint8_t* line = &tile.bytes[static_cast<std::size_t>(row) *
                                              static_cast<std::size_t>(desc_.tileSamples)];
            for (int col = 0; col < columns; ++col) {
                if (line[col] != 0) {
                    ++holeCount_;
                }
            }
        }
    }
}

void Terrain::BeginStroke() {
    if (inStroke_) {
        return;
    }
    inStroke_ = true;
    current_ = Stroke{};
    captured_.clear();
}

void Terrain::EndStroke() {
    if (!inStroke_) {
        return;
    }
    inStroke_ = false;
    captured_.clear();
    if (current_.Empty()) {
        // Goresan yang tidak menyentuh apa pun tidak masuk riwayat: satu Ctrl+Z
        // yang tidak mengubah apa-apa terlihat seperti undo yang rusak.
        return;
    }
    undoBytes_ += current_.bytes;
    undo_.push_back(std::move(current_));
    current_ = Stroke{};
    // Riwayat maju dibuang begitu ada goresan baru — bercabang berarti "redo"
    // tidak lagi punya arti tunggal.
    for (const Stroke& stroke : redo_) {
        undoBytes_ -= std::min(undoBytes_, stroke.bytes);
    }
    redo_.clear();
    TrimJournal();
}

void Terrain::SwapStroke(Stroke& stroke) {
    for (BlockImage& record : stroke.blocks) {
        // Undo mengubah bentuk sama nyatanya dengan goresan yang dibatalkannya.
        // Yang lupa menaikkannya di sini menghasilkan Ctrl+Z yang membetulkan
        // datanya sambil meninggalkan layar menggambar bentuk yang sudah tidak
        // ada — bug yang tampak seperti "undo tidak bekerja".
        BumpTile(record.tile);
        Tile& tile = MaterializeTile(record.tile);
        const int perSide = BlocksPerSide();
        const int bx = (record.block % perSide) * kBlockSize;
        const int by = (record.block / perSide) * kBlockSize;
        const int w = std::min(kBlockSize, desc_.tileSamples - bx);
        const int h = std::min(kBlockSize, desc_.tileSamples - by);
        for (int row = 0; row < h; ++row) {
            Sample* live = &tile.samples[static_cast<std::size_t>((by + row) * desc_.tileSamples + bx)];
            Sample* saved = record.image.data() + static_cast<std::ptrdiff_t>(row) * w;
            std::swap_ranges(live, live + w, saved);
        }
    }

    for (ByteBlockImage& record : stroke.byteBlocks) {
        if (record.layer == kHoleChannel) {
            BumpTile(record.tile);
        } else {
            BumpTilePaint(record.tile);
        }
        ByteTile& tile = MaterializeByteTile(record.layer, record.tile);
        const int perSide = BlocksPerSide();
        const int bx = (record.block % perSide) * kBlockSize;
        const int by = (record.block / perSide) * kBlockSize;
        const int w = std::min(kBlockSize, desc_.tileSamples - bx);
        const int h = std::min(kBlockSize, desc_.tileSamples - by);
        for (int row = 0; row < h; ++row) {
            uint8_t* live = &tile.bytes[static_cast<std::size_t>((by + row) * desc_.tileSamples + bx)];
            uint8_t* saved = record.image.data() + static_cast<std::ptrdiff_t>(row) * w;
            if (record.layer < 0) {
                // Jumlah lubang dijaga bertahap, jadi ia harus ikut bertukar
                // bersama isinya. Menghitung ulang seluruh peta setelah tiap undo
                // akan membuat Ctrl+Z pada terrain besar terasa tersendat karena
                // pekerjaan yang tidak ada hubungannya dengan besar goresannya.
                for (int col = 0; col < w; ++col) {
                    const bool before = live[col] != 0;
                    const bool after = saved[col] != 0;
                    if (before == after) {
                        continue;
                    }
                    if (after) {
                        ++holeCount_;
                    } else {
                        --holeCount_;
                    }
                }
            }
            std::swap_ranges(live, live + w, saved);
        }
    }
}

bool Terrain::Undo() {
    if (inStroke_ || undo_.empty()) {
        return false;
    }
    Stroke stroke = std::move(undo_.back());
    undo_.pop_back();
    SwapStroke(stroke);
    redo_.push_back(std::move(stroke));
    return true;
}

bool Terrain::Redo() {
    if (inStroke_ || redo_.empty()) {
        return false;
    }
    Stroke stroke = std::move(redo_.back());
    redo_.pop_back();
    SwapStroke(stroke);
    undo_.push_back(std::move(stroke));
    return true;
}

void Terrain::ClearHistory() {
    undo_.clear();
    redo_.clear();
    undoBytes_ = 0;
}

void Terrain::TrimJournal() {
    // Yang dibuang yang terlama, bukan yang terbaru: undo satu langkah harus
    // selalu bisa, seberapa besar pun goresannya.
    while (undoBytes_ > desc_.undoBudgetBytes && undo_.size() > 1) {
        undoBytes_ -= std::min(undoBytes_, undo_.front().bytes);
        undo_.erase(undo_.begin());
    }
}

std::size_t Terrain::TilesResident() const {
    std::size_t count = 0;
    for (const std::unique_ptr<Tile>& tile : tiles_) {
        if (tile != nullptr) {
            ++count;
        }
    }
    return count;
}

bool Terrain::TileResident(int tileX, int tileY) const {
    if (tileX < 0 || tileY < 0 || tileX >= desc_.tilesX || tileY >= desc_.tilesY) {
        return false;
    }
    return tiles_[static_cast<std::size_t>(tileY * desc_.tilesX + tileX)] != nullptr;
}

std::size_t Terrain::BytesResident() const {
    const std::size_t samplesPerTile = static_cast<std::size_t>(desc_.tileSamples) *
                                       static_cast<std::size_t>(desc_.tileSamples);

    // Peta bobot dan peta hole ikut dihitung: keduanya dialokasikan malas dengan
    // aturan yang sama, jadi angka yang tidak menyebut keduanya adalah angka yang
    // mengecil tepat ketika terrainnya membesar.
    std::size_t byteTiles = 0;
    for (const LayerData& layer : layers_) {
        for (const std::unique_ptr<ByteTile>& tile : layer.tiles) {
            if (tile != nullptr) {
                ++byteTiles;
            }
        }
    }
    for (const std::unique_ptr<ByteTile>& tile : holes_) {
        if (tile != nullptr) {
            ++byteTiles;
        }
    }

    return TilesResident() * samplesPerTile * sizeof(Sample) + byteTiles * samplesPerTile +
           undoBytes_ + current_.bytes;
}

}  // namespace sim::terrain
