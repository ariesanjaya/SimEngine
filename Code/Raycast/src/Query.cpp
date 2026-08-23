#include "Sim/Raycast/Query.h"

#include "SceneBackend.h"

namespace sim::raycast {

RayHit Raycast(const RayScene& scene, const Vec3& origin, const Vec3& direction,
               float maxDistance) {
    const SceneBackend* backend = scene.Internal();
    return backend != nullptr ? backend->Raycast(origin, direction, maxDistance) : RayHit{};
}

bool Occluded(const RayScene& scene, const Vec3& origin, const Vec3& direction,
              float maxDistance) {
    const SceneBackend* backend = scene.Internal();
    return backend != nullptr && backend->Occluded(origin, direction, maxDistance);
}

ClosestPoint FindClosestPoint(const RayScene& scene, const Vec3& point, float maxDistance) {
    const SceneBackend* backend = scene.Internal();
    return backend != nullptr ? backend->FindClosestPoint(point, maxDistance) : ClosestPoint{};
}

}  // namespace sim::raycast
