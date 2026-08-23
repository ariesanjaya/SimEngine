#include "Bvh.h"

#include <algorithm>
#include <array>
#include <numeric>

namespace sim::raycast {
namespace {

/// Bin per sumbu saat menilai pembagian.
///
/// **Terbin, bukan diurutkan penuh.** SAH eksak menuntut mengurutkan primitif di
/// setiap simpul — O(n log² n) untuk seluruh pohon — sementara 12 bin
/// menghasilkan pohon yang praktis sama baiknya dalam satu kali lintasan linear.
/// Angka 12 sendiri tidak istimewa; di bawah 8 kualitasnya mulai turun terlihat,
/// di atas 16 tidak ada yang bertambah.
constexpr uint32_t kBinCount = 12;

/// Berhenti membagi di sini. Daun berisi beberapa primitif lebih murah daripada
/// pohon yang lebih dalam: uji segitiga jauh lebih murah daripada dua kali
/// pengujian kotak plus dua kali lompatan cache.
constexpr uint32_t kMaxLeafSize = 4;

/// Biaya menelusuri satu simpul relatif terhadap menguji satu primitif.
constexpr float kTraversalCost = 1.0f;

struct Bin {
    Aabb bounds;
    uint32_t count = 0;
};

}  // namespace

void Bvh::Clear() {
    nodes_.clear();
    order_.clear();
}

void Bvh::Build(std::span<const Aabb> bounds) {
    Clear();
    if (bounds.empty()) {
        return;
    }

    order_.resize(bounds.size());
    std::iota(order_.begin(), order_.end(), 0u);

    // Dipesan penuh lebih dulu: `BuildRange` memegang referensi ke simpul yang
    // baru dibuatnya sementara ia merekursi, dan vektor yang tumbuh di tengah
    // rekursi memindahkan simpul itu. Batas 2n−1 adalah jumlah simpul maksimum
    // sebuah pohon biner berdaun n.
    nodes_.reserve(bounds.size() * 2);

    BuildRange(bounds, 0, static_cast<uint32_t>(bounds.size()));
}

uint32_t Bvh::BuildRange(std::span<const Aabb> bounds, uint32_t first, uint32_t count) {
    const uint32_t nodeIndex = static_cast<uint32_t>(nodes_.size());
    nodes_.push_back(BvhNode{});

    Aabb nodeBounds;
    Aabb centroidBounds;
    for (uint32_t i = 0; i < count; ++i) {
        const Aabb& box = bounds[order_[first + i]];
        nodeBounds.Expand(box);
        centroidBounds.Expand(box.Centre());
    }
    nodes_[nodeIndex].bounds = nodeBounds;

    const auto makeLeaf = [&]() {
        nodes_[nodeIndex].start = first;
        nodes_[nodeIndex].count = count;
        return nodeIndex;
    };

    if (count <= 1) {
        return makeLeaf();
    }

    const Vec3 extent = centroidBounds.Extent();
    int axis = 0;
    if (extent.y > extent[axis]) {
        axis = 1;
    }
    if (extent.z > extent[axis]) {
        axis = 2;
    }
    // Seluruh centroid berimpit: tidak ada bidang yang memisahkan apa pun, dan
    // membagi rata di sini hanya menambah kedalaman tanpa memangkas apa-apa.
    if (!(extent[axis] > 0.0f)) {
        return makeLeaf();
    }

    std::array<Bin, kBinCount> bins{};
    const float scale = static_cast<float>(kBinCount) / extent[axis];
    const float origin = centroidBounds.min[axis];

    const auto binOf = [&](const Aabb& box) {
        const float offset = (box.Centre()[axis] - origin) * scale;
        const auto index = static_cast<int32_t>(offset);
        return static_cast<uint32_t>(std::clamp(index, 0, static_cast<int32_t>(kBinCount) - 1));
    };

    for (uint32_t i = 0; i < count; ++i) {
        const Aabb& box = bounds[order_[first + i]];
        Bin& bin = bins[binOf(box)];
        bin.bounds.Expand(box);
        ++bin.count;
    }

    // Sapuan dua arah: luas dan cacah kumulatif di kiri tiap bidang, lalu di
    // kanannya. Menghitungnya ulang per bidang akan menjadikan penilaian ini
    // kuadratik terhadap jumlah bin.
    std::array<float, kBinCount - 1> leftArea{};
    std::array<uint32_t, kBinCount - 1> leftCount{};
    Aabb sweep;
    uint32_t running = 0;
    for (uint32_t i = 0; i + 1 < kBinCount; ++i) {
        sweep.Expand(bins[i].bounds);
        running += bins[i].count;
        leftArea[i] = sweep.SurfaceArea();
        leftCount[i] = running;
    }

    float bestCost = std::numeric_limits<float>::max();
    uint32_t bestSplit = 0;
    sweep = Aabb{};
    running = 0;
    for (uint32_t i = kBinCount - 1; i > 0; --i) {
        sweep.Expand(bins[i].bounds);
        running += bins[i].count;
        const uint32_t left = leftCount[i - 1];
        const uint32_t right = running;
        if (left == 0 || right == 0) {
            continue;
        }
        const float cost = leftArea[i - 1] * static_cast<float>(left) +
                           sweep.SurfaceArea() * static_cast<float>(right);
        if (cost < bestCost) {
            bestCost = cost;
            bestSplit = i;
        }
    }

    if (bestSplit == 0) {
        return makeLeaf();  // tidak ada bidang yang memisahkan
    }

    // **Dinormalkan terhadap luas simpul ini sebelum dibandingkan.** `bestCost`
    // di atas masih dalam satuan luas×cacah; biaya daun dalam satuan cacah.
    // Membandingkan keduanya tanpa membagi berarti simpul yang besar hampir
    // selalu memilih membelah dan simpul yang kecil hampir tidak pernah.
    const float parentArea = nodeBounds.SurfaceArea();
    if (count <= kMaxLeafSize && parentArea > 0.0f) {
        const float splitCost = kTraversalCost + bestCost / parentArea;
        const float leafCost = static_cast<float>(count);
        if (splitCost >= leafCost) {
            return makeLeaf();
        }
    }

    const auto begin = order_.begin() + first;
    const auto middle = std::partition(begin, begin + count, [&](uint32_t primitive) {
        return binOf(bounds[primitive]) < bestSplit;
    });
    const auto leftSize = static_cast<uint32_t>(std::distance(begin, middle));
    if (leftSize == 0 || leftSize == count) {
        return makeLeaf();  // penjepitan bin menaruh semuanya di satu sisi
    }

    // **Anak kiri implisit di `nodeIndex + 1`**, karena ia dibangun tepat
    // sesudah simpul ini; yang perlu disimpan hanya anak kanan. Itu yang
    // membuat `start` cukup satu angka untuk simpul dalam maupun daun.
    BuildRange(bounds, first, leftSize);
    const uint32_t right = BuildRange(bounds, first + leftSize, count - leftSize);

    nodes_[nodeIndex].start = right;
    nodes_[nodeIndex].count = 0;
    return nodeIndex;
}

}  // namespace sim::raycast
