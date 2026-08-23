#pragma once

#include "Sim/Core/Math.h"

#include <cstdint>
#include <limits>
#include <span>
#include <vector>

/// BVH biner ber-SAH, dan kotak sejajar sumbu yang dipakainya.
///
/// **Berkas ini ada di `src/`, bukan di `include/`.** `Aabb` di bawah adalah
/// kembaran ketiga di repo ini — `render::Aabb` sudah ada, dan `SceneView` punya
/// ujinya sendiri — dan itu sengaja tidak diperbaiki di sini: `Sim::Render`
/// tidak boleh ditarik ke dalam modul ini, dan memindahkan `Aabb` ke `Sim::Core`
/// menyentuh Render, yang bukan pekerjaan R0. Karena tipe ini tidak pernah muncul
/// di header publik, ia detail implementasi, bukan kosakata kedua.
namespace sim::raycast {

struct Aabb {
    Vec3 min{std::numeric_limits<float>::max()};
    Vec3 max{std::numeric_limits<float>::lowest()};

    void Expand(const Vec3& point) {
        min = glm::min(min, point);
        max = glm::max(max, point);
    }
    void Expand(const Aabb& other) {
        min = glm::min(min, other.min);
        max = glm::max(max, other.max);
    }
    Vec3 Centre() const { return (min + max) * 0.5f; }
    Vec3 Extent() const { return glm::max(max - min, Vec3(0.0f)); }
    bool Empty() const { return min.x > max.x || min.y > max.y || min.z > max.z; }

    /// Luas permukaan, yaitu besaran yang dipakai SAH. Kotak kosong berluas nol,
    /// bukan negatif — tanpa penjepitan di `Extent()` sebuah kotak kosong
    /// menghasilkan biaya negatif dan memenangkan setiap pembagian.
    float SurfaceArea() const {
        const Vec3 d = Extent();
        return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
    }

    /// Jarak kuadrat dari sebuah titik ke kotak ini; nol bila di dalamnya.
    float DistanceSquared(const Vec3& point) const {
        const Vec3 outside = glm::max(glm::max(min - point, point - max), Vec3(0.0f));
        return glm::dot(outside, outside);
    }
};

/// Satu simpul. Daun bila `count` bukan nol.
///
/// **Anak kiri implisit.** Ia selalu berada di `nodeIndex + 1` karena
/// pembangunnya merekursi ke kiri lebih dulu, jadi yang perlu disimpan hanya
/// anak kanan — dan `start` bisa melayani simpul dalam maupun daun dengan satu
/// medan yang sama.
struct BvhNode {
    Aabb bounds;
    /// Daun: indeks pertama ke dalam `Order()`. Simpul dalam: indeks anak
    /// **kanan**; yang kiri ada di simpul berikutnya.
    uint32_t start = 0;
    /// Nol berarti simpul dalam.
    uint32_t count = 0;
};

/// Membangun BVH atas sekumpulan kotak, dan mengembalikan urutan primitifnya.
///
/// **Bekerja atas kotak, bukan atas segitiga.** Tingkat bawah membangunnya dari
/// kotak segitiga dan tingkat atas dari kotak instance; satu pembangun untuk
/// keduanya berarti perbaikan pada heuristiknya berlaku di dua tempat sekaligus.
class Bvh {
public:
    /// SAH terbin. `bounds` diindeks nomor primitif.
    void Build(std::span<const Aabb> bounds);
    void Clear();

    const std::vector<BvhNode>& Nodes() const { return nodes_; }
    /// Nomor primitif, disusun ulang sehingga tiap daun menempati rentang rapat.
    const std::vector<uint32_t>& Order() const { return order_; }
    bool Empty() const { return nodes_.empty(); }
    const Aabb& Bounds() const { return nodes_.empty() ? empty_ : nodes_.front().bounds; }

private:
    uint32_t BuildRange(std::span<const Aabb> bounds, uint32_t first, uint32_t count);

    std::vector<BvhNode> nodes_;
    std::vector<uint32_t> order_;
    Aabb empty_;
};

}  // namespace sim::raycast
