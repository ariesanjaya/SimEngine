#include "Sim/Render/Frustum.h"

#include <cmath>

namespace sim::render {

Aabb TransformAabb(const Aabb& local, const Mat4& transform) {
    // Pusat dipindahkan seperti titik biasa; setengah-lebarnya dipindahkan lewat
    // nilai mutlak matriksnya. Cara ini memberi kotak sejajar sumbu terkecil yang
    // masih memuat kotak yang diputar, dan ia hanya butuh tiga hasil kali titik —
    // bukan delapan sudut yang ditransformasikan lalu dicari batasnya.
    const Vec3 centre = local.Centre();
    const Vec3 extent = local.Extent();
    const Vec3 worldCentre = Vec3(transform * Vec4(centre, 1.0f));

    const Mat3 basis(transform);
    const Vec3 worldExtent(
        std::abs(basis[0][0]) * extent.x + std::abs(basis[1][0]) * extent.y +
            std::abs(basis[2][0]) * extent.z,
        std::abs(basis[0][1]) * extent.x + std::abs(basis[1][1]) * extent.y +
            std::abs(basis[2][1]) * extent.z,
        std::abs(basis[0][2]) * extent.x + std::abs(basis[1][2]) * extent.y +
            std::abs(basis[2][2]) * extent.z);

    Aabb result;
    result.min = worldCentre - worldExtent;
    result.max = worldCentre + worldExtent;
    return result;
}

void Frustum::Extract(const Mat4& m) {
    // Baris matriks, bukan kolomnya: glm menyimpan kolom-mayor, jadi `m[i]`
    // adalah kolom ke-i. Yang dibutuhkan rumus Gribb–Hartmann adalah barisnya.
    const Vec4 row0(m[0][0], m[1][0], m[2][0], m[3][0]);
    const Vec4 row1(m[0][1], m[1][1], m[2][1], m[3][1]);
    const Vec4 row2(m[0][2], m[1][2], m[2][2], m[3][2]);
    const Vec4 row3(m[0][3], m[1][3], m[2][3], m[3][3]);

    // Ruang clip Vulkan: x,y ∈ [-w, w] dan z ∈ [0, w]. Dua bidang terakhir
    // karena itu berbeda dari rumus OpenGL yang beredar luas — memakai rumus
    // OpenGL di sini menghasilkan frustum yang terlalu panjang ke belakang, dan
    // objek di belakang kamera ikut lolos.
    planes_[0] = row3 + row0;  // kiri
    planes_[1] = row3 - row0;  // kanan
    planes_[2] = row3 + row1;  // bawah
    planes_[3] = row3 - row1;  // atas
    planes_[4] = row2;         // z >= 0
    planes_[5] = row3 - row2;  // z <= w

    for (Vec4& plane : planes_) {
        const float length = std::sqrt(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
        if (length > 1e-8f) {
            // Dinormalkan supaya `dot(n, p) + d` benar-benar jarak, bukan sekadar
            // angka yang tandanya benar. Tanda saja cukup untuk membuang, tapi
            // tidak cukup untuk memilih LOD atau menyusutkan kotak bayangan —
            // dan keduanya akan memakai bidang yang sama.
            plane /= length;
        }
    }
}

bool Frustum::Contains(const Vec3& point) const {
    for (const Vec4& plane : planes_) {
        if (plane.x * point.x + plane.y * point.y + plane.z * point.z + plane.w < 0.0f) {
            return false;
        }
    }
    return true;
}

bool Frustum::Intersects(const Aabb& box) const {
    const Vec3 centre = box.Centre();
    const Vec3 extent = box.Extent();
    for (const Vec4& plane : planes_) {
        // Jarak pusat ke bidang, dikurangi jangkauan kotak ke arah bidang itu.
        // Ini uji "sudut positif" yang ditulis tanpa cabang: `|n| · e` adalah
        // seberapa jauh sudut terjauh kotak menonjol ke arah normalnya.
        const float distance =
            plane.x * centre.x + plane.y * centre.y + plane.z * centre.z + plane.w;
        const float reach = std::abs(plane.x) * extent.x + std::abs(plane.y) * extent.y +
                            std::abs(plane.z) * extent.z;
        if (distance + reach < 0.0f) {
            return false;
        }
    }
    return true;
}

std::size_t CullAabbs(const Frustum& frustum, std::span<const Aabb> boxes,
                      std::vector<uint32_t>& visible) {
    visible.clear();
    visible.reserve(boxes.size());
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        if (frustum.Intersects(boxes[i])) {
            visible.push_back(static_cast<uint32_t>(i));
        }
    }
    return visible.size();
}

Mat4 PerspectiveReversedZ(float fovYRadians, float aspect, float nearZ, float farZ) {
    const float f = 1.0f / std::tan(fovYRadians * 0.5f);
    Mat4 result(0.0f);
    result[0][0] = f / aspect;
    // Y dibalik seperti pada `Perspective` di Core: ruang clip Vulkan sumbu
    // Y-nya berlawanan dengan OpenGL, dan membalik di sini lebih murah daripada
    // viewport bertinggi negatif.
    result[1][1] = -f;
    result[2][2] = nearZ / (farZ - nearZ);
    result[2][3] = -1.0f;
    result[3][2] = (farZ * nearZ) / (farZ - nearZ);
    return result;
}

Mat4 PerspectiveReversedZInfinite(float fovYRadians, float aspect, float nearZ) {
    const float f = 1.0f / std::tan(fovYRadians * 0.5f);
    Mat4 result(0.0f);
    result[0][0] = f / aspect;
    result[1][1] = -f;
    result[2][2] = 0.0f;
    result[2][3] = -1.0f;
    result[3][2] = nearZ;
    return result;
}

}  // namespace sim::render
