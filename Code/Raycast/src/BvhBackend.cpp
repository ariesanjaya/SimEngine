#include "BvhBackend.h"

#include "Sim/Core/Intersect.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace sim::raycast {
namespace {

/// Kedalaman tumpukan penelusuran.
///
/// **Tumpukan tetap di stack, bukan `std::vector`.** Penelusuran adalah gelung
/// terpanas modul ini, dan sebuah alokasi heap per sinar akan mendominasi
/// biayanya. 64 memberi ruang untuk pohon berdaun 2^64 — jauh di atas apa pun
/// yang bisa dibangun — jadi luapan tidak mungkin terjadi tanpa BVH yang rusak.
constexpr std::size_t kStackDepth = 64;

/// Hasil penelusuran di dalam satu geometri, ruang lokalnya sendiri.
struct LocalHit {
    bool hit = false;
    float distance = 0.0f;
    uint32_t primitive = 0;
    Vec2 barycentric{0.0f};
};

Vec3 InverseDirection(const Vec3& direction) {
    // Pembagian dengan nol dibiarkan menghasilkan ±inf; `RayAabb` menjawab benar
    // untuk sinar yang sejajar sumbu. Lihat catatannya di `Sim/Core/Intersect.h`.
    return Vec3(1.0f) / direction;
}

/// Sinar terhadap satu geometri. `anyHit` berhenti pada perpotongan pertama.
LocalHit IntersectMesh(const MeshGeometry& mesh, const Vec3& origin, const Vec3& direction,
                       float maxDistance, bool anyHit) {
    LocalHit result;
    if (mesh.bvh.Empty()) {
        return result;
    }

    const std::vector<BvhNode>& nodes = mesh.bvh.Nodes();
    const std::vector<uint32_t>& order = mesh.bvh.Order();
    const Vec3 inverse = InverseDirection(direction);

    std::array<uint32_t, kStackDepth> stack{};
    std::size_t depth = 0;
    stack[depth++] = 0;
    float nearest = maxDistance;

    while (depth > 0) {
        const uint32_t nodeIndex = stack[--depth];
        const BvhNode& node = nodes[nodeIndex];

        float entry = 0.0f;
        if (!RayAabb(origin, inverse, node.bounds.min, node.bounds.max, nearest, entry)) {
            continue;
        }

        if (node.count != 0) {
            for (uint32_t i = 0; i < node.count; ++i) {
                const uint32_t primitive = order[node.start + i];
                Vec3 a;
                Vec3 b;
                Vec3 c;
                mesh.Triangle(primitive, a, b, c);
                const TriangleHit hit = RayTriangle(origin, direction, a, b, c, nearest);
                if (!hit) {
                    continue;
                }
                nearest = hit.distance;
                result.hit = true;
                result.distance = hit.distance;
                result.primitive = primitive;
                result.barycentric = hit.barycentric;
                if (anyHit) {
                    return result;
                }
            }
            continue;
        }

        // **Yang lebih dekat ditelusuri lebih dulu**, dan itu bukan penyetelan:
        // begitu sebuah perpotongan ditemukan, ia mempersempit `nearest` untuk
        // seluruh sisa penelusuran. Urutan terbalik membuat pemangkasan itu
        // datang paling akhir, saat tidak ada lagi yang bisa dipangkas.
        const uint32_t children[2] = {nodeIndex + 1u, node.start};

        float nearDistance[2] = {0.0f, 0.0f};
        bool visit[2] = {false, false};
        for (int child = 0; child < 2; ++child) {
            const BvhNode& candidate = nodes[children[child]];
            visit[child] = RayAabb(origin, inverse, candidate.bounds.min, candidate.bounds.max,
                                   nearest, nearDistance[child]);
        }

        if (visit[0] && visit[1]) {
            const int first = nearDistance[0] <= nearDistance[1] ? 0 : 1;
            stack[depth++] = children[1 - first];  // yang jauh di bawah
            stack[depth++] = children[first];
        } else if (visit[0]) {
            stack[depth++] = children[0];
        } else if (visit[1]) {
            stack[depth++] = children[1];
        }
    }

    return result;
}

/// Titik terdekat di dalam satu geometri, ruang lokalnya sendiri.
struct LocalClosest {
    bool found = false;
    float distanceSquared = 0.0f;
    Vec3 position{0.0f};
    uint32_t primitive = 0;
};

LocalClosest ClosestOnMesh(const MeshGeometry& mesh, const Vec3& point, float maxDistance) {
    LocalClosest result;
    if (mesh.bvh.Empty()) {
        return result;
    }

    const std::vector<BvhNode>& nodes = mesh.bvh.Nodes();
    const std::vector<uint32_t>& order = mesh.bvh.Order();

    std::array<uint32_t, kStackDepth> stack{};
    std::size_t depth = 0;
    stack[depth++] = 0;
    float best = maxDistance * maxDistance;

    while (depth > 0) {
        const uint32_t nodeIndex = stack[--depth];
        const BvhNode& node = nodes[nodeIndex];
        if (node.bounds.DistanceSquared(point) >= best) {
            continue;
        }

        if (node.count != 0) {
            for (uint32_t i = 0; i < node.count; ++i) {
                const uint32_t primitive = order[node.start + i];
                Vec3 a;
                Vec3 b;
                Vec3 c;
                mesh.Triangle(primitive, a, b, c);
                const Vec3 candidate = ClosestPointOnTriangle(point, a, b, c);
                const Vec3 offset = candidate - point;
                const float distanceSquared = glm::dot(offset, offset);
                if (distanceSquared >= best) {
                    continue;
                }
                best = distanceSquared;
                result.found = true;
                result.distanceSquared = distanceSquared;
                result.position = candidate;
                result.primitive = primitive;
            }
            continue;
        }

        const uint32_t children[2] = {nodeIndex + 1u, node.start};
        const float distances[2] = {nodes[children[0]].bounds.DistanceSquared(point),
                                    nodes[children[1]].bounds.DistanceSquared(point)};
        const int first = distances[0] <= distances[1] ? 0 : 1;
        if (distances[1 - first] < best) {
            stack[depth++] = children[1 - first];
        }
        if (distances[first] < best) {
            stack[depth++] = children[first];
        }
    }

    return result;
}

}  // namespace

GeometryId SceneBackend::AddMesh(const void* positions, std::size_t stride,
                                 std::size_t vertexCount, std::span<const uint32_t> indices) {
    if (positions == nullptr || stride < sizeof(Vec3) || vertexCount == 0 || indices.empty() ||
        indices.size() % 3 != 0) {
        return GeometryId::Invalid;
    }
    // Indeks di luar batas ditolak di sini, sekali, alih-alih menjadi pembacaan
    // liar di dalam gelung penelusuran yang tidak punya cara melaporkannya.
    for (const uint32_t index : indices) {
        if (index >= vertexCount) {
            return GeometryId::Invalid;
        }
    }

    MeshGeometry mesh;
    mesh.positions = static_cast<const std::byte*>(positions);
    mesh.stride = stride;
    mesh.vertexCount = vertexCount;
    mesh.indices = indices.data();
    mesh.triangleCount = indices.size() / 3;

    std::vector<Aabb> bounds(mesh.triangleCount);
    for (std::size_t i = 0; i < mesh.triangleCount; ++i) {
        Vec3 a;
        Vec3 b;
        Vec3 c;
        mesh.Triangle(static_cast<uint32_t>(i), a, b, c);
        bounds[i].Expand(a);
        bounds[i].Expand(b);
        bounds[i].Expand(c);
    }
    mesh.bvh.Build(bounds);

    geometries_.push_back(std::move(mesh));
    committed_ = false;
    return static_cast<GeometryId>(geometries_.size() - 1);
}

InstanceId SceneBackend::AddInstance(GeometryId geometry, const Mat4& transform,
                                     uint64_t userData) {
    const auto index = static_cast<std::size_t>(geometry);
    if (geometry == GeometryId::Invalid || index >= geometries_.size()) {
        return InstanceId::Invalid;
    }

    Instance instance;
    instance.geometry = geometry;
    instance.userData = userData;
    instances_.push_back(instance);
    SetInstanceTransform(static_cast<InstanceId>(instances_.size() - 1), transform);
    return static_cast<InstanceId>(instances_.size() - 1);
}

void SceneBackend::SetInstanceTransform(InstanceId instance, const Mat4& transform) {
    const auto index = static_cast<std::size_t>(instance);
    if (instance == InstanceId::Invalid || index >= instances_.size()) {
        return;
    }
    Instance& target = instances_[index];
    target.transform = transform;
    target.inverse = glm::inverse(transform);
    target.normalMatrix = glm::transpose(Mat3(target.inverse));

    const Mat3 linear(transform);
    target.minimumScale = std::min({glm::length(linear[0]), glm::length(linear[1]),
                                    glm::length(linear[2])});
    committed_ = false;
}

Aabb SceneBackend::WorldBoundsOf(const Instance& instance) const {
    Aabb result;
    const auto geometryIndex = static_cast<std::size_t>(instance.geometry);
    if (geometryIndex >= geometries_.size()) {
        return result;
    }
    const Aabb& local = geometries_[geometryIndex].bvh.Bounds();
    if (local.Empty()) {
        return result;
    }
    // Kedelapan sudut, bukan kedua titik ekstremnya: kotak yang diputar tidak
    // lagi sejajar sumbu, dan mentransform hanya min dan max menghasilkan kotak
    // yang tidak melingkupi bendanya.
    for (int corner = 0; corner < 8; ++corner) {
        const Vec3 point((corner & 1) ? local.max.x : local.min.x,
                         (corner & 2) ? local.max.y : local.min.y,
                         (corner & 4) ? local.max.z : local.min.z);
        result.Expand(Vec3(instance.transform * Vec4(point, 1.0f)));
    }
    return result;
}

void SceneBackend::Commit() {
    instanceBounds_.clear();
    instanceBounds_.reserve(instances_.size());
    for (const Instance& instance : instances_) {
        instanceBounds_.push_back(WorldBoundsOf(instance));
    }
    top_.Build(instanceBounds_);
    committed_ = true;
}

void SceneBackend::ClearInstances() {
    instances_.clear();
    instanceBounds_.clear();
    top_.Clear();
    committed_ = false;
}

void SceneBackend::Clear() {
    geometries_.clear();
    instances_.clear();
    instanceBounds_.clear();
    top_.Clear();
    committed_ = false;
}

std::size_t SceneBackend::TriangleCount() const {
    std::size_t total = 0;
    for (const MeshGeometry& mesh : geometries_) {
        total += mesh.triangleCount;
    }
    return total;
}

uint64_t SceneBackend::UserDataOf(InstanceId instance) const {
    const auto index = static_cast<std::size_t>(instance);
    return index < instances_.size() ? instances_[index].userData : 0;
}

RayHit SceneBackend::Raycast(const Vec3& origin, const Vec3& direction,
                             float maxDistance) const {
    RayHit result;
    const float length = glm::length(direction);
    if (!committed_ || top_.Empty() || !(length > 0.0f) || !(maxDistance > 0.0f)) {
        return result;
    }
    const Vec3 ray = direction / length;
    const Vec3 inverse = InverseDirection(ray);

    const std::vector<BvhNode>& nodes = top_.Nodes();
    const std::vector<uint32_t>& order = top_.Order();

    std::array<uint32_t, kStackDepth> stack{};
    std::size_t depth = 0;
    stack[depth++] = 0;
    float nearest = maxDistance;

    while (depth > 0) {
        const uint32_t nodeIndex = stack[--depth];
        const BvhNode& node = nodes[nodeIndex];

        float entry = 0.0f;
        if (!RayAabb(origin, inverse, node.bounds.min, node.bounds.max, nearest, entry)) {
            continue;
        }

        if (node.count != 0) {
            for (uint32_t i = 0; i < node.count; ++i) {
                const uint32_t instanceIndex = order[node.start + i];
                const Instance& instance = instances_[instanceIndex];
                const MeshGeometry& mesh =
                    geometries_[static_cast<std::size_t>(instance.geometry)];

                // **Arah lokal sengaja tidak dinormalkan.** Skala instance ikut
                // terbawa ke dalamnya, dan justru itu yang membuat `t` di ruang
                // lokal sama persis dengan `t` di ruang dunia — sehingga jarak
                // dari dua instance berskala berbeda tetap bisa dibandingkan.
                const Vec3 localOrigin = Vec3(instance.inverse * Vec4(origin, 1.0f));
                const Vec3 localRay = Vec3(instance.inverse * Vec4(ray, 0.0f));

                const LocalHit hit =
                    IntersectMesh(mesh, localOrigin, localRay, nearest, /*anyHit=*/false);
                if (!hit.hit) {
                    continue;
                }

                nearest = hit.distance;
                result.hit = true;
                result.instance = static_cast<InstanceId>(instanceIndex);
                result.primitive = hit.primitive;
                result.userData = instance.userData;
                result.distance = hit.distance;
                result.position = origin + ray * hit.distance;
                result.barycentric = hit.barycentric;

                Vec3 a;
                Vec3 b;
                Vec3 c;
                mesh.Triangle(hit.primitive, a, b, c);
                const Vec3 localNormal = glm::cross(b - a, c - a);
                result.normal = glm::normalize(instance.normalMatrix * localNormal);
            }
            continue;
        }

        const uint32_t children[2] = {nodeIndex + 1u, node.start};
        float nearDistance[2] = {0.0f, 0.0f};
        bool visit[2] = {false, false};
        for (int child = 0; child < 2; ++child) {
            const BvhNode& candidate = nodes[children[child]];
            visit[child] = RayAabb(origin, inverse, candidate.bounds.min, candidate.bounds.max,
                                   nearest, nearDistance[child]);
        }
        if (visit[0] && visit[1]) {
            const int first = nearDistance[0] <= nearDistance[1] ? 0 : 1;
            stack[depth++] = children[1 - first];
            stack[depth++] = children[first];
        } else if (visit[0]) {
            stack[depth++] = children[0];
        } else if (visit[1]) {
            stack[depth++] = children[1];
        }
    }

    return result;
}

bool SceneBackend::Occluded(const Vec3& origin, const Vec3& direction,
                            float maxDistance) const {
    const float length = glm::length(direction);
    if (!committed_ || top_.Empty() || !(length > 0.0f) || !(maxDistance > 0.0f)) {
        return false;
    }
    const Vec3 ray = direction / length;
    const Vec3 inverse = InverseDirection(ray);

    const std::vector<BvhNode>& nodes = top_.Nodes();
    const std::vector<uint32_t>& order = top_.Order();

    std::array<uint32_t, kStackDepth> stack{};
    std::size_t depth = 0;
    stack[depth++] = 0;

    while (depth > 0) {
        const uint32_t nodeIndex = stack[--depth];
        const BvhNode& node = nodes[nodeIndex];

        float entry = 0.0f;
        if (!RayAabb(origin, inverse, node.bounds.min, node.bounds.max, maxDistance, entry)) {
            continue;
        }

        if (node.count != 0) {
            for (uint32_t i = 0; i < node.count; ++i) {
                const Instance& instance = instances_[order[node.start + i]];
                const MeshGeometry& mesh =
                    geometries_[static_cast<std::size_t>(instance.geometry)];
                const Vec3 localOrigin = Vec3(instance.inverse * Vec4(origin, 1.0f));
                const Vec3 localRay = Vec3(instance.inverse * Vec4(ray, 0.0f));
                if (IntersectMesh(mesh, localOrigin, localRay, maxDistance, /*anyHit=*/true).hit) {
                    return true;
                }
            }
            continue;
        }

        // Tanpa pengurutan: yang dicari jawaban ya/tidak, dan mengurutkan anak
        // menurut jarak hanya berguna kalau ada yang bisa dipersempit olehnya.
        stack[depth++] = nodeIndex + 1u;
        stack[depth++] = node.start;
    }

    return false;
}

ClosestPoint SceneBackend::FindClosestPoint(const Vec3& point, float maxDistance) const {
    ClosestPoint result;
    if (!committed_ || top_.Empty() || !(maxDistance > 0.0f)) {
        return result;
    }

    const std::vector<BvhNode>& nodes = top_.Nodes();
    const std::vector<uint32_t>& order = top_.Order();

    std::array<uint32_t, kStackDepth> stack{};
    std::size_t depth = 0;
    stack[depth++] = 0;
    float best = maxDistance * maxDistance;

    while (depth > 0) {
        const uint32_t nodeIndex = stack[--depth];
        const BvhNode& node = nodes[nodeIndex];
        if (node.bounds.DistanceSquared(point) >= best) {
            continue;
        }

        if (node.count != 0) {
            for (uint32_t i = 0; i < node.count; ++i) {
                const uint32_t instanceIndex = order[node.start + i];
                const Instance& instance = instances_[instanceIndex];
                const MeshGeometry& mesh =
                    geometries_[static_cast<std::size_t>(instance.geometry)];

                // **Jari-jari dunia diubah menjadi jari-jari lokal dengan skala
                // terkecil, dan itu memang konservatif.** Di bawah skala tak
                // seragam, satu meter dunia bukan satu jarak lokal yang tunggal;
                // membagi dengan yang terkecil menghasilkan jari-jari lokal yang
                // terlalu besar — memangkas lebih sedikit, tidak pernah
                // memangkas yang seharusnya ikut.
                const float scale = instance.minimumScale;
                const float localRadius =
                    scale > 1e-6f ? std::sqrt(best) / scale : std::sqrt(best);

                const Vec3 localPoint = Vec3(instance.inverse * Vec4(point, 1.0f));
                const LocalClosest candidate = ClosestOnMesh(mesh, localPoint, localRadius);
                if (!candidate.found) {
                    continue;
                }

                // Jaraknya diukur ulang di ruang dunia: yang di ruang lokal
                // berskala lain, dan membandingkan keduanya akan memilih
                // instance yang paling kecil skalanya alih-alih yang terdekat.
                const Vec3 world = Vec3(instance.transform * Vec4(candidate.position, 1.0f));
                const Vec3 offset = world - point;
                const float distanceSquared = glm::dot(offset, offset);
                if (distanceSquared >= best) {
                    continue;
                }

                best = distanceSquared;
                result.found = true;
                result.instance = static_cast<InstanceId>(instanceIndex);
                result.primitive = candidate.primitive;
                result.userData = instance.userData;
                result.position = world;
                result.distance = std::sqrt(distanceSquared);
            }
            continue;
        }

        const uint32_t children[2] = {nodeIndex + 1u, node.start};
        const float distances[2] = {nodes[children[0]].bounds.DistanceSquared(point),
                                    nodes[children[1]].bounds.DistanceSquared(point)};
        const int first = distances[0] <= distances[1] ? 0 : 1;
        if (distances[1 - first] < best) {
            stack[depth++] = children[1 - first];
        }
        if (distances[first] < best) {
            stack[depth++] = children[first];
        }
    }

    return result;
}

}  // namespace sim::raycast
