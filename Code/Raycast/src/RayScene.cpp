#include "Sim/Raycast/RayScene.h"

#include "SceneBackend.h"

#include "Sim/Raycast/Backend.h"

namespace sim::raycast {

const char* ToString(BackendKind kind) {
    switch (kind) {
        case BackendKind::Bvh: return "BVH";
        case BackendKind::Embree: return "Embree";
    }
    return "unknown";
}

BackendKind SelectedBackend() { return BackendKind::Bvh; }

// Konstruktor dan destruktor ada di sini, bukan di header, dan itu syarat:
// `std::unique_ptr<SceneBackend>` menuntut tipe yang lengkap di tempat
// destruktornya dibangkitkan. Membiarkannya di header berarti setiap pemanggil
// harus melihat `SceneBackend` — persis yang aturan "tidak ada tipe backend di
// header" ada untuk mencegah.
RayScene::RayScene() : backend_(std::make_unique<SceneBackend>()) {}
RayScene::~RayScene() = default;
RayScene::RayScene(RayScene&&) noexcept = default;
RayScene& RayScene::operator=(RayScene&&) noexcept = default;

GeometryId RayScene::AddMesh(const void* positions, std::size_t stride, std::size_t vertexCount,
                             std::span<const uint32_t> indices) {
    return backend_->AddMesh(positions, stride, vertexCount, indices);
}

GeometryId RayScene::AddMesh(std::span<const Vec3> positions,
                             std::span<const uint32_t> indices) {
    return backend_->AddMesh(positions.data(), sizeof(Vec3), positions.size(), indices);
}

InstanceId RayScene::AddInstance(GeometryId geometry, const Mat4& transform,
                                 uint64_t userData) {
    return backend_->AddInstance(geometry, transform, userData);
}

void RayScene::SetInstanceTransform(InstanceId instance, const Mat4& transform) {
    backend_->SetInstanceTransform(instance, transform);
}

void RayScene::Commit() { backend_->Commit(); }
void RayScene::ClearInstances() { backend_->ClearInstances(); }
void RayScene::Clear() { backend_->Clear(); }

bool RayScene::IsCommitted() const { return backend_->IsCommitted(); }
std::size_t RayScene::GeometryCount() const { return backend_->GeometryCount(); }
std::size_t RayScene::InstanceCount() const { return backend_->InstanceCount(); }
std::size_t RayScene::TriangleCount() const { return backend_->TriangleCount(); }
uint64_t RayScene::UserDataOf(InstanceId instance) const {
    return backend_->UserDataOf(instance);
}

}  // namespace sim::raycast
