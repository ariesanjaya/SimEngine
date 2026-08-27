#include "EmbreeBackend.h"

#include <embree4/rtcore.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sim::raycast {
namespace {

/// Embree membaca simpul terakhir dengan muatan 16 byte.
///
/// **Itu bukan detail yang bisa diabaikan.** Buffer `Vec3` yang rapat berukuran
/// tepat `n * 12` byte, dan membaca 16 byte pada elemen terakhir melewati
/// ujungnya empat byte — yang tidak menghasilkan galat, hanya nilai sampah
/// sesekali di segitiga terakhir, pada sebagian alokasi saja. Buffer dibagi
/// hanya kalau langkahnya sudah menyediakan ruang itu.
constexpr std::size_t kSafeSharedStride = 16;

RTCDevice MakeDevice() {
    // Verbositas nol: Embree menulis ke stderr sendiri, dan log mesin ini punya
    // jalurnya sendiri.
    return rtcNewDevice("verbose=0");
}

Vec3 ReadPosition(const void* base, std::size_t stride, std::size_t index) {
    const auto* bytes = static_cast<const uint8_t*>(base);
    Vec3 out;
    std::memcpy(&out, bytes + index * stride, sizeof(Vec3));
    return out;
}

}  // namespace

Vec3 SceneBackend::Geometry::Position(uint32_t vertex) const {
    return ReadPosition(positions, stride, vertex);
}

void SceneBackend::Geometry::Triangle(uint32_t primitive, Vec3& a, Vec3& b, Vec3& c) const {
    const std::size_t base = static_cast<std::size_t>(primitive) * 3;
    a = Position(indices[base + 0]);
    b = Position(indices[base + 1]);
    c = Position(indices[base + 2]);
}

struct SceneBackend::Impl {
    RTCDevice device = nullptr;
    /// Scene tingkat atas: berisi instance, bukan segitiga.
    RTCScene top = nullptr;
    /// Satu scene per geometri, supaya ia bisa dipakai beberapa instance.
    std::vector<RTCScene> geometryScenes;
    /// Salinan berpadding untuk geometri yang buffernya tidak aman dibagi.
    std::vector<std::vector<float>> paddedPositions;
};

SceneBackend::SceneBackend() : impl_(std::make_unique<Impl>()) {
    impl_->device = MakeDevice();
    impl_->top = rtcNewScene(impl_->device);
}

SceneBackend::~SceneBackend() {
    if (impl_ == nullptr) {
        return;
    }
    for (RTCScene scene : impl_->geometryScenes) {
        if (scene != nullptr) {
            rtcReleaseScene(scene);
        }
    }
    if (impl_->top != nullptr) {
        rtcReleaseScene(impl_->top);
    }
    if (impl_->device != nullptr) {
        rtcReleaseDevice(impl_->device);
    }
}

GeometryId SceneBackend::AddMesh(const void* positions, std::size_t stride,
                                  std::size_t vertexCount, std::span<const uint32_t> indices) {
    // Validasi yang sama dengan jalur BVH, dan jawaban yang sama: geometri yang
    // tidak masuk akal ditolak di sini, bukan dilaporkan sebagai sinar yang
    // meleset nanti.
    if (positions == nullptr || vertexCount == 0 || indices.size() < 3 ||
        indices.size() % 3 != 0 || stride < sizeof(Vec3)) {
        return GeometryId::Invalid;
    }
    for (const uint32_t index : indices) {
        if (index >= vertexCount) {
            return GeometryId::Invalid;
        }
    }

    RTCScene scene = rtcNewScene(impl_->device);
    RTCGeometry geometry = rtcNewGeometry(impl_->device, RTC_GEOMETRY_TYPE_TRIANGLE);

    const void* sharedBase = positions;
    std::size_t sharedStride = stride;
    if (stride < kSafeSharedStride) {
        // Disalin sekali, dengan satu simpul cadangan di ujungnya. Ongkosnya
        // dibayar di sini alih-alih dijadikan pembacaan di luar batas yang
        // hanya kadang-kadang terlihat.
        impl_->paddedPositions.emplace_back((vertexCount + 1) * 4, 0.0f);
        std::vector<float>& padded = impl_->paddedPositions.back();
        for (std::size_t i = 0; i < vertexCount; ++i) {
            const Vec3 p = ReadPosition(positions, stride, i);
            padded[i * 4 + 0] = p.x;
            padded[i * 4 + 1] = p.y;
            padded[i * 4 + 2] = p.z;
        }
        sharedBase = padded.data();
        sharedStride = 4 * sizeof(float);
    }

    rtcSetSharedGeometryBuffer(geometry, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, sharedBase,
                               0, sharedStride, vertexCount);
    // Indeks disalin: `std::span` milik pemanggil dan boleh hilang begitu
    // panggilan ini selesai, sedangkan Embree membacanya sampai scene dilepas.
    Geometry record;
    record.indices.assign(indices.begin(), indices.end());
    rtcSetSharedGeometryBuffer(geometry, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3,
                               record.indices.data(), 0, 3 * sizeof(uint32_t),
                               indices.size() / 3);

    rtcCommitGeometry(geometry);
    const uint32_t embreeId = rtcAttachGeometry(scene, geometry);
    rtcReleaseGeometry(geometry);
    rtcCommitScene(scene);

    record.positions = sharedBase;
    record.stride = sharedStride;
    record.vertexCount = vertexCount;
    record.embreeGeometry = embreeId;
    for (std::size_t i = 0; i < vertexCount; ++i) {
        record.localBounds.Expand(ReadPosition(sharedBase, sharedStride, i));
    }

    impl_->geometryScenes.push_back(scene);
    geometries_.push_back(std::move(record));
    committed_ = false;
    return static_cast<GeometryId>(geometries_.size() - 1);
}

InstanceId SceneBackend::AddInstance(GeometryId geometry, const Mat4& transform,
                                      uint64_t userData) {
    const auto index = static_cast<std::size_t>(geometry);
    if (geometry == GeometryId::Invalid || index >= geometries_.size()) {
        return InstanceId::Invalid;
    }

    RTCGeometry instance = rtcNewGeometry(impl_->device, RTC_GEOMETRY_TYPE_INSTANCE);
    rtcSetGeometryInstancedScene(instance, impl_->geometryScenes[index]);
    // Embree menerima matriks column-major, tata letak yang sama dengan glm.
    rtcSetGeometryTransform(instance, 0, RTC_FORMAT_FLOAT4X4_COLUMN_MAJOR, &transform[0][0]);
    rtcCommitGeometry(instance);
    const uint32_t embreeId = rtcAttachGeometry(impl_->top, instance);
    rtcReleaseGeometry(instance);

    Instance record;
    record.geometry = geometry;
    record.transform = transform;
    record.inverse = glm::inverse(transform);
    record.normalMatrix = glm::transpose(Mat3(record.inverse));
    record.userData = userData;
    record.embreeInstance = embreeId;
    instances_.push_back(record);
    committed_ = false;
    return static_cast<InstanceId>(instances_.size() - 1);
}

void SceneBackend::SetInstanceTransform(InstanceId instance, const Mat4& transform) {
    const auto index = static_cast<std::size_t>(instance);
    if (instance == InstanceId::Invalid || index >= instances_.size()) {
        return;
    }
    Instance& record = instances_[index];
    record.transform = transform;
    record.inverse = glm::inverse(transform);
    record.normalMatrix = glm::transpose(Mat3(record.inverse));

    RTCGeometry geometry = rtcGetGeometry(impl_->top, record.embreeInstance);
    rtcSetGeometryTransform(geometry, 0, RTC_FORMAT_FLOAT4X4_COLUMN_MAJOR, &transform[0][0]);
    rtcCommitGeometry(geometry);
    committed_ = false;
}

void SceneBackend::Commit() {
    rtcCommitScene(impl_->top);
    committed_ = true;
}

void SceneBackend::ClearInstances() {
    for (const Instance& instance : instances_) {
        rtcDetachGeometry(impl_->top, instance.embreeInstance);
    }
    instances_.clear();
    committed_ = false;
}

void SceneBackend::Clear() {
    ClearInstances();
    for (RTCScene scene : impl_->geometryScenes) {
        if (scene != nullptr) {
            rtcReleaseScene(scene);
        }
    }
    impl_->geometryScenes.clear();
    impl_->paddedPositions.clear();
    geometries_.clear();
    committed_ = false;
}

std::size_t SceneBackend::TriangleCount() const {
    // **Per geometri, bukan per instance.** `RayScene::TriangleCount` menyebutnya
    // "tanpa dikali instance-nya", dan itulah yang membuat angka ini bisa
    // dipakai memeriksa bahwa `ClearInstances` mempertahankan geometrinya.
    std::size_t total = 0;
    for (const Geometry& geometry : geometries_) {
        total += geometry.indices.size() / 3;
    }
    return total;
}

uint64_t SceneBackend::UserDataOf(InstanceId instance) const {
    const auto index = static_cast<std::size_t>(instance);
    return index < instances_.size() ? instances_[index].userData : 0;
}

RayHit SceneBackend::Raycast(const Vec3& origin, const Vec3& direction,
                              float maxDistance) const {
    RayHit hit;
    // Penjagaan yang sama dengan jalur BVH, dan ia bukan kerapian: arah bernorma
    // nol menjadi NaN sesudah dinormalkan, dan sebuah sinar NaN tidak ditolak
    // Embree melainkan ditelusurinya sampai jawabannya juga NaN.
    const float length = glm::length(direction);
    if (!committed_ || instances_.empty() || !(length > 0.0f) || !(maxDistance > 0.0f)) {
        return hit;
    }
    const Vec3 unit = direction / length;

    RTCRayHit query{};
    query.ray.org_x = origin.x;
    query.ray.org_y = origin.y;
    query.ray.org_z = origin.z;
    query.ray.dir_x = unit.x;
    query.ray.dir_y = unit.y;
    query.ray.dir_z = unit.z;
    query.ray.tnear = 0.0f;
    query.ray.tfar = maxDistance;
    query.ray.mask = 0xFFFFFFFFu;
    query.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    query.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;

    rtcIntersect1(impl_->top, &query);
    if (query.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
        return hit;
    }

    // **`instID` yang dijawab Embree adalah id lampirannya**, dan urutan lampir
    // sama dengan urutan `instances_` selama tidak ada yang dilepas di tengah.
    // `ClearInstances` melepas semuanya sekaligus, jadi kesamaan itu bertahan.
    const uint32_t instanceId = query.hit.instID[0];
    if (instanceId >= instances_.size()) {
        return hit;
    }

    const Instance& instance = instances_[instanceId];
    const auto geometryIndex = static_cast<std::size_t>(instance.geometry);
    if (geometryIndex >= geometries_.size()) {
        return hit;
    }

    hit.hit = true;
    hit.instance = static_cast<InstanceId>(instanceId);
    hit.primitive = query.hit.primID;
    hit.userData = instance.userData;
    hit.distance = query.ray.tfar;
    hit.position = origin + unit * query.ray.tfar;
    // `(u, v)` Embree memakai perjanjian yang sama dengan `RayTriangle` di
    // `Sim/Core/Intersect.h`: titik kenanya `a + u·(b−a) + v·(c−a)`.
    hit.barycentric = Vec2(query.hit.u, query.hit.v);

    // **Normalnya dihitung dari segitiganya, bukan diambil dari `hit.Ng`.**
    // Embree memang mengembalikan normal geometri — tetapi di ruang lokal
    // instance, dengan perjanjian arah dan penskalaan yang miliknya sendiri.
    // Menyalinnya berarti dua backend yang menjawab normal berlawanan pada
    // sebagian mesh, dan itu bukan backend kedua melainkan renderer kedua.
    // Ongkosnya sama persis dengan yang dibayar jalur BVH di tempat yang sama.
    Vec3 a;
    Vec3 b;
    Vec3 c;
    geometries_[geometryIndex].Triangle(query.hit.primID, a, b, c);
    hit.normal = glm::normalize(instance.normalMatrix * glm::cross(b - a, c - a));
    return hit;
}

bool SceneBackend::Occluded(const Vec3& origin, const Vec3& direction,
                             float maxDistance) const {
    const float length = glm::length(direction);
    if (!committed_ || instances_.empty() || !(length > 0.0f) || !(maxDistance > 0.0f)) {
        return false;
    }
    const Vec3 unit = direction / length;

    RTCRay ray{};
    ray.org_x = origin.x;
    ray.org_y = origin.y;
    ray.org_z = origin.z;
    ray.dir_x = unit.x;
    ray.dir_y = unit.y;
    ray.dir_z = unit.z;
    ray.tnear = 0.0f;
    ray.tfar = maxDistance;
    ray.mask = 0xFFFFFFFFu;

    rtcOccluded1(impl_->top, &ray);
    // Embree menandai tertutup dengan menyetel tfar negatif tak-hingga.
    return ray.tfar < 0.0f;
}

ClosestPoint SceneBackend::FindClosestPoint(const Vec3& point, float maxDistance) const {
    ClosestPoint best;
    if (!committed_) {
        return best;
    }

    // **Ditelusuri sendiri, bukan lewat `rtcPointQuery`.** Query titik Embree
    // menuntut callback per-primitif beserta penanganan tumpukan instance-nya
    // sendiri, dan yang dihasilkannya sama saja dengan yang di bawah — sementara
    // yang memanggilnya adalah alat pengarang, beberapa ribu kali per sapuan,
    // bukan jutaan kali per gambar. Jalur yang jarang dipakai tidak sebanding
    // dengan permukaan API yang harus dipahami terpisah.
    //
    // Batas kotak per instance memotong sebagian besar pekerjaannya.
    float bestSquared = maxDistance * maxDistance;
    for (std::size_t at = 0; at < instances_.size(); ++at) {
        const Instance& instance = instances_[at];
        const auto geometryIndex = static_cast<std::size_t>(instance.geometry);
        if (geometryIndex >= geometries_.size()) {
            continue;
        }
        const Geometry& geometry = geometries_[geometryIndex];
        if (geometry.localBounds.Empty()) {
            continue;
        }
        Aabb worldBounds;
        for (int corner = 0; corner < 8; ++corner) {
            const Vec3 pick((corner & 1) != 0 ? geometry.localBounds.max.x : geometry.localBounds.min.x,
                            (corner & 2) != 0 ? geometry.localBounds.max.y : geometry.localBounds.min.y,
                            (corner & 4) != 0 ? geometry.localBounds.max.z : geometry.localBounds.min.z);
            worldBounds.Expand(Vec3(instance.transform * Vec4(pick, 1.0f)));
        }
        if (worldBounds.DistanceSquared(point) > bestSquared) {
            continue;
        }
        const Vec3 local = Vec3(instance.inverse * Vec4(point, 1.0f));

        for (std::size_t tri = 0; tri + 2 < geometry.indices.size(); tri += 3) {
            const Vec3 a = ReadPosition(geometry.positions, geometry.stride,
                                        geometry.indices[tri + 0]);
            const Vec3 b = ReadPosition(geometry.positions, geometry.stride,
                                        geometry.indices[tri + 1]);
            const Vec3 c = ReadPosition(geometry.positions, geometry.stride,
                                        geometry.indices[tri + 2]);
            const Vec3 candidate = ClosestPointOnTriangle(local, a, b, c);
            const Vec3 world = Vec3(instance.transform * Vec4(candidate, 1.0f));
            const Vec3 delta = world - point;
            const float squared = glm::dot(delta, delta);
            if (squared < bestSquared) {
                bestSquared = squared;
                best.found = true;
                best.position = world;
                best.instance = static_cast<InstanceId>(at);
                best.primitive = static_cast<uint32_t>(tri / 3);
                best.userData = instance.userData;
                best.distance = std::sqrt(squared);
            }
        }
    }
    return best;
}

}  // namespace sim::raycast
