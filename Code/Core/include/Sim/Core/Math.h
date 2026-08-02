#pragma once

// Alias matematika. Semua kode SimEngine memakai nama-nama ini, bukan glm::
// langsung, supaya penggantian pustaka nanti tidak menyentuh seluruh repo.
//
// Konvensi (dikunci di docs/PLAN-RENDER.md):
//   - tangan-kanan, Y-up, satuan meter
//   - sudut disimpan dalam radian, ditampilkan dalam derajat
//   - depth Vulkan [0,1] (GLM_FORCE_DEPTH_ZERO_TO_ONE di CMake)

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>

namespace sim {

using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using IVec2 = glm::ivec2;
using IVec3 = glm::ivec3;
using UVec2 = glm::uvec2;
using Quat = glm::quat;
using Mat3 = glm::mat3;
using Mat4 = glm::mat4;

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kTwoPi = kPi * 2.0f;
inline constexpr float kHalfPi = kPi * 0.5f;
inline constexpr float kDegToRad = kPi / 180.0f;
inline constexpr float kRadToDeg = 180.0f / kPi;

inline constexpr Vec3 kAxisX{1.0f, 0.0f, 0.0f};
inline constexpr Vec3 kAxisY{0.0f, 1.0f, 0.0f};
inline constexpr Vec3 kAxisZ{0.0f, 0.0f, 1.0f};
inline constexpr Vec3 kUp = kAxisY;

/// Proyeksi perspektif untuk Vulkan.
///
/// glm::perspective berasal dari OpenGL, yang sumbu Y clip-space-nya terbalik
/// dibanding Vulkan. Membalik [1][1] di sini lebih murah daripada memakai
/// viewport bertinggi negatif, dan berlaku seragam untuk semua pass.
inline Mat4 Perspective(float fovYRadians, float aspect, float zNear, float zFar) {
    Mat4 proj = glm::perspective(fovYRadians, aspect, zNear, zFar);
    proj[1][1] *= -1.0f;
    return proj;
}

inline Mat4 LookAt(const Vec3& eye, const Vec3& target, const Vec3& up = kUp) {
    return glm::lookAt(eye, target, up);
}

}  // namespace sim
