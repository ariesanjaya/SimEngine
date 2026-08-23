#pragma once

#include "Bvh.h"

#include "Sim/Raycast/Query.h"
#include "Sim/Raycast/RayScene.h"

#include <cstddef>
#include <cstring>
#include <vector>

namespace sim::raycast {

/// Satu mesh beserta BVH segitiganya, di ruang lokalnya sendiri.
///
/// **Posisinya tidak disalin.** Yang disimpan pointer, stride, dan cacah — lihat
/// kontrak umur di `RayScene::AddMesh`. Yang benar-benar milik struct ini hanya
/// BVH-nya, dan itu memang tidak ada di tempat lain.
struct MeshGeometry {
    const std::byte* positions = nullptr;
    std::size_t stride = 0;
    std::size_t vertexCount = 0;
    const uint32_t* indices = nullptr;
    std::size_t triangleCount = 0;
    Bvh bvh;

    /// `memcpy`, bukan reinterpret: `positions` boleh menunjuk ke tengah sebuah
    /// struct vertex, dan tidak ada yang menjamin alamatnya sejajar `Vec3`.
    Vec3 Position(uint32_t vertex) const {
        Vec3 value;
        std::memcpy(&value, positions + static_cast<std::size_t>(vertex) * stride, sizeof(Vec3));
        return value;
    }

    void Triangle(uint32_t primitive, Vec3& a, Vec3& b, Vec3& c) const {
        const std::size_t base = static_cast<std::size_t>(primitive) * 3;
        a = Position(indices[base + 0]);
        b = Position(indices[base + 1]);
        c = Position(indices[base + 2]);
    }
};

struct Instance {
    GeometryId geometry = GeometryId::Invalid;
    Mat4 transform{1.0f};
    Mat4 inverse{1.0f};
    /// Invers-transpos bagian rotasinya, untuk membawa normal ke ruang dunia.
    /// Disimpan, bukan dihitung per kena: sebuah inversi matriks per perpotongan
    /// adalah harga yang dibayar setiap sinar untuk angka yang tidak berubah
    /// selama benda itu diam.
    Mat3 normalMatrix{1.0f};
    /// Faktor skala terkecil di antara ketiga sumbu. Dipakai `FindClosestPoint`
    /// mengubah jari-jari dunia menjadi jari-jari lokal secara konservatif.
    float minimumScale = 1.0f;
    uint64_t userData = 0;
};

/// Scene dua tingkat beserta ketiga query-nya.
///
/// Kelasnya hidup di `src/`, dan itulah yang membuat `RayScene` di header publik
/// bisa memegangnya lewat pointer ke tipe tak lengkap — tidak ada satu pun
/// detail penelusuran yang bocor ke pemanggil.
class SceneBackend {
public:
    GeometryId AddMesh(const void* positions, std::size_t stride, std::size_t vertexCount,
                       std::span<const uint32_t> indices);
    InstanceId AddInstance(GeometryId geometry, const Mat4& transform, uint64_t userData);
    void SetInstanceTransform(InstanceId instance, const Mat4& transform);
    void Commit();
    void ClearInstances();
    void Clear();

    bool IsCommitted() const { return committed_; }
    std::size_t GeometryCount() const { return geometries_.size(); }
    std::size_t InstanceCount() const { return instances_.size(); }
    std::size_t TriangleCount() const;
    uint64_t UserDataOf(InstanceId instance) const;

    RayHit Raycast(const Vec3& origin, const Vec3& direction, float maxDistance) const;
    bool Occluded(const Vec3& origin, const Vec3& direction, float maxDistance) const;
    ClosestPoint FindClosestPoint(const Vec3& point, float maxDistance) const;

private:
    /// Kotak dunia sebuah instance: delapan sudut kotak lokalnya, ditransform.
    Aabb WorldBoundsOf(const Instance& instance) const;

    std::vector<MeshGeometry> geometries_;
    std::vector<Instance> instances_;
    std::vector<Aabb> instanceBounds_;
    Bvh top_;
    bool committed_ = false;
};

}  // namespace sim::raycast
