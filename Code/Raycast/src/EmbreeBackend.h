#pragma once

// Backend ray query di atas Intel Embree — R6 di docs/PLAN-EMBREE.md.
//
// **Antarmukanya sama persis dengan `BvhBackend`, sampai ke nama metodenya.**
// Itu yang membuat pemilihannya bisa terjadi saat kompilasi alih-alih lewat
// tabel virtual: `Raycast` dipanggil jutaan kali per gambar acuan, dan sebuah
// panggilan tak-langsung di sana adalah ongkos yang dibayar untuk fleksibilitas
// yang tidak dipakai siapa pun saat berjalan. Yang memilih backend adalah orang
// yang mengonfigurasi build, sekali.
//
// **Tidak ada satu pun tipe Embree yang bocor keluar dari berkas ini.** Header
// publik `Sim::Raycast` tidak menyebut `RTCScene`, `RTCDevice`, maupun
// `embree4/rtcore.h` — dan itu syarat, bukan kerapian: seluruh mesin menautkan
// `Sim::Raycast`, dan sebuah `#include` Embree di header publiknya akan menyeret
// Embree ke setiap unit terjemahan yang menyentuh picking.

#include "Bvh.h"

#include "Sim/Core/Intersect.h"
#include "Sim/Raycast/Query.h"
#include "Sim/Raycast/RayScene.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace sim::raycast {

class SceneBackend {
public:
    SceneBackend();
    ~SceneBackend();

    SceneBackend(const SceneBackend&) = delete;
    SceneBackend& operator=(const SceneBackend&) = delete;

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
    struct Impl;
    /// **Pimpl, dan bukan demi waktu kompilasi.** Ia yang menjaga janji di atas:
    /// tanpa sebuah tipe tak-lengkap di sini, header ini harus menyebut tipe
    /// Embree, dan berkas ini disertakan `SceneBackend.h` yang dipakai seluruh
    /// modul.
    std::unique_ptr<Impl> impl_;

    /// Geometri disimpan **juga** di sisi kita, bukan hanya di Embree.
    ///
    /// Embree menjawab `(geomID, primID)` dan jarak; ia tidak menyimpan posisi
    /// dalam bentuk yang bisa ditanyai titik terdekat. `FindClosestPoint`
    /// karena itu berjalan di atas salinan ini — dan salinan itu **berbagi
    /// buffer** dengan yang diberikan pemanggil, sama seperti jalur BVH:
    /// menyalin posisi mesh dua kali adalah puluhan megabyte untuk pertanyaan
    /// yang jarang ditanyakan.
    struct Geometry {
        const void* positions = nullptr;
        std::size_t stride = 0;
        std::size_t vertexCount = 0;
        std::vector<uint32_t> indices;
        uint32_t embreeGeometry = 0;
        /// Kotak ruang lokal. Embree punya kotaknya sendiri, tetapi tidak
        /// meminjamkannya untuk pertanyaan yang bukan sinar — dan
        /// `FindClosestPoint` di bawah membutuhkannya untuk memangkas.
        Aabb localBounds;

        Vec3 Position(uint32_t vertex) const;
        void Triangle(uint32_t primitive, Vec3& a, Vec3& b, Vec3& c) const;
    };

    struct Instance {
        GeometryId geometry = GeometryId::Invalid;
        Mat4 transform{1.0f};
        Mat4 inverse{1.0f};
        /// Invers-transpos bagian rotasinya. Alasannya sama persis dengan yang
        /// tertulis di `BvhBackend.h`: sebuah inversi matriks per perpotongan
        /// adalah harga yang dibayar setiap sinar untuk angka yang tidak
        /// berubah selama benda itu diam.
        Mat3 normalMatrix{1.0f};
        uint64_t userData = 0;
        uint32_t embreeInstance = 0;
    };

    std::vector<Geometry> geometries_;
    std::vector<Instance> instances_;
    bool committed_ = false;
};

}  // namespace sim::raycast
