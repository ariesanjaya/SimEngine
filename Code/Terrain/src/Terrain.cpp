#include "Sim/Terrain/Terrain.h"

#include <algorithm>
#include <cmath>

namespace sim::terrain {
namespace {

uint64_t BlockKey(int tile, int block) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(tile)) << 32) |
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
    tiles_.resize(static_cast<std::size_t>(desc_.tilesX) * static_cast<std::size_t>(desc_.tilesY));
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
    if (!captured_.insert(BlockKey(tileIndex, blockIndex)).second) {
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
    tile.samples[static_cast<std::size_t>(ly * desc_.tileSamples + lx)] = value;
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
    const int w = SamplesX();
    for (int ty = 0; ty < desc_.tilesY; ++ty) {
        for (int tx = 0; tx < desc_.tilesX; ++tx) {
            Tile& tile = MaterializeTile(ty * desc_.tilesX + tx);
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
    if (current_.blocks.empty()) {
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

std::size_t Terrain::BytesResident() const {
    const std::size_t perTile = static_cast<std::size_t>(desc_.tileSamples) *
                                static_cast<std::size_t>(desc_.tileSamples) * sizeof(Sample);
    return TilesResident() * perTile + undoBytes_ + current_.bytes;
}

}  // namespace sim::terrain
